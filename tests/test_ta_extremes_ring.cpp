/*
 * test_ta_extremes_ring.cpp — TradingView's bar-addressed ring for the window
 * extremes (ta.highest / ta.lowest / ta.highestbars / ta.lowestbars) and the
 * per-context bar index the engine installs for it (ta::bar_context()).
 *
 * The rule (pinned 2026-09-03 with `lab tv` on NYSE:F 1D, range 2025-04-01..
 * 2026-05-01, tapes scratchpad/r5/pins/out-pin-ring-highest{,-c9}): a window
 * call site owns K = length + 1 slots addressed by bar_index % K; a call on
 * bar b writes slot[b % K] = src and returns the extremum over the WRITTEN
 * slots (b - k) % K, k in [0, length). Stale values from earlier executions
 * survive in slots not rewritten, never-written slots read as 0 (pinned
 * 2026-09-04 on BTCUSDT 1D at cadence 13), and the
 * result is na iff bar_index < length - 1 — NOT until `length` executions.
 *
 *   pin-ring-highest.pine:  if bar_index % 7 == 3
 *                               v = ta.highest(high, 5)
 *                               strategy.entry("L", strategy.long, qty=math.round(v*100))
 *   pin-ring-highest-c9.pine: the same with `% 9`.
 *
 * TV's entry sizes are round(v*100) (na -> default qty 1), so the tapes pin v
 * at every execution bar: cadence 7 (bars 3,10,17,...) -> na, 9.73, 10.10,
 * 10.62, 10.78 x5, 11.85 ...; cadence 9 (bars 3,12,21,...) -> na, 9.73, 10.32,
 * 10.70, 10.70, 10.65, 10.87, 11.85 ... (call 6 = 10.65: a per-call buffer
 * would still hold 10.70 — only the bar-addressed ring drops it).
 *
 * Every-bar callers must stay byte-identical to the positional deque in every
 * context; the deque is reproduced here as the reference, with the na rule
 * re-pinned (2026-09-04, NYSE:F 1D, every-bar ta.lowest / ta.highest over a
 * source na on every 4th bar): an na source answers na on its own bar
 * (11/11) AND poisons the window -- the answer on the bars after it is the
 * extremum over the members newer than the na (20/20 entries: lowest ==
 * highest on the bar after each gap), where the old deque skipped the na and
 * answered the window's other members. The same written-na poison governs
 * conditional callers' rescans and their cache (log-20260904t112527z-ea13dfcd;
 * the tape replays at the end of this file).
 *
 * NDEBUG-PROOF: self-rolled CHECK with a failure counter; main() returns
 * nonzero on any failure.
 */

#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

// Bitwise-equal-or-both-na: the identity the every-bar proofs demand.
static bool same(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return a == b;
}

// NYSE:F 1D highs, bars 0..89 of the pinned chart range (bar 0 = 2025-04-01),
// from the campaign feed (`lab bars NYSE:F 1D`). Bar 3 = 9.73, bar 10 = 9.63.
static const double kHighs[90] = {
    10.17, 10.27, 10.2, 9.73, 9.64, 9.52, 9.54, 9.28, 9.35, 9.81, 9.63, 9.62,
    9.69, 9.63, 9.72, 10.0054, 10.09, 10.1, 10.18, 10.19, 10.13, 10.32, 10.39,
    10.24, 10.62, 10.505, 10.464, 10.49, 10.73, 10.63, 10.7, 10.78, 10.81,
    10.785, 10.84, 10.69, 10.5, 10.41, 10.4897, 10.335, 10.28, 10.46, 10.28,
    10.225, 10.365, 10.25, 10.35, 10.45, 10.65, 10.77, 10.59, 10.51, 10.67,
    10.63, 10.53, 10.6, 10.75, 10.87, 10.765, 10.65, 10.92, 10.86, 11.38,
    11.78, 11.96, 11.79, 11.85, 11.9, 11.97, 11.86, 11.895, 11.93, 11.45,
    11.3465, 11.24, 11.45, 11.29, 11.46, 11.429, 11.49, 11.49, 11.29, 11.125,
    11.15, 10.92, 11.02, 11.1, 11.26, 11.385, 11.37};
static const int kNumHighs = 90;

// TV "Size (qty)" column of the pinned tapes at each execution bar
// (round(v*100); 1 = na -> default qty). The first 13 / 10 entries fall
// inside the 90-bar window above.
static const int kTvSizesCadence7[13] = {1, 973, 1010, 1062, 1078, 1078, 1078,
                                         1078, 1078, 1185, 1185, 1185, 1185};
static const int kTvSizesCadence9[10] = {1, 973, 1032, 1070, 1070, 1065, 1087,
                                         1185, 1185, 1145};

static int tv_size(double v) {
    return is_na(v) ? 1 : static_cast<int>(std::llround(v * 100.0));
}

// --- The previous implementation, kept as the every-bar reference ---

// The positional window's extremum under the written-na poison (pinned
// 2026-09-04, NYSE:F 1D pin-lowest-na-everybar): an na member resets the
// running extremum, so the result is the extremum over the members newer than
// the newest na (na when there is none). The pre-poison deque took the
// extremum over ALL non-na members.
static double ref_extremum(const std::deque<double>& buffer, bool want_max) {
    double best = na<double>();
    for (double v : buffer) {
        if (is_na(v)) { best = na<double>(); continue; }
        if (is_na(best) || (want_max ? v > best : v < best)) best = v;
    }
    return best;
}

struct RefHighest {
    std::deque<double> buffer;
    int length;
    explicit RefHighest(int len) : length(len) {}
    double compute(double src) {
        buffer.push_back(src);
        while ((int)buffer.size() > length) buffer.pop_front();
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();   // pinned 2026-09-04 (NYSE:F 1D): an na source answers na
        return ref_extremum(buffer, true);
    }
    double recompute(double src) {
        if (buffer.empty()) return compute(src);
        buffer.back() = src;
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();
        return ref_extremum(buffer, true);
    }
};

