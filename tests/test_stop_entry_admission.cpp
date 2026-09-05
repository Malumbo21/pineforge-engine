/*
 * test_stop_entry_admission.cpp — round 7: TradingView's admission of a
 * strategy.entry(stop=) under margin simulation, pinned by 22 `lab tv`
 * tapes (2026-09-05, ledger note log-20260905t053924z-15615295, rule and
 * call-by-call table scratchpad/r7/pins/flatten-PINS.md, tapes
 * scratchpad/r7/pins/flatten-<slug>/tv_trades.csv, sources <slug>.pine).
 *
 * The pinned rule (margin_long/short > 0, default process_orders_on_close):
 *
 *   1. PLACEMENT on the bar B of the call: accepted iff
 *        lot_floored(qty) * tick_half_up(close(B)) * pv * fx * margin%/100
 *          <= strategy.equity(B) as the script reads it
 *      (post-exit realized equity on a flattening bar; on a reversal only
 *      the new side counts; a position closed on B counts nothing). Not the
 *      raw sub-tick close, not high/low, not the level.
 *   2. A rejected placement is DROPPED, never re-evaluated; a rejected
 *      same-id re-issue also CANCELS the resting order of an earlier
 *      accepted issue; qty is recomputed only by the script's next call.
 *   3. An accepted order rests until touched; later closes are not
 *      re-checked.
 *   4. FILL: the same floored qty * tick(fill price) <= equity at the fill,
 *      where the fill price is the LEVEL on an intrabar touch (not the bar
 *      open — the engine's KI-62 premise was wrong) or the rounded open on
 *      a gap-through; a rejected fill drops the order (no partial / trim).
 *   5. Market re-entries on the flattening bar keep the round-5 market rule.
 *
 * Feed bars are the registry's NYSE:F 15 (mintick 0.01, whole shares) and
 * OANDA:XAUUSD 15 (mintick 0.005, lot 0.01) bars, UTC, `lab bars`. Tape
 * times are UTC+8 in the CSVs; they are quoted here in UTC.
 *
 * Engine (c2032d1) pre-fix: no placement check on stops at all, and the
 * fill-time gate costed the bar OPEN. Every "TV result" column below is
 * what the tape shows; the "pre-fix" notes say what the engine did.
 */

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

constexpr int64_t kMin15 = 15LL * 60LL * 1000LL;

// NYSE:F 15, 2025-08-12 19:45Z .. 2025-08-14 15:15Z (feed 80f404ae85ef).
// Index map: b0 = 08-12 19:45Z; b1..b26 = 08-13 13:30Z..19:45Z;
// b27.. = 08-14 13:30Z, 13:45Z, 14:00Z, 14:15Z, 14:30Z, 14:45Z, 15:00Z, 15:15Z.
enum FBar {
    F0812_1945 = 0,
    F0813_1330 = 1, F0813_1345 = 2, F0813_1400 = 3, F0813_1415 = 4,
    F0813_1430 = 5, F0813_1445 = 6, F0813_1500 = 7, F0813_1945 = 26,
    F0814_1330 = 27, F0814_1345 = 28, F0814_1400 = 29, F0814_1415 = 30,
    F0814_1430 = 31, F0814_1445 = 32, F0814_1500 = 33, F0814_1515 = 34,
};

std::vector<Bar> f_bars() {
    const int64_t t0812 = 1755027900000LL;   // 2025-08-12 19:45Z
    const int64_t t0813 = 1755091800000LL;   // 2025-08-13 13:30Z
    const int64_t t0814 = 1755178200000LL;   // 2025-08-14 13:30Z
    std::vector<Bar> b;
    b.push_back(mk(t0812, 11.23, 11.25, 11.2, 11.24));
    const double d13[][4] = {
        {11.29, 11.29, 11.19, 11.25},      {11.255, 11.325, 11.25, 11.325},
        {11.325, 11.365, 11.32, 11.33},    {11.335, 11.335, 11.26, 11.285},
        {11.285, 11.34, 11.28, 11.335},    {11.33, 11.335, 11.3, 11.325},
        {11.33, 11.36, 11.325, 11.355},    {11.355, 11.415, 11.355, 11.39},
        {11.39, 11.4, 11.375, 11.385},     {11.385, 11.385, 11.345, 11.37},
        {11.375, 11.42, 11.37, 11.415},    {11.415, 11.45, 11.415, 11.425},
        {11.425, 11.45, 11.425, 11.44},    {11.445, 11.45, 11.435, 11.445},
        {11.44, 11.45, 11.41, 11.41},      {11.415, 11.445, 11.415, 11.425},
        {11.425, 11.43, 11.4, 11.415},     {11.415, 11.435, 11.415, 11.425},
        {11.425, 11.45, 11.415, 11.415},   {11.415, 11.44, 11.415, 11.435},
        {11.44, 11.445, 11.42, 11.43},     {11.43, 11.45, 11.43, 11.435},
        {11.435, 11.455, 11.435, 11.455},  {11.455, 11.47, 11.455, 11.465},
        {11.465, 11.485, 11.46, 11.475},   {11.475, 11.48, 11.425, 11.425},
    };
    for (int i = 0; i < 26; ++i) {
        b.push_back(mk(t0813 + i * kMin15, d13[i][0], d13[i][1], d13[i][2],
                       d13[i][3]));
    }
    const double d14[][4] = {
        {11.3, 11.32, 11.215, 11.225},     {11.225, 11.27, 11.22, 11.265},
        {11.265, 11.3, 11.25, 11.275},     {11.27, 11.275, 11.25, 11.265},
        {11.265, 11.3, 11.265, 11.29},     {11.29, 11.315, 11.29, 11.305},
        {11.3, 11.315, 11.295, 11.295},    {11.3, 11.315, 11.29, 11.305},
    };
    for (int i = 0; i < 8; ++i) {
        b.push_back(mk(t0814 + i * kMin15, d14[i][0], d14[i][1], d14[i][2],
                       d14[i][3]));
    }
    return b;
}

