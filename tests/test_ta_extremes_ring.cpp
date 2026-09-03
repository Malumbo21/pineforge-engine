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
 * survive in slots not rewritten, never-written slots are skipped, and the
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
 * Every-bar callers must stay byte-identical to the previous positional deque
 * in every context; the deque is reproduced here as the reference.
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

// --- The previous implementation, kept verbatim as the every-bar reference ---

struct RefHighest {
    std::deque<double> buffer;
    int length;
    explicit RefHighest(int len) : length(len) {}
    double compute(double src) {
        buffer.push_back(src);
        while ((int)buffer.size() > length) buffer.pop_front();
        if ((int)buffer.size() < length) return na<double>();
        double hi = na<double>();
        for (int i = 0; i < (int)buffer.size(); i++)
            if (!is_na(buffer[i]) && (is_na(hi) || buffer[i] > hi)) hi = buffer[i];
        return hi;
    }
    double recompute(double src) {
        if (buffer.empty()) return compute(src);
        buffer.back() = src;
        if ((int)buffer.size() < length) return na<double>();
        double hi = na<double>();
        for (int i = 0; i < (int)buffer.size(); i++)
            if (!is_na(buffer[i]) && (is_na(hi) || buffer[i] > hi)) hi = buffer[i];
        return hi;
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
        double lo = na<double>();
        for (int i = 0; i < (int)buffer.size(); i++)
            if (!is_na(buffer[i]) && (is_na(lo) || buffer[i] < lo)) lo = buffer[i];
        return lo;
    }
    double recompute(double src) {
        if (buffer.empty()) return compute(src);
        buffer.back() = src;
        if ((int)buffer.size() < length) return na<double>();
        double lo = na<double>();
        for (int i = 0; i < (int)buffer.size(); i++)
            if (!is_na(buffer[i]) && (is_na(lo) || buffer[i] < lo)) lo = buffer[i];
        return lo;
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

    // na input: recorded as a positional gap (skipped by the extremum) and
    // answers na on that bar; the offset then points at the real bar. The
    // deque skipped na without advancing, so {9, na, 7, 2} at length 3
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
            // Highest / Lowest: positional deque incl. na gaps -> identical
            // with gaps planted every 7th bar.
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

int main() {
    test_cadence7_pin();
    test_cadence9_pin();
    test_cadence_pin_lowest_mirror();
    test_na_until_bar_length_minus_1();
    test_bars_variants_ring();
    test_every_bar_identity();
    test_htf_context_sanity();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
