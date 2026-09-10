#include <pineforge/engine.hpp>
#include <pineforge/reservation_expansion.hpp>
#include <cmath>
#include <stdexcept>

namespace pineforge {
inline namespace reservation_expansion_v1 {
void ReservationExpansion::capture(uint64_t receiver, int64_t cycle, PositionSide side, double capacity) {
    if (capture_ || receiver == 0 || cycle <= 0
        || (side != PositionSide::LONG && side != PositionSide::SHORT)
        || !std::isfinite(capacity) || capacity <= 0)
        throw std::invalid_argument("invalid or repeated reservation expansion capture");
    capture_ = ReservationExpansionCapture{cycle, side, std::nullopt};
}
void ReservationExpansion::close_population(uint64_t admitted_incarnation) {
    if (admitted_incarnation == 0)
        throw std::invalid_argument("reservation closure requires an admitted incarnation");
    if (population_open()) capture_->first_later_admission = admitted_incarnation;
}
bool ReservationExpansion::owns_exposure(int64_t cycle, PositionSide side) const {
    return capture_ && capture_->position_cycle == cycle && capture_->side == side;
}
void ReservationExpansion::grow(double& qty,
        int64_t before_cycle, PositionSide before_side, double before_qty,
        int64_t after_cycle, PositionSide after_side, double after_qty, double epsilon) const {
    if (owns_exposure(before_cycle, before_side) && owns_exposure(after_cycle, after_side)
        && std::isfinite(qty) && after_qty > before_qty + epsilon)
        qty += after_qty - before_qty;
}
void ReservationGrowthSource::assign_capture(uint64_t source, uint64_t receiver) {
    if (source == 0 || receiver == 0 || source == receiver)
        throw std::invalid_argument("reservation source requires distinct live incarnations");
    reservation_owner_ = receiver;
}
} // inline namespace reservation_expansion_v1
} // namespace pineforge