// NYSE:F 15, 2025-09-19 13:30Z .. 15:30Z.
enum F0919Bar {
    S1330 = 0, S1345 = 1, S1400 = 2, S1415 = 3, S1430 = 4, S1445 = 5,
    S1500 = 6, S1515 = 7, S1530 = 8,
};

std::vector<Bar> f0919_bars() {
    const int64_t t = 1758288600000LL;       // 2025-09-19 13:30Z
    const double d[][4] = {
        {11.785, 11.8, 11.62, 11.625},     {11.62, 11.69, 11.61, 11.675},
        {11.675, 11.725, 11.67, 11.715},   {11.715, 11.715, 11.67, 11.68},
        {11.68, 11.69, 11.64, 11.645},     {11.64, 11.64, 11.6, 11.62},
        {11.62, 11.63, 11.61, 11.62},      {11.615, 11.625, 11.59, 11.605},
        {11.605, 11.615, 11.59, 11.61},
    };
    std::vector<Bar> b;
    for (int i = 0; i < 9; ++i) {
        b.push_back(mk(t + i * kMin15, d[i][0], d[i][1], d[i][2], d[i][3]));
    }
    return b;
}

// OANDA:XAUUSD 15, 2025-08-18 14:30Z .. 16:45Z (feed 248086b8b82d).
enum XBar {
    X1430 = 0, X1445 = 1, X1500 = 2, X1515 = 3, X1530 = 4, X1545 = 5,
    X1600 = 6, X1615 = 7, X1630 = 8, X1645 = 9,
};

std::vector<Bar> xau_bars() {
    const int64_t t = 1755527400000LL;       // 2025-08-18 14:30Z
    const double d[][4] = {
        {3335.915, 3338.76, 3334.895, 3335.745},
        {3335.725, 3336.13, 3333.005, 3334.375},
        {3334.41, 3335.28, 3332.335, 3334.73},
        {3334.765, 3335.965, 3333.375, 3334.765},
        {3334.77, 3335.71, 3333.175, 3335.145},
        {3335.125, 3336.9, 3334.0, 3335.72},
        {3335.73, 3335.905, 3332.01, 3333.41},
        {3333.415, 3333.445, 3331.995, 3332.47},
        {3332.48, 3334.705, 3331.775, 3332.705},
        {3332.675, 3334.545, 3332.45, 3334.33},
    };
    std::vector<Bar> b;
    for (int i = 0; i < 10; ++i) {
        b.push_back(mk(t + i * kMin15, d[i][0], d[i][1], d[i][2], d[i][3]));
    }
    return b;
}

class Probe : public BacktestEngine {
public:
    Probe(double capital, double margin, double mintick, double lot) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = mintick;
        syminfo_mintick_ = mintick;
        qty_step_ = lot;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin;
        margin_short_ = margin;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(false);
    }
    // The Pine body, called with bar_index_ on every bar.
    std::function<void(Probe&, int)> script;
    void on_bar(const Bar& /*bar*/) override {
        if (script) script(*this, bar_index_);
    }

    // strategy.equity as the script reads it on this bar.
    double equity() const {
        return current_equity() + open_profit(current_bar_.close);
    }
    double position_size() const { return signed_position_size(); }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool pending(const std::string& id) const {
        for (const auto& o : pending_orders_) if (o.id == id) return true;
        return false;
    }
    size_t pending_count() const { return pending_orders_.size(); }
    // Placement verdict per bar for id: true = a PendingOrder exists right
    // after the strategy.entry call on that bar.
    std::vector<std::pair<int, bool>> placements;
    void entry_stop(const std::string& id, bool is_long, double level,
                    double qty, const std::string& comment = "") {
        strategy_entry(id, is_long, kNaN, level, qty, comment);
        placements.emplace_back(bar_index_, pending(id));
    }
    bool placed_on(int bar) const {
        for (const auto& p : placements) if (p.first == bar) return p.second;
        return false;
    }
    bool called_on(int bar) const {
        for (const auto& p : placements) if (p.first == bar) return true;
        return false;
    }
    void enable_margin_call() { set_margin_call_enabled(true); }
    double close_now() const { return current_bar_.close; }
    using BacktestEngine::strategy_entry;
    using BacktestEngine::strategy_exit;
    using BacktestEngine::strategy_close;
    using BacktestEngine::strategy_close_all;
    const std::vector<PyramidEntry>& pyramid_entries() const {
        return pyramid_entries_;
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_entry_price_;
};

