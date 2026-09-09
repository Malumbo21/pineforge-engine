// SPDX-License-Identifier: Apache-2.0
// Illustrative provider-neutral mapping, not an exchange protocol:
//   trade,<source sequence>,<Unix milliseconds>,<price>,<quantity>
//   heartbeat
// One complete message per call. No state, I/O, or configuration is needed.
// Example Linux build:
//   c++ -std=c++17 -shared -fPIC -Iinclude runner/examples/demo_parser.cpp -o demo_parser.so
// macOS: replace -shared with -dynamiclib and use demo_parser.dylib.
#include <pineforge/live_parser.h>

#include <array>
#include <charconv>
#include <cmath>
#include <string_view>

namespace {
template<class T> bool number(std::string_view text, T& value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
}

extern "C" PF_LIVE_PARSER_API uint32_t pf_live_parser_abi_version(void) {
    return PF_LIVE_PARSER_ABI_VERSION;
}

extern "C" PF_LIVE_PARSER_API int pf_live_parse_message(
    const char* message, size_t message_size,
    const char* config_json, size_t config_size,
    pf_live_parser_emit_v1_fn emit, void* user) {
    if ((!message && message_size) || (!config_json && config_size) || !emit
        || message_size > PF_LIVE_PARSER_MAX_MESSAGE_BYTES) return -1;
    const std::string_view text(message ? message : "", message_size);
    if (text == "heartbeat") return 0;
    std::array<std::string_view, 5> cells{};
    size_t offset = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto end = text.find(',', offset);
        if ((i + 1 == cells.size()) != (end == std::string_view::npos)) return -1;
        cells[i] = text.substr(offset, end == std::string_view::npos ? end : end - offset);
        if (end != std::string_view::npos) offset = end + 1;
    }
    pf_live_parser_event_v1_t event{};
    event.kind = PF_LIVE_PARSER_TICK;
    if (cells[0] != "trade" || !number(cells[1], event.sequence)
        || !number(cells[2], event.timestamp) || !number(cells[3], event.price)
        || !number(cells[4], event.quantity) || !event.sequence || event.timestamp < 0
        || !std::isfinite(event.price) || event.price <= 0
        || !std::isfinite(event.quantity) || event.quantity <= 0) return -1;
    return emit(&event, user) == 0 ? 0 : -1;
}
