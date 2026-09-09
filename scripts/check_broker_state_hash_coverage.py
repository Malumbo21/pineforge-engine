#!/usr/bin/env python3
"""CI gate: every member declared in any ``// @broker-state begin`` ..
``// @broker-state end`` region in include/pineforge/engine.hpp is either
referenced by name in src/engine_state_hash.cpp (outside comments) or listed
(with a non-empty reason) in scripts/broker_state_hash_waivers.txt. A new
broker-state member that is neither fails the build (spec §3.4). Multiple
marker pairs are supported (e.g. one around the main position/order/risk
block, a second, tighter pair around an isolated member declared far away
in the class).

The same rule covers ``struct PendingOrder`` (task 7, carried from the task-5
review): every scalar/string member -- the list is reflected by
scripts/gen_pending_order_mirror.py's parser, the one source of truth for
what PendingOrder declares -- must be referenced as ``o.<name>`` inside the
hash function's ``for (const auto& o : pending_orders_)`` loop or waived as
``pending_order.<name>  # reason`` in the waivers file.

Nested physical lots are checked separately: every PyramidEntry member must
have its type-appropriate f.<fold>(e.<name>) inside the pyramid_entries_ loop,
or a justified pyramid_entry.<name> waiver. Merely hashing the container name,
mentioning a member, or hashing it outside its owning loop does not cover it.
Both member lists use the same fail-closed named-struct parser."""
from __future__ import annotations
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_pending_order_mirror import members as struct_members  # noqa: E402

PENDING_WAIVER_PREFIX = "pending_order."
PYRAMID_WAIVER_PREFIX = "pyramid_entry."
PYRAMID_FOLD = {
    "double": "d", "int": "i", "int64_t": "i", "uint64_t": "u",
    "bool": "b", "std::string": "s",
}

MEMBER_RE = re.compile(r"^\s+[\w:<>, ]+?\s+(\w+_)\s*(?:=|;|\{)", re.M)
# The marker must be the whole (trimmed) line -- not merely a substring, so a
# prose mention like "the ``// @broker-state begin`` marker" in an unrelated
# comment can never be parsed as a real region boundary.
REGION_RE = re.compile(
    r"^[ \t]*// @broker-state begin[ \t]*$(.*?)"
    r"^[ \t]*// @broker-state end[ \t]*$",
    re.M | re.S,
)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_cpp_comments(src: str) -> str:
    """Strip ``//`` and ``/* */`` comments so a bare mention of a member's
    name in an explanatory comment (e.g. "the per-PASS working state
    (dual_entry_path_) is waived") does not count as hashing it. Good enough
    for this codebase's actual content: no ``//`` or ``/*`` appears inside a
    string/char literal in engine_state_hash.cpp."""
    return LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub("", src))


def _regions(hpp: str) -> list[str]:
    regions = REGION_RE.findall(hpp)
    if not regions:
        print("check_broker_state_hash_coverage: no // @broker-state begin/end "
              "region found in engine.hpp", file=sys.stderr)
        sys.exit(2)
    return regions


def _members(regions: list[str]) -> set[str]:
    members: set[str] = set()
    for region in regions:
        members |= set(MEMBER_RE.findall(region))
    return members


def _load_waivers(path: Path) -> dict[str, str]:
    waivers: dict[str, str] = {}
    for lineno, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "#" not in raw_line:
            print(f"check_broker_state_hash_coverage: waiver line {lineno} has no "
                  f"'# reason': {raw_line!r}", file=sys.stderr)
            sys.exit(1)
        name, reason = raw_line.split("#", 1)
        name = name.strip()
        reason = reason.strip()
        if not name:
            # A line that is entirely whitespace before '#' is a stray/typo
            # line, not a real waiver -- never silently absorb it as "".
            continue
        if not reason:
            print(f"check_broker_state_hash_coverage: waiver for {name!r} "
                  f"(line {lineno}) has no reason after '#'", file=sys.stderr)
            sys.exit(1)
        waivers[name] = reason
    return waivers