struct RefLowest {
    std::deque<double> buffer;
    int length;
    explicit RefLowest(int len) : length(len) {}
    double compute(double src) {
        buffer.push_back(src);
        while ((int)buffer.size() > length) buffer.pop_front();
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();   // pinned 2026-09-04 (NYSE:F 1D): an na source answers na
        return ref_extremum(buffer, false);
    }
    double recompute(double src) {
        if (buffer.empty()) return compute(src);
        buffer.back() = src;
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();
        return ref_extremum(buffer, false);
    }
};

struct RefHighestBars {
    int length_;
    std::deque<double> buffer_;
    explicit RefHighestBars(int len) : length_(len) {}
    double compute(double src) {
        if (is_na(src)) return na<double>();
        buffer_.push_back(src);
        while ((int)buffer_.size() > length_) buffer_.pop_front();
        if ((int)buffer_.size() < length_) return na<double>();
        int max_idx = 0;
        double max_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] > max_val) { max_val = buffer_[i]; max_idx = i; }
        return (double)(max_idx - ((int)buffer_.size() - 1));
    }
    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (is_na(src)) return na<double>();
        if ((int)buffer_.size() < length_) return na<double>();
        int max_idx = 0;
        double max_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] > max_val) { max_val = buffer_[i]; max_idx = i; }
        return (double)(max_idx - ((int)buffer_.size() - 1));
    }
};

struct RefLowestBars {
    int length_;
    std::deque<double> buffer_;
    explicit RefLowestBars(int len) : length_(len) {}
    double compute(double src) {
        if (is_na(src)) return na<double>();
        buffer_.push_back(src);
        while ((int)buffer_.size() > length_) buffer_.pop_front();
        if ((int)buffer_.size() < length_) return na<double>();
        int min_idx = 0;
        double min_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] < min_val) { min_val = buffer_[i]; min_idx = i; }
        return (double)(min_idx - ((int)buffer_.size() - 1));
    }
    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (is_na(src)) return na<double>();
        if ((int)buffer_.size() < length_) return na<double>();
        int min_idx = 0;
        double min_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] < min_val) { min_val = buffer_[i]; min_idx = i; }
        return (double)(min_idx - ((int)buffer_.size() - 1));
    }
};

// Deterministic LCG series; `na_every` > 0 plants an na gap every that many
// bars (0 = gap-free).
static std::vector<double> make_series(int n, unsigned seed, int na_every) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(n));
    unsigned x = seed;
    for (int i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        double v = 100.0 + static_cast<double>((x >> 8) % 100000) / 1000.0;
        if (na_every > 0 && (i % na_every) == na_every - 1) v = na<double>();
        out.push_back(v);
    }
    return out;
}

// --- Cadence pins: the two TV tapes, replayed bar by bar as the chart does ---

static void test_cadence7_pin() {
    std::printf("test_cadence7_pin\n");
    ta::Highest hi(5);
    int call = 0;
    for (int b = 0; b < kNumHighs; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 7 != 3) continue;
        double v = hi.compute(kHighs[b]);
        CHECK(call < 13);
        if (call < 13) CHECK(tv_size(v) == kTvSizesCadence7[call]);
        if (call == 0) CHECK(is_na(v));                 // bar 3 < length - 1 = 4
        if (call == 1) CHECK(near(v, 9.73));            // max{bar 3, bar 10}: 2 executions, not na
        if (call == 5) CHECK(near(v, 10.78));           // bar 31's 10.78 survives in slot 1
        ++call;
    }
    CHECK(call == 13);
    CHECK(!ta::bar_context().installed);   // every scope restored on exit
}

static void test_cadence9_pin() {
    std::printf("test_cadence9_pin\n");
    ta::Highest hi(5);
    int call = 0;
    for (int b = 0; b < kNumHighs; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 9 != 3) continue;
        double v = hi.compute(kHighs[b]);
        CHECK(call < 10);
        if (call < 10) CHECK(tv_size(v) == kTvSizesCadence9[call]);
        // Call 6 (bar 48, high 10.65): slots read are bars 48,47,46,45,44 ->
        // 48%6=0 (this call), 47%6=5, 46%6=4, 45%6=3, 44%6=2; 10.70 (bar 30,
        // slot 0) was just overwritten and 10.335 (bar 39, slot 3) is beaten,
        // so 10.65 — a per-call buffer would still answer 10.70.
        if (call == 5) CHECK(near(v, 10.65));
        ++call;
    }
    CHECK(call == 10);
}

// The same tapes with the cadence guard evaluated through the ring but the
// remaining bars also *seen* by the context (no call) — i.e. the guard is the
// only thing that differs from an every-bar caller, exactly as in Pine.
static void test_cadence_pin_lowest_mirror() {
    std::printf("test_cadence_pin_lowest_mirror\n");
    for (int cadence : {7, 9}) {
        ta::Highest hi(5);
        ta::Lowest lo(5);
        for (int b = 0; b < kNumHighs; ++b) {
            ta::BarContextScope scope(b, 0);
            if (b % cadence != 3) continue;
            double h = hi.compute(kHighs[b]);
            double l = lo.compute(-kHighs[b]);
            CHECK(same(l, is_na(h) ? h : -h));   // lowest(-x) == -highest(x)
        }
    }
}

// --- na iff bar_index < length - 1 (never "until length executions") ---

