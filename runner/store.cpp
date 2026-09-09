// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pineforge::live {
namespace {

constexpr std::size_t max_record_bytes = 1024 * 1024;

[[noreturn]] void database_error() {
    // SQLite errors can quote values. Do not leak an opaque payload or config.
    throw std::runtime_error("native ledger database operation failed");
}

void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) database_error();
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) database_error();
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int col, const std::string& value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            sqlite3_bind_text(stmt_, col, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) database_error();
    }
    void bind(int col, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
            throw std::runtime_error("native ledger ordinal exceeds SQLite range");
        if (sqlite3_bind_int64(stmt_, col, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            database_error();
    }
    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        database_error();
    }
    void done() { if (row()) database_error(); }
    std::string text(int col) const {
        const auto* ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
        const int size = sqlite3_column_bytes(stmt_, col);
        if (!ptr) database_error();
        return std::string(ptr, static_cast<std::size_t>(size));
    }
    std::uint64_t integer(int col) const {
        if (sqlite3_column_type(stmt_, col) != SQLITE_INTEGER) database_error();
        const sqlite3_int64 value = sqlite3_column_int64(stmt_, col);
        if (value < 0) database_error();
        return static_cast<std::uint64_t>(value);
    }
private:
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { exec(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};

std::uint64_t scalar(sqlite3* db, const char* sql) {
    Statement q(db, sql);
    if (!q.row()) database_error();
    return q.integer(0);
}

void validate_bytes(const std::string& bytes, const char* label) {
    if (bytes.empty() || bytes.size() > max_record_bytes)
        throw std::runtime_error(std::string("native ledger invalid ") + label + " size");
}

std::string safe_category(std::string_view category) {
    if (category.empty() || category.size() > 64) return "delivery_failed";
    for (const char c : category)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return "delivery_failed";
    return std::string(category);
}

StoredEvent read_event(const Statement& q, int start = 0) {
    StoredEvent event;
    event.ordinal = q.integer(start);
    event.id = q.text(start + 1);
    event.payload = q.text(start + 2);
    const auto attempts = q.integer(start + 3);
    if (attempts > std::numeric_limits<std::uint32_t>::max()) database_error();
    event.attempts = static_cast<std::uint32_t>(attempts);
    return event;
}

} // namespace

struct Ledger::Impl {
    sqlite3* db = nullptr;
    int lock_fd = -1;
    int database_fd = -1;
    ~Impl() {
        if (db) sqlite3_close_v2(db);
        if (database_fd >= 0) close(database_fd);
        if (lock_fd >= 0) close(lock_fd);
    }

    StoredEvent require_head(const std::string& id) const {
        Statement q(db, "SELECT ordinal,event_id,payload,attempts FROM events "
                        "WHERE acknowledged=0 ORDER BY ordinal LIMIT 1");
        if (!q.row()) throw std::runtime_error("native ledger has no pending delivery");
        auto event = read_event(q);
        if (event.id != id) throw std::runtime_error("native ledger delivery must preserve head order");
        return event;
    }
};

Ledger::Ledger(const std::string& path, const std::string& deployment_identity)
    : impl_(std::make_unique<Impl>()) {
    validate_bytes(deployment_identity, "deployment identity");
    if (path.empty() || path == ":memory:" || path.find('\0') != std::string::npos)
        throw std::runtime_error("native ledger requires an on-disk path");
    const std::string canonical = std::filesystem::weakly_canonical(path).string();
    impl_->lock_fd = open((canonical + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->lock_fd < 0 || flock(impl_->lock_fd, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("native ledger is locked or its lock file cannot be opened");
    impl_->database_fd = open(canonical.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st {};
    if (impl_->database_fd < 0 || fstat(impl_->database_fd, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1)
        throw std::runtime_error("native ledger requires an exclusively owned regular file");
    if (sqlite3_open_v2(canonical.c_str(), &impl_->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        database_error();
    sqlite3_busy_timeout(impl_->db, 5000);
    exec(impl_->db, "PRAGMA foreign_keys=ON");
    exec(impl_->db, "PRAGMA synchronous=FULL");
    {
        Statement q(impl_->db, "PRAGMA journal_mode=WAL");
        if (!q.row() || q.text(0) != "wal") database_error();
    }
    Transaction tx(impl_->db);
    const auto table_count = scalar(impl_->db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table'");
    if (table_count == 0) {
        exec(impl_->db,
            "CREATE TABLE metadata (singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
            "schema_version INTEGER NOT NULL CHECK(schema_version=1),identity TEXT NOT NULL);"
            "CREATE TABLE inputs (input_index INTEGER PRIMARY KEY CHECK(input_index>=0),"
            "canonical_json TEXT NOT NULL,state_hash TEXT NOT NULL);"
            "CREATE TABLE events (ordinal INTEGER PRIMARY KEY CHECK(ordinal>0),"
            "input_index INTEGER NOT NULL REFERENCES inputs(input_index),"
            "input_position INTEGER NOT NULL CHECK(input_position>=0),"
            "event_id TEXT NOT NULL UNIQUE,payload TEXT NOT NULL,"
            "attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts BETWEEN 0 AND 4294967295),"
            "acknowledged INTEGER NOT NULL DEFAULT 0 CHECK(acknowledged IN (0,1)),"
            "last_error TEXT NOT NULL DEFAULT '',UNIQUE(input_index,input_position));"
            "CREATE INDEX pending_events ON events(ordinal) WHERE acknowledged=0;");
        Statement q(impl_->db, "INSERT INTO metadata VALUES(1,1,?)");
        q.bind(1, deployment_identity);
        q.done();
    } else if (table_count != 3) {
        throw std::runtime_error("native ledger is not an empty or supported ledger database");
    }
    {
        Statement q(impl_->db, "SELECT schema_version,identity FROM metadata WHERE singleton=1");
        if (!q.row() || q.integer(0) != 1 || q.text(1) != deployment_identity)
            throw std::runtime_error("native ledger deployment identity or schema mismatch");
        if (q.row() || scalar(impl_->db, "SELECT COUNT(*) FROM metadata") != 1) database_error();
    }
    // Refuse holes/corruption before any delivery can occur. All source rows
    // and immutable event bytes are also compared by the runner during replay.
    const auto count = scalar(impl_->db, "SELECT COUNT(*) FROM inputs");
    if (count && (scalar(impl_->db, "SELECT MIN(input_index) FROM inputs") != 0 ||
                  scalar(impl_->db, "SELECT MAX(input_index) FROM inputs") != count - 1))
        throw std::runtime_error("native ledger input sequence has gaps");
    const auto events = scalar(impl_->db, "SELECT COUNT(*) FROM events");
    if (events && (scalar(impl_->db, "SELECT MIN(ordinal) FROM events") != 1 ||
                   scalar(impl_->db, "SELECT MAX(ordinal) FROM events") != events))
        throw std::runtime_error("native ledger event sequence has gaps");
    if (scalar(impl_->db,
        "SELECT COUNT(*) FROM (SELECT input_index FROM events GROUP BY input_index "
        "HAVING MIN(input_position)<>0 OR MAX(input_position)+1<>COUNT(*))") != 0)
        throw std::runtime_error("native ledger per-input event ordering has gaps");
    if (scalar(impl_->db,
        "SELECT COUNT(*) FROM events AS current JOIN events AS previous "
        "ON previous.ordinal=current.ordinal-1 WHERE current.input_index<previous.input_index "
        "OR (current.input_index=previous.input_index AND current.input_position<>previous.input_position+1) "
        "OR (current.input_index>previous.input_index AND current.input_position<>0)") != 0)
        throw std::runtime_error("native ledger event order differs from input order");
    if (scalar(impl_->db,
        "SELECT COUNT(*) FROM events WHERE acknowledged=1 AND ordinal>"
        "(SELECT MIN(ordinal) FROM events WHERE acknowledged=0)") != 0)
        throw std::runtime_error("native ledger acknowledged events violate delivery order");
    {
        Statement q(impl_->db, "PRAGMA foreign_key_check");
        if (q.row()) database_error();
    }
    {
        Statement q(impl_->db, "PRAGMA quick_check");
        if (!q.row() || q.text(0) != "ok") database_error();
    }
    tx.commit();
}

Ledger::~Ledger() = default;

std::uint64_t Ledger::input_count() const {
    return scalar(impl_->db, "SELECT COALESCE(MAX(input_index)+1,0) FROM inputs");
}

std::optional<RecordedInput> Ledger::input(std::uint64_t index) const {
    Statement q(impl_->db, "SELECT canonical_json,state_hash FROM inputs WHERE input_index=?");
    q.bind(1, index);
    if (!q.row()) return std::nullopt;
    RecordedInput result;
    result.index = index;
    result.canonical_json = q.text(0);
    result.state_hash = q.text(1);
    Statement e(impl_->db, "SELECT ordinal,event_id,payload,attempts FROM events "
                           "WHERE input_index=? ORDER BY input_position");
    e.bind(1, index);
    while (e.row()) result.events.push_back(read_event(e));
    return result;
}

void Ledger::commit_input(std::uint64_t index, const std::string& canonical_json,
                          std::uint64_t state_hash, const std::vector<Event>& events) {
    validate_bytes(canonical_json, "input");
    for (const auto& e : events) {
        validate_bytes(e.id, "event id");
        validate_bytes(e.payload, "event payload");
    }
    Transaction tx(impl_->db);
    const std::string hash = std::to_string(state_hash);
    if (const auto old = input(index)) {
        bool same = old->canonical_json == canonical_json && old->state_hash == hash &&
                    old->events.size() == events.size();
        for (std::size_t i = 0; same && i < events.size(); ++i)
            same = old->events[i].id == events[i].id && old->events[i].payload == events[i].payload;
        if (!same) throw std::runtime_error("native ledger replay differs from committed input, state or events");
        tx.commit();
        return;
    }
    if (index != input_count()) throw std::runtime_error("native ledger input sequence must be contiguous");
    {
        Statement q(impl_->db, "INSERT INTO inputs(input_index,canonical_json,state_hash) VALUES(?,?,?)");
        q.bind(1, index); q.bind(2, canonical_json); q.bind(3, hash); q.done();
    }
    auto ordinal = scalar(impl_->db, "SELECT COALESCE(MAX(ordinal),0) FROM events");
    for (std::size_t i = 0; i < events.size(); ++i) {
        Statement q(impl_->db, "INSERT INTO events(ordinal,input_index,input_position,event_id,payload) "
                               "VALUES(?,?,?,?,?)");
        q.bind(1, ++ordinal); q.bind(2, index); q.bind(3, static_cast<std::uint64_t>(i));
        q.bind(4, events[i].id); q.bind(5, events[i].payload); q.done();
    }
    tx.commit();
}

std::optional<StoredEvent> Ledger::pending_event() const {
    Statement q(impl_->db, "SELECT ordinal,event_id,payload,attempts FROM events "
                           "WHERE acknowledged=0 ORDER BY ordinal LIMIT 1");
    if (!q.row()) return std::nullopt;
    return read_event(q);
}

std::uint64_t Ledger::pending_count() const {
    return scalar(impl_->db, "SELECT COUNT(*) FROM events WHERE acknowledged=0");
}

void Ledger::begin_delivery(const std::string& event_id) {
    Transaction tx(impl_->db);
    const auto head = impl_->require_head(event_id);
    if (head.attempts == std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("native ledger delivery attempt counter exhausted");
    Statement q(impl_->db, "UPDATE events SET attempts=attempts+1,last_error='delivery_in_progress' WHERE event_id=?");
    q.bind(1, event_id); q.done(); tx.commit();
}

void Ledger::record_delivery_failure(const std::string& event_id, const std::string& error_category) {
    Transaction tx(impl_->db);
    if (impl_->require_head(event_id).attempts == 0)
        throw std::runtime_error("native ledger delivery has not begun");
    Statement q(impl_->db, "UPDATE events SET last_error=? WHERE event_id=?");
    q.bind(1, safe_category(error_category)); q.bind(2, event_id); q.done(); tx.commit();
}

void Ledger::acknowledge(const std::string& event_id) {
    Transaction tx(impl_->db);
    if (impl_->require_head(event_id).attempts == 0)
        throw std::runtime_error("native ledger delivery has not begun");
    Statement q(impl_->db, "UPDATE events SET acknowledged=1,last_error='' WHERE event_id=?");
    q.bind(1, event_id); q.done(); tx.commit();
}

} // namespace pineforge::live
