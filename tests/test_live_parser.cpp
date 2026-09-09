// SPDX-License-Identifier: Apache-2.0
// Build this file normally for the test executable; also build it as two MODULE
// libraries with PF_LIVE_PARSER_TEST_PLUGIN, one additionally defining
// PF_LIVE_PARSER_TEST_ABI=2. argv: demo_plugin fixture_plugin wrong_abi_plugin.
#include <pineforge/live_parser.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef PF_LIVE_PARSER_TEST_PLUGIN

#ifndef PF_LIVE_PARSER_TEST_ABI
#define PF_LIVE_PARSER_TEST_ABI PF_LIVE_PARSER_ABI_VERSION
#endif

extern "C" PF_LIVE_PARSER_API uint32_t pf_live_parser_abi_version(void) {
    return PF_LIVE_PARSER_TEST_ABI;
}

extern "C" PF_LIVE_PARSER_API int pf_live_parse_message(
    const char* message, size_t message_size, const char* config, size_t config_size,
    pf_live_parser_emit_v1_fn emit, void* user) {
    const std::string_view text(message, message_size);
    if (text.empty() || text == "heartbeat") return 0;
    if (message_size == PF_LIVE_PARSER_MAX_MESSAGE_BYTES) return 0;
    if (text == "throw") throw std::runtime_error("provider-secret-not-for-error-output");
    pf_live_parser_event_v1_t tick{};
    tick.kind = PF_LIVE_PARSER_TICK;
    tick.timestamp = 60000;
    tick.sequence = 18446744073709551615ULL;
    tick.price = 100;
    tick.quantity = 1;
    pf_live_parser_event_v1_t bar{};
    bar.kind = PF_LIVE_PARSER_BAR;
    bar.timestamp = 60000;
    bar.open = 100;
    bar.high = 102;
    bar.low = 99;
    bar.close = 101;
    bar.volume = 3;
    pf_live_parser_event_v1_t time{};
    time.kind = PF_LIVE_PARSER_TIME;
    time.timestamp = 120000;
    if (text == "batch") {
        for (const auto* event : {&tick, &bar, &time}) if (emit(event, user)) return -1;
        return 0;
    }
    if (text == "two-trades") {
        tick.timestamp=180001;tick.sequence=1;
        if(emit(&tick,user)) return -1;
        tick.timestamp=180002;tick.sequence=2;tick.price=101;
        return emit(&tick,user);
    }
    if (text == "config") {
        if (std::string_view(config, config_size) != "{\"scale\":2}") return -1;
        tick.price = 200;
        return emit(&tick, user);
    }
    if (text == "partial-fail") { if (emit(&tick, user)) return -1; return -1; }
    if (text == "partial-invalid") {
        if (emit(&tick, user)) return -1;
        bar.low = 200;
        return emit(&bar, user);
    }
    if (text == "positive-return") { if (emit(&tick, user)) return -1; return 1; }
    if (text == "null") return emit(nullptr, user);
    if (text == "limit" || text == "overflow") {
        const size_t count = PF_LIVE_PARSER_MAX_EVENTS + (text == "overflow" ? 1 : 0);
        for (size_t i = 0; i < count; ++i) {
            tick.sequence = i + 1;
            // Deliberately broken plugin: ignore callback failure. The host
            // must retain its refusal even if the plugin claims success.
            (void)emit(&tick, user);
        }
        return 0;
    }
    if (text == "bad-kind") tick.kind = 999;
    else if (text == "reserved") tick.reserved = 1;
    else if (text == "negative-time") tick.timestamp = -1;
    else if (text == "zero-sequence") tick.sequence = 0;
    else if (text == "zero-price") tick.price = 0;
    else if (text == "zero-quantity") tick.quantity = 0;
    else if (text == "nan-price") tick.price = std::numeric_limits<double>::quiet_NaN();
    else if (text == "inf-quantity") tick.quantity = std::numeric_limits<double>::infinity();
    else if (text == "ignored-fields") {
        tick.open = std::numeric_limits<double>::quiet_NaN();
        tick.volume = 999;
    } else if (text == "bar-negative-zero") { bar.volume = -0.0; return emit(&bar, user); }
    else if (text == "bar-zero-low") { bar.low = 0; return emit(&bar, user); }
    else if (text == "bar-inverted") { bar.high = 90; return emit(&bar, user); }
    else if (text == "bar-infinite") { bar.volume = std::numeric_limits<double>::infinity(); return emit(&bar, user); }
    else if (text == "bar-negative-volume") { bar.volume = -1; return emit(&bar, user); }
    else if (text == "bar-unaligned") { bar.timestamp = 1; return emit(&bar, user); }
    else if (text == "bar-overflow") {
        bar.timestamp = std::numeric_limits<int64_t>::max() / 60000 * 60000;
        return emit(&bar, user);
    } else return -1;
    (void)emit(&tick, user); // Verify host refuses even non-propagated failure.
    return 0;
}

