/*
 * test_aapl15_margin_brackets.cpp — round 7 family N: the NASDAQ:AAPL 15m
 * near-miss singletons (algoai, shojiy, willowsportz, fast-scalper,
 * therealbouga) — three engine mechanisms, all replayed on the registry's own
 * NASDAQ:AAPL 15 bars (feed ae2b03d3736f) and NYSE:F 15 bars (feed
 * 80f404ae85ef) with the tapes' capital and orders.
 *
 * M1 — campaign pin log-20260905t112243z-b6ddd126 (lab tv tape
 *   scratchpad/r7/pins/aapl15-mcopen-willow): a forced-liquidation slice on a
 *   bar whose OPEN already breaches margin is sized with the position MARKED
 *   AT THE TICK-ROUNDED OPEN — slice = max(1, 4 x floor(x)),
 *   x = (q x P - E(P)) / P with P = round_to_mintick(open) — the same
 *   on-tick ledger the adverse-extreme cascade marks on. The engine marked
 *   the raw half-tick open (196.135 -> 408 where TV prints 412 at 196.14).
 *   A. willowsportz 04-22 13:30Z: 412 @196.14 (the whole tape row-for-row:
 *      12 / 36 / 156 / 412 / 676 / 4165).
 *   B. algoai 06-20 13:30Z (o 198.235): 64 @198.24, then the 'Short Exit'
 *      stop 3803 @200.00 on the same bar (the engine printed 60 / 3807).
 *
 * M2 — note log-20260905t112259z-33f32db4: on a bar whose OPEN carries a
 *   declined all-in reversal, TradingView's sequence is decline -> bracket
 *   dormant -> margin slice -> REVIVE, so the resting stop is live again for
 *   the rest of the bar; at an adverse-extreme cascade a revived marketable
 *   bracket closes the remainder AT THE SLICE PRICE on the same bar.
 *   C. algoai 10-30 13:30Z (lab tv tape aapl15-mcopen1-stop-algoai + the
 *      probe's declined ema9/21 reversal): 1 @271.96 open slice, then the
 *      'X' stop 2814 @273.69 AT ITS LEVEL (the engine left the bracket
 *      dormant: 176 @274.11 and a next-bar close).
 *   D. fast-scalper 07-21 13:30Z (probe rows TV#160/161): the declined
 *      reversal keeps the 213.08 stop dormant across the O->L->H path; the
 *      high 214.86 breaches -> 268 @214.86 'Margin call' AND the revived,
 *      now-marketable stop closes 4621 @214.86 on the same bar (the engine
 *      closed the remainder next bar @214.68: REVIVE-B skipped a re-issued
 *      bracket carrying a frozen full-position qty).
 *   E. control (lab tv tape aapl15-mcext-stop-scalper-b, no reversal): the
 *      stop fills at its level 212.83 x4883, no 07-21 slice — 1 / 20 / 108 /
 *      4883 row-for-row.
 *
 * M3 — note log-20260905t112315z-a234f071 (census 51/51 AAPL + 56/56 F
 *   therealbouga entries, 0 exceptions): layered strategy.exit legs from one
 *   entry — 'TP1' qty_percent=50 + the default 'TP2' (limit+stop) — split
 *   EXACTLY 50/50, bound ONCE at the fill and unchanged by the per-bar
 *   re-issues (strategy.entry re-issued too, refused by pyramiding=0) and by
 *   which leg fires first. The engine lost the split whenever the legs were
 *   armed on a REVERSAL bar: the partial froze against the OLD position
 *   (~25% shape: 125/364 of 489) and a later re-issue then dropped it behind
 *   the still-deferred 100% sibling (0% shape: 502 'S TP2').
 *   F. therealbouga AAPL 05-07 13:30Z: long 236 -> short 502, re-issued
 *      every bar; the 'S TP2' stop at 19:30Z closes 251 @196.10, 251 held.
 *   G. therealbouga AAPL 06-24 14:30Z: short 250 -> long 490 (no re-issue):
 *      'L TP1' 245 @203.26; the 06-25 reversal closes the other 245 @201.41
 *      and its own 'S TP2' stop then closes 245 @202.61.
 *   H. therealbouga F 08-08 13:45Z: short 4591 -> long 8890: 4445 @11.43 +
 *      4445 @11.53 on 08-11 13:30Z; the flat-open 08-11 entry keeps its
 *      4349/4349 split (the stop at 11.26 closes exactly half).
 */

