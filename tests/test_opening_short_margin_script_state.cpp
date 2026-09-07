// R23 TradingView controls: a full opening-bar short liquidation is visible
// to the close-time script; a replacement may receive its own explicit bracket.
// Compact command fixtures use synthetic timestamps and fixed exit distances.
#include <pineforge/engine.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }

enum class Mode { DYNAMIC, EXPLICIT_BRACKET, DIFFERENT_ID, EXPLICIT_QTY, FIXED, PARTIAL_CLOSE };
class ScriptView : public BacktestEngine {
public:
    Mode mode;
    double visible_first = qnan, visible_second = qnan;
    double first_equity = qnan;
    std::size_t first_closed = 0;
    ScriptView(Mode value, double capital = 10117.291322) : mode(value) {
        initial_capital_ = capital;
        default_qty_type_ = mode == Mode::FIXED ? QtyType::FIXED : QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = mode == Mode::FIXED ? 0.08733 : 100.0;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 1) {
            visible_first = signed_position_size();
            first_equity = current_equity();
            first_closed = trades_.size();
        }
        if (bar_index_ == 2) visible_second = signed_position_size();
        if (bar_index_ == 0 || (bar_index_ == 1 && mode != Mode::PARTIAL_CLOSE)) {
            const std::string id = mode == Mode::DIFFERENT_ID && bar_index_ == 0 ? "First" : "Short";
            const double qty = mode == Mode::EXPLICIT_QTY ? (bar_index_ == 0 ? 0.08733 : 0.08739) : qnan;
            strategy_entry(id, false, qnan, qnan, qty);
        }
        if (mode == Mode::EXPLICIT_BRACKET) {
            if (bar_index_ == 1) strategy_exit("Short Exit", "Short", 115639.51, 115944.61);
        } else {
            const double average = signed_position_size() == 0.0 ? qnan : position_entry_price_;
            const double distance = bar_index_ <= 1 ? 101.40652319727 : 109.08;
            strategy_exit("Short Exit", "Short", average - 2 * distance, average + distance);
        }
        if (bar_index_ == 1 && mode == Mode::PARTIAL_CLOSE) {
            strategy_close("Short", "half", qnan, 50.0);
        }
        if (bar_index_ == 3) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

const std::vector<Bar> bars = {
    {115842.32, 115842.32, 115842.32, 115842.32, 1, 1000},
    {115842.33, 115852.95, 115621.65, 115761.05, 1, 2000},
    {115761.06, 115812.71, 115603.98, 115688.35, 1, 3000},
    {115688.35, 115950.00, 115688.34, 115905.88, 1, 4000},
    {115905.88, 115916.73, 115800.00, 115854.00, 1, 5000},
};

void test_full_liquidation_and_replacement() {
    for (Mode mode : {Mode::DYNAMIC, Mode::DIFFERENT_ID, Mode::EXPLICIT_QTY}) {
        ScriptView engine(mode);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.visible_first, 0.0));
        CHECK(engine.first_closed == 1);
        CHECK(near(engine.first_equity, 10117.291322 - 0.9274446));
        CHECK(near(engine.visible_second, -0.08711));
        CHECK(engine.rows().size() == 3);
        if (engine.rows().size() != 3) continue;
        CHECK(engine.rows()[0].exit_time == 2000);
        CHECK(engine.rows()[0].exit_id == "__margin_call__");
        CHECK(near(engine.rows()[0].qty, 0.08733));
        CHECK(near(engine.rows()[0].exit_price, 115852.95));
        CHECK(engine.rows()[1].exit_time == 3000);
        CHECK(engine.rows()[1].exit_id == "__margin_call__");
        CHECK(near(engine.rows()[1].qty, 0.00028));
        CHECK(engine.rows()[2].exit_time == 4000);
        CHECK(engine.rows()[2].exit_id == "Short Exit");
        CHECK(near(engine.rows()[2].qty, 0.08711));
        CHECK(near(engine.rows()[2].exit_price, 115870.14));
    }
}

void test_explicit_bracket_survives() {
    ScriptView engine(Mode::EXPLICIT_BRACKET);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(engine.visible_first, 0.0));
    CHECK(engine.rows().size() == 3);
    if (engine.rows().size() != 3) return;
    CHECK(engine.rows()[2].exit_time == 3000);
    CHECK(engine.rows()[2].exit_id == "Short Exit");
    CHECK(near(engine.rows()[2].exit_price, 115639.51));
    CHECK(near(engine.rows()[2].qty, 0.08711));
}

void test_partial_and_funded() {
    ScriptView partial(Mode::DYNAMIC, 10116.7);
    partial.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(partial.visible_first, -0.08729));
    CHECK(partial.first_closed == 1);
    CHECK(partial.rows().size() == 2);
    if (partial.rows().size() == 2) {
        CHECK(partial.rows()[0].exit_id == "__margin_call__");
        CHECK(near(partial.rows()[0].qty, 0.00004));
        CHECK(near(partial.rows()[1].qty, 0.08729));
        CHECK(partial.rows()[1].exit_time == 3000);
    }
    ScriptView funded(Mode::FIXED, 10200.0);
    funded.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(funded.visible_first, -0.08733));
    CHECK(funded.first_closed == 0);
    CHECK(funded.rows().size() == 1);
    if (funded.rows().size() == 1) {
        CHECK(funded.rows()[0].exit_id == "Short Exit");
        CHECK(near(funded.rows()[0].qty, 0.08733));
        CHECK(funded.rows()[0].exit_time == 3000);
    }
}

void test_partial_close_reads_reduced_quantity() {
    ScriptView engine(Mode::PARTIAL_CLOSE, 10116.7);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(engine.visible_first, -0.08729));
    CHECK(engine.rows().size() == 3);
    if (engine.rows().size() != 3) return;
    CHECK(engine.rows()[0].exit_id == "__margin_call__");
    CHECK(near(engine.rows()[0].qty, 0.00004));
    CHECK(engine.rows()[1].exit_comment == "half");
    CHECK(near(engine.rows()[1].qty, 0.04364));
    CHECK(near(engine.rows()[1].exit_price, 115761.06));
    CHECK(engine.rows()[2].exit_id == "Short Exit");
    CHECK(near(engine.rows()[2].qty, 0.04365));
    CHECK(near(engine.rows()[2].exit_price, 115639.51));
}
}
int main() {
    test_full_liquidation_and_replacement();
    test_explicit_bracket_survives();
    test_partial_and_funded();
    test_partial_close_reads_reduced_quantity();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