// Phase A of the flatten tapes: short 800 placed 08-12 19:45Z, fills 08-13
// 13:30Z @11.29, stop exit 11.32 fills 13:45Z (the flattening bar).
// E_post = C - 24.
void phase_a_short(Probe& p, int bar) {
    if (bar == F0812_1945) {
        p.strategy_entry("S", false, kNaN, kNaN, 800.0);
        p.strategy_exit("XS", "S", kNaN, 11.32);
    }
}

void check_phase_a(const Probe& p) {
    CHECK(p.trade_count() >= 1);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(!t.is_long);
        CHECK(t.entry_bar_index == F0813_1330);
        CHECK_NEAR(t.entry_price, 11.29, 1e-9);
        CHECK(t.exit_bar_index == F0813_1345);
        CHECK_NEAR(t.exit_price, 11.32, 1e-9);
        CHECK_NEAR(t.qty, 800.0, 1e-9);
        CHECK_NEAR(t.pnl, -24.0, 1e-9);
    }
}

// Re-issued long stop at L while flat, from bar `from` to F0813_1930, qty from
// the script's own strategy.equity; eod close at F0813_1945.
void reissue_long_stop(Probe& p, int bar, double L, double frac, bool floor_qty,
                       int from = F0813_1345) {
    if (bar >= from && bar <= F0813_1945 - 1 && p.flat()) {
        const double raw = frac * p.equity() / L;
        p.entry_stop("L", true, L, floor_qty ? std::floor(raw) : raw,
                     "p" + std::to_string(bar));
    }
    if (bar == F0813_1945 && p.position_size() > 0) {
        p.strategy_close("L", "eod");
    }
}

// --- pin: flatten-stop-floor (C 10026 -> E 10002, qty floor(E/L) = 883) ---
// TV: fill 08-13 14:30Z @11.32, 883, Signal p0813-1415. 13:45Z rejected
// (883 * 11.33 = 10,004.39 > 10,002; the raw close 11.325 -> 9,999.98 would
// pass), 14:00Z close 11.33 rejected, 14:15Z close 11.285 -> 11.29 accepted.
// Pre-fix the engine admitted 883 at the 14:00Z open 11.325 (10,002.98
// costed at the open) and filled 14:00Z @11.33.
void test_flatten_stop_floor() {
    std::printf("-- flatten-stop-floor: 883 x 11.33 > 10,002 rejects, fills 14:30Z @11.32 --\n");
    for (bool floor_qty : {true, false}) {
        Probe p(10026.0, 100.0, 0.01, 1.0);
        p.script = [&](Probe& e, int bar) {
            phase_a_short(e, bar);
            reissue_long_stop(e, bar, 11.32, 1.0, floor_qty);
        };
        std::vector<Bar> bars = f_bars();
        p.run(bars.data(), (int)bars.size());
        check_phase_a(p);
        CHECK(!p.placed_on(F0813_1345));   // 10,004.39 > 10,002
        CHECK(!p.placed_on(F0813_1400));   // 883 * 11.33 again
        CHECK(p.placed_on(F0813_1415));    // 883 * 11.29 = 9,969.07
        CHECK(p.trade_count() == 2);
        if (p.trade_count() == 2) {
            const Trade& t = p.get_trade(1);
            CHECK(t.is_long);
            CHECK(t.entry_bar_index == F0813_1430);
            CHECK_NEAR(t.entry_price, 11.32, 1e-9);
            CHECK_NEAR(t.qty, 883.0, 1e-9);        // flatten-stop-raw: 883.57 floored
            CHECK(t.entry_comment == "p" + std::to_string((int)F0813_1415));
            CHECK(t.exit_bar_index == F0814_1330);
            CHECK_NEAR(t.exit_price, 11.30, 1e-9);
            CHECK_NEAR(t.pnl, -17.66, 1e-6);
        }
    }
}

// --- pin: fresh-stop-floor (C 10002, no phase A) --- identical to
// flatten-stop-floor: the flattening is irrelevant.
void test_fresh_stop_floor() {
    std::printf("-- fresh-stop-floor: identical without the flattening --\n");
    Probe p(10002.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        reissue_long_stop(e, bar, 11.32, 1.0, true);
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(!p.placed_on(F0813_1345));
    CHECK(!p.placed_on(F0813_1400));
    CHECK(p.placed_on(F0813_1415));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == F0813_1430);
        CHECK_NEAR(t.entry_price, 11.32, 1e-9);
        CHECK_NEAR(t.qty, 883.0, 1e-9);
    }
}

// --- pin: flatten-stop-once --- the 13:45Z placement is rejected and
// DROPPED: never re-evaluated although every later bar would fill it.
void test_flatten_stop_once_dropped() {
    std::printf("-- flatten-stop-once: a rejected placement is dropped, no long trade --\n");
    Probe p(10026.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        if (bar == F0813_1345) {
            e.entry_stop("L", true, 11.32, std::floor(e.equity() / 11.32));
        }
        if (bar == F0813_1945 && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(!p.placed_on(F0813_1345));
    CHECK(p.pending_count() == 0);
    CHECK(p.flat());
    CHECK(p.trade_count() == 1);
}

// --- pin: flatten-stop-90 (qty floor(0.9 E / L) = 795) --- the flattening
// bar accepts when affordable (795 * 11.33 = 9,007.35 <= 10,002); the 14:00Z
// open 11.325 gaps through 11.32 and the fill prints the tick-rounded open
// 11.33, Signal p0813-1345.
void test_flatten_stop_90_accepted_on_flattening_bar() {
    std::printf("-- flatten-stop-90: flattening bar accepts, gap-open fill @11.33 --\n");
    Probe p(10026.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 11.32, 0.9, true);
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(F0813_1345));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == F0813_1400);
        CHECK_NEAR(t.entry_price, 11.33, 1e-9);
        CHECK_NEAR(t.qty, 795.0, 1e-9);
        CHECK(t.entry_comment == "p" + std::to_string((int)F0813_1345));
        CHECK_NEAR(t.exit_price, 11.30, 1e-9);
        CHECK_NEAR(t.pnl, -23.85, 1e-6);
    }
}