#include <cmath>
#include <cstdint>
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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct BarRow {
    int64_t ts;
    double open, high, low, close;
};

template <size_t N>
std::vector<Bar> to_bars(const BarRow (&rows)[N]) {
    std::vector<Bar> out;
    out.reserve(N);
    for (const BarRow& r : rows) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

// NASDAQ:AAPL 15 (feed ae2b03d3736f), 2025-04-21 17:45Z .. 2025-04-23 16:15Z.
static const BarRow kAaplWillow[] = {
    {1745257500000LL, 190.61, 190.7, 190.3, 190.515},   // [0] 04-21 17:45 signal
    {1745258400000LL, 190.52, 190.6, 190.25, 190.55},   // [1] 18:00 entry bar
    {1745259300000LL, 190.55, 190.91, 190.53, 190.81},  // [2] 18:15
    {1745260200000LL, 190.81, 191.14, 190.53, 190.59},  // [3] 18:30
    {1745261100000LL, 190.61, 190.66, 190.18, 190.28},  // [4] 18:45
    {1745262000000LL, 190.27, 190.91, 190.25, 190.81},  // [5] 19:00
    {1745262900000LL, 190.81, 191.38, 190.605, 191.1},  // [6] 19:15
    {1745263800000LL, 191.13, 192.09, 191.06, 191.92},  // [7] 19:30
    {1745264700000LL, 191.91, 193.43, 191.61, 193.03},  // [8] 19:45
    {1745328600000LL, 196.135, 197.5, 195.96, 197.25},  // [9] 04-22 13:30 half-tick open
    {1745329500000LL, 197.28, 197.855, 197.14, 197.81}, // [10] 13:45
    {1745330400000LL, 197.87, 198.93, 197.65, 198.19},  // [11] 14:00
    {1745331300000LL, 198.16, 198.26, 197.68, 198.12},  // [12] 14:15
    {1745332200000LL, 198.1, 198.8, 197.68, 198.51},    // [13] 14:30
    {1745333100000LL, 198.49, 199.39, 198.32, 199.3},   // [14] 14:45
    {1745334000000LL, 199.31, 199.46, 198.92, 199.39},  // [15] 15:00
    {1745334900000LL, 199.4, 199.54, 198.8, 198.83},    // [16] 15:15
    {1745335800000LL, 198.825, 198.86, 197.87, 198.03}, // [17] 15:30
    {1745336700000LL, 198.02, 199.38, 197.98, 199.34},  // [18] 15:45
    {1745337600000LL, 199.36, 200.22, 199.02, 200.04},  // [19] 16:00
    {1745338500000LL, 200.01, 201.55, 199.75, 201.39},  // [20] 16:15
    {1745339400000LL, 201.4, 201.58, 200.44, 200.92},   // [21] 16:30
    {1745340300000LL, 200.89, 201.09, 200.29, 200.59},  // [22] 16:45
    {1745341200000LL, 200.61, 201.01, 199.52, 199.68},  // [23] 17:00
    {1745342100000LL, 199.7, 200.17, 198.33, 198.53},   // [24] 17:15
    {1745343000000LL, 198.525, 199.26, 198.17, 198.82}, // [25] 17:30
    {1745343900000LL, 198.82, 198.87, 198.11, 198.27},  // [26] 17:45
    {1745344800000LL, 198.19, 199.37, 198.14, 199.33},  // [27] 18:00
    {1745345700000LL, 199.32, 200.02, 198.97, 199.88},  // [28] 18:15
    {1745346600000LL, 199.9, 200.14, 199.8, 200},       // [29] 18:30
    {1745347500000LL, 200.02, 200.54, 200.02, 200.15},  // [30] 18:45
    {1745348400000LL, 200.16, 200.42, 199.49, 199.57},  // [31] 19:00
    {1745349300000LL, 199.6, 199.63, 198.57, 198.61},   // [32] 19:15
    {1745350200000LL, 198.6, 199.11, 198, 198.92},      // [33] 19:30
    {1745351100000LL, 198.88, 199.89, 198.69, 199.57},  // [34] 19:45
    {1745415000000LL, 206, 207.5, 204.64, 206.7},       // [35] 04-23 13:30 open slice 676
    {1745415900000LL, 206.68, 207.62, 205.86, 207.36},  // [36] 13:45
    {1745416800000LL, 207.38, 208, 205.74, 206.39},     // [37] 14:00
    {1745417700000LL, 206.38, 207.1, 205.63, 206.8},    // [38] 14:15
    {1745418600000LL, 206.75, 207.95, 206.68, 207.73},  // [39] 14:30
    {1745419500000LL, 207.7, 207.77, 206.71, 206.905},  // [40] 14:45
    {1745420400000LL, 206.93, 207.56, 206.19, 206.82},  // [41] 15:00
    {1745421300000LL, 206.83, 206.85, 204.05, 204.62},  // [42] 15:15
    {1745422200000LL, 204.67, 205.42, 204.17, 204.84},  // [43] 15:30
    {1745423100000LL, 204.88, 205.17, 203.67, 204.29},  // [44] 15:45
    {1745424000000LL, 204.26, 204.36, 203.59, 203.81},  // [45] 16:00 close_all
    {1745424900000LL, 203.81, 204.18, 202.79, 204.11},  // [46] 16:15 fill
};

// NASDAQ:AAPL 15, 2025-06-18 19:15Z .. 2025-06-20 14:00Z (06-19 closed).
static const BarRow kAaplAlgoai0620[] = {
    {1750274100000LL, 195.96, 196.29, 195.6, 195.64},   // [0] 06-18 19:15
    {1750275000000LL, 195.635, 196.3, 195.47, 196.3},   // [1] 19:30 signal
    {1750275900000LL, 196.29, 197.11, 196.07, 196.26},  // [2] 19:45 entry bar
    {1750426200000LL, 198.235, 200.94, 197.52, 200.61}, // [3] 06-20 13:30 half-tick open
    {1750427100000LL, 200.62, 200.715, 199.73, 199.85}, // [4] 13:45
    {1750428000000LL, 199.83, 199.93, 198.98, 199.55},  // [5] 14:00
};

// NASDAQ:AAPL 15, 2025-07-17 19:15Z .. 2025-07-21 14:00Z.
static const BarRow kAaplScalper[] = {
    {1752779700000LL, 210.825, 211.06, 210.825, 210.99}, // [0] 07-17 19:15
    {1752780600000LL, 211, 211.05, 210.68, 210.72},      // [1] 19:30 signal
    {1752781500000LL, 210.71, 210.75, 209.74, 210.02},   // [2] 19:45 entry bar
    {1752845400000LL, 210.87, 211.01, 209.9, 210.03},    // [3] 07-18 13:30
    {1752846300000LL, 210.01, 210.51, 209.89, 210.32},   // [4] 13:45
    {1752847200000LL, 210.33, 210.62, 209.71, 210.1},    // [5] 14:00
    {1752848100000LL, 210.11, 210.31, 209.89, 209.95},   // [6] 14:15
    {1752849000000LL, 209.96, 210.51, 209.78, 210.29},   // [7] 14:30
    {1752849900000LL, 210.34, 211.01, 210.27, 210.49},   // [8] 14:45
    {1752850800000LL, 210.5, 211, 210.44, 210.77},       // [9] 15:00
    {1752851700000LL, 210.74, 210.9, 210.42, 210.83},    // [10] 15:15
    {1752852600000LL, 210.87, 211.005, 210.7, 210.97},   // [11] 15:30
    {1752853500000LL, 210.97, 211.1, 210.93, 211.08},    // [12] 15:45
    {1752854400000LL, 211.07, 211.13, 210.9, 210.94},    // [13] 16:00
    {1752855300000LL, 210.92, 211.105, 210.67, 211},     // [14] 16:15
    {1752856200000LL, 211.02, 211.76, 210.88, 211.64},   // [15] 16:30
    {1752857100000LL, 211.66, 211.79, 211.2, 211.32},    // [16] 16:45
    {1752858000000LL, 211.31, 211.4, 211.05, 211.22},    // [17] 17:00
    {1752858900000LL, 211.25, 211.43, 211.1, 211.19},    // [18] 17:15
    {1752859800000LL, 211.18, 211.53, 211.02, 211.32},   // [19] 17:30
    {1752860700000LL, 211.33, 211.44, 210.97, 211.095},  // [20] 17:45
    {1752861600000LL, 211.1, 211.26, 210.88, 210.93},    // [21] 18:00
    {1752862500000LL, 210.95, 210.97, 210.765, 210.94},  // [22] 18:15
    {1752863400000LL, 210.93, 211.06, 210.86, 211.01},   // [23] 18:30
    {1752864300000LL, 211.01, 211.055, 210.79, 210.97},  // [24] 18:45 stop re-issue
    {1752865200000LL, 210.96, 211.04, 210.88, 211.02},   // [25] 19:00
    {1752866100000LL, 211.02, 211.195, 210.895, 210.95}, // [26] 19:15
    {1752867000000LL, 210.94, 211.065, 210.84, 210.95},  // [27] 19:30
    {1752867900000LL, 210.96, 211.35, 210.835, 211.225}, // [28] 19:45 reversal signal
    {1753104600000LL, 212.06, 214.86, 211.63, 214.67},   // [29] 07-21 13:30
    {1753105500000LL, 214.68, 215.78, 213.96, 214.01},   // [30] 13:45
    {1753106400000LL, 214.05, 214.76, 214.01, 214.73},   // [31] 14:00
};

// The tapes' broker: 1x margin both sides, margin calls on, market fills at
// the next open, integer lots, mintick 0.01, no commission. FIXED default
// sizing by default (the tapes' fixed lots); PERCENT_OF_EQUITY 100 for the
// all-in reversal shapes.
class Probe : public BacktestEngine {
public:
    explicit Probe(double capital, double default_qty = 1.0) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = default_qty;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }
    std::function<void(Probe&, int)> script;
    void on_bar(const Bar& /*bar*/) override {
        if (script) script(*this, bar_index_);
    }
    void all_in() {
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
    }
    void set_default_qty(double q) { default_qty_value_ = q; }
    void entry_default(const std::string& id, bool is_long) {
        strategy_entry(id, is_long, kNaN, kNaN, kNaN, "");
    }
    void entry_market(const std::string& id, bool is_long, double qty) {
        strategy_entry(id, is_long, kNaN, kNaN, qty, "");
    }
    void exit_stop(const std::string& id, const std::string& from, double stop) {
        strategy_exit(id, from, kNaN, stop);
    }
    void exit_limit_pct(const std::string& id, const std::string& from,
                        double limit, double pct) {
        strategy_exit(id, from, limit, kNaN, kNaN, kNaN, kNaN, pct);
    }
    void exit_limit_stop(const std::string& id, const std::string& from,
                         double limit, double stop) {
        strategy_exit(id, from, limit, stop);
    }
    void close_all() { strategy_close_all(); }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    int margin_call_rows() const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_comment == "Margin call") ++n;
        }
        return n;
    }
    int rows_exiting_on(int bar) const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_bar_index == bar) ++n;
        }
        return n;
    }
    int long_rows() const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).is_long) ++n;
        }
        return n;
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_entry_price_;
};

