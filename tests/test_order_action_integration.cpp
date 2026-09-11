// Native order-action integration seams. This test does not call Engine::run,
// load a feed, compile Pine, or compare against a reference engine.
#include <pineforge/engine.hpp>
#include <cstdio>
#include <cmath>
#include <limits>
#include <utility>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (0)

template<class Tag, auto Member>
struct Access { friend auto access(Tag) { return Member; } };
struct PartialExitAccess { friend auto access(PartialExitAccess); };
struct SameSideAccess { friend auto access(SameSideAccess); };
struct SameBarTransactionAccess { friend auto access(SameBarTransactionAccess); };
template struct Access<PartialExitAccess, &BacktestEngine::execute_partial_exit_qty>;
template struct Access<SameSideAccess, &BacktestEngine::append_same_side_fill>;
template struct Access<SameBarTransactionAccess, &BacktestEngine::apply_same_bar_market_tx_reversal>;

template<class T> struct MemberArguments;
template<class C, class R, class A, class B, class D>
struct MemberArguments<R (C::*)(A, B, D)> { using third = D; };
using ReductionCause = typename MemberArguments<
    decltype(access(PartialExitAccess{}))>::third;

class Book final : public BacktestEngine {
public:
    Book() {
        initial_capital_ = 10000.0;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 1.0;
        slippage_ = 0;
        syminfo_mintick_ = 0.01;
        current_bar_ = {100.0, 130.0, 90.0, 120.0, 1.0, 60000};
        bar_index_ = 1;
    }

    void on_bar(const Bar&) override {}

    void seed_two_lots() {
        position_side_ = PositionSide::LONG;
        position_cycle_seq_ = 4;
        position_entry_price_ = 106.0;
        position_qty_ = 5.0;
        position_entry_count_ = 2;
        position_open_bar_ = 0;
        pyramid_entries_.clear();
        PyramidEntry a{100.0, 1000, 2.0, "A", 0};
        a.entry_incarnation = 11;
        a.entry_commission_account = 2.0;
        PyramidEntry b{110.0, 2000, 3.0, "B", 0};
        b.entry_incarnation = 12;
        b.entry_commission_account = 3.0;
        pyramid_entries_.push_back(a);
        pyramid_entries_.push_back(b);
        id_unclosed_qty_.clear();
        id_unclosed_qty_["A"] = 2.0;
        id_unclosed_qty_["B"] = 3.0;
        trades_.clear();
        net_profit_sum_ = 0.0;
        gross_profit_sum_ = 0.0;
        gross_loss_sum_ = 0.0;
        win_trades_count_ = loss_trades_count_ = eventrades_count_ = 0;
    }

    void seed_three_units() {
        seed_two_lots();
        position_qty_ = 3.0;
        position_entry_price_ = (100.0 + 220.0) / 3.0;
        pyramid_entries_[0].qty = 1.0;
        pyramid_entries_[1].qty = 2.0;
        pyramid_entries_[0].entry_commission_account = 1.0;
        pyramid_entries_[1].entry_commission_account = 2.0;
        id_unclosed_qty_["A"] = 1.0;
        id_unclosed_qty_["B"] = 2.0;
    }

    void partial(double raw_price, double qty) {
        (this->*access(PartialExitAccess{}))(raw_price, qty,
            static_cast<ReductionCause>(0));
    }

    void append(PyramidEntry lot) {
        (this->*access(SameSideAccess{}))(std::move(lot));
    }

    void enable_stream_actions() { stream_observe_actions_ = true; }
    void set_slippage(int ticks) { slippage_ = ticks; }
    void make_short() { position_side_ = PositionSide::SHORT; }

    void transact(PendingOrder& order, double raw_price) {
        double trail = std::numeric_limits<double>::quiet_NaN();
        (this->*access(SameBarTransactionAccess{}))(order, raw_price,
            current_bar_, trail);
    }

    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
    const std::vector<Trade>& trades() const { return trades_; }
    double signed_position() const { return signed_position_size(); }
    PositionSide side() const { return position_side_; }
    int64_t cycle() const { return position_cycle_seq_; }
    int stream_actions() const { return stream_order_actions_len(); }
};

void reduce_preserves_fifo_identity_and_scales_survivor() {
    Book book;
    book.seed_two_lots();
    book.partial(120.0, 3.0);

    CHECK(book.trades().size() == 2);
    CHECK(book.trades()[0].entry_id == "A");
    CHECK(book.trades()[0].qty == 2.0);
    CHECK(book.trades()[1].entry_id == "B");
    CHECK(book.trades()[1].qty == 1.0);
    CHECK(book.lots().size() == 1);
    CHECK(book.lots()[0].entry_id == "B");
    CHECK(book.lots()[0].entry_incarnation == 12);
    CHECK(book.lots()[0].qty == 2.0);
    CHECK(book.lots()[0].entry_commission_account == 2.0);
    CHECK(book.signed_position() == 2.0);
    CHECK(book.cycle() == 4);

    Book zero;
    zero.seed_two_lots();
    zero.partial(120.0, 0.0);
    CHECK(zero.trades().empty() && zero.signed_position() == 5.0);
    zero.partial(120.0, -1.0);
    CHECK(zero.trades().empty() && zero.signed_position() == 5.0);
    zero.partial(120.0, std::numeric_limits<double>::quiet_NaN());
    CHECK(zero.trades().empty() && zero.signed_position() == 5.0);

    Book oversized;
    oversized.seed_two_lots();
    oversized.partial(120.0, 99.0);
    CHECK(oversized.signed_position() == 0.0);
    CHECK(oversized.lots().empty());
    CHECK(oversized.trades().size() == 2);
}

