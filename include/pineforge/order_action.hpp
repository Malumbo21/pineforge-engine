#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace pineforge::order_action {

struct Reduce { double units; };
struct Transact { double signed_units; };

// A pure quantity transition. The plan has no identity, queue lifetime,
// accounting, policy or validity promise; callers apply it synchronously to
// the authoritative position after their own preconditions pass.
class Plan {
public:
    double before() const noexcept { return before_; }
    double after() const noexcept { return after_; }
    double close_units() const noexcept { return close_units_; }
    double open_units() const noexcept { return open_units_; }
    bool no_effect() const noexcept { return before_ == after_; }

private:
    friend std::optional<Plan> plan(double, Reduce) noexcept;
    friend std::optional<Plan> plan(double, Transact) noexcept;
    Plan(double before, double after, double close_units, double open_units) noexcept
        : before_(before), after_(after), close_units_(close_units),
          open_units_(open_units) {}
    const double before_;
    const double after_;
    const double close_units_;
    const double open_units_;
};

inline bool finite(double value) noexcept { return std::isfinite(value); }

inline std::optional<Plan> plan(double signed_position, Reduce request) noexcept {
    if (!finite(signed_position) || !finite(request.units) || request.units < 0.0)
        return std::nullopt;
    const double exposure = std::abs(signed_position);
    const double close = std::min(request.units, exposure);
    const double signed_close = signed_position > 0.0 ? -close
                              : signed_position < 0.0 ? close : 0.0;
    // The transition is deliberately one binary64 operation. A nonzero
    // requested reduction that cannot change the representable position is
    // refused instead of silently claiming execution.
    const double after = signed_position + signed_close;
    if (!finite(after) || !finite(close) || !finite(signed_close))
        return std::nullopt;
    if (request.units > 0.0 && signed_position != 0.0 && after == signed_position)
        return std::nullopt;
    return Plan(signed_position, after, close, 0.0);
}

inline std::optional<Plan> plan(double signed_position, Transact request) noexcept {
    if (!finite(signed_position) || !finite(request.signed_units))
        return std::nullopt;
    // Compute the resulting position once, before decomposition. This is the
    // authoritative native arithmetic operation; close/open are its audit
    // decomposition and never substitute a second position calculation.
    const double after = signed_position + request.signed_units;
    if (!finite(after)) return std::nullopt;
    if (request.signed_units != 0.0 && after == signed_position)
        return std::nullopt; // nonzero request was absorbed by binary64

    double close = 0.0;
    double open = request.signed_units;
    if (signed_position != 0.0 && request.signed_units != 0.0
        && ((signed_position > 0.0) != (request.signed_units > 0.0))) {
        close = std::min(std::abs(signed_position), std::abs(request.signed_units));
        const double remainder = std::abs(request.signed_units) - close;
        open = request.signed_units > 0.0 ? remainder : -remainder;
    }
    if (!finite(close) || close < 0.0 || !finite(open)) return std::nullopt;
    return Plan(signed_position, after, close, open);
}

} // namespace pineforge::order_action
