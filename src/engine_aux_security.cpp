/*
 * engine_aux_security.cpp — native-chart / request.security feed separation
 */

#include "engine_internal.hpp"

#include <pineforge/ta.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pineforge {

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1

bool BacktestEngine::set_aux_security_feed(const Bar* bars, int n,
                                           const std::string& input_tf) {
    if (n == 0) {
        aux_security_bars_.clear();
        aux_security_input_tf_.clear();
        clear_aux_security_chart_ranges();
        return true;
    }
    if (n < 0 || bars == nullptr || input_tf.empty()) {
        last_error_ =
            "auxiliary request.security feed requires bars, a positive count, and input_tf";
        return false;
    }
    int seconds = 0;
    try {
        seconds = tf_to_seconds(input_tf);
    } catch (...) {
        seconds = 0;
    }
    if (seconds <= 0) {
        last_error_ =
            "auxiliary request.security feed requires a fixed positive input_tf";
        return false;
    }
    for (int i = 1; i < n; ++i) {
        if (bars[i].timestamp <= bars[i - 1].timestamp) {
            last_error_ =
                "auxiliary request.security feed timestamps must be strictly increasing";
            return false;
        }
    }
    aux_security_bars_.assign(bars, bars + n);
    aux_security_input_tf_ = input_tf;
    clear_aux_security_chart_ranges();
    last_error_.clear();
    return true;
}


void BacktestEngine::clear_aux_security_chart_ranges() {
    aux_security_chart_begin_.clear();
    aux_security_chart_end_.clear();
}


