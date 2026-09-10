#!/usr/bin/env python3
"""Compile/link-only checks for the internal generated/native C++ pairing.

Exact base38 headers are a frozen fixture: no Git history or network is needed.
Every translation unit must compile before expected linker failures are tested.
Neither a strategy nor any produced executable is run. C ABI checks are separate.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


BASE_COMMIT = "38dc73e5503fe5395458e5f8df2a2ad78054a1ae"
BASE_ENGINE_SHA256 = "06c937a1ccd31815ca7775268ac699ffdfddb1a1f19de4628b777f37e9a6d193"
CURRENT_NAMESPACE = "engine_script_run_v5"
BASE_NAMESPACE = "engine_script_run_v2"
FIXTURE = Path(__file__).resolve().parents[1] / "tests/fixtures/script_cpp_abi/base38"


def entry_diagnostic(lines, namespace, method):
    # "run" also occurs inside engine_script_run_vN. Match the qualified
    # method itself, so a missing fill_report cannot masquerade as missing run.
    owner = "pineforge::" + (namespace + "::" if namespace else "")
    needle = owner + "BacktestEngine::" + method + "("
    return next((line for line in lines if needle in line), None)


def frozen_headers(destination, fixture=FIXTURE, commit=BASE_COMMIT,
                   namespace=BASE_NAMESPACE, engine_sha=BASE_ENGINE_SHA256):
    """Authenticate and unpack the exact tracked base38 header closure."""
    manifest = json.loads((fixture / "manifest.json").read_text())
    if (manifest["source_commit"] != commit
            or manifest["internal_namespace"] != namespace
            or manifest["files"]["pineforge/engine.hpp"]["sha256"] != engine_sha):
        raise RuntimeError("stale-header fixture does not identify the pinned base38 contract")
    archive = (fixture / "headers.json.gz").read_bytes()
    if hashlib.sha256(archive).hexdigest() != manifest["archive_sha256"]:
        raise RuntimeError("stale-header fixture archive digest mismatch")
    contents = json.loads(gzip.decompress(archive))
    if contents.keys() != manifest["files"].keys():
        raise RuntimeError("stale-header fixture file set mismatch")
    for name, content in contents.items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or relative.parts[0] != "pineforge":
            raise RuntimeError("invalid stale-header fixture path: " + name)
        raw = content.encode()
        blob = b"blob " + str(len(raw)).encode() + b"\0" + raw
        expected = manifest["files"][name]
        if (hashlib.sha256(raw).hexdigest() != expected["sha256"]
                or hashlib.sha1(blob).hexdigest() != expected["git_blob"]):
            raise RuntimeError("stale-header fixture digest mismatch: " + name)
        path = destination / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)


def caller(namespace, generated=False):
    header = f'''#include <pineforge/engine.hpp>
#include <type_traits>
static_assert(std::is_same<pineforge::BacktestEngine,
              pineforge::{namespace}::BacktestEngine>::value,
              "unexpected internal C++ namespace");
'''
    if generated:
        # Shape of supported codegen c8ffe587 emit_top.py's entry wrappers.
        # No Pine source is compiled and no GeneratedStrategy is instantiated.
        return header + '''
class GeneratedStrategy final : public pineforge::BacktestEngine {
    void on_bar(const pineforge::Bar&) override {}
    void prepare_script_run(const pineforge::Bar*, int, bool) override {}
};
extern "C" void pairing_generated_run(void* handle, pineforge::Bar* bars,
                                      int count, pineforge::ReportC* report) {
    auto* strategy = static_cast<GeneratedStrategy*>(handle);
    strategy->run(bars, count);
    strategy->run(bars, count, "", "", false, 4,
                  pineforge::MagnifierDistribution::ENDPOINTS);
    strategy->fill_report(report);
}
int main(int argc, char** argv) {
    pairing_generated_run(argv, nullptr, argc, nullptr);
    return 0;
}
'''
    return header + '''
int main(int argc, char** argv) {
    auto* strategy = reinterpret_cast<pineforge::BacktestEngine*>(argv);
    strategy->run(nullptr, argc);
    strategy->run(nullptr, argc, "", "", {}, pineforge::SymInfo{});
    strategy->fill_report(nullptr);
    return 0;
}
'''


# Only old entry-point symbols, compiled against the exact frozen old header.
# This is a linker control, NOT a historical runtime or economic simulation.
BASE_SYMBOL_CONTROL = '''#include <pineforge/engine.hpp>
namespace pineforge { namespace engine_script_run_v2 {
void BacktestEngine::run(const Bar*, int) {}
void BacktestEngine::run(const Bar*, int, const std::string&, const std::string&,
                        bool, int, MagnifierDistribution) {}
void BacktestEngine::run(const Bar*, int, const std::string&, const std::string&,
                        const std::unordered_map<std::string, std::string>&,
                        const SymInfo&, const StrategyOverrides*, bool, int,
                        MagnifierDistribution) {}
void BacktestEngine::fill_report(ReportC*) const {}
}}
'''
LEGACY_CALLER = '''namespace pineforge {
struct Bar;
class BacktestEngine { public: void run(const Bar*, int); };
}
int main(int argc, char** argv) {
    auto* strategy = reinterpret_cast<pineforge::BacktestEngine*>(argv);
    strategy->run(nullptr, argc);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--generated-include", required=True)
    parser.add_argument("--extra-flag", action="append", default=[])
    args = parser.parse_args()
    # Literal diagnostic controls guard the link-failure parser itself.
    for namespace in (BASE_NAMESPACE, CURRENT_NAMESPACE):
        report_only = [f"undefined pineforge::{namespace}::BacktestEngine::fill_report(pineforge::ReportC*) const"]
        if (entry_diagnostic(report_only, namespace, "run") is not None
                or entry_diagnostic(report_only, namespace, "fill_report") is None):
            raise RuntimeError("link diagnostics confuse namespace and method names")
    print("qualified-method diagnostic controls passed")
    with tempfile.TemporaryDirectory(prefix="pf-script-cpp-abi-") as temporary:
        root = Path(temporary)
        old_include = root / "base38/include"
        frozen_headers(old_include)
        cap_include = root / "basef864/include"
        frozen_headers(cap_include, FIXTURE.parent / "basef864",
                       "f864be590931ba08c8df5af983b33b2c29be9c67", "engine_script_run_v3",
                       "54b35fffaa163a31467f8ba883e44e02f013a37216b558dfe3a4f28fbbe84dc2")
        prior_include = root / "basec45/include"
        frozen_headers(prior_include, FIXTURE.parent / "basec45",
                       "c45cf5a4d0e67a2ac098d9066977e1fa21c408a9", "engine_script_run_v4",
                       "3b4e2937a9b5f275dd119144373b1bf15e433092009500092cd32ea34963b293")
        common = [args.compiler, "-std=c++17", "-O0", *args.extra_flag]

        def compile_object(name, source, include):
            path = root / (name + ".cpp")
            path.write_text(source)
            obj = root / (name + ".o")
            compiled = subprocess.run(
                [*common, "-I", str(include), "-I", args.generated_include,
                 "-c", str(path), "-o", str(obj)],
                capture_output=True, text=True, timeout=60,
            )
            if compiled.returncode:
                raise RuntimeError(name + " failed to compile (not a pairing rejection):\n"
                                   + compiled.stderr)
            return obj

        current_native = compile_object("current_native", caller(CURRENT_NAMESPACE), args.include)
        current_generated = compile_object("current_generated", caller(CURRENT_NAMESPACE, True), args.include)
        stale_native = compile_object("base38_native", caller(BASE_NAMESPACE), old_include)
        stale_generated = compile_object("base38_generated", caller(BASE_NAMESPACE, True), old_include)
        old_symbols = compile_object("base38_symbol_control", BASE_SYMBOL_CONTROL, old_include)
        legacy = compile_object("legacy_unversioned", LEGACY_CALLER, old_include)

        cap_native = compile_object("basef864_native", caller("engine_script_run_v3"), cap_include)
        cap_generated = compile_object("basef864_generated", caller("engine_script_run_v3", True), cap_include)
        cap_symbols = compile_object("basef864_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v3"), cap_include)

        prior_native = compile_object("basec45_native", caller("engine_script_run_v4"), prior_include)
        prior_generated = compile_object("basec45_generated", caller("engine_script_run_v4", True), prior_include)
        prior_symbols = compile_object("basec45_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v4"), prior_include)

        priority_caller = """#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