// --- pin: flatten-samedir-90 --- a SHORT re-entry on the bar that stopped
// the short out: the just-closed same-direction position does NOT count at
// placement (795 * 11.33 = 9,007.35 <= 10,002). Fill 14:00Z @11.32 (the
// 11.325 -> 11.33 open is above the short stop, the 11.32 low touches it).
void test_flatten_samedir_90_closed_position_not_counted() {
    std::printf("-- flatten-samedir-90: the position closed on B counts nothing --\n");
    Probe p(10026.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        if (bar >= F0813_1345 && bar <= F0813_1945 - 1 && e.flat()) {
            e.entry_stop("S2", false, 11.32,
                         std::floor(0.9 * e.equity() / 11.32),
                         "p" + std::to_string(bar));
        }
        if (bar == F0813_1945 && e.position_size() != 0) e.strategy_close_all();
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(F0813_1345));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(!t.is_long);
        CHECK(t.entry_id == "S2");
        CHECK(t.entry_bar_index == F0813_1400);
        CHECK_NEAR(t.entry_price, 11.32, 1e-9);
        CHECK_NEAR(t.qty, 795.0, 1e-9);
        CHECK_NEAR(t.exit_price, 11.30, 1e-9);
        CHECK_NEAR(t.pnl, 15.9, 1e-6);
    }
}

// --- pin: flatten-stop-m50 --- margin 50 halves the cost: 883 * 11.33 *
// 0.5 = 5,002.20 <= 10,002 accepts on the flattening bar, fill 14:00Z @11.33.
void test_flatten_stop_m50_halves_cost() {
    std::printf("-- flatten-stop-m50: margin 50 halves the cost --\n");
    Probe p(10026.0, 50.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 11.32, 1.0, true);
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(F0813_1345));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.entry_bar_index == F0813_1400);
        CHECK_NEAR(t.entry_price, 11.33, 1e-9);
        CHECK_NEAR(t.qty, 883.0, 1e-9);
        CHECK_NEAR(t.pnl, -26.49, 1e-6);
    }
}

// --- pin: flatten-stop-floor-c10029 (C 10029 -> E_post 10005) ---
// 883 * 11.33 = 10,004.39 <= 10,005 accepts on the flattening bar itself:
// the basis is the POST-exit realized equity (pre-exit mark-to-market equity
// 10,001 would reject) and exactly the tick 11.33 (threshold in
// (11.3273, 11.3307]). Fill 14:00Z @11.33 (10,004.39 <= 10,005 at the fill).
void test_flatten_stop_floor_c10029_post_exit_equity() {
    std::printf("-- flatten-stop-floor-c10029: post-exit equity, basis exactly 11.33 --\n");
    Probe p(10029.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 11.32, 1.0, true);
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(F0813_1345));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.entry_bar_index == F0813_1400);
        CHECK_NEAR(t.entry_price, 11.33, 1e-9);
        CHECK_NEAR(t.qty, 883.0, 1e-9);
    }
}

// --- pin: flatten-closenext-90 --- strategy.close("S") + the long stop on
// the same bar with the short still open: a reversal, costed on the NEW side
// only (794 * 11.33 = 8,996.02 <= MTM 9,998); both execute at the next open
// (exit short @11.33 "xnext", long 794 @11.33).
void test_flatten_closenext_90_reversal_new_side_only() {
    std::printf("-- flatten-closenext-90: reversal costs the new side only --\n");
    Probe p(10026.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == F0812_1945) e.strategy_entry("S", false, kNaN, kNaN, 800.0);
        if (bar == F0813_1345) e.strategy_close("S", "xnext");
        if (bar >= F0813_1345 && bar <= F0813_1945 - 1 && e.position_size() <= 0) {
            e.entry_stop("L", true, 11.32, std::floor(0.9 * e.equity() / 11.32),
                         "p" + std::to_string(bar));
        }
        if (bar == F0813_1945 && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0813_1345));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& s = p.get_trade(0);
        CHECK(!s.is_long);
        CHECK_NEAR(s.entry_price, 11.29, 1e-9);
        CHECK(s.exit_bar_index == F0813_1400);
        CHECK_NEAR(s.exit_price, 11.33, 1e-9);
        CHECK_NEAR(s.pnl, -32.0, 1e-9);
        const Trade& t = p.get_trade(1);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == F0813_1400);
        CHECK_NEAR(t.entry_price, 11.33, 1e-9);
        CHECK_NEAR(t.qty, 794.0, 1e-9);
        CHECK_NEAR(t.pnl, -23.82, 1e-6);
    }
}