static void test_na_until_bar_length_minus_1() {
    std::printf("test_na_until_bar_length_minus_1\n");
    // A caller executing on bars 0..3 of a length-5 window: na on every one,
    // then bar 4 answers from the written slots even though bar 4 is only
    // the 2nd execution of the second object below.
    {
        ta::Highest every(5);
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            CHECK(is_na(every.compute(1.0 + b)));
        }
        ta::BarContextScope scope(4, 0);
        CHECK(near(every.compute(0.5), 4.0));
    }
    {
        ta::Highest sparse(5);
        { ta::BarContextScope scope(2, 0); CHECK(is_na(sparse.compute(7.0))); }
        { ta::BarContextScope scope(4, 0); CHECK(near(sparse.compute(3.0), 7.0)); }  // max{bar 2, bar 4}
        { ta::BarContextScope scope(5, 0); CHECK(near(sparse.compute(2.0), 7.0)); }  // bar 2 still in slot 2
    }
    // Slot walk: K = 6, bar 8 reads slots 8%6=2 (this call, 1.0), 7%6=1
    // (never), 6%6=0 (never), 5%6=5 (bar 5, 2.0), 4%6=4 (bar 4, 3.0). Bar 2
    // wrote slot 2 — overwritten by bar 8 — so the answer is 3.0, not 7.0.
    {
        ta::Highest sparse(5);
        { ta::BarContextScope scope(2, 0); sparse.compute(7.0); }
        { ta::BarContextScope scope(4, 0); sparse.compute(3.0); }
        { ta::BarContextScope scope(5, 0); sparse.compute(2.0); }
        { ta::BarContextScope scope(8, 0); CHECK(near(sparse.compute(1.0), 3.0)); }
    }
    // Origin: a feed whose first bar is Pine bar 7 warms up over ITS first
    // `length` bars (identical to the deque), not TV's bar_index rule alone.
    {
        ta::Highest hi(3);
        { ta::BarContextScope scope(7, 7); CHECK(is_na(hi.compute(1.0))); }
        { ta::BarContextScope scope(8, 7); CHECK(is_na(hi.compute(2.0))); }
        { ta::BarContextScope scope(9, 7); CHECK(near(hi.compute(0.5), 2.0)); }
    }
    // length <= 0 stays na forever (compat with the deque's empty window).
    {
        ta::Highest zero(0);
        ta::Lowest neg(-2);
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            CHECK(is_na(zero.compute(1.0)));
            CHECK(is_na(neg.compute(1.0)));
        }
    }
}

// --- The *Bars variants: bars back = k of the extremum slot ---

static void test_bars_variants_ring() {
    std::printf("test_bars_variants_ring\n");
    // Cadence 7, length 5, K = 6, over the pinned highs. Derived from the rule
    // (no TV tape for highestbars): offsets are slot distances, so bar 3's
    // stale 9.73 sits 1 slot behind bar 10 and beats it; bar 31's 10.78 walks
    // back 1,2,3,4 slots across bars 38..59 and bar 66's 11.85 resets to 0.
    static const double kExpect[10] = {0, -1, 0, 0, 0, -1, -2, -3, -4, 0};
    ta::HighestBars hb(5);
    ta::LowestBars lb(5);
    int call = 0;
    for (int b = 0; b < 70; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 7 != 3) continue;
        double v = hb.compute(kHighs[b]);
        double w = lb.compute(-kHighs[b]);
        if (call == 0) {
            CHECK(is_na(v));
            CHECK(is_na(w));
        } else {
            CHECK(near(v, kExpect[call]));
            CHECK(near(w, kExpect[call]));   // mirror: lowestbars(-x) == highestbars(x)
        }
        ++call;
    }
    CHECK(call == 10);

    // Fresh-window tie rule: the OLDEST equal extremum wins (as the deque scan
    // did), in both context and standalone mode.
    {
        ta::HighestBars t(4);
        ta::LowestBars u(4);
        double xs[4] = {5.0, 9.0, 9.0, 1.0};
        double ys[4] = {5.0, 1.0, 1.0, 9.0};
        double v = na<double>(), w = na<double>();
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            v = t.compute(xs[b]);
            w = u.compute(ys[b]);
        }
        CHECK(near(v, -2.0));
        CHECK(near(w, -2.0));
        ta::HighestBars t2(4);
        for (int b = 0; b < 4; ++b) v = t2.compute(xs[b]);
        CHECK(near(v, -2.0));
    }

    // na input: written as a positional member (it poisons the older ones)
    // and answers na on that bar; the offset then points at the real bar.
    // The deque skipped na without advancing, so {9, na, 7, 2} at length 3
    // answered -2 (the 9, actually 3 bars back) — TV's series is positional.
    {
        ta::HighestBars t(3);
        double v = na<double>();
        double xs[4] = {9.0, na<double>(), 7.0, 2.0};
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            v = t.compute(xs[b]);
            if (b == 1) CHECK(is_na(v));
        }
        CHECK(near(v, -1.0));   // window {na, 7, 2} -> 7, one bar back
    }
}

// --- Every-bar callers: byte-identical to the deque, in every mode ---

// mode 0: no context installed (standalone / unit-test cadence)
// mode 1: chart context, bars 0..n-1, origin 0
// mode 2: chart context after a bar_index_offset: bars 1000.., origin 1000
// Every third bar also issues a recompute() with a different value (bar
// magnifier / lookahead partial ticks) before moving on.
template <class Ring, class Ref>
static void every_bar_identity(int length, int mode, int na_every, unsigned seed) {
    const int n = 257;
    std::vector<double> xs = make_series(n, seed, na_every);
    std::vector<double> ys = make_series(n, seed ^ 0x9e3779b9u, na_every);
    Ring ring(length);
    Ref ref(length);
    for (int b = 0; b < n; ++b) {
        long long bar = mode == 2 ? 1000 + b : b;
        long long origin = mode == 2 ? 1000 : 0;
        if (mode == 0) {
            CHECK(same(ring.compute(xs[b]), ref.compute(xs[b])));
            if (b % 3 == 2) CHECK(same(ring.recompute(ys[b]), ref.recompute(ys[b])));
        } else {
            ta::BarContextScope scope(bar, origin);
            CHECK(same(ring.compute(xs[b]), ref.compute(xs[b])));
            if (b % 3 == 2) CHECK(same(ring.recompute(ys[b]), ref.recompute(ys[b])));
        }
    }
}

