// SPDX-License-Identifier: Apache-2.0
#include "json.hpp"
#include <cassert>
#include <functional>
#include <iostream>
using namespace pineforge::live;
bool refuses(const std::function<void()> &fn) {
    try {
        fn();
        return false;
    } catch (const std::exception &) {
        return true;
    }
}
int main() {
    auto j = parse_json("{\"seq\":18446744073709551615,\"x\":[true,null,\"\\ud83d\\ude80\"]}");
    assert(j.at("seq").integer<std::uint64_t>() == UINT64_MAX);
    assert(parse_json(j.dump()).dump() == j.dump());
    assert(j.at("x").items[2].text() == "\xf0\x9f\x9a\x80");
    for (auto raw : {"{\"a\":1,\"a\":2}", "{\"a\":1,}", "[1,]", "01", "-.1", "1.", "1e",
                     "true false", "\"\\ud800\"", "\"\\udc00\"", "\"\xc0\xaf\""})
        assert(refuses([&] { parse_json(raw); }));
    assert(refuses([] { parse_json("1e999").real(); }));
    assert(refuses([] { parse_json("-1").integer<std::uint64_t>(); }));
    assert(refuses([] { parse_json("1.0").integer<std::uint64_t>(); }));
    assert(refuses([] { parse_json("18446744073709551616").integer<std::uint64_t>(); }));
    assert(refuses([] { parse_json(std::string(33, '[') + std::string(33, ']')); }));
    assert(refuses([] { parse_json(std::string(1024 * 1024 + 1, ' ')); }));
    assert(refuses([] { only_fields(parse_json("{\"bogus\":1}"), {"type"}); }));
    std::cout << "test_live_json: OK\n";
}