// --- pin: flatten-closeimm-90 --- strategy.close("S", immediately=true)
// flattens on the bar itself (exit 13:45Z @11.33 "ximm"); the stop entry
// placed after it is costed from the post-close equity 9,994: floor(0.9 *
// 9,994 / 11.32) = 794, 794 * 11.33 = 8,996.02 <= 9,994 -> fill 14:00Z @11.33.
void test_flatten_closeimm_90() {
    std::printf("-- flatten-closeimm-90: same-bar immediate flatten, entry accepted --\n");
    Probe p(10026.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == F0812_1945) e.strategy_entry("S", false, kNaN, kNaN, 800.0);
        if (bar == F0813_1345) e.strategy_close("S", "ximm", kNaN, kNaN, true);
        if (bar >= F0813_1345 && bar <= F0813_1945 - 1 && e.position_size() <= 0) {
            e.entry_stop("L", true, 11.32, std::floor(0.9 * e.equity() / 11.32),
                         "p" + std::to_string(bar));
        }
        if (bar == F0813_1945 && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0813_1345));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& s = p.get_trade(0);
        CHECK(s.exit_bar_index == F0813_1345);
        CHECK_NEAR(s.exit_price, 11.33, 1e-9);
        CHECK_NEAR(s.pnl, -32.0, 1e-9);
        const Trade& t = p.get_trade(1);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == F0813_1400);
        CHECK_NEAR(t.entry_price, 11.33, 1e-9);
        CHECK_NEAR(t.qty, 794.0, 1e-9);
    }
}

// --- pin: flatten-market-floor --- the round-5 MARKET rule on the flattening
// bar is unchanged: floor(10,002 / 11.325) = 883 * 11.33 rejected 13:45Z,
// floor(10,002 / 11.33) = 882 * 11.33 = 9,993.06 accepted 14:00Z, fill 14:15Z
// at the 11.335 -> 11.34 open (882 * 11.34 = 10,001.88 <= 10,002).
void test_flatten_market_floor_round5_rule_unchanged() {
    std::printf("-- flatten-market-floor: round-5 market rule unchanged --\n");
    Probe p(10026.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        if (bar >= F0813_1345 && bar <= F0813_1945 - 1 && e.flat()) {
            e.strategy_entry("L", true, kNaN, kNaN,
                             std::floor(e.equity() / e.close_now()),
                             "p" + std::to_string(bar));
            e.placements.emplace_back(bar, e.pending("L"));
        }
        if (bar == F0813_1945 && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(!p.placed_on(F0813_1345));
    CHECK(p.placed_on(F0813_1400));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.entry_bar_index == F0813_1415);
        CHECK_NEAR(t.entry_price, 11.34, 1e-9);
        CHECK_NEAR(t.qty, 882.0, 1e-9);
        CHECK_NEAR(t.pnl, -35.28, 1e-6);
    }
}

// --- pin: fresh-floorraw-0814 (C 10000, raw qty 884.956, L 11.30 from 08-14
// 14:45Z) --- the check uses the FLOORED qty: 884 * tick(11.305 -> 11.31) =
// 9,998.04 <= 10,000 accepts (raw 884.956 * 11.31 = 10,008.9 would reject);
// fill 15:00Z at the 11.30 open (through the level), 884 shares.
void test_fresh_floorraw_0814_floored_qty() {
    std::printf("-- fresh-floorraw-0814: the floored qty is what is costed --\n");
    Probe p(10000.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar >= F0814_1445 && bar <= F0814_1515 && e.flat()) {
            e.entry_stop("L", true, 11.30, e.equity() / 11.30,
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0814_1445));
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 884.0, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 11.30, 1e-9);
    CHECK(p.trade_count() == 0);
}

// --- pin: fresh-gap-once (C 10000, long stop 11.24 x 889 placed 08-12 19:45Z
// at close 11.24 = L, accepted: 9,992.36 <= 10,000) --- 08-13 opens 11.29
// THROUGH the level: the fill check 889 * 11.29 = 10,036.81 > 10,000 REJECTS
// the fill and the order is dropped — no partial fill, no slice, no trade.
void test_fresh_gap_once_fill_rejected_and_dropped() {
    std::printf("-- fresh-gap-once: gap-through costed at the rounded open, rejected and dropped --\n");
    Probe p(10000.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == F0812_1945) {
            e.entry_stop("L", true, 11.24, std::floor(e.equity() / 11.24),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0812_1945));
    CHECK(p.flat());
    CHECK(p.pending_count() == 0);
    CHECK(p.trade_count() == 0);
}

// --- pin: fresh-gap-replace --- re-issued while flat: every 08-13 close x
// 889 > 10,000 (13:30Z 11.25 -> 10,001.25 already rejects); the first close
// at or below 11.248 is 08-14 13:30Z (11.225 -> 11.23) -> accepted -> the
// 13:45Z bar (open 11.225 -> 11.23 < L, high 11.27) touches -> fill @11.24.
void test_fresh_gap_replace() {
    std::printf("-- fresh-gap-replace: first affordable close 08-14 13:30Z, fill 13:45Z @11.24 --\n");
    Probe p(10000.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar >= F0812_1945 && bar <= F0814_1515 && e.flat()) {
            e.entry_stop("L", true, 11.24, std::floor(e.equity() / 11.24),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0812_1945));
    for (int b = F0813_1330; b <= F0813_1945; ++b) CHECK(!p.placed_on(b));
    CHECK(p.placed_on(F0814_1330));
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 889.0, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 11.24, 1e-9);
    CHECK(p.trade_count() == 0);
    // The one placement that filled was the 08-14 13:30Z re-issue.
    int last_call = -1;
    for (const auto& pl : p.placements) last_call = pl.first;
    CHECK(last_call == F0814_1330);
}

