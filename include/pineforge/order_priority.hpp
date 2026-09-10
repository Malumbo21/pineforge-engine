#pragma once
#include <array>
#include <cstdint>

namespace pineforge::broker {

// Immutable assignments bound to exact pending objects, not reusable user IDs.
// A decision changes only the sequence tie-break, never phase or eligibility.
struct OrderPriorityAssignment {
    uint64_t incarnation;
    int64_t sequence;
};
struct OrderPriorityDecision {
    std::array<OrderPriorityAssignment, 2> assignments;
    int64_t sequence(uint64_t incarnation, int64_t fallback) const {
        for (const auto& assignment : assignments) {
            if (assignment.incarnation == incarnation) return assignment.sequence;
        }
        return fallback;
    }
};

} // namespace pineforge::broker