void print_trades(const Probe& p) {
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        std::printf("      trade %d: %s entry bar %d @ %.5f qty %.4f exit bar %d @ %.5f pnl %.5f [%s|%s]\n",
                    i, t.is_long ? "long" : "short", t.entry_bar_index,
                    t.entry_price, t.qty, t.exit_bar_index, t.exit_price,
                    t.pnl, t.exit_comment.c_str(), t.exit_id.c_str());
    }
}

// exit_tag: "Margin call" rows carry it as exit_comment; a bracket fill
// carries the strategy.exit id in exit_id and an empty comment; a
// strategy.close_all fill carries neither.
void check_trade(const Probe& p, int i, bool is_long, int entry_bar,
                 double entry_price, double qty, int exit_bar,
                 double exit_price, const char* exit_tag, double pnl) {
    CHECK(i < p.trade_count());
    if (i >= p.trade_count()) return;
    const Trade& t = p.get_trade(i);
    CHECK(t.is_long == is_long);
    CHECK(t.entry_bar_index == entry_bar);
    CHECK_NEAR(t.entry_price, entry_price, 1e-9);
    CHECK_NEAR(t.qty, qty, 1e-9);
    CHECK(t.exit_bar_index == exit_bar);
    CHECK_NEAR(t.exit_price, exit_price, 1e-9);
    const std::string tag(exit_tag);
    if (tag == "Margin call") {
        CHECK(t.exit_comment == "Margin call");
    } else if (!tag.empty()) {
        CHECK(t.exit_id == tag);
    } else {
        CHECK(t.exit_comment.empty());
    }
    CHECK_NEAR(t.pnl, pnl, 5e-3);
}