// --- pin: fresh-touch-once (C 10004.2, short stop 11.23 x 890 placed 08-12
// 19:45Z: 890 * 11.24 = 10,003.6 <= 10,004.2 accepted) --- 08-13 13:30Z
// opens 11.29 > L and touches (low 11.19): the fill is costed at the LEVEL,
// 890 * 11.23 = 9,994.7 <= E -> FILLS @11.23; the open would cost 10,048.1 >
// E (KI-62's "costs the bar open" is refuted). Pre-fix: declined, no trade.
void test_fresh_touch_once_fill_costed_at_level() {
    std::printf("-- fresh-touch-once: a touch is costed at the level, 890 x 11.23 fills --\n");
    Probe p(10004.2, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == F0812_1945) {
            e.entry_stop("S", false, 11.23, std::floor(e.equity() / 11.23),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0812_1945));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 890.0, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 11.23, 1e-9);
    CHECK(p.trade_count() == 0);
}

// The same tape with the margin call on: TV admits the under-margined 890
// and the ordinary margin call slices it from the fill bar on (8 @11.25 on
// 13:30Z, 24 @11.33 on 13:45Z, then 1 / 4 / 4) — the entry itself is
// untouched (890 @11.23) and every slice comes out of those 890. The slice
// SIZES are the KI-31 cascade's business, not this pin's: the engine takes
// the fill bar's adverse extreme from the bar's high (11.29, the open —
// which the short never saw, the level was touched after it) where TV
// marks the post-fill path (close 11.25); that entry-bar chronology is a
// separate lead (32 @11.29 here vs TV's 8 @11.25) and is deliberately not
// asserted.
void test_fresh_touch_once_margin_call_slices() {
    std::printf("-- fresh-touch-once + margin call: admitted fill, sliced from 890 --\n");
    Probe p(10004.2, 100.0, 0.01, 1.0);
    p.enable_margin_call();
    p.script = [&](Probe& e, int bar) {
        if (bar == F0812_1945) {
            e.entry_stop("S", false, 11.23, std::floor(e.equity() / 11.23),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_entry_price_, 11.23, 1e-9);
    double sliced = 0.0;
    for (int i = 0; i < p.trade_count(); ++i) {
        sliced += p.get_trade(i).qty;
        CHECK(p.get_trade(i).entry_bar_index == F0813_1330);
        CHECK_NEAR(p.get_trade(i).entry_price, 11.23, 1e-9);
    }
    CHECK_NEAR(sliced + p.position_qty_, 890.0, 1e-9);
    CHECK(p.trade_count() >= 1);
    if (p.trade_count() >= 1) {
        CHECK(p.get_trade(0).exit_bar_index == F0813_1330);   // sliced on the fill bar
    }
}

// --- pin: fresh-0919-once (C 10000, short stop 11.62 x 860 placed 09-19
// 13:30Z) --- close 11.625 rounds half-UP to 11.63: 860 * 11.63 = 10,001.8 >
// 10,000 -> rejected and dropped, although the 13:45Z open 11.62 = L would
// have been affordable (9,993.2): the check is at placement, not fill-only.
void test_fresh_0919_once_half_up_rejected() {
    std::printf("-- fresh-0919-once: 11.625 rounds half-up, rejected at placement --\n");
    Probe p(10000.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == S1330) {
            e.entry_stop("S", false, 11.62, std::floor(e.equity() / 11.62),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = f0919_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(!p.placed_on(S1330));
    CHECK(p.flat());
    CHECK(p.trade_count() == 0);
}

// --- pin: fresh-0919-replace --- 13:45Z..14:30Z closes (11.68 / 11.72 /
// 11.68 / 11.65) rejected, the 14:45Z close 11.62 accepted (9,993.2), fill
// 15:00Z at the 11.62 open, 860 shares.
void test_fresh_0919_replace() {
    std::printf("-- fresh-0919-replace: accept 14:45Z, fill 15:00Z @11.62 --\n");
    Probe p(10000.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar >= S1330 && bar <= S1530 && e.flat()) {
            e.entry_stop("S", false, 11.62, std::floor(e.equity() / 11.62),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = f0919_bars();
    p.run(bars.data(), (int)bars.size());
    for (int b = S1330; b <= S1430; ++b) CHECK(!p.placed_on(b));
    CHECK(p.placed_on(S1445));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 860.0, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 11.62, 1e-9);
    CHECK(p.trade_count() == 0);
}

// --- the probe itself (waranyutrkm F@15) decoded with the rule ---
// 08-13: equity 10,262.39, qty 906: 13:45Z 906 * 11.33 = 10,264.98 > E
// reject (pre-fix: admitted at the 14:00Z open, 906 * 11.325 = 10,260.45),
// 14:00Z reject, 14:15Z 906 * 11.29 = 10,228.74 accept -> 14:30Z @11.32 = TV.
// 09-19: equity 10,298.91, qty 886: 13:30Z 886 * 11.63 = 10,304.18 reject ...
// 14:45Z 886 * 11.62 = 10,295.32 accept -> 15:00Z @11.62 = TV (pre-fix the
// engine admitted at the 13:45Z open 11.62 = L).
void test_probe_decode_f_0813_and_0919() {
    std::printf("-- probe decode: F 08-13 -> 14:30Z @11.32, 09-19 -> 15:00Z @11.62 --\n");
    {
        Probe p(10262.39, 100.0, 0.01, 1.0);
        p.script = [&](Probe& e, int bar) {
            if (bar >= F0813_1345 && bar <= F0813_1945 - 1 && e.flat()) {
                e.entry_stop("L", true, 11.32, 906.0, "p" + std::to_string(bar));
            }
        };
        std::vector<Bar> bars = f_bars();
        p.run(bars.data(), (int)bars.size());
        CHECK(!p.placed_on(F0813_1345));
        CHECK(!p.placed_on(F0813_1400));
        CHECK(p.placed_on(F0813_1415));
        CHECK(p.position_side_ == PositionSide::LONG);
        CHECK_NEAR(p.position_qty_, 906.0, 1e-9);
        CHECK_NEAR(p.position_entry_price_, 11.32, 1e-9);
        CHECK(p.trade_count() == 0);
        // The engine opened it on the 14:30Z bar.
        CHECK(!p.pyramid_entries().empty());
        if (!p.pyramid_entries().empty()) {
            CHECK(p.pyramid_entries().back().entry_bar_index == F0813_1430);
        }
    }
    {
        Probe p(10298.91, 100.0, 0.01, 1.0);
        p.script = [&](Probe& e, int bar) {
            if (bar >= S1330 && bar <= S1530 && e.flat()) {
                e.entry_stop("S", false, 11.62, 886.0, "p" + std::to_string(bar));
            }
        };
        std::vector<Bar> bars = f0919_bars();
        p.run(bars.data(), (int)bars.size());
        for (int b = S1330; b <= S1430; ++b) CHECK(!p.placed_on(b));
        CHECK(p.placed_on(S1445));
        CHECK(p.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(p.position_qty_, 886.0, 1e-9);
        CHECK_NEAR(p.position_entry_price_, 11.62, 1e-9);
        CHECK(!p.pyramid_entries().empty());
        if (!p.pyramid_entries().empty()) {
            CHECK(p.pyramid_entries().back().entry_bar_index == S1500);
        }
    }
}

// XAU phase A: long 3 at 14:30Z, fills 14:45Z @3335.725, stop exit 3332.34
// fills 15:00Z; E_post = C - 10.155.
void xau_phase_a(Probe& p, int bar) {
    if (bar == X1430) {
        p.strategy_entry("Lg", true, kNaN, kNaN, 3.0);
        p.strategy_exit("XL", "Lg", kNaN, 3332.34);
    }
}

void check_xau_phase_a(const Probe& p) {
    CHECK(p.trade_count() >= 1);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == X1445);
        CHECK_NEAR(t.entry_price, 3335.725, 1e-9);
        CHECK(t.exit_bar_index == X1500);
        CHECK_NEAR(t.exit_price, 3332.34, 1e-9);
        CHECK_NEAR(t.pnl, -10.155, 1e-9);
    }
}

double xau_qty(const Probe& p, double L) {
    return std::floor(100.0 * p.equity() / L) / 100.0;
}

// --- pin: xau-flatten-once (C 11000 -> E 10989.845, qty 3.29) --- the
// flattening bar accepts a close 2.39 ABOVE the short level (3.29 * 3334.73
// = 10,971.26 <= E: equity, not the level, is the bound); the accepted
// order rests 3 bars and fills on the 16:00Z touch at the level 3332.34.
void test_xau_flatten_once_rests_until_touched() {
    std::printf("-- xau-flatten-once: accepted above the level, rests 3 bars, fills at the level --\n");
    Probe p(11000.0, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar == X1500) {
            e.entry_stop("S", false, 3332.34, xau_qty(e, 3332.34),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = xau_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(X1500));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 3.29, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 3332.34, 1e-9);
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == X1600);
    }
    CHECK(p.trade_count() == 1);
}

// --- pin: xau-flatten-once-c10983 (C 10983.155 -> E 10973) --- the resting
// order is NOT re-checked at later closes (the 15:45Z close 3335.72 would
// cost 10,974.52 > E) and the 16:00Z touch is costed at the LEVEL (10,963.40
// <= E) although the open 3335.73 costs 10,974.55 > E. Pre-fix: declined.
void test_xau_flatten_once_c10983_no_recheck_fill_at_level() {
    std::printf("-- xau-flatten-once-c10983: no re-check while resting, touch costed at the level --\n");
    Probe p(10983.155, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar == X1500) {
            e.entry_stop("S", false, 3332.34, xau_qty(e, 3332.34),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = xau_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(X1500));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 3.29, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 3332.34, 1e-9);
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == X1600);
    }
}