def _collection_loop_body(src: str, collection: str, variable: str) -> str:
    """Brace-balanced body of one owning collection loop, comments stripped."""
    loop_re = re.compile(
        rf"for\s*\(\s*const\s+auto&\s+{re.escape(variable)}\s*:\s*"
        rf"{re.escape(collection)}\s*\)\s*\{{")
    matches = list(loop_re.finditer(src))
    if not matches:
        print("check_broker_state_hash_coverage: could not find "
              f"`for (const auto& {variable} : {collection}) {{` in engine_state_hash.cpp",
              file=sys.stderr)
        sys.exit(2)
    if len(matches) > 1:
        print("check_broker_state_hash_coverage: found "
              f"{len(matches)} `for (const auto& {variable} : {collection}) {{` loops in "
              "engine_state_hash.cpp; expected exactly one (the nested coverage "
              "rule inspects a single loop body)", file=sys.stderr)
        sys.exit(2)
    depth, start = 1, matches[0].end()
    for i in range(start, len(src)):
        ch = src[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[start:i]
    print(f"check_broker_state_hash_coverage: unbalanced {collection} loop", file=sys.stderr)
    sys.exit(2)


def _pyramid_folded(loop: str, cpp_type: str, member: str) -> bool:
    fold = PYRAMID_FOLD.get(cpp_type)
    if fold is None:
        return False
    # Require an actual typed serialization call, not a read/assignment or an
    # unrelated reference in the same source. Existing integer casts are fine.
    value = rf"e\.{re.escape(member)}"
    if fold == "i":
        value = rf"(?:{value}|static_cast<int64_t>\(\s*{value}\s*\))"
    return re.search(rf"\bf\.{fold}\(\s*{value}\s*\)\s*;", loop) is not None


def main(root: Path = ROOT) -> int:
    hpp = (root / "include/pineforge/engine.hpp").read_text(encoding="utf-8")
    regions = _regions(hpp)
    members = _members(regions)

    src_raw = (root / "src/engine_state_hash.cpp").read_text(encoding="utf-8")
    src = _strip_cpp_comments(src_raw)

    all_waivers = _load_waivers(root / "scripts/broker_state_hash_waivers.txt")
    waivers = {k: v for k, v in all_waivers.items()
               if not k.startswith((PENDING_WAIVER_PREFIX, PYRAMID_WAIVER_PREFIX))}
    po_waivers = {k[len(PENDING_WAIVER_PREFIX):]: v
                  for k, v in all_waivers.items() if k.startswith(PENDING_WAIVER_PREFIX)}
    pe_waivers = {k[len(PYRAMID_WAIVER_PREFIX):]: v
                  for k, v in all_waivers.items() if k.startswith(PYRAMID_WAIVER_PREFIX)}

    orphans = sorted(w for w in waivers if w not in members)
    if orphans:
        print("check_broker_state_hash_coverage: waiver(s) naming a member not "
              f"in any // @broker-state region: {orphans}", file=sys.stderr)
        return 1

    missing = sorted(
        m for m in members
        if not re.search(rf"\b{re.escape(m)}\b", src) and m not in waivers
    )
    if missing:
        print("check_broker_state_hash_coverage: unhashed, unwaived broker-state members:", missing)
        return 1

    # --- struct PendingOrder: every scalar/string member, o.<name> in the loop ---
    po_members = [n for _t, n in struct_members(hpp)]
    po_orphans = sorted(w for w in po_waivers if w not in po_members)
    if po_orphans:
        print("check_broker_state_hash_coverage: pending_order.* waiver(s) naming a "
              f"member not in struct PendingOrder: {po_orphans}", file=sys.stderr)
        return 1
    loop = _collection_loop_body(src, "pending_orders_", "o")
    po_missing = sorted(
        m for m in po_members
        if not re.search(rf"\bo\.{re.escape(m)}\b", loop) and m not in po_waivers
    )
    if po_missing:
        print("check_broker_state_hash_coverage: PendingOrder members neither hashed "
              "(o.<name> in the pending_orders_ loop) nor waived (pending_order.<name>):",
              po_missing)
        return 1
    # --- struct PyramidEntry: inspect every physical-lot field recursively ---
    pe_members = struct_members(hpp, "PyramidEntry")
    pe_orphans = sorted(set(pe_waivers) - {n for _t, n in pe_members})
    if pe_orphans:
        print("check_broker_state_hash_coverage: pyramid_entry.* waiver(s) naming a "
              f"member not in struct PyramidEntry: {pe_orphans}", file=sys.stderr)
        return 1
    pe_loop = _collection_loop_body(src, "pyramid_entries_", "e")
    pe_missing = sorted(n for t, n in pe_members
                        if n not in pe_waivers and not _pyramid_folded(pe_loop, t, n))
    pe_redundant = sorted(n for t, n in pe_members
                          if n in pe_waivers and _pyramid_folded(pe_loop, t, n))
    if pe_missing or pe_redundant:
        print("check_broker_state_hash_coverage: PyramidEntry requires one typed fold "
              "inside its loop or a justified pyramid_entry.* waiver; "
              f"missing={pe_missing}, redundant_waivers={pe_redundant}")
        return 1
    print(f"check_broker_state_hash_coverage: {len(members)} members in {len(regions)} "
          f"region(s), {len(waivers)} waived, OK; PendingOrder {len(po_members)} members, "
          f"{len(po_waivers)} waived, OK; PyramidEntry {len(pe_members)} members, "
          f"{len(pe_waivers)} waived, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