// ---------------------------------------------------------------------------
// A. M1 — aapl15-mcopen-willow: fixed 5457 short from the 04-21 17:45Z signal
// (fill 18:00Z @190.52, capital 1,039,850.98 = willowsportz's exact state).
// Slices 12 @190.60 (entry bar), 36 @190.91, 156 @192.09; the 04-22 13:30Z
// open prints 196.135 -> P = 196.14: x = 103.26 -> 412 (the raw open gives
// 102.999 -> 408, the engine's row); 676 @206.00 on 04-23; close_all 4165
// @203.81. TV's six rows.
// ---------------------------------------------------------------------------
void test_willow_half_tick_open_slice_412() {
    std::printf("-- A. willow 04-22 13:30Z: open slice marked at tick(196.135) = 196.14 -> 412 --\n");
    Probe p(1039850.98, 5457.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_default("S", false);
        if (bar == 45) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kAaplWillow);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 6);
    CHECK(p.margin_call_rows() == 5);
    check_trade(p, 0, false, 1, 190.52, 12.0, 1, 190.60, "Margin call", -0.96);
    check_trade(p, 1, false, 1, 190.52, 36.0, 2, 190.91, "Margin call", -14.04);
    check_trade(p, 2, false, 1, 190.52, 156.0, 7, 192.09, "Margin call", -244.92);
    check_trade(p, 3, false, 1, 190.52, 412.0, 9, 196.14, "Margin call", -2315.44);
    check_trade(p, 4, false, 1, 190.52, 676.0, 35, 206.00, "Margin call", -10464.48);
    check_trade(p, 5, false, 1, 190.52, 4165.0, 46, 203.81, "", -55352.85);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// B. M1 — algoai 06-20 13:30Z (probe rows TV#73/74): a 3867-share short