// --- pin: xau-flatten-replace-c10983 --- re-issued every bar while flat:
// 15:00Z / 15:15Z / 15:30Z accepted (10,971.26 / 10,971.38 / 10,972.63 <=
// 10,973), the 15:45Z re-issue REJECTED (3.29 * 3335.72 = 10,974.52) and
// that CANCELS the resting order — the 16:00Z touch fills nothing; the
// 16:00Z re-issue (close 3333.41 -> 10,966.92) is accepted and fills on the
// 16:15Z touch, Signal p0818-1600. Pre-fix: filled 16:00Z.
void test_xau_flatten_replace_c10983_rejected_reissue_cancels() {
    std::printf("-- xau-flatten-replace-c10983: a rejected re-issue cancels the resting order --\n");
    Probe p(10983.155, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar >= X1500 && bar <= X1645 && e.flat()) {
            e.entry_stop("S", false, 3332.34, xau_qty(e, 3332.34),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = xau_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(X1500));
    CHECK(p.placed_on(X1515));
    CHECK(p.placed_on(X1530));
    CHECK(!p.placed_on(X1545));       // rejected AND the resting order is gone
    CHECK(p.called_on(X1600));        // still flat at 16:00Z: nothing filled
    CHECK(p.placed_on(X1600));
    CHECK(!p.called_on(X1615));       // filled on the 16:15Z bar
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 3.29, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 3332.34, 1e-9);
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == X1615);
        CHECK(p.pyramid_entries().back().entry_comment
              == "p" + std::to_string((int)X1600));
    }
}

