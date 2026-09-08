#!/usr/bin/env python3
"""Link-only check: stale C++ subclass objects must not bind the new vtable.

Neither linked program is executed. The public C ABI is checked separately.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--generated-include", required=True)
    parser.add_argument("--extra-flag", action="append", default=[])
    args = parser.parse_args()
    caller = '''
int main(int argc, char** argv) {
    auto* strategy = reinterpret_cast<pineforge::BacktestEngine*>(argv);
    strategy->run(nullptr, argc);
    return 0;
}
'''
    current = '#include <pineforge/engine.hpp>\n' + caller
    legacy = '''namespace pineforge {
struct Bar;
class BacktestEngine { public: void run(const Bar*, int); };
}
''' + caller
    with tempfile.TemporaryDirectory(prefix="pf-script-cpp-abi-") as temporary:
        root = Path(temporary)
        def link(name, source):
            path = root / (name + ".cpp")
            path.write_text(source)
            return subprocess.run(
                [args.compiler, "-std=c++17", "-O0", "-I", args.include,
                 "-I", args.generated_include, *args.extra_flag, str(path),
                 args.library, "-pthread", "-o", str(root / name)],
                capture_output=True, text=True, timeout=60,
            )
        fresh = link("current", current)
        if fresh.returncode:
            raise RuntimeError("current C++ contract failed to link:\n" + fresh.stderr)
        stale = link("legacy", legacy)
        if stale.returncode == 0:
            raise RuntimeError("legacy unversioned C++ run symbol still links to the new runtime")
        if "undefined" not in stale.stderr.lower() or "BacktestEngine" not in stale.stderr:
            raise RuntimeError("legacy link failed for an unexpected reason:\n" + stale.stderr)
    print("current C++ contract links; stale object rejected; no executable run")


if __name__ == "__main__":
    main()
