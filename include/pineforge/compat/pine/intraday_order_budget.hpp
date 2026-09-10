#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace pineforge::compat::pine {

// Identity supplied by the engine's existing risk clock, not an indicator's
// possibly merged daily bar. This type deliberately does not select a clock.
struct OrderRiskDay { int64_t key; };
inline bool operator==(OrderRiskDay a, OrderRiskDay b) { return a.key == b.key; }
inline bool operator!=(OrderRiskDay a, OrderRiskDay b) { return !(a == b); }

// One counted, committed close can lend its quota slot to one designated
// opposite order. The two operations still emit separate broker fill events,
// FIFO rows and callbacks. A slot is never identified by a reusable Pine ID.
struct CloseQuotaTransfer {
    OrderRiskDay day;
    uint64_t close_fill;
    int source_bar;
    uint64_t inheritor;
};

enum class QuotaAdmission { Blocked, BelowLimit, ReachedLimit };

class IntradayOrderBudget {
public:
    const std::optional<OrderRiskDay>& day() const { return day_; }
    int charged_slots() const { return charged_slots_; }
    bool latched() const { return latched_; }
    const std::optional<CloseQuotaTransfer>& transfer() const { return transfer_; }

    // Renew quota and retire its continuation atomically. A forced close due
    // from the previous day has a separate lifetime and is not stored here.
    void enter_day(OrderRiskDay day) {
        if (day_ && *day_ == day) return;
        day_ = day;
        charged_slots_ = 0;
        latched_ = false;
        expire_transfer();
    }
    void latch() { if (day_) latched_ = true; }
    void expire_transfer() { transfer_.reset(); }
    void decline(uint64_t incarnation) {
        if (transfer_ && transfer_->inheritor == incarnation) expire_transfer();
    }
    bool can_inherit(OrderRiskDay day, int bar, uint64_t incarnation,
                     uint64_t latest_committed_fill) const {
        return transfer_ && day_ && *day_ == day && transfer_->day == day
            && transfer_->source_bar == bar && incarnation != 0
            && transfer_->inheritor == incarnation
            && transfer_->close_fill == latest_committed_fill;
    }

    // The established policy charges a matched attempt BEFORE dispatch. It
    // can charge a no-op; factor A filters only its proven MARKET subset at
    // the caller. Do not silently reinterpret this as a committed-fill count.
    // Both charging methods require the caller's positive configured limit.
    QuotaAdmission admit_matched_attempt(OrderRiskDay day, int limit, int bar,
                                        uint64_t incarnation,
                                        uint64_t latest_committed_fill) {
        enter_day(day);
        const bool inherited = can_inherit(day, bar, incarnation,
                                           latest_committed_fill);
        if (latched_ && !inherited) return QuotaAdmission::Blocked;
        if (inherited) expire_transfer();
        else ++charged_slots_;
        return charged_slots_ >= limit ? QuotaAdmission::ReachedLimit
                                       : QuotaAdmission::BelowLimit;
    }

    // Called only AFTER the direct close commits and receives a broker fill
    // sequence. An uncounted close cannot create a transferable debit.
    void count_committed_close(OrderRiskDay day, int limit, uint64_t close_fill,
                               int source_bar, uint64_t inheritor) {
        enter_day(day);
        expire_transfer();
        if (latched_) return;
        ++charged_slots_;
        if (inheritor != 0) {
            transfer_ = CloseQuotaTransfer{day, close_fill, source_bar, inheritor};
        }
        if (charged_slots_ >= limit) latch();
    }

private:
    std::optional<OrderRiskDay> day_;
    int charged_slots_ = 0;
    bool latched_ = false;
    std::optional<CloseQuotaTransfer> transfer_;
};

} // namespace pineforge::compat::pine