// --- pin: xau-flatten-replace (C 11000) --- every re-issue is affordable;
// the 15:45Z one is the live order at the 16:00Z touch (Signal p0818-1545).
void test_xau_flatten_replace_control() {
    std::printf("-- xau-flatten-replace: affordable re-issues, fills 16:00Z --\n");
    Probe p(11000.0, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar >= X1500 && bar <= X1645 && e.flat()) {
            e.entry_stop("S", false, 3332.34, xau_qty(e, 3332.34),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = xau_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(X1545));
    CHECK(!p.called_on(X1600));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 3.29, 1e-9);
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == X1600);
        CHECK(p.pyramid_entries().back().entry_comment
              == "p" + std::to_string((int)X1545));
    }
}

// --- engine scope (no tape): a STOP reversal whose entry leg is rejected at
// placement keeps its CLOSING leg, like the pinned market rule (rampatel BTC
// 2025-05-12 07:15Z). Short 800 held, MTM 9,998 at the 13:45Z close; a long
// stop x 900 costs 900 * 11.33 = 10,197 > 9,998 -> the entry leg is dropped,
// the order rests close-only, the 14:00Z gap-through closes the short at
// 11.33 and opens nothing.
void test_rejected_stop_reversal_keeps_close_leg() {
    std::printf("-- rejected stop reversal: closing leg only --\n");
    Probe p(10026.0, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == F0812_1945) e.strategy_entry("S", false, kNaN, kNaN, 800.0);
        if (bar == F0813_1345) e.entry_stop("L", true, 11.32, 900.0, "rev");
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0813_1345));     // survives as the closing leg
    CHECK(p.flat());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& s = p.get_trade(0);
        CHECK(!s.is_long);
        CHECK(s.exit_bar_index == F0813_1400);
        CHECK_NEAR(s.exit_price, 11.33, 1e-9);
        CHECK(s.exit_id == "L");
    }
    CHECK(p.pending_count() == 0);
}

// Control: margin 0 disables both halves — the 13:45Z placement is accepted
// and the 14:00Z gap-through fills 883 @11.33 whatever the equity.
void test_margin_zero_control() {
    std::printf("-- control: margin 0 has no admission --\n");
    Probe p(10026.0, 0.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 11.32, 1.0, true);
    };
    std::vector<Bar> bars = f_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(F0813_1345));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(p.get_trade(1).entry_bar_index == F0813_1400);
        CHECK_NEAR(p.get_trade(1).entry_price, 11.33, 1e-9);
        CHECK_NEAR(p.get_trade(1).qty, 883.0, 1e-9);
    }
}

}  // namespace

int main() {
    std::printf("--- stop_entry_admission (round 7, log-20260905t053924z-15615295) ---\n");
    test_flatten_stop_floor();
    test_fresh_stop_floor();
    test_flatten_stop_once_dropped();
    test_flatten_stop_90_accepted_on_flattening_bar();
    test_flatten_samedir_90_closed_position_not_counted();
    test_flatten_stop_m50_halves_cost();
    test_flatten_stop_floor_c10029_post_exit_equity();
    test_flatten_closenext_90_reversal_new_side_only();
    test_flatten_closeimm_90();
    test_flatten_market_floor_round5_rule_unchanged();
    test_fresh_floorraw_0814_floored_qty();
    test_fresh_gap_once_fill_rejected_and_dropped();
    test_fresh_gap_replace();
    test_fresh_touch_once_fill_costed_at_level();
    test_fresh_touch_once_margin_call_slices();
    test_fresh_0919_once_half_up_rejected();
    test_fresh_0919_replace();
    test_probe_decode_f_0813_and_0919();
    test_xau_flatten_once_rests_until_touched();
    test_xau_flatten_once_c10983_no_recheck_fill_at_level();
    test_xau_flatten_replace_c10983_rejected_reissue_cancels();
    test_xau_flatten_replace_control();
    test_rejected_stop_reversal_keeps_close_leg();
    test_margin_zero_control();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
