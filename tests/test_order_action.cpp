#include <pineforge/order_action.hpp>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace pineforge::order_action;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Inf = std::numeric_limits<double>::infinity();
void near(double a, double b) { CHECK(std::abs(a - b) < 1e-12); }

void reductions() {
    for (double position : {5.0, -5.0, 0.0}) {
        auto zero = plan(position, Reduce{0.0});
        CHECK(zero.has_value());
        if (zero) { CHECK(zero->close_units() == 0.0); CHECK(zero->open_units() == 0.0); CHECK(zero->no_effect()); }
        auto partial = plan(position, Reduce{2.0});
        CHECK(partial.has_value());
        if (partial) {
            CHECK(partial->before() == position);
            near(partial->close_units(), position == 0.0 ? 0.0 : 2.0);
            CHECK(partial->open_units() == 0.0);
            near(partial->after(), position == 0.0 ? 0.0 : position > 0.0 ? 3.0 : -3.0);
            CHECK(partial->close_units() >= 0.0);
            CHECK(partial->after() == 0.0 || (partial->after() > 0.0) == (position > 0.0));
        }
        auto oversized = plan(position, Reduce{99.0});
        CHECK(oversized.has_value());
        if (oversized) { CHECK(oversized->before() == position); near(oversized->close_units(), std::abs(position)); near(oversized->after(), 0.0); CHECK(oversized->open_units() == 0.0); }
    }
    CHECK(!plan(5.0, Reduce{-1.0}));
    CHECK(!plan(5.0, Reduce{NaN}));
    CHECK(!plan(5.0, Reduce{Inf}));
    CHECK(!plan(NaN, Reduce{1.0}));
    CHECK(!plan(1.0, Reduce{std::numeric_limits<double>::denorm_min()}));
}

void transactions() {
    struct Case { double before, delta, after, close, open; };
    for (const auto c : {
        Case{5.0, 2.0, 7.0, 0.0, 2.0},
        Case{5.0, -2.0, 3.0, 2.0, 0.0},
        Case{5.0, -5.0, 0.0, 5.0, 0.0},
        Case{5.0, -12.0, -7.0, 5.0, -7.0},
        Case{-5.0, -2.0, -7.0, 0.0, -2.0},
        Case{-5.0, 2.0, -3.0, 2.0, 0.0},
        Case{-5.0, 5.0, 0.0, 5.0, 0.0},
        Case{-5.0, 12.0, 7.0, 5.0, 7.0},
        Case{0.0, 3.0, 3.0, 0.0, 3.0},
        Case{0.0, -3.0, -3.0, 0.0, -3.0},
        Case{5.0, 0.0, 5.0, 0.0, 0.0},
    }) {
        auto result = plan(c.before, Transact{c.delta});
        CHECK(result.has_value());
        if (!result) continue;
        CHECK(result->before() == c.before);
        near(result->after(), c.after); near(result->close_units(), c.close); near(result->open_units(), c.open);
        near(c.before + (c.before > 0.0 && c.delta < 0.0 ? -c.close : c.before < 0.0 && c.delta > 0.0 ? c.close : 0.0) + c.open, c.after);
        CHECK(result->close_units() >= 0.0);
        CHECK(result->no_effect() == (c.delta == 0.0));
    }
    CHECK(!plan(Inf, Transact{1.0}));
    CHECK(!plan(1.0, Transact{NaN}));
    CHECK(!plan(1.0, Transact{Inf}));
    CHECK(!plan(1.0, Transact{-Inf}));
    CHECK(!plan(std::numeric_limits<double>::max(), Transact{std::numeric_limits<double>::max()}));
    CHECK(!plan(-std::numeric_limits<double>::max(), Transact{-std::numeric_limits<double>::max()}));
    CHECK(!plan(1.0, Transact{std::numeric_limits<double>::denorm_min()}));

    const double d = std::numeric_limits<double>::denorm_min();
    auto sub_open = plan(0.0, Transact{d});
    CHECK(sub_open.has_value());
    if (sub_open) {
        CHECK(sub_open->before() == 0.0);
        CHECK(sub_open->after() == d);
        CHECK(sub_open->close_units() == 0.0);
        CHECK(sub_open->open_units() == d);
    }
    auto sub_reduce = plan(2.0 * d, Reduce{d});
    CHECK(sub_reduce.has_value());
    if (sub_reduce) {
        CHECK(sub_reduce->before() == 2.0 * d);
        CHECK(sub_reduce->after() == d);
        CHECK(sub_reduce->close_units() == d);
        CHECK(sub_reduce->open_units() == 0.0);
    }
    auto sub_flat = plan(2.0 * d, Transact{-2.0 * d});
    CHECK(sub_flat.has_value());
    if (sub_flat) {
        CHECK(sub_flat->after() == 0.0);
        CHECK(sub_flat->close_units() == 2.0 * d);
        CHECK(sub_flat->open_units() == 0.0);
    }
    const double near_flat = std::nextafter(1.0, 0.0);
    auto residual = plan(1.0, Transact{-near_flat});
    CHECK(residual.has_value());
    if (residual) {
        CHECK(residual->after() == 1.0 - near_flat);
        CHECK(residual->close_units() == near_flat);
        CHECK(residual->open_units() == 0.0);
    }
}

void symmetric_laws() {
    for (double p : {0.25, 1.0, 5.0, 100.0}) {
        for (double q : {0.0, 0.125, 1.0, 7.0, 200.0}) {
            auto long_reduce = plan(p, Reduce{q});
            auto short_reduce = plan(-p, Reduce{q});
            CHECK(long_reduce.has_value() == short_reduce.has_value());
            if (long_reduce && short_reduce) {
                near(long_reduce->close_units(), short_reduce->close_units());
                near(long_reduce->after(), -short_reduce->after());
                CHECK(long_reduce->open_units() == 0.0 && short_reduce->open_units() == 0.0);
            }
            auto long_tx = plan(p, Transact{q});
            auto short_tx = plan(-p, Transact{-q});
            CHECK(long_tx.has_value() == short_tx.has_value());
            if (long_tx && short_tx) {
                near(long_tx->after(), -short_tx->after());
                near(long_tx->close_units(), short_tx->close_units());
                near(long_tx->open_units(), -short_tx->open_units());
            }
            auto long_opposite = plan(p, Transact{-q});
            auto short_opposite = plan(-p, Transact{q});
            CHECK(long_opposite.has_value() == short_opposite.has_value());
            if (long_opposite && short_opposite) {
                near(long_opposite->after(), -short_opposite->after());
                near(long_opposite->close_units(), short_opposite->close_units());
                near(long_opposite->open_units(), -short_opposite->open_units());
            }
        }
    }
}
}

int main() {
    reductions(); transactions(); symmetric_laws();
    std::printf("order action planner: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