static void test_every_bar_identity() {
    std::printf("test_every_bar_identity\n");
    for (int length : {1, 2, 3, 5, 8, 13, 50}) {
        for (int mode = 0; mode < 3; ++mode) {
            // Highest / Lowest: positional deque incl. na gaps (under the
            // written-na poison) -> identical with gaps planted every 7th bar.
            every_bar_identity<ta::Highest, RefHighest>(length, mode, 7, 17u + length);
            every_bar_identity<ta::Lowest, RefLowest>(length, mode, 7, 29u + length);
            every_bar_identity<ta::Highest, RefHighest>(length, mode, 0, 3u + length);
            every_bar_identity<ta::Lowest, RefLowest>(length, mode, 0, 5u + length);
            // *Bars: identical on gap-free series (the deque's na rule was
            // non-positional; see test_bars_variants_ring).
            every_bar_identity<ta::HighestBars, RefHighestBars>(length, mode, 0, 7u + length);
            every_bar_identity<ta::LowestBars, RefLowestBars>(length, mode, 0, 11u + length);
        }
    }
    // recompute() before the first compute() behaves as the first bar (the
    // pristine rule the recompute suite pins), with and without a context.
    {
        ta::Highest a(3);
        RefHighest r(3);
        CHECK(same(a.recompute(2.0), r.recompute(2.0)));
        CHECK(same(a.compute(5.0), r.compute(5.0)));
        CHECK(same(a.compute(1.0), r.compute(1.0)));
        ta::HighestBars hb(2);
        CHECK(is_na(hb.recompute(1.0)));
        ta::BarContextScope scope(0, 0);
        ta::Lowest c(2);
        CHECK(is_na(c.recompute(4.0)));
    }
}

// --- request.security context: its own index, nested inside the chart's ---

// Models the engine's dispatch: the chart scope wraps on_bar with the Pine
// bar_index; each security evaluation is dispatched under the requested
// context's evaluated-bar index (complete: eval_complete_count - 1, partial:
// eval_complete_count). An HTF of ratio 4 with lookahead partial ticks: the
// every-bar HTF caller must equal the deque fed one value per HTF bar, the
// partial recomputes must not advance it, and the chart's own ring must be
// unaffected by the nested scope.
static void test_htf_context_sanity() {
    std::printf("test_htf_context_sanity\n");
    const int ratio = 4;
    const int n = 200;
    std::vector<double> chart = make_series(n, 99u, 0);
    ta::Highest chart_hi(5);
    RefHighest chart_ref(5);
    ta::Highest htf_hi(3);
    RefHighest htf_ref(3);
    ta::Highest htf_cond(3);   // conditional HTF caller: executes on even HTF bars only
    long long eval_complete = 0;
    double bucket_high = na<double>();
    int sub = 0;
    for (int b = 0; b < n; ++b) {
        ta::BarContextScope chart_scope(b, 0);
        // --- security feed for this chart bar (engine: before on_bar) ---
        bucket_high = is_na(bucket_high) ? chart[b] : std::fmax(bucket_high, chart[b]);
        ++sub;
        const bool complete = (sub == ratio);
        if (complete) ++eval_complete;
        const long long sec_bar = complete ? eval_complete - 1 : eval_complete;
        {
            ta::BarContextScope sec_scope(sec_bar, 0);
            CHECK(ta::bar_context().bar_index == sec_bar);
            // codegen: compute on the bucket's first tick, recompute after.
            if (sub == 1) {
                htf_hi.compute(bucket_high);
                htf_ref.compute(bucket_high);
            } else {
                double v = htf_hi.recompute(bucket_high);
                CHECK(same(v, htf_ref.recompute(bucket_high)));
            }
            if (complete && (sec_bar % 2 == 0)) {
                double v = htf_cond.recompute(bucket_high);
                // Conditional HTF caller, K = 4: HTF bar 2 reads slots 2,1,0
                // -> bars 2 and 0 written -> max of the two even buckets so
                // far; never na past HTF bar 1.
                if (sec_bar >= 2) CHECK(!is_na(v));
                if (sec_bar == 0) CHECK(is_na(v));
            }
        }
        CHECK(ta::bar_context().installed && ta::bar_context().bar_index == b);
        if (complete) {
            sub = 0;
            bucket_high = na<double>();
        }
        // --- chart on_bar ---
        CHECK(same(chart_hi.compute(chart[b]), chart_ref.compute(chart[b])));
    }
    CHECK(!ta::bar_context().installed);
}


// --- Cached extremum with an implied bar (pinned 2026-09-04: 8 NYSE:F 1D
// alias tapes 696/696, BINANCE:ETHUSDT.P 15 geometry pin 82/82, jayentriken
// BBWP ETH replay 593/593) ------------------------------------------------

// t382 geometry (`m = bar_index % 49; exec = m < 4 or 37 <= m < 47 or m == 48`,
// ta.lowest(low, 10)): bar 3's stale 9.20 aliases one slot behind bar 48, yet
// TV answers the cached 9.88 (from bar 42) on bars 48..51 and only rescans to
// 9.20 on bar 52, once the cache has aged `length` bars.
static void test_cached_extremum_keeps_over_aliased_slot_until_expiry() {
    std::printf("test_cached_extremum_keeps_over_aliased_slot_until_expiry\n");
    ta::Lowest lo(10);
    auto low_at = [](int b) {
        if (b == 3) return 9.20;
        if (b < 4) return 9.50;
        if (b == 42) return 9.88;
        if (b >= 37 && b <= 46) return 10.00;
        return 10.50;                       // bars 48..52
    };
    auto executes = [](int b) {
        const int m = b % 49;
        return m < 4 || (m >= 37 && m < 47) || m == 48 || (b >= 49 && b <= 52);
    };
    for (int b = 0; b <= 52; ++b) {
        ta::BarContextScope scope(b, 0);
        if (!executes(b)) continue;
        const double v = lo.compute(low_at(b));
        if (b < 9) CHECK(is_na(v));                       // bar_index < length - 1
        if (b == 46) CHECK(near(v, 9.88));                // fresh window minimum, bar 42
        if (b >= 48 && b <= 51) CHECK(near(v, 9.88));     // cache (9.88, 42) younger than 10 bars: slot 3 unread
        if (b == 52) CHECK(near(v, 9.20));                // aged out: rescan reads the aliased slot
    }
}

