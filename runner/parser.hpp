// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pineforge/live_parser.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pineforge::live {

using ParsedEvent = pf_live_parser_event_v1_t;

// Loads a trusted C ABI parser plugin (.so on Linux, .dylib on macOS).
// Each parse stages a complete message before returning any output. The caller
// stores canonical normalized events and pins plugin/config identity separately.
// A parser instance is not intended for concurrent calls.
class Parser {
public:
    explicit Parser(std::string path, std::string config_json = "{}");
    ~Parser();
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;
    Parser(Parser&&) noexcept;
    Parser& operator=(Parser&&) noexcept;

    std::vector<ParsedEvent> parse(std::string_view message) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pineforge::live