void reduce_handles_short_side_and_slippage_once() {
    Book book;
    book.seed_two_lots();
    book.make_short();
    book.set_slippage(2);
    book.partial(120.0, 2.0);
    CHECK(book.signed_position() == -3.0);
    CHECK(book.trades().size() == 1);
    CHECK(!book.trades()[0].is_long);
    CHECK(book.trades()[0].exit_price == 121.0);

    Book long_side;
    long_side.seed_two_lots();
    long_side.set_slippage(2);
    long_side.partial(120.0, 2.0);
    CHECK(long_side.trades().size() == 1);
    CHECK(long_side.trades()[0].exit_price == 119.0);
}

void append_preserves_lot_metadata_and_stream_action() {
    Book book;
    book.seed_two_lots();
    book.enable_stream_actions();
    PyramidEntry lot{120.0, 3000, 1.0, "C", 1};
    lot.entry_comment = "native add";
    lot.entry_incarnation = 13;
    lot.market_pyramid_add = false;
    book.append(lot);

    CHECK(book.lots().size() == 3);
    CHECK(book.lots().back().entry_id == "C");
    CHECK(book.lots().back().entry_incarnation == 13);
    CHECK(!book.lots().back().market_pyramid_add);
    CHECK(book.lots().back().entry_commission_account == 1.0);
    CHECK(book.signed_position() == 6.0);
    CHECK(book.stream_actions() == 1);
    CHECK(book.stream_order_action_at(0).is_entry);
    CHECK(book.stream_order_action_at(0).quantity == 1.0);
    CHECK(book.stream_order_action_at(0).entry_incarnation == 13);
}

PendingOrder transaction_order(bool is_long, double own, double total) {
    PendingOrder order{};
    order.id = is_long ? "BUY" : "SELL";
    order.type = OrderType::MARKET;
    order.is_long = is_long;
    order.incarnation = 44;
    order.pine_frozen_market_instruction =
        PineFrozenMarketInstruction::transaction(own, total);
    return order;
}

void transact_closes_fifo_and_crosses_flat() {
    Book partial;
    partial.seed_three_units();
    auto sell_two = transaction_order(false, 2.0, 2.0);
    partial.transact(sell_two, 120.0);
    CHECK(partial.signed_position() == 1.0);
    CHECK(partial.trades().size() == 1);
    CHECK(partial.trades()[0].qty == 1.0);
    CHECK(partial.trades()[0].entry_id == "A");

    Book stream;
    stream.seed_three_units();
    stream.enable_stream_actions();
    auto stream_sell = transaction_order(false, 2.0, 2.0);
    stream.transact(stream_sell, 120.0);
    CHECK(stream.stream_actions() == 1);
    CHECK(!stream.stream_order_action_at(0).is_entry);
    CHECK(stream.stream_order_action_at(0).quantity == 1.0);

    Book exact;
    exact.seed_three_units();
    auto sell_three = transaction_order(false, 3.0, 3.0);
    exact.transact(sell_three, 120.0);
    CHECK(exact.signed_position() == 0.0);
    CHECK(exact.trades().size() == 2);
    CHECK(exact.trades()[0].qty == 2.0 && exact.trades()[1].qty == 1.0);

    Book crossing;
    crossing.seed_three_units();
    auto sell_three_again = transaction_order(false, 3.0, 3.0);
    crossing.transact(sell_three_again, 120.0);
    CHECK(crossing.signed_position() == 0.0);
    CHECK(crossing.trades().size() == 2);
    // Equal transaction and held quantities produce a flat position and no
    // phantom opening lot; the signed planner's open remainder is zero.
    CHECK(crossing.lots().empty());
}

void transact_crosses_with_open_remainder() {
    Book book;
    book.seed_three_units();
    auto sell_five = transaction_order(false, 5.0, 5.0);
    book.transact(sell_five, 120.0);
    CHECK(book.signed_position() == -2.0);
    CHECK(book.lots().size() == 1);
    CHECK(book.lots().back().entry_id == "SELL");
    CHECK(book.lots().back().qty == 2.0);
    CHECK(book.lots().back().entry_incarnation == 44);
    CHECK(book.trades().size() == 2);
}
}

int main() {
    reduce_preserves_fifo_identity_and_scales_survivor();
    append_preserves_lot_metadata_and_stream_action();
    transact_closes_fifo_and_crosses_flat();
    transact_crosses_with_open_remainder();
    std::printf("order action integration: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
