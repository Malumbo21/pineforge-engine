// Native higher-timeframe request.security feeds (strategy_set_native_security_feed).
//
// TradingView's "D" request on an intraday chart of CME futures and US/Indian
// equities returns the exchange's own daily bar (settlement / official close),
// which no aggregation of the intraday feed reproduces. These pin that a
// completed daily bucket takes the native bar's OHLCV while the aggregator
// keeps deciding when it completes, that other timeframes, the chart and the
// broker are untouched, that a missing native bar keeps the aggregate, and
// that the split (1m auxiliary) feed path substitutes the same way.

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_NATIVE_SECURITY_FEED_V1
#error "native security feed test requires the V1 feature probe"
#endif

namespace {

constexpr int64_t kMinute = 60000;
constexpr int64_t kQuarter = 15 * kMinute;
constexpr int64_t kDay = 86400000;

class DailyProbe final : public BacktestEngine {
public:
    std::vector<double> chart_closes;
    std::vector<double> daily_closes;        // sec 0: "D", completed buckets
    std::vector<double> daily_opens;
    std::vector<double> daily_volumes;
    std::vector<double> hourly_closes;       // sec 1: "60", completed buckets
    std::vector<double> daily_at_chart_close;
    double latest_daily = na<double>();

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "D", input_tf_, false, false);
        register_security_eval(1, "60", input_tf_, false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        if (!is_complete) return;
        if (sec_id == 0) {
            latest_daily = bar.close;
            daily_closes.push_back(bar.close);
            daily_opens.push_back(bar.open);
            daily_volumes.push_back(bar.volume);
        } else if (sec_id == 1) {
            hourly_closes.push_back(bar.close);
        }
    }

    void on_bar(const Bar& bar) override {
        chart_closes.push_back(bar.close);
        daily_at_chart_close.push_back(latest_daily);
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_close_all();
    }
};

bool near(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

// One NYSE-style session (0930-1600 America/New_York) of 15m bars: 26 bars
// from 09:30 to 15:45, closes 100*(day+1) + k so the last 15m close of day d
// is 100*(d+1) + 25.
std::vector<Bar> ny_session(int day, int64_t open_ms) {
    std::vector<Bar> bars;
    for (int k = 0; k < 26; ++k) {
        const double base = 100.0 * (day + 1) + k;
        bars.push_back({base - 0.5, base + 1.0, base - 1.0, base, 10.0,
                        open_ms + k * kQuarter});
    }
    return bars;
}

constexpr int64_t kNyDay1 = 1704205800000;  // 2024-01-02 09:30 America/New_York
constexpr int64_t kNyDay2 = kNyDay1 + kDay;

void test_completed_daily_bucket_carries_the_native_bar() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    // TradingView's own daily bars: the official close differs from the last
    // 15m close (125 / 225), and so do open/volume.
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
        {199.0, 230.0, 190.0, 222.5, 6000.0, kNyDay2},
    };

    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 2) == 0);
    assert(probe.native_security_feed_enabled());

    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());

    // The daily bucket completes on the session's last 15m bar (the
    // aggregator's timing) and carries the native OHLCV, not the aggregate.
    assert((probe.daily_closes == std::vector<double>{111.5, 222.5}));
    assert((probe.daily_opens == std::vector<double>{99.0, 199.0}));
    assert((probe.daily_volumes == std::vector<double>{5000.0, 6000.0}));
    assert(probe.native_security_substitutions() == 2);
    assert(probe.native_security_misses() == 0);
    // Exposed at the day's last chart bar, na before the first completion.
    assert(probe.daily_at_chart_close.size() == 52);
    assert(std::isnan(probe.daily_at_chart_close[24]));
    assert(near(probe.daily_at_chart_close[25], 111.5));
    assert(near(probe.daily_at_chart_close[26], 111.5));
    assert(near(probe.daily_at_chart_close[51], 222.5));
    // The intraday "60" request is aggregated exactly as before: 09:30-10:29
    // closes on the 10:15 bar (k = 3).
    assert(!probe.hourly_closes.empty());
    assert(near(probe.hourly_closes[0], 103.0));
    // Chart and broker never see the native feed.
    assert(probe.chart_closes.size() == 52);
    assert(near(probe.chart_closes[25], 125.0));
    assert(probe.trade_count() == 1);
    assert(near(probe.get_trade(0).entry_price, chart[1].open));
    assert(near(probe.get_trade(0).exit_price, chart[2].open));
}


void test_a_bucket_without_a_native_bar_keeps_its_aggregate() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
    };

    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "1D",
        reinterpret_cast<const pf_bar_t*>(daily), 1) == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.daily_closes == std::vector<double>{111.5, 225.0}));
    assert(probe.native_security_substitutions() == 1);
    assert(probe.native_security_misses() == 1);
}


void test_without_a_native_feed_the_run_is_the_aggregate() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
        {199.0, 230.0, 190.0, 222.5, 6000.0, kNyDay2},
    };
    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 2) == 0);
    // n == 0 clears exactly that timeframe's feed.
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D", nullptr, 0) == 0);
    assert(!probe.native_security_feed_enabled());
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.daily_closes == std::vector<double>{125.0, 225.0}));
    assert(probe.native_security_substitutions() == 0);
    assert(probe.native_security_misses() == 0);
}