// ETH #88 geometry (ta.lowest(low, 10) inside `if strategy.position_size > 0`):
// a 17-bar run whose 7th bar leaves 2641.73 in the slot that aliases k=6
// behind the next run's first bar, then a 50-bar gap. TV answers the run-start
// bar's own low 2648.62 because it beats the expired cache (2650.47) — the
// new-extremum test comes before the rescan. The control run-start (r17g50 @
// bar 67: own low above the cache) rescans and reads the aliased slot.
static void run_eth88_geometry(double run_start_low, double expected) {
    ta::Lowest lo(10);
    for (int b = 0; b <= 87; ++b) {
        ta::BarContextScope scope(b, 0);
        const bool in_run = b >= 20 && b <= 36;
        if (!in_run && b != 87) continue;
        double src;
        if (b == 26) src = 2641.73;                       // the 7th bar of the run: slot 26 % 11 = 4
        else if (in_run) src = 2650.47 + (b % 3) * 0.5;   // minimum 2650.47 at bars 21, 24, ...
        else src = run_start_low;                          // bar 87: 87 - 6 = 81, 81 % 11 = 4 -> the aliased slot
        const double v = lo.compute(src);
        if (b == 35) CHECK(near(v, 2641.73));             // cache (2641.73, 26) still 9 bars young
        if (b == 36) CHECK(near(v, 2650.47));             // aged out at 10 bars: rescan of 27..36
        if (b == 87) CHECK(near(v, expected));
    }
}

static void test_new_extremum_precedes_expiry_rescan() {
    std::printf("test_new_extremum_precedes_expiry_rescan\n");
    run_eth88_geometry(2648.62, 2648.62);   // ETH #88: below the expired cache -> own low, no rescan
    run_eth88_geometry(2652.00, 2641.73);   // r17g50 @67: above the cache -> rescan reads the aliased slot
    CHECK(!ta::bar_context().installed);
}


// --- Never-written slots read as 0 (pinned 2026-09-04, BINANCE:BTCUSDT 1D,
// `if bar_index % 13 == 5`, K = 11): ta.lowest(low, 10) answers na on the
// first call (bar 5 < 9), 0 on calls 2..10 while a never-written slot sits
// in the scanned window, then the stale positive minimum from call 11 on
// once every residue has been written (TV: 77.3k, 84.5k); ta.highest(-close,
// 10) mirrors it (0, then negative); ta.highest(high, 10) is unaffected. ----
static void test_never_written_slots_read_as_zero() {
    std::printf("test_never_written_slots_read_as_zero\n");
    ta::Lowest lo(10);
    ta::Highest hi(10);
    ta::Highest hneg(10);
    int call = 0;
    for (int b = 0; b <= 200; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 13 != 5) continue;
        ++call;
        const double price = 100000.0 + b;         // rising, so every call is a fresh high, never a new low
        const double l = lo.compute(price);
        const double h = hi.compute(price);
        const double n = hneg.compute(-price);
        if (call == 1) { CHECK(is_na(l)); CHECK(is_na(h)); CHECK(is_na(n)); continue; }
        CHECK(near(h, price));                     // a rising series: the current high wins
        if (call <= 10) {                          // residues 5,7,9,0,2,4,6,8,10,1 written so far: a slot in the window is still unwritten
            CHECK(near(l, 0.0));
            CHECK(near(n, 0.0));
        } else {                                   // call 11 writes residue 3: every slot written, stale values return
            CHECK(!is_na(l) && l > 0.0);
            CHECK(!is_na(n) && n < 0.0);
        }
    }
    CHECK(call == 16);
}



// --- A written na POISONS the ring (pinned 2026-09-04, ledger
// log-20260904t112527z-ea13dfcd; tapes scratchpad/r6/pins/ringkind/{out-1,
// out-nafirst,out-mlsite} on OANDA:XAUUSD 1D and r6/pins/out-pin-lowest-na-
// everybar on NYSE:F 1D). In the oldest-first rescan a slot written with na
// resets the running extremum and the next (newer) slot restarts it, so the
// answer is the extremum over the slots NEWER than the newest na slot in the
// window (never-written 0 slots among them included), na when the newest slot
// is itself na; the na write also poisons the cache to (na, b), so the next
// valid call restarts from its own src without reading the slots. ----------

// Every-bar, mid-stream gaps: NYSE:F 1D lows, bars 0..41 of the pinned range
// (bar 0 = 2025-04-01; `lab bars NYSE:F 1D`), `x = bar_index % 4 == 0 ? na :
// low`, v = ta.lowest(x, 5), h = ta.highest(x, 5), entries on odd bars with
// qty = na ? 7 : 1 + round(v * 100). TV: v == h on every bar after an na bar
// (5, 9, 13, ...: the bar's own low, a single live member) -- the old "skip
// the gap" reading kept the pre-gap extremum (bar 9: 8.44 from bar 6; TV
// 9.20), and the old "leave the cache alone" na rule would have kept it too
// (the cache was only 3 bars old). 20/20 entries.
static const double kFordLows[42] = {
    9.81, 9.82, 9.53, 9.2, 9.0, 8.55, 8.4406, 8.88, 9.04, 9.2, 9.38, 9.29,
    9.405, 9.35, 9.53, 9.71, 9.815, 9.97, 9.9701, 9.925, 9.86, 10.02, 10.215,
    10.095, 10.055, 10.12, 10.26, 10.35, 10.48, 10.42, 10.48, 10.55, 10.64,
    10.63, 10.66, 10.45, 10.34, 10.29, 10.21, 10.15, 10.11, 10.12};
