#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
#include <limits>

namespace pineforge::compat::pine {

std::optional<broker::OrderPriorityDecision> OrderPriority::select(
        const OrderPriorityContext& ctx,
        const std::vector<PendingOrder>& book) const {
    if (!attached_ || !retained_parent_first_
        || !ctx.broker_flat || !ctx.process_orders_on_close
        || ctx.calc_on_order_fills || ctx.coof_scheduler_active
        || ctx.bar_magnifier_enabled || ctx.stream_warmup_mode
        || !ctx.stream_idle || book.size() != 2) return std::nullopt;

    const PendingOrder* parent = nullptr;
    const PendingOrder* child = nullptr;
    for (const PendingOrder& order : book) {
        if (order.type == OrderType::ENTRY) parent = &order;
        else if (order.type == OrderType::EXIT) child = &order;
    }
    if (!parent || !child) return std::nullopt;

    // Preserve every legacy exclusion. Incarnation adjacency and exact book
    // size are Pine evidence boundaries, not native dependency invariants.
    const uint64_t cancelled_incarnation =
        parent->recreated_after_named_cancelled_entry_incarnation;
    const uint64_t surviving_exit_incarnation =
        parent->named_cancel_surviving_exit_incarnation;
    const bool parent_is_exact_fresh_stop =
        parent->type == OrderType::ENTRY
        && parent->created_position_side == PositionSide::FLAT
        && !parent->created_by_same_id_replacement
        && cancelled_incarnation != 0
        && cancelled_incarnation < parent->incarnation
        && cancelled_incarnation != child->incarnation
        && surviving_exit_incarnation > cancelled_incarnation
        && surviving_exit_incarnation < parent->incarnation
        && parent->created_bar == ctx.bar_index - 1
        && std::isnan(parent->qty)
        && !parent->created_during_coof_recalc
        && !parent->created_after_position_close_in_bar
        && !parent->over_pyramiding_cap_at_placement
        && !parent->stop_limit_activated
        && std::isfinite(parent->stop_price)
        && std::isnan(parent->limit_price)
        && std::isnan(parent->trail_points)
        && std::isnan(parent->trail_price)
        && std::isnan(parent->trail_offset)
        && parent->oca_name.empty()
        && parent->oca_type == 0;
    const double child_qp = std::isnan(child->qty_percent)
        ? 100.0 : child->qty_percent;
    const bool child_is_exact_retained_bracket =
        child->type == OrderType::EXIT
        && !child->from_entry.empty()
        && child->created_by_same_id_replacement
        && child->replaced_exit_order_incarnation
            == surviving_exit_incarnation
        && !child->created_while_in_position
        && child->created_position_side == PositionSide::FLAT
        && child->created_bar == ctx.bar_index - 1
        && !child->created_during_coof_recalc
        && !child->created_after_position_close_in_bar
        && !child->requested_partial
        && std::isnan(child->qty)
        && child_qp >= 100.0 - 1e-9
        && std::isfinite(child->stop_price)
        && std::isfinite(child->limit_price)
        && std::isnan(child->profit_ticks)
        && std::isnan(child->loss_ticks)
        && std::isnan(child->trail_points)
        && std::isnan(child->trail_price)
        && std::isnan(child->trail_offset)
        && child->oca_name.empty()
        && child->oca_type == 0;
    const bool exact_pair = parent_is_exact_fresh_stop
        && child_is_exact_retained_bracket
        && child->from_entry == parent->id
        && child->created_seq < parent->created_seq
        && child->incarnation != 0
        && parent->incarnation != 0
        && parent->incarnation
            < std::numeric_limits<uint64_t>::max()
        && child->incarnation == parent->incarnation + 1;
    if (!exact_pair) return std::nullopt;
    return broker::OrderPriorityDecision{{{
        {parent->incarnation, child->created_seq},
        {child->incarnation, parent->created_seq},
    }}};
}

} // namespace pineforge::compat::pine