void test_feed_validation_fails_closed() {
    const Bar unordered[] = {
        {1.0, 1.0, 1.0, 1.0, 1.0, kNyDay2},
        {1.0, 1.0, 1.0, 1.0, 1.0, kNyDay1},
    };
    DailyProbe probe;
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(unordered), 2) == -1);
    assert(!probe.last_error().empty());
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "",
        reinterpret_cast<const pf_bar_t*>(unordered), 1) == -1);
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "bogus",
        reinterpret_cast<const pf_bar_t*>(unordered), 1) == -1);
    assert(strategy_set_native_security_feed(
        nullptr, "D", reinterpret_cast<const pf_bar_t*>(unordered), 1) == -1);
    assert(!probe.native_security_feed_enabled());

    // Historical runs only: a stream refuses to start over a native feed.
    const Bar daily[] = {{1.0, 1.0, 1.0, 1.0, 1.0, kNyDay1}};
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 1) == 0);
    std::vector<Bar> warmup = ny_session(0, kNyDay1);
    assert(!probe.stream_begin(warmup.data(), static_cast<int>(warmup.size()),
                               "15", "15"));
    assert(probe.last_error().find("native request.security feed")
           != std::string::npos);
}


// The campaign's finer-tf retry: native 15m chart + 1m auxiliary feed. The
// "D" evaluator is then fed from the auxiliary slice, and its completed
// bucket must take the native daily bar exactly as on the plain path.
void test_split_aux_feed_path_substitutes_the_same_native_bar() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    std::vector<Bar> aux;
    for (const Bar& bar : chart) {
        for (int m = 0; m < 15; ++m) {
            const double v = bar.close - 1.0 + m / 15.0;
            aux.push_back({v, v, v, v, 1.0, bar.timestamp + m * kMinute});
        }
    }
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
        {199.0, 230.0, 190.0, 222.5, 6000.0, kNyDay2},
    };

    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux.data()),
        static_cast<int>(aux.size()), "1") == 0);
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 2) == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.daily_closes == std::vector<double>{111.5, 222.5}));
    assert(probe.native_security_substitutions() == 2);
    assert(probe.native_security_misses() == 0);
    assert(near(probe.chart_closes[25], 125.0));
}


// The CME shape: a 1700-1600 America/Chicago session whose daily bar is
// stamped at the 17:00 open and closes at 16:00 the next calendar day, with
// TradingView's close the 15:00 settlement rather than the 15:45 bar's close.
void test_overnight_cme_session_labels_by_session_day() {
    constexpr int64_t open1 = 1704236400000;  // 2024-01-02 17:00 America/Chicago
    constexpr int64_t open2 = open1 + kDay;
    constexpr int64_t open3 = open2 + kDay;
    std::vector<Bar> chart;
    for (int day = 0; day < 2; ++day) {
        for (int k = 0; k < 92; ++k) {  // 17:00 .. 15:45 next day
            const double base = 5000.0 + 100.0 * day + k;
            chart.push_back({base - 0.25, base + 0.5, base - 0.5, base, 10.0,
                             (day == 0 ? open1 : open2) + k * kQuarter});
        }
    }
    // The third session's first bar: an overnight session's daily bucket is
    // finalized by the next session's first chart bar.
    chart.push_back({5200.0, 5200.5, 5199.5, 5200.0, 10.0, open3});
    const Bar daily[] = {
        {4999.0, 5100.0, 4990.0, 5077.25, 1.0, open1},
        {5099.0, 5200.0, 5090.0, 5177.25, 1.0, open2},
        {5199.0, 5300.0, 5190.0, 5277.25, 1.0, open3},
    };
    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/Chicago");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "1700-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 3) == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    // Both completed session-days carry the settlement print of the native
    // bar stamped at their 17:00 open, matched by session-day label.
    assert((probe.daily_closes == std::vector<double>{5077.25, 5177.25}));
    assert(probe.native_security_substitutions() == 2);
    assert(probe.native_security_misses() == 0);
    // Completion timing stays the aggregator's: the first session's value is
    // exposed no earlier than its last chart bar (15:45 CT, index 91) and no
    // later than the next session's first bar (index 92).
    assert(probe.daily_at_chart_close.size() == 185);
    assert(std::isnan(probe.daily_at_chart_close[90]));
    assert(near(probe.daily_at_chart_close[92], 5077.25));
    assert(near(probe.daily_at_chart_close[184], 5177.25));
}

}  // namespace

int main() {
    test_completed_daily_bucket_carries_the_native_bar();
    test_a_bucket_without_a_native_bar_keeps_its_aggregate();
    test_without_a_native_feed_the_run_is_the_aggregate();
    test_feed_validation_fails_closed();
    test_split_aux_feed_path_substitutes_the_same_native_bar();
    test_overnight_cme_session_labels_by_session_day();
    return 0;
}
