#!/usr/bin/env python3
"""Metadata-only mutation tests for nested physical-lot hash coverage.

Every mutation is written under TemporaryDirectory; engine source is read only.
No compile, engine execution, strategy, corpus, feed or grader is involved.
"""
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
import unittest

import check_broker_state_hash_coverage as checker
from gen_pending_order_mirror import members

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/pineforge/engine.hpp").read_text()
EVENTS = (ROOT / "include/pineforge/broker_events.hpp").read_text()
SOURCE = (ROOT / "src/engine_state_hash.cpp").read_text()
WAIVERS = (ROOT / "scripts/broker_state_hash_waivers.txt").read_text()


class PhysicalLotCoverage(unittest.TestCase):
    def check(self, header=HEADER, source=SOURCE, waivers=WAIVERS, events=EVENTS):
        with tempfile.TemporaryDirectory(prefix="pf-lot-hash-check-") as temp:
            root = Path(temp)
            for name, content in [
                ("include/pineforge/engine.hpp", header),
                ("include/pineforge/broker_events.hpp", events),
                ("src/engine_state_hash.cpp", source),
                ("scripts/broker_state_hash_waivers.txt", waivers),
            ]:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
            output = StringIO()
            with redirect_stdout(output), redirect_stderr(output):
                try:
                    code = checker.main(root)
                except SystemExit as exc:
                    code = exc.code if isinstance(exc.code, int) else 2
                    output.write(str(exc.code))
            return code, output.getvalue()

    def test_actual_source_and_named_parser(self):
        self.assertEqual(self.check()[0], 0)
        self.assertEqual(len(members(HEADER, "PyramidEntry")), 18)
        self.assertEqual(members(HEADER), members(HEADER, "PendingOrder"))

    def test_each_lot_field_requires_its_own_fold(self):
        # A correct container loop is insufficient if any child is omitted.
        start = SOURCE.index("for (const auto& e : pyramid_entries_) {")
        end = SOURCE.index("\n    }", start)
        loop = SOURCE[start:end]
        for _type, name in members(HEADER, "PyramidEntry"):
            with self.subTest(member=name):
                altered = loop.replace(f"e.{name}", f"e.missing_{name}")
                code, output = self.check(source=SOURCE[:start] + altered + SOURCE[end:])
                self.assertEqual(code, 1, output)
                self.assertIn(name, output)

    def test_new_field_refuses_until_explicit_decision(self):
        header = HEADER.replace("struct PyramidEntry {", "struct PyramidEntry {\n    double future_margin_basis = 0;")
        code, output = self.check(header=header)
        self.assertEqual(code, 1, output)
        self.assertIn("future_margin_basis", output)

    def test_comments_reads_wrong_types_and_external_folds_do_not_cover(self):
        fold = "f.d(e.entry_commission_account);"
        for replacement in [
            "// " + fold,
            "const double ignored = e.entry_commission_account;",
            "f.u(e.entry_commission_account);",
        ]:
            with self.subTest(replacement=replacement):
                self.assertEqual(self.check(source=SOURCE.replace(fold, replacement))[0], 1)
        source = SOURCE.replace(fold, "") + "\nvoid unrelated() { f.d(e.entry_commission_account); }\n"
        self.assertEqual(self.check(source=source)[0], 1)

    def test_missing_duplicate_or_unbalanced_owner_loop_refuses(self):
        opening = "for (const auto& e : pyramid_entries_) {"
        self.assertEqual(self.check(source=SOURCE.replace(opening, "if (false) {"))[0], 2)
        self.assertEqual(self.check(source=SOURCE + "\n" + opening + "}\n")[0], 2)
        output = StringIO()
        with redirect_stderr(output), self.assertRaises(SystemExit) as stopped:
            checker._collection_loop_body(opening, "pyramid_entries_", "e")
        self.assertEqual(stopped.exception.code, 2)

    def test_explicit_waiver_is_checked_and_cannot_hide_typos(self):
        source = SOURCE.replace("f.b(e.market_pyramid_add);", "")
        waiver = "\npyramid_entry.market_pyramid_add # test-only lifecycle decision\n"
        self.assertEqual(self.check(source=source, waivers=WAIVERS + waiver)[0], 0)
        self.assertEqual(self.check(waivers=WAIVERS + waiver)[0], 1)  # now redundant
        self.assertEqual(self.check(waivers=WAIVERS + "\npyramid_entry.typo # no such field\n")[0], 1)
        self.assertEqual(self.check(waivers=WAIVERS + "\npyramid_entry.market_pyramid_add #\n")[0], 1)

    def test_unclassified_nested_declaration_refuses(self):
        for declaration in ["double first, second;", "double first = 0, second = 0;",
                            "std::vector<double> state;", "double value() const;"]:
            with self.subTest(declaration=declaration):
                header = HEADER.replace("struct PyramidEntry {", "struct PyramidEntry {\n    " + declaration)
                self.assertNotEqual(self.check(header=header)[0], 0)

    def test_pending_order_coverage_still_refuses_omission(self):
        source = SOURCE.replace("f.d(o.stop_price);", "")
        self.assertNotEqual(source, SOURCE)
        code, output = self.check(source=source)
        self.assertEqual(code, 1, output)
        self.assertIn("stop_price", output)

    def test_every_opening_owner_field_requires_its_own_fold(self):
        for _type, name in members(EVENTS, "OpeningOwner"):
            with self.subTest(member=name):
                source = SOURCE.replace(f"owner.{name}", f"owner.missing_{name}")
                code, output = self.check(source=source)
                self.assertEqual(code, 1, output)
                self.assertIn(name, output)
        events = EVENTS.replace("struct OpeningOwner {", "struct OpeningOwner {\n int64_t extra;")
        code, output = self.check(events=events)
        self.assertEqual(code, 1, output)
        self.assertIn("extra", output)

    def test_new_receipt_state_or_decision_alternative_requires_review(self):
        mutations = [
            ("OpeningOwner owner_;", "OpeningOwner owner_;\n double extra;"),
            ("std::optional<OpeningReceipt> pending_;", "std::optional<OpeningReceipt> pending_;\n bool extra_ = false;"),
            ("struct Check {", "struct Check { double extra;"),
            ("struct Exempt {}", "struct Exempt { double extra; }"),
            ("std::variant<Check, Exempt>", "std::variant<Check, Exempt, int>"),
            ("OpeningDecision { Check, Exempt }", "OpeningDecision { Check, Exempt, Extra }"),
            ("OpeningContinuation { None, RemainingAdversePath }", "OpeningContinuation { None, RemainingAdversePath, Extra }"),
        ]
        for before, after in mutations:
            with self.subTest(mutation=after):
                self.assertIn(before, EVENTS)
                self.assertNotEqual(self.check(events=EVENTS.replace(before, after))[0], 0)

    def test_opening_receipt_folds_are_unconditional_and_owned(self):
        fold = "f.d(receipt->raw_fill_base());"
        for replacement in ["// " + fold, "const auto ignored = receipt->raw_fill_base();",
                            "f.u(receipt->raw_fill_base());",
                            "if (receipt->decision() == broker::OpeningDecision::Check) " + fold]:
            with self.subTest(replacement=replacement):
                self.assertEqual(self.check(source=SOURCE.replace(fold, replacement))[0], 1)
        self.assertEqual(self.check(source=SOURCE.replace(fold, "") + "\n" + fold)[0], 1)
        self.assertEqual(self.check(source=SOURCE.replace("f.b(opening_obligations_.pending());", ""))[0], 1)
        self.assertEqual(self.check(source=SOURCE.replace("f.b(receipt->requires_adverse_pass());", ""))[0], 1)


if __name__ == "__main__":
    unittest.main()