// TV "Size (qty)" of the V / H entries signalled on bars 1, 3, ..., 39.
static const int kFordTapeV[20] = {7, 7, 856, 845, 921, 921, 936, 936, 998, 994,
                                   1003, 1003, 1013, 1013, 1043, 1043, 1064, 1046, 1030, 1016};
static const int kFordTapeH[20] = {7, 7, 856, 889, 921, 939, 936, 972, 998, 998,
                                   1003, 1023, 1013, 1036, 1043, 1056, 1064, 1067, 1030, 1030};

static bool matches_qty(double v, int qty) {
    if (qty == 7) return is_na(v);
    if (is_na(v)) return false;
    return std::fabs(v - (qty - 1) / 100.0) <= 0.0051;   // round(v * 100) resolution
}

static void test_everybar_na_gap_poisons_the_window() {
    std::printf("test_everybar_na_gap_poisons_the_window\n");
    for (int mode = 0; mode < 2; ++mode) {          // 0: standalone cadence, 1: chart context
        ta::Lowest lo(5);
        ta::Highest hi(5);
        int entry = 0;
        for (int b = 0; b < 42; ++b) {
            const double x = (b % 4 == 0) ? na<double>() : kFordLows[b];
            double v, h;
            if (mode == 0) {
                v = lo.compute(x);
                h = hi.compute(x);
            } else {
                ta::BarContextScope scope(b, 0);
                v = lo.compute(x);
                h = hi.compute(x);
            }
            if (b % 4 == 0) { CHECK(is_na(v)); CHECK(is_na(h)); }          // the na bar answers na
            if (b % 2 == 1 && b <= 39) {   // bar 41's signal has no next bar to fill on
                CHECK(entry < 20);
                if (entry < 20) {
                    CHECK(matches_qty(v, kFordTapeV[entry]));
                    CHECK(matches_qty(h, kFordTapeH[entry]));
                }
                ++entry;
            }
            if (b >= 4 && b % 4 == 1) CHECK(same(v, h));   // the bar after a gap: a single live member
            if (b == 9) { CHECK(near(v, 9.2)); CHECK(near(h, 9.2)); }   // not 8.4406 (bar 6, older than bar 8's na)
            if (b == 11) { CHECK(near(v, 9.2)); CHECK(near(h, 9.38)); } // bars 9..11 live
        }
        CHECK(entry == 20);
    }
    CHECK(!ta::bar_context().installed);
}

// Conditional, cadence 13 / length 10 (K = 11) on OANDA:XAUUSD 1D
// (2025-03-01..2025-11-01, bar 0 = 2025-03-03; tape r6/pins/ringkind/out-1):
// the zero-fill control ta.lowest(low, 10) -- na, 0 x 9 while a never-written
// slot sits in the window, then the stale minima 2880.3 / 3017.7 / 3210.1 --
// and the sign-mixed COMPUTED source o = sma(close, 5) - close, where the
// never-written 0 never wins because every live member is negative: the
// source kind does not change the rule (13/13 on low, close - 2000,
// sma(close, 5), low[1], hl2 and o; only C and o are replayed here).
static const int kXauCallBars[13] = {5, 18, 31, 44, 57, 70, 83, 96, 109, 122, 135, 148, 161};
static const double kXauLowAtCalls[13] = {
    2880.310, 3017.655, 3210.065, 3237.790, 3279.235, 3302.015, 3255.850,
    3319.695, 3345.005, 3325.280, 3626.030, 3734.440, 4140.735};
static const double kXauTvLowestLow[13] = {NAN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2880.3, 3017.7, 3210.1};
static const double kXauOscAtCalls[13] = {
    19.9530, -30.1400, -42.4510, -50.4460, -28.0200, 14.0790, 51.2330,
    -8.3920, -47.9130, -4.4120, -35.5470, -8.6510, -117.0400};
// TV: sign via qty 3/5/9 (neg/pos/zero) and |v| via 11 + round(|v| * 100).
static const double kXauTvLowestOsc[13] = {
    NAN, -30.14, -42.45, -50.45, -50.45, -50.45, -50.45, -50.45, -47.91,
    -50.45, -50.45, -50.45, -117.04};

static void test_cadence13_zero_fill_control_and_computed_source() {
    std::printf("test_cadence13_zero_fill_control_and_computed_source\n");
    ta::Lowest lo(10);
    ta::Lowest lo_osc(10);
    for (int i = 0; i < 13; ++i) {
        ta::BarContextScope scope(kXauCallBars[i], 0);
        const double v = lo.compute(kXauLowAtCalls[i]);
        const double w = lo_osc.compute(kXauOscAtCalls[i]);
        if (i == 0) { CHECK(is_na(v)); CHECK(is_na(w)); continue; }
        CHECK(!is_na(v) && near(v, kXauTvLowestLow[i], 0.051));
        CHECK(!is_na(w) && near(w, kXauTvLowestOsc[i], 0.006));
        if (i >= 1 && i <= 9) CHECK(near(v, 0.0));   // a never-written slot in the window reads 0
        if (i >= 1) CHECK(w < 0.0);                  // the 0 never beats a negative member
    }
}