void BacktestEngine::prepare_aux_security_chart_ranges(
        const Bar* chart_bars, int n_chart, const std::string& chart_tf) {
    clear_aux_security_chart_ranges();
    if (!aux_security_feed_enabled()) return;
    if (chart_bars == nullptr || n_chart <= 0) {
        throw std::runtime_error(
            "auxiliary request.security feed requires at least one native chart bar");
    }

    int chart_seconds = 0;
    int aux_seconds = 0;
    try {
        chart_seconds = tf_to_seconds(chart_tf);
        aux_seconds = tf_to_seconds(aux_security_input_tf_);
    } catch (...) {
        chart_seconds = 0;
        aux_seconds = 0;
    }
    if (chart_seconds <= 0 || aux_seconds <= 0
        || aux_seconds >= chart_seconds) {
        throw std::runtime_error(
            "auxiliary request.security feed input_tf must be fixed and strictly finer than the native chart timeframe");
    }
    if (chart_seconds % aux_seconds != 0) {
        throw std::runtime_error(
            "auxiliary request.security feed input_tf must evenly divide the native chart timeframe");
    }
    for (int i = 1; i < n_chart; ++i) {
        if (chart_bars[i].timestamp <= chart_bars[i - 1].timestamp) {
            throw std::runtime_error(
                "native chart feed timestamps must be strictly increasing with an auxiliary security feed");
        }
    }

    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    aux_security_chart_begin_.assign(static_cast<std::size_t>(n_chart), missing);
    aux_security_chart_end_.assign(static_cast<std::size_t>(n_chart), missing);
    TimeframeAggregator chart_router(chart_tf, aux_security_input_tf_,
                                     syminfo_.timezone, syminfo_.session);
    const CalendarPeriod chart_period = calendar_period_for(chart_tf);
    const bool calendar_chart = chart_period != CalendarPeriod::NONE;
    // Calendar chart timestamps are the ACTUAL exchange bar opens and define
    // the authoritative partition.  A native daily bar may begin at a shifted
    // special-session open (NSE Muhurat), or may coalesce more than one
    // nominal session key (the CME Labor-Day Sunday/Monday sessions).  Route
    // each auxiliary bar through [chart_ts[i], chart_ts[i + 1]) while retaining
    // the native label for chart and broker semantics.  Nominal calendar keys
    // remain useful only for ignoring wider leading/trailing feed coverage.
    std::vector<int64_t> chart_route_keys;
    chart_route_keys.reserve(static_cast<std::size_t>(n_chart));
    for (int i = 0; i < n_chart; ++i) {
        // Key each native bar by the session it COVERS: OANDA stamps daily
        // bars at the 17:00 ET break, one hour before the 1800-1700 session
        // the bar actually carries, and the raw stamp would key one session
        // too early -- the tail prefilter below would then drop the last
        // bar's entire slice as beyond last_chart_key.
        const int64_t key = calendar_chart
            ? session_period_open_ms(
                  session_covered_instant_ms(chart_bars[i].timestamp,
                                             syminfo_.timezone,
                                             syminfo_.session),
                  syminfo_.timezone, syminfo_.session, chart_period)
            : chart_bars[i].timestamp;
        if (!chart_route_keys.empty() && key <= chart_route_keys.back()) {
            throw std::runtime_error(
                "native chart feed trading-period identities must be unique and strictly increasing with an auxiliary security feed");
        }
        chart_route_keys.push_back(key);
    }
    const int64_t first_chart_key = chart_route_keys.front();
    const int64_t last_chart_key = chart_route_keys.back();
    std::size_t chart_index = 0;
    auto record_aux = [&](std::size_t aux_index) {
        if (aux_security_chart_begin_[chart_index] == missing) {
            aux_security_chart_begin_[chart_index] = aux_index;
        }
        aux_security_chart_end_[chart_index] = aux_index + 1;
    };
    if (calendar_chart) {
        const int64_t first_chart_timestamp = chart_bars[0].timestamp;
        for (std::size_t aux_index = 0;
             aux_index < aux_security_bars_.size(); ++aux_index) {
            const int64_t aux_timestamp =
                aux_security_bars_[aux_index].timestamp;
            const int64_t period_key = session_period_open_ms(
                aux_timestamp, syminfo_.timezone, syminfo_.session,
                chart_period);
            if (period_key < first_chart_key || period_key > last_chart_key
                || aux_timestamp < first_chart_timestamp) {
                continue;
            }
            while (chart_index + 1 < static_cast<std::size_t>(n_chart)
                   && chart_bars[chart_index + 1].timestamp <= aux_timestamp) {
                ++chart_index;
            }
            record_aux(aux_index);
        }
    } else {
        for (std::size_t aux_index = 0;
             aux_index < aux_security_bars_.size(); ++aux_index) {
            const int64_t label = chart_router.bar_label_ms(
                aux_security_bars_[aux_index].timestamp);
            // Intraday native bars remain exact session-grid labels.  Wider
            // leading/trailing coverage is inert, but an interior grid hole
            // must fail closed rather than attach to a neighbouring bar.
            if (label < first_chart_key || label > last_chart_key) {
                continue;
            }
            while (chart_index + 1 < static_cast<std::size_t>(n_chart)
                   && chart_route_keys[chart_index] < label) {
                ++chart_index;
            }
            if (chart_route_keys[chart_index] != label) {
                throw std::runtime_error(
                    "auxiliary request.security bar does not map to a native chart bar");
            }
            record_aux(aux_index);
        }
    }
    for (int i = 0; i < n_chart; ++i) {
        if (aux_security_chart_begin_[static_cast<std::size_t>(i)] == missing) {
            throw std::runtime_error(
                "native chart bar has no matching auxiliary request.security bars");
        }
    }
}


