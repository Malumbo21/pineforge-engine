#pragma once
#include <cstdint>
#include <optional>

namespace pineforge {
enum class PositionSide;

// First standalone C++ ABI for the reservation ownership model.
inline namespace reservation_expansion_v1 {

// The containing EXIT incarnation owns this capture. Admission is an immutable
// historical cause; neither cancellation nor a new exposure erases the receipt.
struct ReservationExpansionCapture {
    int64_t position_cycle;
    PositionSide side;
    std::optional<uint64_t> first_later_admission;
};

class ReservationExpansion {
public:
    void capture(uint64_t receiver, int64_t cycle, PositionSide side, double capacity);
    void close_population(uint64_t admitted_incarnation);
    const std::optional<ReservationExpansionCapture>& capture() const { return capture_; }
    bool population_open() const { return capture_ && !capture_->first_later_admission; }
    bool owns_exposure(int64_t cycle, PositionSide side) const;
    bool live_all(int64_t cycle, PositionSide side) const {
        return population_open() && owns_exposure(cycle, side);
    }
    // The caller resolves the exact actionable receiver. This operation knows
    // only exposure facts and the committed primary delta; qty is the sole
    // mutable capacity, and QuantityRequest is never rebased here.
    // Preconditions for standalone callers: finite nonnegative qty, endpoint
    // quantities and epsilon; the resulting capacity must remain finite. The
    // caller supplies real before/after exposure facts. This method preserves
    // the native after_qty > before_qty + epsilon arithmetic without repricing
    // or re-rounding; it does not validate every standalone precondition.
    void grow(double& qty, int64_t before_cycle, PositionSide before_side, double before_qty,
              int64_t after_cycle, PositionSide after_side, double after_qty, double epsilon) const;
private:
    std::optional<ReservationExpansionCapture> capture_;
};

// The containing source incarnation is the source identity. This is the only
// authoritative edge; replacement is allowed only at a new successful capture.
class ReservationGrowthSource {
public:
    void assign_capture(uint64_t source, uint64_t receiver);
    const std::optional<uint64_t>& reservation_owner() const { return reservation_owner_; }
private:
    std::optional<uint64_t> reservation_owner_;
};
} // inline namespace reservation_expansion_v1
} // namespace pineforge