// Leading-na sites at the same cadence (tape out-nafirst): the first 1 / 2 /
// 5 calls of ta.lowest(sma(close, N), 10) write na. Every zero run ends at
// bar 83 when the na slot re-enters at k = 1 (src is the only live member),
// and the stale first valid write returns from bar 109 on, once it is newer
// than the na slot -- poison 13/13 on all 7 sites (zero-fill 8-10/13, skip
// 7-10/13). F and G are the sign-mixed sma(close, N) - close twins.
static const double kNaFirstSrc[5][13] = {
    {NAN, 2997.9843, 3111.9871, 3316.0614, 3285.6300, 3329.3643, 3357.6014, 3327.4057, 3350.1189, 3355.1350, 3478.2407, 3689.8500, 3964.3786},        // A: sma 14
    {NAN, NAN, 3086.7670, 3254.7987, 3287.3295, 3303.4298, 3355.1255, 3333.7838, 3343.2762, 3343.9192, 3435.9715, 3649.7668, 3891.3310},              // B: sma 20
    {NAN, NAN, NAN, NAN, NAN, 3235.7817, 3301.0254, 3322.9830, 3332.4742, 3348.5481, 3376.0111, 3455.1191, 3593.3572},                                // E: sma 59
    {NAN, NAN, -143.0280, -79.3963, -7.6805, -19.1802, 80.9505, -13.8162, -30.3588, 5.2092, -190.3435, -110.0882, -316.4390},                         // F: sma 20 - close
    {NAN, NAN, NAN, NAN, -86.4224, -38.3341, 46.4009, -20.1672, -26.5463, 5.3768, -238.9141, -275.7978, -544.8904},                                   // G: sma 46 - close
};
static const double kNaFirstTv[5][13] = {
    {NAN, 2998.00, 0.00, 0.00, 0.00, 0.00, 3357.60, 3327.40, 2998.00, 2998.00, 2998.00, 2998.00, 3112.00},
    {NAN, NAN, 3086.80, 0.00, 0.00, 0.00, 3355.10, 3333.80, 3086.80, 3086.80, 3086.80, 3086.80, 3086.80},
    {NAN, NAN, NAN, NAN, NAN, 3235.80, 3301.00, 3323.00, 3332.50, 3348.50, 3376.00, 3235.80, 3235.80},
    {NAN, NAN, -143.03, -143.03, -143.03, -143.03, 80.95, -13.82, -30.36, -143.03, -190.34, -190.34, -316.44},
    {NAN, NAN, NAN, NAN, -86.42, -86.42, 46.40, -20.17, -26.55, 5.38, -238.91, -275.80, -544.89},
};
static const double kNaFirstTol[5] = {0.051, 0.051, 0.051, 0.0051, 0.0051};   // qty resolution 0.1 / 0.01

static void test_leading_na_site_poison_pin() {
    std::printf("test_leading_na_site_poison_pin\n");
    for (int s = 0; s < 5; ++s) {
        ta::Lowest lo(10);
        ta::LowestBars lb(10);
        for (int i = 0; i < 13; ++i) {
            ta::BarContextScope scope(kXauCallBars[i], 0);
            const double v = lo.compute(kNaFirstSrc[s][i]);
            const double k = lb.compute(kNaFirstSrc[s][i]);
            if (is_na(kNaFirstTv[s][i])) {
                CHECK(is_na(v));
                CHECK(is_na(k));
            } else {
                CHECK(!is_na(v) && near(v, kNaFirstTv[s][i], kNaFirstTol[s]));
                CHECK(!is_na(k));
            }
            if (s == 0) {
                // Site A, ta.lowest(sma(close, 14), 10): the bars-since of the
                // extremum among the non-poisoned slots (derived, no tape).
                if (i == 1) CHECK(near(k, 0.0));     // bar 18: restart on src (slot 6 behind it never read)
                if (i == 2) CHECK(near(k, -3.0));    // bar 31: never-written slot 6 = bar 28, the oldest 0 past bar 27's na
                if (i == 6) CHECK(near(k, 0.0));     // bar 83: the na slot at k = 1, src alone
                if (i == 8) CHECK(near(k, -3.0));    // bar 109: 2998 lives in slot 7 = bar 106
                if (i == 12) CHECK(near(k, -9.0));   // bar 161: 3112 in slot 9 = bar 152
            }
        }
    }
    // Site A, first three calls, spelled out: bar 5 writes na (cache (na, 5)),
    // bar 18 restarts on 2998 (a rescan would have read the never-written
    // slot 6 = bar 17 as 0 -- TV says 2998), bar 31 rescans: slots 0..4
    // (bars 22..26) never written -> 0, bar 27 = slot 5 -> na, poison; bar
    // 28 = slot 6 never written -> 0 restarts; 2998 (bar 29) and 3112 (bar
    // 31) do not beat it -> 0.
    {
        ta::Lowest lo(10);
        { ta::BarContextScope scope(5, 0);  CHECK(is_na(lo.compute(na<double>()))); }
        { ta::BarContextScope scope(18, 0); CHECK(near(lo.compute(2997.9843), 2997.9843)); }
        { ta::BarContextScope scope(31, 0); CHECK(near(lo.compute(3111.9871), 0.0)); }
    }
}

// Replica of the market-logic VFDO-S site (tape out-mlsite, OANDA:XAUUSD 1D
// 2025-04-01..2026-05-01): osc = resid / stdev(resid, 50) is na for its
// first ~100 bars; pivot-gated ta.lowest(osc, 11) on pivot-low bars and
// ta.highest(osc, 11) on pivot-high bars; qty = 100000 + round(v * 1000).
// 32 calls: 16 na reads (the poisoned cache), one stale aliased read (bar
// 240 on the highest ring, src 0.7236: the aged cache rescans bars 230..240;
// bar 227's 1.3252 sits in slot 227 % 12 = 11, which the window addresses as
// bar 239, k = 1, past the poison of the na written at bar 70 in slot 10) and
// one true 0 read (bar 273, highest ring: past the na slots the live members
// are two never-written 0s and src -0.6354, so 0). Poison 32/32; zero-fill
// 24/32; skip 25/32.
struct MlCall { int bar; bool low_ring; double osc; double tv; };
static const MlCall kMlCalls[32] = {
    {18, false, NAN, NAN},      {25, true, NAN, NAN},       {28, false, NAN, NAN},
    {35, true, NAN, NAN},       {41, false, NAN, NAN},      {45, true, NAN, NAN},
    {50, false, NAN, NAN},      {52, true, NAN, NAN},       {57, false, NAN, NAN},
    {67, true, NAN, NAN},       {70, false, NAN, NAN},      {74, true, NAN, NAN},
    {80, true, NAN, NAN},       {84, false, NAN, NAN},      {89, true, NAN, NAN},
    {96, false, NAN, NAN},
    {104, true, 1.4057, 1.406},  {147, false, 1.5218, 1.522}, {153, true, 1.1918, 1.192},
    {165, false, 0.9717, 0.972}, {168, true, 1.0785, 1.078},  {177, false, 0.8948, 0.895},
    {195, false, 4.2483, 4.248}, {198, true, 3.8822, 3.882},  {218, false, 1.1347, 1.135},
    {220, true, 1.8796, 1.880},  {227, false, 1.3252, 1.325}, {231, true, 1.4513, 1.451},
    {240, false, 0.7236, 1.325}, {241, true, 0.8939, 0.894},  {255, true, -1.0579, -1.058},
    {273, false, -0.6354, 0.000},
};

