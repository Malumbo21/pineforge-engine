// SPDX-License-Identifier: Apache-2.0
// A hand-written native strategy. Codegen output exposes the same lifecycle.
#include <pineforge/engine.hpp>

class NativeExample final : public pineforge::BacktestEngine {
  public:
    NativeExample() {
        initial_capital_ = 100000;
        default_qty_value_ = 1;
    }
    void on_bar(const pineforge::Bar &) override {
        // Alternating market entry/close gives a small deterministic test.
        if (bar_index_ % 4 == 1)
            strategy_entry("Long", true);
        if (bar_index_ % 4 == 3)
            strategy_close_all();
    }
};
extern "C" {
PF_API void *strategy_create(const char *) {
    try {
        return new NativeExample;
    } catch (...) {
        return nullptr;
    }
}
PF_API void strategy_free(void *s) { delete static_cast<NativeExample *>(s); }
PF_API void strategy_set_input(void *s, const char *key, const char *value) {
    static_cast<NativeExample *>(s)->set_input(key, value);
}
PF_API void strategy_set_override(void *, const char *, const char *) {}
// Retain the core object that exports native streaming symbols from the static
// engine library; no per-strategy streaming implementation is necessary.
PF_API int native_example_abi_version() { return pf_abi_version(); }
}