#else

#include "parser.hpp"
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <functional>
#include <type_traits>
#include <utility>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); ++failures; \
} } while (0)

bool refuses(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error&) { return true; }
    return false;
}

bool same(const pineforge::live::ParsedEvent& a, const pineforge::live::ParsedEvent& b) {
    return a.kind == b.kind && a.reserved == b.reserved && a.timestamp == b.timestamp
        && a.sequence == b.sequence && a.price == b.price && a.quantity == b.quantity
        && a.open == b.open && a.high == b.high && a.low == b.low
        && a.close == b.close && a.volume == b.volume;
}

int reject_event(const pf_live_parser_event_v1_t*, void* count) {
    ++*static_cast<int*>(count);
    return -1;
}
}

int main(int argc, char** argv) {
    using pineforge::live::Parser;
    static_assert(std::is_standard_layout<pf_live_parser_event_v1_t>::value, "C ABI layout");
    static_assert(std::is_trivial<pf_live_parser_event_v1_t>::value, "C ABI POD");
    if (argc != 4) {
        std::fprintf(stderr, "usage: test_live_parser demo_plugin fixture_plugin wrong_abi_plugin\n");
        return 2;
    }
    try {
        CHECK(refuses([&] { Parser missing(std::string(argv[1]) + ".missing"); }));
        CHECK(refuses([&] { Parser wrong(argv[3]); }));
        CHECK(refuses([&] { Parser bad("", "{}"); }));
        for (const auto* config : {"", "[]", "null", "{", "{\"secret\":", "{} trailing"})
            CHECK(refuses([&] { Parser bad(argv[1], config); }));
        CHECK(refuses([&] { Parser bad(argv[1], std::string(PF_LIVE_PARSER_MAX_MESSAGE_BYTES + 1, ' ')); }));

        Parser demo(argv[1]);
        const std::string good = "trade,18446744073709551615,60000,100.25,0.5";
        const auto parsed = demo.parse(good);
        CHECK(parsed.size() == 1);
        if (parsed.size() == 1) {
            CHECK(parsed[0].kind == PF_LIVE_PARSER_TICK);
            CHECK(parsed[0].timestamp == 60000);
            CHECK(parsed[0].sequence == std::numeric_limits<uint64_t>::max());
            CHECK(parsed[0].price == 100.25 && parsed[0].quantity == 0.5);
            CHECK(parsed[0].open == 0 && parsed[0].volume == 0 && parsed[0].reserved == 0);
            const auto repeated = demo.parse(good);
            CHECK(repeated.size() == 1 && same(parsed[0], repeated[0]));
        }
        CHECK(demo.parse("heartbeat").empty());
        for (const auto* bad : {"", "trade", "trade,1,60000,100", "trade,1,60000,100,1,extra",
             "trade,0,60000,100,1", "trade,1,-1,100,1", "trade,1,60000,0,1",
             "trade,1,60000,100,-1", "trade,1,60000,nan,1", "trade,1,60000,100,inf",
             "trade,18446744073709551616,60000,100,1", "trade,1,9223372036854775808,100,1",
             "trade,1,60000,1e999,1", "trade,1,60000,100,1x", "trade,1,60000,100, 1"})
            CHECK(refuses([&] { (void)demo.parse(bad); }));
        CHECK(refuses([&] { (void)demo.parse(good + std::string(1, '\0')); }));
        CHECK(refuses([&] { (void)demo.parse(std::string(PF_LIVE_PARSER_MAX_MESSAGE_BYTES + 1, 'x')); }));

        // Length-delimited input must not read unrelated trailing bytes.
        const auto buffer = good + ",unrelated";
        CHECK(demo.parse(std::string_view(buffer.data(), good.size())).size() == 1);

        Parser fixture(argv[2]);
        const auto batch = fixture.parse("batch");
        CHECK(batch.size() == 3);
        if (batch.size() == 3) {
            CHECK(batch[0].kind == PF_LIVE_PARSER_TICK);
            CHECK(batch[1].kind == PF_LIVE_PARSER_BAR && batch[1].high == 102 && batch[1].volume == 3);
            CHECK(batch[2].kind == PF_LIVE_PARSER_TIME && batch[2].timestamp == 120000);
        }
        CHECK(fixture.parse(std::string_view{}).empty());
        CHECK(fixture.parse(std::string(PF_LIVE_PARSER_MAX_MESSAGE_BYTES, 'x')).empty());
        CHECK(fixture.parse("limit").size() == PF_LIVE_PARSER_MAX_EVENTS);
        for (const auto* bad : {"partial-fail", "partial-invalid", "positive-return", "null", "overflow",
             "bad-kind", "reserved", "negative-time", "zero-sequence", "zero-price", "zero-quantity",
             "nan-price", "inf-quantity", "bar-zero-low", "bar-inverted", "bar-infinite",
             "bar-negative-volume", "bar-unaligned", "bar-overflow", "config"}) {
            CHECK(refuses([&] { (void)fixture.parse(bad); }));
            // Rejected staged output cannot leak into a later call.
            const auto recovered = fixture.parse("batch");
            CHECK(recovered.size() == batch.size());
            for (size_t i = 0; i < recovered.size() && i < batch.size(); ++i)
                CHECK(same(recovered[i], batch[i]));
        }
        try { (void)fixture.parse("throw"); CHECK(false); }
        catch (const std::runtime_error& error) {
            CHECK(std::string(error.what()) == "parser plugin threw an exception");
        }
        const auto normalized = fixture.parse("ignored-fields");
        CHECK(normalized.size() == 1 && normalized[0].open == 0 && normalized[0].volume == 0);
        const auto zero = fixture.parse("bar-negative-zero");
        CHECK(zero.size() == 1 && zero[0].volume == 0 && !std::signbit(zero[0].volume));
        Parser configured(argv[2], "{\"scale\":2}");
        const auto changed = configured.parse("config");
        CHECK(changed.size() == 1 && changed[0].price == 200);
        Parser moved(std::move(configured));
        CHECK(refuses([&] { (void)configured.parse("batch"); }));
        CHECK(moved.parse("config").size() == 1);
        moved = std::move(demo);
        CHECK(moved.parse(good).size() == 1);
        CHECK(refuses([&] { (void)demo.parse(good); }));

        // The example must itself obey callback rejection, independently of
        // the host's defensive failure latch.
        void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        CHECK(library != nullptr);
        if (library) {
            const auto parse = reinterpret_cast<decltype(&pf_live_parse_message)>(dlsym(library, "pf_live_parse_message"));
            CHECK(parse != nullptr);
            if (parse) {
                int count = 0;
                CHECK(parse(good.data(), good.size(), "{}", 2, reject_event, &count) == -1);
                CHECK(count == 1);
            }
            dlclose(library);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected parser test failure: %s\n", error.what());
        return 1;
    }
    return failures ? 1 : 0;
}
#endif
