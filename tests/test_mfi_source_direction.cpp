// MFI source direction uses Pine's absolute float-comparison band. The
// 2026-09-09 TV controls compare builtin MFI with explicit up/down money flow
// at sources 0.001, 10, and 60000, including equal-decimal HLC3 residues.
// Only literal/synthetic indicator inputs are used here; no strategy or feed.

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace pineforge;

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const char* label) {
    if (condition) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL %s\n", label);
    }
}

void check_value(double actual, double expected, const char* label) {
    if (std::isfinite(actual) && std::fabs(actual - expected) <= 1e-11) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL %s: actual=%.17g expected=%.17g\n",
                    label, actual, expected);
    }
}

// Independently specified three-transition window:
//   base -> base+1, volume 2: positive budget 2*(base+1)
//   base+1 -> base, volume 3: negative budget 3*base
// The fourth sample supplies the test's stated extra flow. Expected values
// use these fixed budgets, never a comparison helper or a mirrored MFI loop.
void seed_window(ta::MFI& mfi, double base) {
    check(is_na(mfi.compute(base, 9.0)), "initial sample remains warmup");
    check(is_na(mfi.compute(base + 1.0, 2.0)), "second sample remains warmup");
    check(is_na(mfi.compute(base, 3.0)), "third sample remains warmup");
}

double percent(double positive, double negative) {
    return 100.0 * positive / (positive + negative);
}

double observe_fourth(double base, double source) {
    ta::MFI mfi(3);
    seed_window(mfi, base);
    return mfi.compute(source, 5.0);
}

void test_absolute_band_at_three_scales() {
    for (double base : {0.001, 10.0, 60000.0}) {
        const double positive = 2.0 * (base + 1.0);
        const double negative = 3.0 * base;
        const double neutral = percent(positive, negative);
        check_value(observe_fourth(base, base), neutral, "exact equality is neutral");

        // Both directions strictly inside the TV-pinned absolute band.
        const double tiny_up = base + 5e-11;
        const double tiny_down = base - 5e-11;
        check(tiny_up > base && tiny_up - base < 1e-10,
              "positive below-band fixture is distinct in binary64");
        check(tiny_down < base && base - tiny_down < 1e-10,
              "negative below-band fixture is distinct in binary64");
        check_value(observe_fourth(base, tiny_up), neutral, "below-band rise is neutral");
        check_value(observe_fourth(base, tiny_down), neutral, "below-band fall is neutral");

        // Above-band controls at all three scales refute relative tolerance,
        // tick rounding, and suppressing every small decimal change.
        const double above_up = base + 1.2e-10;
        const double above_down = base - 1.2e-10;
        check(above_up - base > 1e-10, "positive above-band fixture");
        check(base - above_down > 1e-10, "negative above-band fixture");
        check_value(observe_fourth(base, above_up),
                    percent(positive + 5.0 * above_up, negative),
                    "above-band rise retains all positive money");
        check_value(observe_fourth(base, above_down),
                    percent(positive, negative + 5.0 * above_down),
                    "above-band fall retains all negative money");

        const double large_up = base + 1e-8;
        const double large_down = base - 1e-8;
        check_value(observe_fourth(base, large_up),
                    percent(positive + 5.0 * large_up, negative),
                    "large rise remains positive");
        check_value(observe_fourth(base, large_down),
                    percent(positive, negative + 5.0 * large_down),
                    "large fall remains negative");
    }
}

void test_equal_decimal_hlc3_residue() {
    const double first = (8.80 + 8.68 + 8.78) / 3.0;
    const double second = (8.80 + 8.69 + 8.77) / 3.0;
    check(first != second && std::fabs(first - second) < 1e-10,
          "equal-decimal HLC3 fixture carries a binary64 residue");
    check_value(observe_fourth(first, second),
                percent(2.0 * (first + 1.0), 3.0 * first),
                "HLC3 residue contributes no flow");
    check_value(observe_fourth(second, first),
                percent(2.0 * (second + 1.0), 3.0 * second),
                "reversed HLC3 residue contributes no flow");
}

void test_recompute_replaces_flow_without_advancing() {
    for (double base : {0.001, 10.0, 60000.0}) {
        ta::MFI mfi(3);
        seed_window(mfi, base);
        const double positive = 2.0 * (base + 1.0);
        const double negative = 3.0 * base;
        const double up = base + 1.2e-10;
        const double down = base - 1.2e-10;
        check_value(mfi.compute(up, 5.0),
                    percent(positive + 5.0 * up, negative),
                    "initial fourth sample carries positive flow");
        for (int repeat = 0; repeat < 3; ++repeat) {
            check_value(mfi.recompute(base - 5e-11, 11.0),
                        percent(positive, negative),
                        "recompute removes the prior positive flow");
            check_value(mfi.recompute(down, 7.0),
                        percent(positive, negative + 7.0 * down),
                        "recompute replaces direction and volume");
        }
        check_value(mfi.recompute(base, 13.0), percent(positive, negative),
                    "final exact-tie recompute is neutral");
        // The next committed sample evicts the old positive transition. The
        // retained negative budget is 3*base; the new positive is 7*(base+2).
        check_value(mfi.compute(base + 2.0, 7.0),
                    percent(7.0 * (base + 2.0), negative),
                    "recompute preserves next-window eviction");
    }
}

void test_existing_zero_flow_nan_and_warmup_contracts() {
    ta::MFI flat(3), rising(3), falling(3);
    for (int index = 0; index < 3; ++index) {
        check(is_na(flat.compute(10.0, 2.0)), "flat warmup unchanged");
        check(is_na(rising.compute(10.0 + index, 2.0)), "rising warmup unchanged");
        check(is_na(falling.compute(10.0 - index, 2.0)), "falling warmup unchanged");
    }
    check_value(flat.compute(10.0, 2.0), 100.0, "both-zero flow still returns 100");
    check_value(rising.compute(13.0, 2.0), 100.0, "one-sided positive flow unchanged");
    check_value(falling.compute(7.0, 2.0), 0.0, "one-sided negative flow unchanged");

    ta::MFI missing_source(2);
    missing_source.compute(10.0, 1.0);
    missing_source.compute(12.0, 1.0);
    check_value(missing_source.compute(na<double>(), 1.0), 100.0,
                "NaN source supplies no directional flow");
    check_value(missing_source.compute(10.0, 1.0), 100.0,
                "first source after NaN remains neutral");
    check_value(missing_source.compute(9.0, 1.0), 0.0,
                "direction resumes after a finite previous source");

    ta::MFI missing_volume(2);
    missing_volume.compute(10.0, 1.0);
    missing_volume.compute(12.0, na<double>());
    check(is_na(missing_volume.compute(11.0, 1.0)),
          "NaN volume on directional flow still propagates");
}

}  // namespace

int main() {
    test_absolute_band_at_three_scales();
    test_equal_decimal_hlc3_residue();
    test_recompute_replaces_flow_without_advancing();
    test_existing_zero_flow_nan_and_warmup_contracts();
    std::printf("test_mfi_source_direction: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
