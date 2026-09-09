#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace pineforge::broker {

// Stable causal identity, independent of a reusable Pine entry ID, FIFO trade
// rows and observer delivery. Zero IDs are reserved for native test fixtures;
// the production fill dispatcher supplies the committed cycle/fill/order IDs.
struct OpeningOwner {
    int64_t positionCycle;
    uint64_t producerFill;
    uint64_t orderIncarnation;
    int barIndex;
    int64_t timestamp;

};

inline bool operator==(const OpeningOwner& owner, const OpeningOwner& other) {
    return owner.positionCycle == other.positionCycle
        && owner.producerFill == other.producerFill
        && owner.orderIncarnation == other.orderIncarnation
        && owner.barIndex == other.barIndex && owner.timestamp == other.timestamp;
}

enum class OpeningDecision { Check, Exempt };
enum class OpeningContinuation { None, RemainingAdversePath };

// The decision is made when a real opening fill commits. An exempt event is
// still present until its checkpoint; it cannot carry an adverse continuation.
// Taking a receipt never promotes an exemption into a check.
class OpeningReceipt {
    struct Check { OpeningContinuation continuation; };
    struct Exempt {};
    using Decision = std::variant<Check, Exempt>;

public:
    static OpeningReceipt check(
            OpeningOwner owner, double raw_fill_base,
            OpeningContinuation continuation = OpeningContinuation::None) {
        return OpeningReceipt(owner, raw_fill_base, Check{continuation});
    }
    static OpeningReceipt exempt(OpeningOwner owner, double raw_fill_base) {
        return OpeningReceipt(owner, raw_fill_base, Exempt{});
    }

    const OpeningOwner& owner() const { return owner_; }
    double raw_fill_base() const { return raw_fill_base_; }
    OpeningDecision decision() const {
        return std::holds_alternative<Check>(decision_)
            ? OpeningDecision::Check : OpeningDecision::Exempt;
    }
    bool requires_adverse_pass() const {
        const auto* check = std::get_if<Check>(&decision_);
        return check
            && check->continuation == OpeningContinuation::RemainingAdversePath;
    }

private:
    OpeningReceipt(OpeningOwner owner, double raw_fill_base, Decision decision)
        : owner_(owner), raw_fill_base_(raw_fill_base), decision_(decision) {}

    OpeningOwner owner_;
    double raw_fill_base_;
    Decision decision_;
};

// One coalescing checkpoint for the live position. Successful qualifying fills
// replace it; rejected/no-op attempts have no transition. Full close, reversal
// and accepted incompatible fills invalidate it. This is not a queue of all
// past fills: the checkpoint evaluates the current aggregate position using
// the latest eligible producer's raw match price.
class OpeningObligations {
public:
    bool pending() const { return pending_.has_value(); }
    bool actionable() const {
        return pending_ && pending_->decision() == OpeningDecision::Check;
    }
    bool requires_adverse_pass() const {
        return pending_ && pending_->requires_adverse_pass();
    }
    double raw_fill_base() const {
        return pending_ ? pending_->raw_fill_base()
            : std::numeric_limits<double>::quiet_NaN();
    }
    const std::optional<OpeningReceipt>& peek() const { return pending_; }

    void replace(OpeningReceipt receipt) { pending_ = std::move(receipt); }
    void invalidate() { pending_.reset(); }

    // Consume before the financial consumer can return, recurse or book a
    // close. A receipt from a different position cycle cannot reach it.
    std::optional<OpeningReceipt> take(int64_t live_position_cycle) {
        auto receipt = std::exchange(pending_, std::nullopt);
        if (receipt && receipt->owner().positionCycle != live_position_cycle)
            return std::nullopt;
        return receipt;
    }

    // The pre-priced-exit checkpoint commits consumption only after it books
    // a slice. It cannot erase an event that a later producer has replaced.
    bool consume(const OpeningOwner& owner) {
        if (!pending_ || !(pending_->owner() == owner)) return false;
        pending_.reset();
        return true;
    }

private:
    std::optional<OpeningReceipt> pending_;
};

} // namespace pineforge::broker
