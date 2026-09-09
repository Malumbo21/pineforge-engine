// Literal value-state tests only. No engine, strategy, market data or grading.
#include <pineforge/broker_events.hpp>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

using pineforge::broker::OpeningContinuation;
using pineforge::broker::OpeningDecision;
using pineforge::broker::OpeningObligations;
using pineforge::broker::OpeningOwner;
using pineforge::broker::OpeningReceipt;

namespace {
int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (false)

static_assert(std::is_same_v<
    decltype(std::declval<const OpeningObligations&>().peek()),
    const std::optional<OpeningReceipt>&>,
    "inspection must not expose a mutable pending receipt");

bool same_owner(const OpeningOwner& a, const OpeningOwner& b) {
    return a.positionCycle == b.positionCycle
        && a.producerFill == b.producerFill
        && a.orderIncarnation == b.orderIncarnation
        && a.barIndex == b.barIndex
        && a.timestamp == b.timestamp;
}

void absent(const OpeningObligations& obligations) {
    CHECK(!obligations.pending());
    CHECK(!obligations.actionable());
    CHECK(!obligations.requires_adverse_pass());
    CHECK(!obligations.peek().has_value());
}

void check_receipt(const OpeningReceipt& receipt, const OpeningOwner& owner,
                   OpeningDecision decision, double raw, bool adverse) {
    CHECK(same_owner(receipt.owner(), owner));
    CHECK(receipt.decision() == decision);
    CHECK(std::isnan(raw) ? std::isnan(receipt.raw_fill_base())
                          : receipt.raw_fill_base() == raw);
    CHECK(receipt.requires_adverse_pass() == adverse);
}

void test_absent_and_check_take() {
    OpeningObligations obligations;
    absent(obligations);
    CHECK(!obligations.take(11).has_value());
    absent(obligations);

    const OpeningOwner owner{11, 7, 3, 42, 123456};
    obligations.replace(OpeningReceipt::check(owner, 19.25,
                                             OpeningContinuation::None));
    CHECK(obligations.pending());
    CHECK(obligations.actionable());
    CHECK(!obligations.requires_adverse_pass());
    CHECK(obligations.raw_fill_base() == 19.25);
    const auto taken = obligations.take(11);
    CHECK(taken.has_value());
    absent(obligations);  // consumed before the caller can inspect the result
    if (taken) check_receipt(*taken, owner, OpeningDecision::Check, 19.25, false);
    CHECK(!obligations.take(11).has_value());
}

void test_exempt_is_pending_but_never_promoted() {
    const OpeningOwner owner{12, 8, 4, 43, 123457};
    OpeningObligations obligations;
    obligations.replace(OpeningReceipt::exempt(owner, 20.5));
    for (int read = 0; read < 3; ++read) {
        CHECK(obligations.pending());
        CHECK(!obligations.actionable());
        CHECK(!obligations.requires_adverse_pass());
        CHECK(obligations.raw_fill_base() == 20.5);
        CHECK(obligations.peek().has_value());
        if (obligations.peek())
            check_receipt(*obligations.peek(), owner, OpeningDecision::Exempt, 20.5, false);
    }
    const auto taken = obligations.take(12);
    CHECK(taken.has_value());
    if (taken) check_receipt(*taken, owner, OpeningDecision::Exempt, 20.5, false);
    absent(obligations);
}

void test_latest_receipt_replaces_owner_and_continuation() {
    const OpeningOwner old_owner{13, 1, 10, 50, 200000};
    const OpeningOwner new_owner{13, 2, 11, 50, 200000};
    OpeningObligations obligations;
    obligations.replace(OpeningReceipt::check(old_owner, 30.0,
                           OpeningContinuation::RemainingAdversePath));
    CHECK(obligations.requires_adverse_pass());
    obligations.replace(OpeningReceipt::exempt(new_owner, 31.0));
    CHECK(obligations.pending());
    CHECK(!obligations.actionable());
    CHECK(!obligations.requires_adverse_pass());
    obligations.consume(old_owner);
    CHECK(obligations.pending());  // stale cleanup cannot erase the replacement
    if (obligations.peek())
        check_receipt(*obligations.peek(), new_owner, OpeningDecision::Exempt, 31.0, false);

    obligations.replace(OpeningReceipt::check(new_owner, 32.0,
                           OpeningContinuation::RemainingAdversePath));
    CHECK(obligations.actionable());
    CHECK(obligations.requires_adverse_pass());
    const auto taken = obligations.take(13);
    CHECK(taken.has_value());
    if (taken) check_receipt(*taken, new_owner, OpeningDecision::Check, 32.0, true);
    absent(obligations);
}

void test_noop_and_every_owner_dimension() {
    const OpeningOwner owner{14, 3, 12, 51, 200001};
    const auto receipt = OpeningReceipt::check(owner, 40.0,
                           OpeningContinuation::RemainingAdversePath);
    OpeningObligations obligations;
    obligations.replace(receipt);
    // A rejected/no-op producer performs no replacement. Repeated inspection
    // must preserve the receipt without reconstructing eligibility from state.
    for (int read = 0; read < 3; ++read) {
        CHECK(obligations.actionable());
        CHECK(obligations.requires_adverse_pass());
        if (obligations.peek())
            check_receipt(*obligations.peek(), owner, OpeningDecision::Check, 40.0, true);
    }

    OpeningOwner others[] = {
        {15, 3, 12, 51, 200001}, {14, 4, 12, 51, 200001},
        {14, 3, 13, 51, 200001}, {14, 3, 12, 52, 200001},
        {14, 3, 12, 51, 200002},
    };
    for (const auto& other : others) {
        obligations.consume(other);
        CHECK(obligations.pending());
        if (obligations.peek()) CHECK(same_owner(obligations.peek()->owner(), owner));
    }
    obligations.consume(owner);
    absent(obligations);
    obligations.consume(owner);  // repeated consumption is a no-op
    absent(obligations);
}

void test_stale_cycle_and_explicit_invalidation() {
    const OpeningOwner previous_cycle{20, 6, 8, 60, 300000};
    OpeningObligations obligations;
    obligations.replace(OpeningReceipt::check(previous_cycle, 50.0,
                           OpeningContinuation::RemainingAdversePath));
    CHECK(!obligations.take(21).has_value());
    absent(obligations);  // stale owner is discarded, not left pending forever
    obligations.replace(OpeningReceipt::exempt(previous_cycle, 51.0));
    CHECK(!obligations.take(19).has_value());
    absent(obligations);
    obligations.replace(OpeningReceipt::check(previous_cycle, 52.0,
                                             OpeningContinuation::None));
    obligations.invalidate();
    absent(obligations);
    obligations.invalidate();
    absent(obligations);
}

void test_reused_id_is_a_new_incarnation() {
    // No Pine string belongs to OpeningOwner. Two fills bearing the same
    // external label are distinguished by physical cycle/incarnation identity.
    const OpeningOwner first{25, 40, 100, 70, 400000};
    const OpeningOwner replacement{26, 41, 101, 70, 400000};
    OpeningObligations obligations;
    obligations.replace(OpeningReceipt::check(first, 61.0,
                           OpeningContinuation::RemainingAdversePath));
    obligations.replace(OpeningReceipt::check(replacement, 62.0,
                                             OpeningContinuation::None));
    obligations.consume(first);
    CHECK(obligations.pending());
    CHECK(!obligations.requires_adverse_pass());
    const auto taken = obligations.take(26);
    CHECK(taken.has_value());
    if (taken) check_receipt(*taken, replacement, OpeningDecision::Check, 62.0, false);
    absent(obligations);
}

void test_copy_is_independent() {
    const OpeningOwner owner{30, 50, 110, 80, 500000};
    OpeningObligations original;
    original.replace(OpeningReceipt::check(owner, 70.0,
                       OpeningContinuation::RemainingAdversePath));
    OpeningObligations copy = original;
    const auto copied_receipt = copy.take(30);
    CHECK(copied_receipt.has_value());
    absent(copy);
    CHECK(original.pending());
    CHECK(original.actionable());
    CHECK(original.requires_adverse_pass());
    const auto detached_snapshot = original.peek();
    original.invalidate();
    absent(original);
    CHECK(detached_snapshot.has_value());
    if (detached_snapshot)
        check_receipt(*detached_snapshot, owner, OpeningDecision::Check, 70.0, true);
}

void test_invalid_prices_are_data_and_still_consumed() {
    const OpeningOwner owner{40, 60, 120, 90, 600000};
    const double prices[] = {std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), 0.0, -1.0};
    for (const double price : prices) {
        OpeningObligations obligations;
        obligations.replace(OpeningReceipt::check(owner, price,
                           OpeningContinuation::RemainingAdversePath));
        CHECK(obligations.pending());
        const auto taken = obligations.take(40);
        CHECK(taken.has_value());
        absent(obligations);
        if (taken) check_receipt(*taken, owner, OpeningDecision::Check, price, true);
        // Price validity is the financial consumer's decision; the value
        // container neither normalizes the price nor retries the old event.
        CHECK(!obligations.take(40).has_value());
    }
}
}  // namespace

int main() {
    test_absent_and_check_take();
    test_exempt_is_pending_but_never_promoted();
    test_latest_receipt_replaces_owner_and_continuation();
    test_noop_and_every_owner_dimension();
    test_stale_cycle_and_explicit_invalidation();
    test_reused_id_is_a_new_incarnation();
    test_copy_is_independent();
    test_invalid_prices_are_data_and_still_consumed();
    std::printf("opening obligation: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
