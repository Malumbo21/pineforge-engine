#pragma once

#include <cstdint>
#include <cmath>
#include <limits>

namespace pineforge {

// A cancellation is a broker-book result with a causal source.  The engine
// owns this state; compatibility layers choose when a source instruction is
// cancelled and bind the resulting receipt to the affected order.
inline namespace order_cancellation_v1 {

enum class CancellationCause : int32_t {
    None = 0,
    Replacement = 1,
    Dependency = 2,
};

enum class CancellationState : int32_t {
    Live = 0,
    Cancelled = 1,
};

enum class CloseClaimRelease : int32_t {
    NotApplicable = 0,
    Pending = 1,
    Released = 2,
};

// One order-owned receipt covers both cancellation sites currently modelled
// by the broker.  The cause and source identity make the result auditable;
// CloseClaimRelease is a state transition, so a close reservation can be
// returned at most once even if the fill loop revisits the order.
class OrderCancellationReceipt {
public:
    CancellationCause cause() const { return cause_; }
    CancellationState state() const { return state_; }
    CloseClaimRelease close_claim_release() const { return close_claim_release_; }
    uint64_t source_incarnation() const { return source_incarnation_; }
    int64_t source_sequence() const { return source_sequence_; }
    uint64_t target_incarnation() const { return target_incarnation_; }
    int64_t target_owner() const { return target_owner_; }
    uint64_t target_revision() const { return target_revision_; }
    double close_claim_consumed() const { return close_claim_consumed_; }
    double close_claim_retired() const { return close_claim_retired_; }

    bool cancelled() const { return state_ == CancellationState::Cancelled; }
    bool has_close_claim() const {
        return close_claim_release_ != CloseClaimRelease::NotApplicable;
    }

    // Returns false when a second or invalid cancellation tries to overwrite
    // the original causal receipt.  First cause wins by construction.
    bool cancel(CancellationCause cause, uint64_t source_incarnation,
                int64_t source_sequence, uint64_t target_incarnation,
                int64_t target_owner = 0, uint64_t target_revision = 0) {
        if (state_ != CancellationState::Live
            || cause == CancellationCause::None
            || source_incarnation == 0 || source_sequence <= 0
            || target_incarnation == 0 || target_owner < 0
            || target_revision == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        cause_ = cause;
        state_ = CancellationState::Cancelled;
        source_incarnation_ = source_incarnation;
        source_sequence_ = source_sequence;
        target_incarnation_ = target_incarnation;
        target_owner_ = target_owner;
        target_revision_ = target_revision;
        return true;
    }

    // Capture the placement-time close claim.  NaN is the existing sentinel
    // for a close that did not debit the id ledger, so it remains a no-op.
    void bind_close_claim(double consumed, double retired) {
        close_claim_consumed_ = consumed;
        close_claim_retired_ = retired;
        close_claim_release_ = std::isfinite(consumed) && consumed > 0.0
            ? CloseClaimRelease::Pending
            : CloseClaimRelease::NotApplicable;
    }

    // Return the captured claim exactly once.  The caller supplies the
    // order-id ledger so the generic receipt has no knowledge of Pine ids.
    bool release_close_claim_once(double& ledger) {
        if (state_ != CancellationState::Cancelled
            || cause_ != CancellationCause::Dependency
            || close_claim_release_ != CloseClaimRelease::Pending) {
            return false;
        }
        ledger += close_claim_consumed_ + close_claim_retired_;
        close_claim_release_ = CloseClaimRelease::Released;
        return true;
    }

private:
    CancellationCause cause_ = CancellationCause::None;
    CancellationState state_ = CancellationState::Live;
    CloseClaimRelease close_claim_release_ = CloseClaimRelease::NotApplicable;
    uint64_t source_incarnation_ = 0;
    int64_t source_sequence_ = 0;
    uint64_t target_incarnation_ = 0;
    int64_t target_owner_ = 0;
    uint64_t target_revision_ = 0;
    double close_claim_consumed_ = std::numeric_limits<double>::quiet_NaN();
    double close_claim_retired_ = 0.0;
};

} // inline namespace order_cancellation_v1
} // namespace pineforge
