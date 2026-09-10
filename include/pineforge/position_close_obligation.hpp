#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace pineforge::broker {

// Synchronous simulator intervention, not an external execution observation.
// Taking this request permits an attempt; it does not assert a flat position.
struct PositionCloseRequest {
    uint64_t action_id;
    int64_t position_cycle;
    int after_bar;
    std::string comment;
};

class PositionCloseObligation {
public:
    bool pending() const { return due_.has_value(); }
    const std::optional<PositionCloseRequest>& peek() const { return due_; }
    void schedule(PositionCloseRequest request) { due_ = std::move(request); }
    std::optional<PositionCloseRequest> take_at_open(int bar, int64_t live_cycle) {
        if (!due_ || bar <= due_->after_bar) return std::nullopt;
        auto request = std::exchange(due_, std::nullopt);
        if (request->position_cycle != live_cycle) return std::nullopt;
        return request;
    }
private:
    std::optional<PositionCloseRequest> due_;
};

} // namespace pineforge::broker
