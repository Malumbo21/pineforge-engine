#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace pineforge {

// Concrete lower bounds resolved when an exit binds an exposure cycle. Other
// path/price/lifetime constraints remain independent of this lower bound.
struct ExitLegActivationBounds {
    int64_t position_cycle;
    int64_t stop_first_bar;
    int64_t limit_first_bar;
};

class ExitLegActivation {
public:
    void bind(ExitLegActivationBounds bounds) {
        if (bounds.position_cycle <= 0 || bounds.stop_first_bar < 0 || bounds.limit_first_bar < 0)
            throw std::invalid_argument("invalid exit-leg activation bounds");
        bounds_ = bounds;
    }
    void unbind() { bounds_.reset(); }
    const std::optional<ExitLegActivationBounds>& bounds() const { return bounds_; }
    // An unbound value imposes no lower-bound constraint. Production EXIT
    // producers bind on live exposure; exposure creation binds retained exits.
    bool stop_ready(int64_t cycle, int64_t bar) const {
        return !bounds_ || (bounds_->position_cycle == cycle && bar >= bounds_->stop_first_bar);
    }
    bool limit_ready(int64_t cycle, int64_t bar) const {
        return !bounds_ || (bounds_->position_cycle == cycle && bar >= bounds_->limit_first_bar);
    }
private:
    std::optional<ExitLegActivationBounds> bounds_;
};

} // namespace pineforge