void BacktestEngine::feed_aux_security_for_chart_bar(int chart_index) {
    const std::size_t idx = static_cast<std::size_t>(chart_index);
    if (idx >= aux_security_chart_begin_.size()
        || idx >= aux_security_chart_end_.size()) {
        throw std::runtime_error(
            "auxiliary request.security chart routing is not initialized");
    }
    const std::size_t begin = aux_security_chart_begin_[idx];
    const std::size_t end = aux_security_chart_end_[idx];

    // Lower-TF arrays belong to the native chart slice, not a raw UTC bucket.
    // Accumulate the entire symbol-clock-aligned slice here and publish it only
    // after its final auxiliary bar. This keeps an overnight 17:00-17:00 daily
    // bar as one array even though its raw timestamps cross UTC midnight.
    for (std::size_t i = begin; i < end; ++i) {
        const Bar& aux_bar = aux_security_bars_[i];
        const bool calling_bar_complete = (i + 1 == end);
        for (auto& state : security_eval_states_) {
            if (!state.lower_tf_array_requested) {
                feed_security_at_calling_bar_boundary(
                    state, aux_bar, calling_bar_complete);
                continue;
            }
            if (security_input_precedes_range_start(state, aux_bar.timestamp)) {
                continue;
            }
            if (state.lower_tf_use_input) {
                state.lower_tf_input_buffer.push_back(aux_bar);
            } else if (state.lower_tf_emulation) {
                std::vector<Bar> synthetic = internal::synthesize_lower_tf_bars(
                    aux_bar, state.lower_tf_ratio, state.lower_tf_seconds);
                if (synthetic.empty()) {
                    throw std::runtime_error(
                        "request.security_lower_tf could not synthesize auxiliary sub-bars");
                }
                state.lower_tf_input_buffer.insert(
                    state.lower_tf_input_buffer.end(),
                    synthetic.begin(), synthetic.end());
            } else {
                throw std::runtime_error(
                    "request.security_lower_tf auxiliary routing is not initialized");
            }
        }
    }

    struct SecurityNaWarmupScope {
        bool previous;
        explicit SecurityNaWarmupScope(bool enabled)
            : previous(ta::ema_na_warmup_flag()) {
            ta::ema_na_warmup_flag() = enabled;
        }
        ~SecurityNaWarmupScope() { ta::ema_na_warmup_flag() = previous; }
    } warmup_scope(security_range_start_na_warmup_);

    for (auto& state : security_eval_states_) {
        if (!state.lower_tf_array_requested) continue;
        int aggregate_ratio = state.lower_tf_emulation
            ? 1 : state.lower_tf_input_aggregation_ratio;
        if (aggregate_ratio < 1) aggregate_ratio = 1;
        const int count = static_cast<int>(state.lower_tf_input_buffer.size());
        std::vector<Bar> requested_bars;
        requested_bars.reserve(
            static_cast<std::size_t>(count / aggregate_ratio + 1));
        if (aggregate_ratio == 1) {
            requested_bars.assign(state.lower_tf_input_buffer.begin(),
                                  state.lower_tf_input_buffer.end());
        } else {
            for (int i = 0; i + aggregate_ratio <= count;
                 i += aggregate_ratio) {
                Bar aggregate = state.lower_tf_input_buffer[
                    static_cast<std::size_t>(i)];
                double volume = aggregate.volume;
                for (int j = 1; j < aggregate_ratio; ++j) {
                    const Bar& next = state.lower_tf_input_buffer[
                        static_cast<std::size_t>(i + j)];
                    aggregate.high = std::max(aggregate.high, next.high);
                    aggregate.low = std::min(aggregate.low, next.low);
                    aggregate.close = next.close;
                    volume += next.volume;
                }
                aggregate.volume = volume;
                requested_bars.push_back(aggregate);
            }
        }

        state.lower_tf_sub_bar_index = 0;
        for (const Bar& bar : requested_bars) {
            state.feed_count++;
            state.current_bar = bar;
            state.current_sub_bar_count = 1;
            state.eval_complete_count++;
            dispatch_security_eval(state, bar, true,
                                   state.eval_complete_count - 1);
            state.lower_tf_sub_bar_index++;
        }
        state.lower_tf_input_buffer.clear();
    }
}

#endif  // PINEFORGE_HAS_AUX_SECURITY_FEED_V1


// ---- native higher-timeframe request.security feeds -------------------------
//
// TradingView's request.security(syminfo.tickerid, "D", close) on an intraday
// chart of CME_MINI:ES1! returns the 15:00 CT settlement, on NASDAQ:AAPL the
// official closing print, on NSE:NIFTY the exchange's official OHLC -- values
// no aggregation of the intraday feed produces (pinned 2026-09-04 by lab tv
// limit-fill probes: 9/9 ES1!, 5/5 AAPL and 2/2 NIFTY fills equal the 1D
// feed's close and none the last 15m close). The campaign holds those daily
// bars; this path lets a completed daily bucket carry them while the
// aggregator keeps deciding WHEN the bucket completes.