int main() {
    pineforge::compat::pine::OrderPriority policy;
    policy.attach();
    pineforge::compat::pine::OrderPriorityContext context{};
    std::vector<pineforge::PendingOrder> orders(2);
    return policy.select(context, orders).has_value() ? 1 : 0;
}
"""
        priority_symbols = """#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
namespace pineforge::compat::pine {
std::optional<broker::OrderPriorityDecision> OrderPriority::select(
    const OrderPriorityContext&, const std::vector<PendingOrder>&) const { return std::nullopt; }
}
"""
        current_priority = compile_object("current_pending_priority", priority_caller, args.include)
        prior_priority = compile_object("basec45_pending_priority", priority_caller, prior_include)
        prior_priority_symbols = compile_object("basec45_pending_priority_symbols", priority_symbols, prior_include)

        def link(name, obj, runtime, missing_namespace=None):
            linked = subprocess.run(
                [*common, str(obj), str(runtime), "-pthread", "-o", str(root / name)],
                capture_output=True, text=True, timeout=60,
            )
            if missing_namespace is None:
                if linked.returncode:
                    raise RuntimeError(name + " positive control failed to link:\n" + linked.stderr)
                print(name + ": linked (not executed)")
                return
            if not linked.returncode:
                raise RuntimeError(name + " stale C++ pairing unexpectedly linked")
            # Require the missing engine entry symbol, not an arbitrary linker
            # failure (missing library, compiler flags, unrelated dependency).
            required_methods = ("run", "fill_report") if missing_namespace else ("run",)
            entries = {method: entry_diagnostic(linked.stderr.splitlines(), missing_namespace, method)
                       for method in required_methods}
            if ("undefined" not in linked.stderr.lower()
                    or any(line is None for line in entries.values())):
                raise RuntimeError(name + " failed for an unexpected reason:\n" + linked.stderr)
            print(name + ": rejected missing pineforge::"
                  + (missing_namespace + "::" if missing_namespace else "")
                  + "BacktestEngine entry symbols")
            for method in required_methods:
                print("  " + entries[method].strip())

        link("current_native_to_current", current_native, args.library)
        link("current_generated_to_current", current_generated, args.library)
        link("base38_native_to_v2_symbol_control", stale_native, old_symbols)
        link("base38_generated_to_v2_symbol_control", stale_generated, old_symbols)
        link("base38_native_to_current", stale_native, args.library, BASE_NAMESPACE)
        link("base38_generated_to_current", stale_generated, args.library, BASE_NAMESPACE)
        link("current_native_to_v2_symbol_control", current_native, old_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v2_symbol_control", current_generated, old_symbols, CURRENT_NAMESPACE)
        link("unversioned_to_current", legacy, args.library, "")
        link("basef864_native_to_v3_symbol_control", cap_native, cap_symbols)
        link("basef864_generated_to_v3_symbol_control", cap_generated, cap_symbols)
        link("basef864_native_to_current", cap_native, args.library, "engine_script_run_v3")
        link("basef864_generated_to_current", cap_generated, args.library, "engine_script_run_v3")
        link("current_native_to_v3_symbol_control", current_native, cap_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v3_symbol_control", current_generated, cap_symbols, CURRENT_NAMESPACE)
        link("basec45_native_to_v4_symbol_control", prior_native, prior_symbols)
        link("basec45_generated_to_v4_symbol_control", prior_generated, prior_symbols)
        link("basec45_native_to_current", prior_native, args.library, "engine_script_run_v4")
        link("basec45_generated_to_current", prior_generated, args.library, "engine_script_run_v4")
        link("current_native_to_v4_symbol_control", current_native, prior_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v4_symbol_control", current_generated, prior_symbols, CURRENT_NAMESPACE)
        link("current_pending_priority_to_current", current_priority, args.library)
        link("basec45_pending_priority_to_v4_symbols", prior_priority, prior_priority_symbols)
        for name, obj, runtime, expected in [
            ("basec45_pending_priority_to_current", prior_priority, args.library, "pineforge::PendingOrder"),
            ("current_pending_priority_to_v4_symbols", current_priority, prior_priority_symbols,
             "pineforge::engine_script_run_v5::PendingOrder"),
        ]:
            result = subprocess.run([*common, str(obj), str(runtime), "-pthread", "-o", str(root / name)],
                                    capture_output=True, text=True, timeout=60)
            if (result.returncode == 0 or "undefined" not in result.stderr.lower()
                    or "OrderPriority::select(" not in result.stderr or expected not in result.stderr):
                raise RuntimeError(name + " did not reject the expected PendingOrder type: " + result.stderr)
            print(name + ": rejected stale standalone PendingOrder argument type (not executed)")
    print("15 translation units compiled; 10 positive links; 15 rejected links; no executable run")


if __name__ == "__main__":
    main()
