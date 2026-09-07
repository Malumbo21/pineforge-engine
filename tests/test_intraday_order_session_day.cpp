// Covered TV controls exhaust six broker fills, then resume exactly at the
// declared trading-session day boundary (17:00 New York, with DST). Constant
// synthetic prices isolate the risk clock from strategy signals and PnL.
#include <pineforge/engine.hpp>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;
namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
constexpr int64_t hour = 3600000;
constexpr int64_t minute = 60000;

class SessionOrders : public BacktestEngine {
public:
    bool market_close;
    explicit SessionOrders(const std::string& display_zone, bool close_command = false)
        : market_close(close_command) {
        initial_capital_ = 1000000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        max_intraday_filled_orders_ = 6;
        process_orders_on_close_ = true;
        commission_value_ = 0;
        slippage_ = 0;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1;
        set_syminfo_timezone("America/New_York");
        set_syminfo_session("1700-1700");
        set_chart_timezone(display_zone);
        intraday_cap_count_pooc_full_close_fills_ = true;
    }
    void on_bar(const Bar&) override {
        if (market_close && signed_position_size() > 0) strategy_close("L");
        const bool request = bar_index_ == 0 || bar_index_ == 2 || bar_index_ == 4
            || (bar_index_ >= 6 && bar_index_ <= 10)
            || bar_index_ == 12 || bar_index_ == 14 || bar_index_ == 16;
        if (request && signed_position_size() == 0) {
            strategy_entry("L", true);
            if (!market_close)
                strategy_exit("X", "L", 101.0, std::numeric_limits<double>::quiet_NaN());
        }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

std::vector<Bar> session_bars(int64_t day, int reset_hour) {
    const int64_t offsets[] = {
        6*hour, 6*hour+15*minute, 8*hour, 8*hour+15*minute,
        10*hour, 10*hour+15*minute, 11*hour, 15*hour+45*minute,
        16*hour, reset_hour*hour-15*minute, reset_hour*hour,
        reset_hour*hour+15*minute, reset_hour*hour+30*minute,
        reset_hour*hour+45*minute, reset_hour*hour+60*minute,
        reset_hour*hour+75*minute, 24*hour, 24*hour+15*minute,
    };
    std::vector<Bar> bars;
    for (int64_t offset : offsets) bars.push_back({100, 101, 100, 100, 1, day+offset});
    return bars;
}

void test_session_boundary_uses_exchange_clock_and_dst() {
    struct Date { int64_t day; int reset_hour; };
    for (const Date date : {Date{1744243200000LL,21}, Date{1762128000000LL,22},
                            Date{1741305600000LL,22}, Date{1741564800000LL,21}}) {
        const auto bars = session_bars(date.day, date.reset_hour);
        for (const char* chart_zone : {"", "UTC", "Asia/Taipei", "America/New_York"}) {
          for (bool market_close : {false, true}) {
            SessionOrders engine(chart_zone, market_close);
            engine.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(engine.rows().size() == 6);
            if (engine.rows().size() != 6) continue;
            CHECK(engine.rows()[0].entry_time == date.day+6*hour);
            CHECK(engine.rows()[1].entry_time == date.day+8*hour);
            CHECK(engine.rows()[2].entry_time == date.day+10*hour);
            CHECK(engine.rows()[3].entry_time == date.day+date.reset_hour*hour);
            CHECK(engine.rows()[4].entry_time == date.day+date.reset_hour*hour+30*minute);
            CHECK(engine.rows()[5].entry_time == date.day+date.reset_hour*hour+60*minute);
          }
        }
    }
}

class LegacyClock : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}
    void exhaust_at(int64_t time) {
        max_intraday_filled_orders_ = 6;
        current_bar_.timestamp = time;
        _intraday_cap_currently_latched();
        intraday_fill_count_ = 6;
        intraday_cap_hit_ = true;
    }
    bool latched_at(int64_t time) {
        current_bar_.timestamp = time;
        return _intraday_cap_currently_latched();
    }
};

void test_continuous_and_unconfigured_sessions_keep_chart_clock() {
    for (const char* session : {"", "24x7", "regular"}) {
        LegacyClock utc;
        utc.set_syminfo_session(session);
        utc.exhaust_at(1744243200000LL+15*hour);
        CHECK(utc.latched_at(1744243200000LL+21*hour));
        CHECK(!utc.latched_at(1744243200000LL+24*hour));
        LegacyClock shifted;
        shifted.set_syminfo_session(session);
        shifted.set_chart_timezone("Asia/Taipei");
        shifted.exhaust_at(1744243200000LL+15*hour);
        CHECK(shifted.latched_at(1744243200000LL+15*hour+45*minute));
        CHECK(!shifted.latched_at(1744243200000LL+16*hour));
    }
}
}

int main() {
    test_session_boundary_uses_exchange_clock_and_dst();
    test_continuous_and_unconfigured_sessions_keep_chart_clock();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