bool BacktestEngine::set_native_security_feed(const std::string& timeframe,
                                              const Bar* bars, int n) {
    int seconds = 0;
    try {
        seconds = tf_to_seconds(timeframe);
    } catch (...) {
        seconds = 0;
    }
    if (timeframe.empty() || seconds == 0) {
        last_error_ =
            "native request.security feed requires a parseable timeframe";
        return false;
    }
    auto existing = native_security_feeds_.begin();
    while (existing != native_security_feeds_.end()
           && existing->seconds != seconds) {
        ++existing;
    }
    if (n == 0) {
        if (existing != native_security_feeds_.end()) {
            native_security_feeds_.erase(existing);
        }
        last_error_.clear();
        return true;
    }
    if (n < 0 || bars == nullptr) {
        last_error_ =
            "native request.security feed requires bars and a positive count";
        return false;
    }
    for (int i = 1; i < n; ++i) {
        if (bars[i].timestamp <= bars[i - 1].timestamp) {
            last_error_ =
                "native request.security feed timestamps must be strictly increasing";
            return false;
        }
    }
    NativeSecurityFeed feed;
    feed.tf = timeframe;
    feed.seconds = seconds;
    feed.bars.assign(bars, bars + n);
    if (existing != native_security_feeds_.end()) {
        *existing = std::move(feed);
    } else {
        native_security_feeds_.push_back(std::move(feed));
    }
    last_error_.clear();
    return true;
}


void BacktestEngine::prepare_native_security_feeds() {
    diag_native_security_substitutions_ = 0;
    diag_native_security_misses_ = 0;
    for (auto& state : security_eval_states_) {
        state.native_feed_index = -1;
        state.native_bars_by_label.clear();
        if (native_security_feeds_.empty()) continue;
        // Only an aggregating (coarser-than-input) request has buckets to
        // substitute; passthrough, lower-TF emulation and input passthrough
        // read the feed itself.
        if (state.lower_tf_emulation || state.lower_tf_use_input
            || !state.aggregator.is_active()) {
            continue;
        }
        int requested_seconds = 0;
        try {
            requested_seconds = tf_to_seconds(state.tf);
        } catch (...) {
            requested_seconds = 0;
        }
        if (requested_seconds == 0) continue;
        for (std::size_t i = 0; i < native_security_feeds_.size(); ++i) {
            if (native_security_feeds_[i].seconds != requested_seconds) continue;
            state.native_feed_index = static_cast<int>(i);
            const auto& bars = native_security_feeds_[i].bars;
            state.native_bars_by_label.reserve(bars.size());
            for (const Bar& bar : bars) {
                // Key by the label the aggregate carries: the covered session
                // instant (OANDA's 17:00 stamp covers the 18:00 session, see
                // session_covered_instant_ms) labelled exactly as this
                // state's aggregator labels its own buckets.
                const int64_t label = state.aggregator.bar_label_ms(
                    session_covered_instant_ms(bar.timestamp,
                                               syminfo_.timezone,
                                               syminfo_.session));
                // A later native bar under one label (a session the run's
                // calendar coalesces) is the period's final print: keep it.
                state.native_bars_by_label[label] = bar;
            }
            break;
        }
    }
}


bool BacktestEngine::substitute_native_security_bar(SecurityEvalState& state,
                                                    Bar& bar) {
    if (state.native_feed_index < 0) return false;
    const auto found = state.native_bars_by_label.find(bar.timestamp);
    if (found == state.native_bars_by_label.end()) {
        ++diag_native_security_misses_;
        return false;
    }
    const Bar& native = found->second;
    bar.open = native.open;
    bar.high = native.high;
    bar.low = native.low;
    bar.close = native.close;
    bar.volume = native.volume;
    ++diag_native_security_substitutions_;
    return true;
}

}  // namespace pineforge
