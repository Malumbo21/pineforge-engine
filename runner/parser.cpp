// SPDX-License-Identifier: Apache-2.0
#include "parser.hpp"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pineforge::live {
namespace {

static_assert(std::is_standard_layout<ParsedEvent>::value, "parser ABI must be POD");
static_assert(std::is_trivial<ParsedEvent>::value, "parser ABI must be POD");

enum class EmitFailure { none, invalid_event, too_many_events, allocation };

struct Emission {
    std::vector<ParsedEvent> events;
    EmitFailure failure = EmitFailure::none;
};

bool positive(double value) { return std::isfinite(value) && value > 0; }

bool normalize(const ParsedEvent& event, ParsedEvent& result) {
    if (event.reserved != 0 || event.timestamp < 0) return false;
    result = {};
    result.kind = event.kind;
    result.timestamp = event.timestamp;
    switch (event.kind) {
    case PF_LIVE_PARSER_TICK:
        if (!event.sequence || !positive(event.price) || !positive(event.quantity)) return false;
        result.sequence = event.sequence;
        result.price = event.price;
        result.quantity = event.quantity;
        return true;
    case PF_LIVE_PARSER_BAR:
        if (event.timestamp % 60000 != 0
            || event.timestamp > std::numeric_limits<std::int64_t>::max() - 60000
            || !positive(event.open) || !positive(event.high)
            || !positive(event.low) || !positive(event.close)
            || !std::isfinite(event.volume) || event.volume < 0
            || event.high < std::max(event.open, event.close)
            || event.low > std::min(event.open, event.close)
            || event.high < event.low) return false;
        result.open = event.open;
        result.high = event.high;
        result.low = event.low;
        result.close = event.close;
        result.volume = event.volume == 0 ? 0 : event.volume;
        return true;
    case PF_LIVE_PARSER_TIME:
        return true;
    default:
        return false;
    }
}

int collect(const ParsedEvent* event, void* user) noexcept {
    auto& emission = *static_cast<Emission*>(user);
    if (emission.failure != EmitFailure::none) return -1;
    if (emission.events.size() >= PF_LIVE_PARSER_MAX_EVENTS
        || emission.events.size() >= PF_LIVE_PARSER_MAX_OUTPUT_BYTES / sizeof(ParsedEvent)) {
        emission.failure = EmitFailure::too_many_events;
        return -1;
    }
    ParsedEvent normalized{};
    if (!event || !normalize(*event, normalized)) {
        emission.failure = EmitFailure::invalid_event;
        return -1;
    }
    try {
        emission.events.push_back(normalized);
    } catch (...) {
        emission.failure = EmitFailure::allocation;
        return -1;
    }
    return 0;
}

template<class Function>
Function symbol(void* library, const char* name) {
    dlerror();
    void* address = dlsym(library, name);
    const char* error = dlerror();
    if (error || !address) throw std::runtime_error(std::string("parser plugin lacks ABI symbol: ") + name);
    return reinterpret_cast<Function>(address);
}

} // namespace

struct Parser::Impl {
    void* library = nullptr;
    decltype(&pf_live_parse_message) parse = nullptr;
    std::string config;

    ~Impl() { if (library) dlclose(library); }
};

Parser::Parser(std::string path, std::string config_json) : impl_(std::make_unique<Impl>()) {
    if (path.empty() || path.find('\0') != std::string::npos)
        throw std::runtime_error("parser plugin path is invalid");
    if (config_json.size() > PF_LIVE_PARSER_MAX_MESSAGE_BYTES)
        throw std::runtime_error("parser configuration exceeds 1 MiB");
    try {
        if (parse_json(config_json).kind != Json::Kind::Object)
            throw std::runtime_error("configuration must be an object");
    } catch (...) {
        // Never include provider configuration (which may carry credentials).
        throw std::runtime_error("parser configuration must be a valid JSON object");
    }
    impl_->config = std::move(config_json);
    // Resolve even a bare filename against cwd, not the loader's library search
    // path, so the caller fingerprints the same artifact that is actually loaded.
    const auto absolute = std::filesystem::absolute(path).string();
    impl_->library = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!impl_->library) throw std::runtime_error("cannot load parser plugin");
    const auto version = symbol<decltype(&pf_live_parser_abi_version)>(impl_->library, "pf_live_parser_abi_version");
    std::uint32_t abi = 0;
    try { abi = version(); }
    catch (...) { throw std::runtime_error("parser ABI version function threw an exception"); }
    if (abi != PF_LIVE_PARSER_ABI_VERSION) throw std::runtime_error("parser plugin ABI version mismatch");
    impl_->parse = symbol<decltype(impl_->parse)>(impl_->library, "pf_live_parse_message");
}

Parser::~Parser() = default;
Parser::Parser(Parser&&) noexcept = default;
Parser& Parser::operator=(Parser&&) noexcept = default;

std::vector<ParsedEvent> Parser::parse(std::string_view message) const {
    if (!impl_) throw std::runtime_error("parser was moved from");
    if (message.size() > PF_LIVE_PARSER_MAX_MESSAGE_BYTES)
        throw std::runtime_error("parser message exceeds 1 MiB");
    Emission emission;
    int status = -1;
    try {
        status = impl_->parse(message.empty() ? "" : message.data(), message.size(),
                              impl_->config.data(), impl_->config.size(), collect, &emission);
    } catch (...) {
        // A conforming C plugin never throws; avoid leaking partially parsed
        // data or provider error text if a C++ plugin violates that contract.
        throw std::runtime_error("parser plugin threw an exception");
    }
    switch (emission.failure) {
    case EmitFailure::invalid_event: throw std::runtime_error("parser emitted an invalid normalized event");
    case EmitFailure::too_many_events: throw std::runtime_error("parser output exceeds the event/byte limit");
    case EmitFailure::allocation: throw std::runtime_error("cannot allocate parser output");
    case EmitFailure::none: break;
    }
    if (status != 0) throw std::runtime_error("parser rejected provider message");
    return std::move(emission.events);
}

} // namespace pineforge::live
