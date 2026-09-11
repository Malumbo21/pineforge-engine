#pragma once

#include "order_action.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace pineforge::execution {

// Whole-book reduction is explicit: it never relies on a rounded aggregate
// quantity being exactly equal to the sum of individual physical lots.
struct Flatten {};
using Action = std::variant<Flatten, order_action::Reduce, order_action::Transact>;

// The caller has already matched/admitted the action and resolved its price.
// Settlement does not apply another lot step, price grid, slippage or entry cap.
struct Fill {
    double price;
    std::string id;
    std::string comment;
    uint64_t incarnation = 0;
    // An observed execution charge in account currency, including rebates.
    // Otherwise the engine's configured schedule quotes the current fill.
    std::optional<double> commission_account = std::nullopt;
};

enum class Status {
    Applied, NoEffect, InvalidPrice, InvalidQuantity, InvalidBook,
    UnrepresentableQuantity, InvalidAccounting
};

struct Result {
    Status status = Status::NoEffect;
    // Aggregate projections of the physical effects; the lot/trade roster is
    // authoritative. Opening units are signed; closing units are nonnegative.
    double closed_units = 0.0;
    double opened_units = 0.0;
};

} // namespace pineforge::execution
