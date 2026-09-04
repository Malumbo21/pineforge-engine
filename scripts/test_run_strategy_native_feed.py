#!/usr/bin/env python3
"""Focused runner proof for native higher-timeframe request.security feeds.

``inputs.json::native_security_feeds`` maps a requested timeframe ("D") to the
exchange's own OHLCV export of that timeframe; the runner hashes each file at
metadata time, loads it once, installs it through
``strategy_set_native_security_feed`` before the chart run, and records every
identity in the report and the runtime provenance.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from run_strategy import (
    Strategy,
    build_fingerprint,
    build_runtime_provenance,
    inputs_run_kwargs,
)


def _write_bars(path: Path, closes: list[float], step_ms: int = 60_000) -> None:
    rows = ["timestamp,open,high,low,close,volume"]
    for index, close in enumerate(closes):
        timestamp = 1_704_205_800_000 + index * step_ms
        rows.append(
            f"{timestamp},{close},{close + 1},{close - 1},{close},10")
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")


class _FakeLibrary:
    def __init__(self, *, native_result: int = 0) -> None:
        self.events = []
        self.native_result = native_result

    def strategy_create(self, _params):
        self.events.append(("create",))
        return 7

    def run_backtest_full(self, _state, bars, count, *_rest):
        self.events.append((
            "run", count,
            [float(bars[index].close) for index in range(count)],
        ))

    def strategy_get_last_error(self, _state):
        return b"rejected native feed" if self.native_result != 0 else b""

    def report_free(self, _report):
        pass

    def strategy_free(self, _state):
        pass

    def strategy_set_native_security_feed(self, _state, timeframe, bars, count):
        self.events.append((
            "native", timeframe.decode(), count,
            [float(bars[index].close) for index in range(count)],
        ))
        return self.native_result


class _FeatureAbsentLibrary(_FakeLibrary):
    def __getattribute__(self, name):
        if name == "strategy_set_native_security_feed":
            raise AttributeError(name)
        return super().__getattribute__(name)


def _strategy(fake) -> Strategy:
    strategy = Strategy.__new__(Strategy)
    strategy.lib = fake
    return strategy


class NativeSecurityFeedRunnerTests(unittest.TestCase):
    def test_metadata_resolves_relative_exports_and_hashes_them(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            daily = root / "daily.csv"
            _write_bars(chart, [100.0])
            _write_bars(daily, [111.5], step_ms=86_400_000)
            _, kwargs = inputs_run_kwargs(
                {"native_security_feeds": {"D": "daily.csv"}}, root, chart)
            feeds = kwargs["native_security_feeds"]
            self.assertEqual(list(feeds), ["D"])
            self.assertEqual(feeds["D"]["path"], daily.resolve())
            self.assertEqual(len(feeds["D"]["source_file_sha256"]), 64)

            _, legacy = inputs_run_kwargs({}, root, chart)
            self.assertNotIn("native_security_feeds", legacy)
            _, empty = inputs_run_kwargs(
                {"native_security_feeds": {}}, root, chart)
            self.assertNotIn("native_security_feeds", empty)

            with self.assertRaises(FileNotFoundError):
                inputs_run_kwargs(
                    {"native_security_feeds": {"D": "missing.csv"}}, root, chart)
            with self.assertRaises(ValueError):
                inputs_run_kwargs(
                    {"native_security_feeds": ["daily.csv"]}, root, chart)
            with self.assertRaises(ValueError):
                inputs_run_kwargs(
                    {"native_security_feeds": {"": "daily.csv"}}, root, chart)

    def test_feed_identity_mutates_runtime_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            daily_a = root / "daily-a.csv"
            daily_b = root / "daily-b.csv"
            _write_bars(chart, [100.0])
            _write_bars(daily_a, [111.5, 222.5], step_ms=86_400_000)
            _write_bars(daily_b, [111.5, 222.5], step_ms=86_400_000)

            _, kwargs_a = inputs_run_kwargs(
                {"native_security_feeds": {"D": "daily-a.csv"}}, root, chart)
            _, kwargs_b = inputs_run_kwargs(
                {"native_security_feeds": {"D": "daily-b.csv"}}, root, chart)
            _, kwargs_w = inputs_run_kwargs(
                {"native_security_feeds": {"W": "daily-a.csv"}}, root, chart)
            _, kwargs_none = inputs_run_kwargs({}, root, chart)
            provenance_a = build_runtime_provenance(kwargs_a, None)
            self.assertEqual(
                provenance_a["native_security_feeds"]["D"]["source_path"],
                str(daily_a.resolve()))
            self.assertNotIn("native_security_feeds",
                             build_runtime_provenance(kwargs_none, None))
            digest_a = build_fingerprint({"runtime": provenance_a})["digest"]
            for other in (kwargs_b, kwargs_w, kwargs_none):
                provenance = build_runtime_provenance(other, None)
                self.assertNotEqual(provenance_a, provenance)
                self.assertNotEqual(
                    digest_a,
                    build_fingerprint({"runtime": provenance})["digest"])

            _write_bars(daily_a, [111.5, 333.5], step_ms=86_400_000)
            _, kwargs_content = inputs_run_kwargs(
                {"native_security_feeds": {"D": "daily-a.csv"}}, root, chart)
            self.assertNotEqual(
                provenance_a, build_runtime_provenance(kwargs_content, None))

    def test_runner_installs_native_feeds_before_the_chart_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            daily = root / "daily.csv"
            _write_bars(chart, [100.0, 200.0])
            _write_bars(daily, [111.5, 222.5], step_ms=86_400_000)
            _, kwargs = inputs_run_kwargs(
                {"native_security_feeds": {"D": "daily.csv"}}, root, chart)
            fake = _FakeLibrary()

            report = _strategy(fake).run(
                chart, input_tf="15", script_tf="15",
                native_security_feeds=kwargs["native_security_feeds"])

            self.assertEqual(fake.events[1],
                             ("native", "D", 2, [111.5, 222.5]))
            self.assertEqual(fake.events[2], ("run", 2, [100.0, 200.0]))
            recorded = report["native_security_feeds"]["D"]
            self.assertEqual(recorded["path"], str(daily.resolve()))
            self.assertEqual(recorded["source_file_sha256"],
                             kwargs["native_security_feeds"]["D"]["source_file_sha256"])
            self.assertEqual(len(recorded["source_values_sha256"]), 64)

    def test_engine_rejection_and_missing_feature_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            daily = root / "daily.csv"
            _write_bars(chart, [100.0])
            _write_bars(daily, [111.5], step_ms=86_400_000)
            _, kwargs = inputs_run_kwargs(
                {"native_security_feeds": {"D": "daily.csv"}}, root, chart)
            with self.assertRaisesRegex(RuntimeError, "rejected native feed"):
                _strategy(_FakeLibrary(native_result=-1)).run(
                    chart, input_tf="15", script_tf="15",
                    native_security_feeds=kwargs["native_security_feeds"])
            with self.assertRaisesRegex(RuntimeError, "lacks native"):
                _strategy(_FeatureAbsentLibrary()).run(
                    chart, input_tf="15", script_tf="15",
                    native_security_feeds=kwargs["native_security_feeds"])
            # A file replaced after metadata resolution is refused by hash.
            _write_bars(daily, [999.0], step_ms=86_400_000)
            with self.assertRaisesRegex(RuntimeError, "changed after"):
                _strategy(_FakeLibrary()).run(
                    chart, input_tf="15", script_tf="15",
                    native_security_feeds=kwargs["native_security_feeds"])

    def test_feature_absent_without_native_request_keeps_legacy_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            _write_bars(chart, [100.0])
            fake = _FeatureAbsentLibrary()
            report = _strategy(fake).run(chart, input_tf="15", script_tf="15")
            self.assertEqual([event[0] for event in fake.events],
                             ["create", "run"])
            self.assertNotIn("native_security_feeds", report)


if __name__ == "__main__":
    unittest.main()