static void test_market_logic_replica_pin() {
    std::printf("test_market_logic_replica_pin\n");
    ta::Lowest lo(11);
    ta::Highest hi(11);
    int ok = 0;
    for (const MlCall& c : kMlCalls) {
        ta::BarContextScope scope(c.bar, 0);
        const double v = c.low_ring ? lo.compute(c.osc) : hi.compute(c.osc);
        const bool hit = is_na(c.tv) ? is_na(v) : (!is_na(v) && near(v, c.tv, 0.0007));
        CHECK(hit);
        ok += hit;
        if (c.bar == 240) CHECK(!is_na(v) && near(v, 1.3252, 1e-6));   // the aliased bar-227 slot, not src 0.7236
        if (c.bar == 273) CHECK(!is_na(v) && near(v, 0.0));            // a never-written slot beats every negative member
    }
    CHECK(ok == 32);
}

// The *Bars variants under poison (derived from the rule; no tape): the
// offset is the bars-since of the extremum among the non-poisoned members.
static void test_bars_variants_under_poison() {
    std::printf("test_bars_variants_under_poison\n");
    // Every bar, length 5: {9, 8, na, 7, 2, 1} -> bar 5's live members are
    // bars 3..5. highest = 7 at bar 3 (-2), lowest = 1 at bar 5 (0). The old
    // "skip the gap" reading answered 8 at bar 1 (-4) for the highest.
    {
        ta::HighestBars hb(5);
        ta::LowestBars lb(5);
        const double xs[6] = {9.0, 8.0, na<double>(), 7.0, 2.0, 1.0};
        double h = na<double>(), l = na<double>();
        for (int b = 0; b < 6; ++b) {
            ta::BarContextScope scope(b, 0);
            h = hb.compute(xs[b]);
            l = lb.compute(xs[b]);
            if (b == 2) { CHECK(is_na(h)); CHECK(is_na(l)); }
        }
        CHECK(near(h, -2.0));
        CHECK(near(l, 0.0));
    }
    // Same series through a Range (the composites share the ring): bar 5's
    // range is 7 - 1 = 6 over the live members, not 9 - 1 = 8.
    {
        ta::Range r(5);
        const double xs[6] = {9.0, 8.0, na<double>(), 7.0, 2.0, 1.0};
        double v = na<double>();
        for (int b = 0; b < 6; ++b) {
            ta::BarContextScope scope(b, 0);
            v = r.compute(xs[b]);
            if (b == 2) CHECK(is_na(v));
        }
        CHECK(near(v, 6.0));
    }
    // A leading-na series (the finding-331 stochRSI shape) is unchanged by
    // the poison: the first live bar is a single-member window.
    {
        ta::Stoch st(3);
        CHECK(is_na(st.compute(na<double>(), na<double>(), na<double>())));
        CHECK(is_na(st.compute(na<double>(), na<double>(), na<double>())));
        CHECK(is_na(st.compute(na<double>(), na<double>(), na<double>())));
        CHECK(is_na(st.compute(50.0, 50.0, 50.0)));            // hi == lo -> division by zero -> na
        CHECK(near(st.compute(60.0, 60.0, 60.0), 100.0));      // window {50, 60}: (60 - 50) / 10
    }
}

// An na source poisons the cache (re-pinned 2026-09-04: the OANDA:XAUUSD 15
// geometry `ta.lowest(bar_index % 13 == 6 ? na : z, 10)` with the inserted na
// calls from bar 100 answers na on them, 34/34 -- and every valid call after
// one answers its own src, which the previous "leave the cache alone" reading
// only reproduced for a falling series). With a RISING series the old rule
// rescanned (aged cache) and read the never-written / stale slots; the
// poisoned cache restarts on src instead.
static void test_na_source_poisons_the_cache() {
    std::printf("test_na_source_poisons_the_cache\n");
    for (int falling = 0; falling < 2; ++falling) {
        ta::Lowest lo(10);
        int checked = 0;
        for (int b = 0; b <= 200; ++b) {
            ta::BarContextScope scope(b, 0);
            const bool valid_call = b % 13 == 5;
            const bool na_call = b >= 100 && b % 13 == 6;
            if (!valid_call && !na_call) continue;
            if (na_call) {
                CHECK(is_na(lo.compute(na<double>())));
                continue;
            }
            const double src = falling ? 3000.0 - b : 3000.0 + b;
            const double v = lo.compute(src);
            if (b >= 122) { CHECK(near(v, src)); ++checked; }   // the na call 12 bars earlier poisoned the cache
        }
        CHECK(checked == 7);
    }
    CHECK(!ta::bar_context().installed);
}

int main() {
    test_cadence7_pin();
    test_cadence9_pin();
    test_cadence_pin_lowest_mirror();
    test_na_until_bar_length_minus_1();
    test_bars_variants_ring();
    test_every_bar_identity();
    test_htf_context_sanity();
    test_cached_extremum_keeps_over_aliased_slot_until_expiry();
    test_new_extremum_precedes_expiry_rescan();
    test_never_written_slots_read_as_zero();
    test_na_source_poisons_the_cache();
    test_everybar_na_gap_poisons_the_window();
    test_cadence13_zero_fill_control_and_computed_source();
    test_leading_na_site_poison_pin();
    test_market_logic_replica_pin();
    test_bars_variants_under_poison();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
