// Literal lifecycle contract test. Compiled Pine/indicator reuse is a separate
// Cloud diagnostic: this test establishes the engine-owned dispatch boundary.
#include <pineforge/engine.hpp>
#include <cassert>
#include <stdexcept>
#include <vector>

using namespace pineforge;

class ScriptProbe final : public BacktestEngine {
public:
    int preparations = 0;
    int configurations = 0;
    int value = -999;
    bool prepared = false;
    bool allow_precalc = false;
    bool fail_preparation = false;
    std::vector<int> observed;

    ScriptProbe() { initial_capital_ = 12345.0; }

    void prepare_script_run(const Bar*, int, bool allow) override {
        ++preparations;
        prepared = true;
        allow_precalc = allow;
        observed.clear();
        value = std::stoi(inputs_.at("seed"));
        assert(trades_.empty());
        assert(signed_position_size() == 0.0);
        assert(initial_capital_ == 12345.0);
        if (fail_preparation) throw std::runtime_error("literal preparation failure");
    }

    void configure_security_evaluators() override {
        assert(prepared);
        assert(observed.empty());
        assert(value == std::stoi(inputs_.at("seed")));
        ++configurations;
    }

    void on_bar(const Bar&) override {
        assert(prepared);
        observed.push_back(++value);
    }
};

int main() {
    const Bar bars[] = {
        {10, 11, 9, 10, 1, 60000},
        {11, 12, 10, 11, 2, 120000},
        {12, 13, 11, 12, 3, 180000},
    };
    ScriptProbe p;
    p.set_input("seed", "7");
    // Enter via the base API, as stream_begin and other native callers do.
    BacktestEngine& base = p;
    base.run(bars, 1);
    assert(p.preparations == 1 && p.allow_precalc);
    assert((p.observed == std::vector<int>{8}));
    base.run(bars, 3);
    assert(p.preparations == 2 && p.allow_precalc);
    assert((p.observed == std::vector<int>{8, 9, 10}));

    p.prepared = false;
    base.run(bars, 3, "1", "1");
    assert(p.preparations == 3 && !p.allow_precalc);
    assert(p.configurations == 1);
    assert((p.observed == std::vector<int>{8, 9, 10}));
    base.run(bars, 3, "", "");
    assert(p.preparations == 4 && p.allow_precalc);
    base.run(bars, 3, "", "1");
    assert(p.preparations == 5 && !p.allow_precalc);
    base.run(bars, 3, "1", "");
    assert(p.preparations == 6 && !p.allow_precalc);
    base.run(bars, 3, "", "", true);
    assert(p.preparations == 7 && !p.allow_precalc);

    // A changed input persists and is resolved afresh, not reset to defaults.
    p.set_input("seed", "19");
    base.run(bars, 2, "1", "1");
    assert((p.observed == std::vector<int>{20, 21}));
    base.run(nullptr, 0);
    assert(p.observed.empty() && p.value == 19);
    const int before_failure = p.preparations;
    p.fail_preparation = true;
    base.run(bars, 3);
    assert(p.preparations == before_failure + 1);
    assert(p.observed.empty());
    p.fail_preparation = false;
    base.run(bars, 2);
    assert((p.observed == std::vector<int>{20, 21}));

    p.set_input("seed", "7");
    const int before_stream = p.preparations;
    assert(base.stream_begin(bars, 2, "1", "1"));
    assert(p.preparations == before_stream + 1 && !p.allow_precalc);
    assert((p.observed == std::vector<int>{8, 9}));
    assert(base.stream_push_tick(TradeTick{180000, 1, 12, 1}));
    assert(base.stream_advance_time(240000));
    assert(p.preparations == before_stream + 1);
    assert(p.observed.size() >= 3 && p.observed[2] == 10);
    assert(base.stream_end());
    assert(p.preparations == before_stream + 1);

    assert(base.stream_begin(bars, 2, "1", "1"));
    assert(p.preparations == before_stream + 2);
    assert((p.observed == std::vector<int>{8, 9}));
    assert(base.stream_end());
    base.run(bars, 3);
    assert(p.preparations == before_stream + 3 && p.allow_precalc);
    assert((p.observed == std::vector<int>{8, 9, 10}));
}