// carried into the half-tick open 198.235 with a 'Short Exit' stop at 200.00.
// Capital 770,950 puts the on-tick mark at x = 16.07 (-> 64) and the raw mark
// at x = 15.87 (-> 60, the engine's row); the stop then closes the 3803
// survivor at its level on the same bar (the extreme 200.94 comes after it on
// the O-L-H-C path: no second slice).
// ---------------------------------------------------------------------------
void test_algoai_0620_half_tick_open_slice_64_then_stop() {
    std::printf("-- B. algoai 06-20 13:30Z: 64 @198.24 then 'Short Exit' 3803 @200.00 --\n");
    Probe p(770950.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 1) {
            e.entry_market("S", false, 3867.0);
            e.exit_stop("Short Exit", "S", 200.0);
        }
    };
    std::vector<Bar> bars = to_bars(kAaplAlgoai0620);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 2);
    CHECK(p.margin_call_rows() == 1);
    check_trade(p, 0, false, 2, 196.29, 64.0, 3, 198.24, "Margin call", -124.80);
    check_trade(p, 1, false, 2, 196.29, 3803.0, 3, 200.00, "Short Exit", -14109.13);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// E. M2 control — aapl15-mcext-stop-scalper-b: fixed 5012 short from the
// 07-17 19:30Z signal (fill 19:45Z @210.71, capital 1,056,333.80), stop
// 212.83, NO reversal: 1 @210.75 (entry bar), 20 @210.87 (07-18 open), 108
// @211.76 (16:30Z high), then the stop fills at its level 212.83 x4883 on
// 07-21 with no slice (the stop precedes the extreme on the path).
// ---------------------------------------------------------------------------
void test_scalper_b_control_stop_at_level_no_slice() {
    std::printf("-- E. scalper-b control: 1 / 20 / 108 slices, then 'X' 4883 @212.83, no 07-21 slice --\n");
    Probe p(1056333.80, 5012.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 1) {
            e.entry_default("S", false);
            e.exit_stop("X", "S", 212.83);
        }
    };
    std::vector<Bar> bars = to_bars(kAaplScalper);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 4);
    CHECK(p.margin_call_rows() == 3);
    check_trade(p, 0, false, 2, 210.71, 1.0, 2, 210.75, "Margin call", -0.04);
    check_trade(p, 1, false, 2, 210.71, 20.0, 3, 210.87, "Margin call", -3.20);
    check_trade(p, 2, false, 2, 210.71, 108.0, 15, 211.76, "Margin call", -113.40);
    check_trade(p, 3, false, 2, 210.71, 4883.0, 29, 212.83, "X", -10351.96);
    CHECK(p.flat());
}

}  // namespace

int main() {
    std::printf("--- aapl15_margin_brackets (round 7 family N) ---\n");
    test_willow_half_tick_open_slice_412();
    test_algoai_0620_half_tick_open_slice_64_then_stop();
    test_scalper_b_control_stop_at_level_no_slice();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
