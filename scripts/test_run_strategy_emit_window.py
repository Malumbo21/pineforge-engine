#!/usr/bin/env python3
"""The emit window starts on the chart-feed bar that precedes TradingView's
first entry bar.

run_strategy.py gates the engine's strategy commands (``trade_start_time``)
at the start of the TV tape's emit window and used to put that start at
``first TV entry - one bar interval`` (the feed's first-row gap), assuming
the bar the first entry was placed on sits one interval before the bar it
filled on. Across a weekend, a holiday or an overnight session break it does
not: the signal bar fell behind the gate (and its one-script-TF buffer,
engine_strategy_commands.cpp trading_is_active), the strategy.entry was
dropped, and six single-entry tapes graded no-trades on every ladder
candidate (round 7, ledger log-20260905t054904z-a9baf07e).

The start is now the loaded feed bar before the bar at or just before the
first TV entry — walked over the feed the engine is fed, after the probe's
ohlcv_start_ms / range-end bounds — whenever that is earlier than the old
start, never later (_tv_entry_emit_window). Trades keep being reported
from the old bound: the first entry's fill on TV's first entry bar is
written, a fill on the signal bar itself (which TV did not report) is not,
and on a gapless feed nothing changes at all.
"""

from __future__ import annotations

import contextlib
import csv
import io
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

import run_strategy
from run_strategy import (
    TvEntryWindow,
    _describe_tv_entry_window,
    _feed_timestamps,
    _filter_trades_to_window,
    _infer_bar_interval_ms,
    _load_bars,
    _load_tv_entry_span,
    _tv_entry_emit_window,
)

MIN15 = 15 * 60 * 1000
HOUR = 60 * MIN15 // 15
DAY = 24 * HOUR


def _utc_ms(y: int, m: int, d: int, hh: int = 0, mm: int = 0) -> int:
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


def _utc(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")


def _taipei(ms: int) -> str:
    """A tape stamp as the campaign's verifier writes it (utc_plus_8)."""
    tz = timezone(timedelta(hours=8))
    return datetime.fromtimestamp(ms / 1000.0, tz=tz).strftime("%Y-%m-%d %H:%M")


def _weekday_stamps(first: int, last: int, hh: int, mm: int) -> list[int]:
    """One bar per weekday from ``first`` to ``last`` (UTC dates) at hh:mm UTC."""
    out = []
    day = datetime.fromtimestamp(first / 1000.0, tz=timezone.utc)
    end = datetime.fromtimestamp(last / 1000.0, tz=timezone.utc)
    while day <= end:
        if day.weekday() < 5:
            out.append(_utc_ms(day.year, day.month, day.day, hh, mm))
        day += timedelta(days=1)
    return out


def _rth_15m_stamps(days: list[tuple[int, int, int]]) -> list[int]:
    """NASDAQ regular-hours 15m bars, 13:30..19:45 UTC, for each (y, m, d)."""
    out = []
    for y, m, d in days:
        start = _utc_ms(y, m, d, 13, 30)
        out.extend(range(start, _utc_ms(y, m, d, 19, 45) + 1, MIN15))
    return out


def _write_feed(path: Path, stamps: list[int]) -> None:
    with path.open("w", encoding="utf-8") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for ts in stamps:
            f.write(f"{ts},10,11,9,10.5,100\n")


def _write_tape(path: Path, rows: list[tuple[int, str, str, str, str]]) -> None:
    with path.open("w", encoding="utf-8-sig") as f:
        f.write("Trade number,Type,Date and time,Signal,Price USD\n")
        for n, typ, when, signal, price in rows:
            f.write(f"{n},{typ},{when},{signal},{price}\n")


# The ws-report-v1 export's range: every campaign tape runs 2025-04-01 to
# 2026-05-01, so the range-end bound never cuts the feeds below.
WS_METRICS = {
    "symbol": "NYSE:F", "interval": "1D",
    "from": "2025-04-01", "to": "2026-05-01",
    "deepBacktesting": True, "tapeChannel": "ws-report-v1",
    "wsProvenance": {
        "schemaVersion": 1,
        "requestedRange": {"from": 1743465600000, "to": 1777593600000},
        "returnedRange": {"from": 1743514200000, "to": 1777555800000},
        "rangeProof": "covered",
    },
}
RANGE_TO_MS = 1777593600000


def _trade(entry_ms: int, exit_ms: int, *, is_long: bool = True) -> dict:
    return {
        "is_long": is_long, "entry_time": entry_ms, "exit_time": exit_ms,
        "entry_price": 10.0, "exit_price": 10.5, "qty": 1.0,
        "pnl": 0.5, "pnl_pct": 5.0, "max_runup": 0.5, "max_drawdown": 0.0,
        "commission": 0.0, "entry_bar_index": 0, "exit_bar_index": 1,
        "open_at_end": False,
    }


class _FakeStrategy:
    """Stands in for Strategy: records the run kwargs, returns ``trades``."""
    calls: list[dict] = []
    trades: list[dict] = []

    def __init__(self, so_path: Path) -> None:
        self.lib = None

    def run(self, bars_csv: Path, params=None, **kwargs) -> dict:
        _FakeStrategy.calls.append({"bars_csv": bars_csv, **kwargs})
        return {
            "trades": [dict(t) for t in _FakeStrategy.trades],
            "trace": [], "trace_names": [],
            "net_profit": sum(t["pnl"] for t in _FakeStrategy.trades),
            "input_bars_processed": 0,
        }


def _run_main(strategy_dir: Path, feed: Path, out: Path, trades: list[dict],
              *extra: str) -> str:
    """Drive run_strategy.main() end to end with the engine stubbed out;
    returns everything it printed."""
    _FakeStrategy.calls.clear()
    _FakeStrategy.trades = trades
    argv = ["run_strategy.py", str(strategy_dir), "--ohlcv", str(feed),
            "-o", str(out), *extra]
    buf = io.StringIO()
    with mock.patch.object(run_strategy, "ensure_derived", lambda: None), \
            mock.patch.object(run_strategy, "find_strategy_lib",
                              lambda d, so_name="strategy.so": d / so_name), \
            mock.patch.object(run_strategy, "Strategy", _FakeStrategy), \
            mock.patch.object(run_strategy, "REFERENCE_OHLCV", feed), \
            mock.patch.object(sys, "argv", argv), \
            contextlib.redirect_stdout(buf):
        rc = run_strategy.main()
    assert rc == 0, buf.getvalue()
    return buf.getvalue()


def _entry_rows(path: Path) -> list[str]:
    with path.open(encoding="utf-8") as f:
        return [r["Date and time"] for r in csv.DictReader(f)
                if r["Type"].startswith("Entry")]


def _probe(d: Path, stamps: list[int], tape: list[tuple[int, str, str, str, str]] | None,
           *, inputs: dict | None = None, metrics: dict | None = WS_METRICS) -> tuple[Path, Path]:
    feed = d / "feed.csv"
    _write_feed(feed, stamps)
    if tape is not None:
        _write_tape(d / "tv_trades.csv", tape)
    if metrics is not None:
        (d / "metrics.json").write_text(json.dumps(metrics), encoding="utf-8")
    (d / "inputs.json").write_text(
        json.dumps({"tv_trades_csv_tz": "utc_plus_8", **(inputs or {})}), encoding="utf-8")
    return feed, d / "engine_trades.csv"


# --- the six round-7 tapes ---------------------------------------------

# (probe, chart feed bars around the first TV entry, first TV entry = TV's
#  fill bar, the bar the entry was placed on, bar interval = script TF).
# The old gate was ``first entry - interval - script TF``; every signal bar
# below lies before it, and is the bar the new start lands on.
SIX_TAPES = [
    ("jos-protrader NQ1@1D",
     [_utc_ms(2025, 6, 24, 22), _utc_ms(2025, 6, 25, 22), _utc_ms(2025, 6, 26, 22),
      _utc_ms(2025, 6, 29, 22), _utc_ms(2025, 6, 30, 22)],
     _utc_ms(2025, 6, 29, 22), _utc_ms(2025, 6, 26, 22), DAY),
    ("rakesh NIFTY@1D",
     [_utc_ms(2025, 11, 12, 3, 45), _utc_ms(2025, 11, 13, 3, 45), _utc_ms(2025, 11, 14, 3, 45),
      _utc_ms(2025, 11, 17, 3, 45), _utc_ms(2025, 11, 18, 3, 45)],
     _utc_ms(2025, 11, 17, 3, 45), _utc_ms(2025, 11, 14, 3, 45), DAY),
    ("vasanth F@1D",
     [_utc_ms(2025, 10, 1, 13, 30), _utc_ms(2025, 10, 2, 13, 30), _utc_ms(2025, 10, 3, 13, 30),
      _utc_ms(2025, 10, 6, 13, 30), _utc_ms(2025, 10, 7, 13, 30)],
     _utc_ms(2025, 10, 6, 13, 30), _utc_ms(2025, 10, 3, 13, 30), DAY),
    ("stockhunter2025 XAUUSD@1D",
     [_utc_ms(2026, 1, 6, 22), _utc_ms(2026, 1, 7, 22), _utc_ms(2026, 1, 8, 22),
      _utc_ms(2026, 1, 11, 22), _utc_ms(2026, 1, 12, 22)],
     _utc_ms(2026, 1, 11, 22), _utc_ms(2026, 1, 8, 22), DAY),
    ("heneralmomo25 XAUUSD@1D (DST shift)",
     [_utc_ms(2025, 10, 28, 21), _utc_ms(2025, 10, 29, 21), _utc_ms(2025, 10, 30, 21),
      _utc_ms(2025, 11, 2, 22), _utc_ms(2025, 11, 3, 22)],
     _utc_ms(2025, 11, 2, 22), _utc_ms(2025, 10, 30, 21), DAY),
    ("ayusattv AAPL@15",
     [_utc_ms(2025, 4, 9, 19, 15), _utc_ms(2025, 4, 9, 19, 30), _utc_ms(2025, 4, 9, 19, 45),
      _utc_ms(2025, 4, 10, 13, 30), _utc_ms(2025, 4, 10, 13, 45)],
     _utc_ms(2025, 4, 10, 13, 30), _utc_ms(2025, 4, 9, 19, 45), MIN15),
]

# The old gates the diagnosis named (signal bar vs gate), UTC.
OLD_GATES = {
    "jos-protrader NQ1@1D": _utc_ms(2025, 6, 27, 22),
    "rakesh NIFTY@1D": _utc_ms(2025, 11, 15, 3, 45),
    "vasanth F@1D": _utc_ms(2025, 10, 4, 13, 30),
    "stockhunter2025 XAUUSD@1D": _utc_ms(2026, 1, 9, 22),
    "heneralmomo25 XAUUSD@1D (DST shift)": _utc_ms(2025, 10, 31, 22),
    "ayusattv AAPL@15": _utc_ms(2025, 4, 10, 13),
}


class SixTapesTests(unittest.TestCase):
    def test_the_start_is_the_signal_bar_on_every_tape(self) -> None:
        for name, feed, first_entry, signal_bar, tf in SIX_TAPES:
            with self.subTest(name):
                w = _tv_entry_emit_window(feed, first_entry, first_entry, tf)
                old_start = first_entry - tf
                self.assertEqual(old_start - tf, OLD_GATES[name])
                # The defect: the signal bar sat before the old gate.
                self.assertLess(signal_bar, OLD_GATES[name])
                # The fix: the window opens on the signal bar itself.
                self.assertEqual(w.fill_bar_ms, first_entry)
                self.assertEqual(w.signal_bar_ms, signal_bar)
                self.assertEqual(w.start_ms, signal_bar)
                self.assertLess(w.start_ms, old_start)
                # Trades keep being reported from the old bound: the fill on
                # TV's first entry bar is in, a fill on the signal bar is out.
                self.assertEqual(w.report_start_ms, old_start)
                self.assertEqual(w.end_ms, first_entry)
                report = (w.report_start_ms, w.end_ms)
                self.assertEqual(
                    [t["entry_time"] for t in _filter_trades_to_window(
                        [_trade(signal_bar, first_entry), _trade(first_entry, first_entry + tf)],
                        report)],
                    [first_entry])
                self.assertIn("widened from", _describe_tv_entry_window(w))


# --- the rule on synthetic feeds --------------------------------------

class EmitWindowRuleTests(unittest.TestCase):
    # NYSE:F 1D: weekday bars at 13:30 UTC, 2025-09-29 .. 2025-10-10.
    F_1D = _weekday_stamps(_utc_ms(2025, 9, 29), _utc_ms(2025, 10, 10), 13, 30)
    MONDAY = _utc_ms(2025, 10, 6, 13, 30)
    FRIDAY = _utc_ms(2025, 10, 3, 13, 30)

    def test_friday_signal_monday_fill(self) -> None:
        w = _tv_entry_emit_window(self.F_1D, self.MONDAY, self.MONDAY + 3 * DAY, DAY)
        self.assertEqual(w, TvEntryWindow(
            start_ms=self.FRIDAY, end_ms=self.MONDAY + 3 * DAY,
            report_start_ms=self.MONDAY - DAY, first_entry_ms=self.MONDAY,
            fill_bar_ms=self.MONDAY, signal_bar_ms=self.FRIDAY))
        self.assertEqual(
            _describe_tv_entry_window(w),
            "start 2025-10-03 13:30 UTC = the feed bar before TV's first entry bar "
            "2025-10-06 13:30 UTC (widened from 2025-10-05 13:30 UTC: the gap before "
            "the first entry exceeds one bar interval); entries reported from "
            "2025-10-05 13:30 UTC to 2025-10-09 13:30 UTC")

    def test_overnight_gap_on_a_15m_feed(self) -> None:
        # ayusattv: the 04-09 19:45 UTC signal (the session's last bar) fills
        # on 04-10 13:30 UTC, the next session's first bar.
        feed = _rth_15m_stamps([(2025, 4, 9), (2025, 4, 10)])
        first = _utc_ms(2025, 4, 10, 13, 30)
        w = _tv_entry_emit_window(feed, first, first, MIN15)
        self.assertEqual(w.start_ms, _utc_ms(2025, 4, 9, 19, 45))
        self.assertEqual(w.report_start_ms, _utc_ms(2025, 4, 10, 13, 15))
        self.assertEqual(w.fill_bar_ms, first)

    def test_gapless_feed_is_unchanged(self) -> None:
        # A 24x7 15m feed: the preceding bar IS one interval back.
        feed = list(range(_utc_ms(2026, 4, 29), _utc_ms(2026, 4, 30) + 1, MIN15))
        first = _utc_ms(2026, 4, 29, 9, 45)
        w = _tv_entry_emit_window(feed, first, first + HOUR, MIN15)
        self.assertEqual(w.start_ms, first - MIN15)
        self.assertEqual(w.report_start_ms, first - MIN15)
        self.assertEqual(w.signal_bar_ms, first - MIN15)
        self.assertIn("not earlier: unchanged", _describe_tv_entry_window(w))
        # Midweek on a daily feed likewise.
        w = _tv_entry_emit_window(self.F_1D, _utc_ms(2025, 10, 8, 13, 30),
                                  _utc_ms(2025, 10, 8, 13, 30), DAY)
        self.assertEqual(w.start_ms, _utc_ms(2025, 10, 7, 13, 30))
        self.assertEqual(w.start_ms, w.report_start_ms)

    def test_widen_only_never_later_than_the_old_start(self) -> None:
        # A feed whose first-row gap is a weekend (it starts on a Friday)
        # infers a 3-day interval: the old start of a Wednesday entry is
        # Sunday, earlier than Tuesday's bar, and is kept.
        feed = _weekday_stamps(_utc_ms(2025, 10, 3), _utc_ms(2025, 10, 10), 13, 30)
        wednesday = _utc_ms(2025, 10, 8, 13, 30)
        w = _tv_entry_emit_window(feed, wednesday, wednesday, 3 * DAY)
        self.assertEqual(w.signal_bar_ms, _utc_ms(2025, 10, 7, 13, 30))
        self.assertEqual(w.start_ms, wednesday - 3 * DAY)
        self.assertEqual(w.start_ms, w.report_start_ms)
        # Every shape: the start is at most the old start.
        for name, bars, first, _signal, tf in SIX_TAPES:
            for interval in (tf, 3 * tf, tf // 3):
                with self.subTest(name, interval=interval):
                    w = _tv_entry_emit_window(bars, first, first, interval)
                    self.assertLessEqual(w.start_ms, first - interval)
                    self.assertEqual(w.report_start_ms, first - interval)

    def test_off_grid_stamp_resolves_to_the_bar_at_or_before_it(self) -> None:
        # A tape stamp 5 minutes into a bar (a tz skew) still names that bar
        # as the fill bar and the one before it as the signal bar.
        feed = list(range(_utc_ms(2026, 4, 29), _utc_ms(2026, 4, 30) + 1, MIN15))
        stamp = _utc_ms(2026, 4, 29, 9, 50)
        w = _tv_entry_emit_window(feed, stamp, stamp, MIN15)
        self.assertEqual(w.fill_bar_ms, _utc_ms(2026, 4, 29, 9, 45))
        self.assertEqual(w.signal_bar_ms, _utc_ms(2026, 4, 29, 9, 30))
        self.assertEqual(w.start_ms, _utc_ms(2026, 4, 29, 9, 30))

    def test_first_entry_on_the_feeds_first_row(self) -> None:
        first = self.F_1D[0]
        w = _tv_entry_emit_window(self.F_1D, first, first, DAY)
        self.assertEqual(w.fill_bar_ms, first)
        self.assertIsNone(w.signal_bar_ms)
        self.assertEqual(w.start_ms, first - DAY)
        self.assertEqual(w.report_start_ms, first - DAY)
        self.assertIn("is on the loaded feed's first bar", _describe_tv_entry_window(w))

    def test_first_entry_earlier_than_the_feed_and_an_empty_feed(self) -> None:
        early = self.F_1D[0] - 5 * DAY
        for feed in (self.F_1D, []):
            with self.subTest(bars=len(feed)):
                w = _tv_entry_emit_window(feed, early, early, DAY)
                self.assertIsNone(w.fill_bar_ms)
                self.assertIsNone(w.signal_bar_ms)
                self.assertEqual(w.start_ms, early - DAY)
                self.assertEqual(w.report_start_ms, early - DAY)
                self.assertIn("precedes the loaded feed: unchanged",
                              _describe_tv_entry_window(w))

    def test_first_entry_past_the_feeds_last_bar(self) -> None:
        late = self.F_1D[-1] + 7 * DAY
        w = _tv_entry_emit_window(self.F_1D, late, late, DAY)
        self.assertEqual(w.fill_bar_ms, self.F_1D[-1])
        self.assertEqual(w.signal_bar_ms, self.F_1D[-2])
        self.assertEqual(w.start_ms, self.F_1D[-2])
        self.assertEqual(w.report_start_ms, late - DAY)
        # Nothing at or after the report bound exists to report.
        self.assertEqual(_filter_trades_to_window(
            [_trade(ts, ts + DAY) for ts in self.F_1D], (w.report_start_ms, w.end_ms)), [])


# --- the loaded feed ---------------------------------------------------

class FeedTimestampsTests(unittest.TestCase):
    def test_bounds_match_load_bars(self) -> None:
        stamps = EmitWindowRuleTests.F_1D
        with tempfile.TemporaryDirectory() as tmp:
            feed = Path(tmp) / "feed.csv"
            _write_feed(feed, stamps)
            for start, end in ((None, None), (stamps[3], None), (None, stamps[6]),
                               (stamps[2], stamps[7]), (stamps[-1] + 1, None)):
                with self.subTest(start=start, end=end):
                    bars, n, _ = _load_bars(feed, ohlcv_start_ms=start, ohlcv_end_ms=end)
                    self.assertEqual(_feed_timestamps(feed, ohlcv_start_ms=start,
                                                      ohlcv_end_ms=end),
                                     [bars[i].timestamp for i in range(n)])
            self.assertEqual(_infer_bar_interval_ms(feed), DAY)

    def test_range_start_trim_decides_which_bar_precedes(self) -> None:
        # Under a range-start trim the bars before it are not loaded: trimmed
        # at the fill bar there is no preceding bar (start unchanged);
        # trimmed at the signal bar it is the first loaded bar.
        stamps = EmitWindowRuleTests.F_1D
        monday, friday = EmitWindowRuleTests.MONDAY, EmitWindowRuleTests.FRIDAY
        with tempfile.TemporaryDirectory() as tmp:
            feed = Path(tmp) / "feed.csv"
            _write_feed(feed, stamps)
            at_fill = _tv_entry_emit_window(
                _feed_timestamps(feed, ohlcv_start_ms=monday), monday, monday, DAY)
            self.assertIsNone(at_fill.signal_bar_ms)
            self.assertEqual(at_fill.start_ms, monday - DAY)
            at_signal = _tv_entry_emit_window(
                _feed_timestamps(feed, ohlcv_start_ms=friday), monday, monday, DAY)
            self.assertEqual(at_signal.start_ms, friday)


# --- the tape ----------------------------------------------------------

class TapeSpanTests(unittest.TestCase):
    MONDAY = EmitWindowRuleTests.MONDAY

    def test_entry_rows_in_the_tapes_timezone(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", [
                (2, "Exit short", _taipei(self.MONDAY + 9 * DAY), "X", "9"),
                (2, "Entry short", _taipei(self.MONDAY + 7 * DAY), "S", "10"),
                (1, "Exit long", _taipei(self.MONDAY + 3 * DAY), "X", "10.5"),
                (1, "Entry long", _taipei(self.MONDAY), "L", "10"),
            ])
            self.assertEqual(_load_tv_entry_span(d, {"tv_trades_csv_tz": "utc_plus_8"}),
                             (self.MONDAY, self.MONDAY + 7 * DAY))
            # An IANA zone: 09:30 New York on 2025-10-06 is 13:30 UTC.
            _write_tape(d / "ny.csv", [
                (1, "Exit long", "2025-10-09 09:30", "X", "10.5"),
                (1, "Entry long", "2025-10-06 09:30", "L", "10"),
            ])
            self.assertEqual(
                _load_tv_entry_span(d, {"tv_trades_csv": "ny.csv",
                                        "tv_trades_csv_tz": "America/New_York"}),
                (self.MONDAY, self.MONDAY))

    def test_no_tape_or_no_entry_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.assertIsNone(_load_tv_entry_span(d, {}))
            _write_tape(d / "tv_trades.csv", [])
            self.assertIsNone(_load_tv_entry_span(d, {}))
            _write_tape(d / "tv_trades.csv",
                        [(1, "Exit long", _taipei(self.MONDAY), "Open", "10.5")])
            self.assertIsNone(_load_tv_entry_span(d, {}))


# --- main() end to end -------------------------------------------------

class MainEmitWindowTests(unittest.TestCase):
    F_1D = EmitWindowRuleTests.F_1D
    MONDAY = EmitWindowRuleTests.MONDAY
    FRIDAY = EmitWindowRuleTests.FRIDAY
    THURSDAY_EXIT = _utc_ms(2025, 10, 9, 13, 30)

    def _tape(self) -> list[tuple[int, str, str, str, str]]:
        return [(1, "Exit long", _taipei(self.THURSDAY_EXIT), "X", "10.5"),
                (1, "Entry long", _taipei(self.MONDAY), "L", "10")]

    def _engine_trades(self) -> list[dict]:
        return [
            _trade(_utc_ms(2025, 9, 30, 13, 30), self.FRIDAY),   # pre-window
            _trade(self.FRIDAY, self.MONDAY),                    # a fill ON the signal bar
            _trade(self.MONDAY, self.THURSDAY_EXIT),             # TV's trade
            _trade(_utc_ms(2025, 10, 10, 13, 30), RANGE_TO_MS),  # after TV's last entry
        ]

    def test_friday_signal_monday_fill_1d_feed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, self._tape())
            printed = _run_main(d, feed, out, self._engine_trades())
            call = _FakeStrategy.calls[0]
            # The engine's gate opens on Friday's bar, the feed is bounded
            # at TV's range end as before.
            self.assertEqual(call["trade_start_time_ms"], self.FRIDAY)
            self.assertEqual(call["ohlcv_end_ms"], RANGE_TO_MS)
            self.assertIsNone(call.get("ohlcv_start_ms"))
            # Only the fill on TV's first entry bar is written: not the fill
            # on the signal bar (TV did not report it), not the pre-window
            # trade, not the entry after TV's last entry.
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30"])
            self.assertIn(
                "  emit-window: start 2025-10-03 13:30 UTC = the feed bar before TV's "
                "first entry bar 2025-10-06 13:30 UTC (widened from 2025-10-05 13:30 UTC",
                printed)
            self.assertEqual(printed.count("range-end:"), 1)
            self.assertIn("1 trades (4 raw)", printed)

    def test_broker_flags_keep_their_meaning(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, self._tape())
            # --allow-trading-before-window: no gate, the same rows.
            _run_main(d, feed, out, self._engine_trades(), "--allow-trading-before-window")
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30"])
            # --disable-trading-before-window is what a tape implies anyway.
            _run_main(d, feed, out, self._engine_trades(), "--disable-trading-before-window")
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.FRIDAY)
            # --no-trim-output: no window at all, every trade written.
            printed = _run_main(d, feed, out, self._engine_trades(), "--no-trim-output")
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertEqual(len(_entry_rows(out)), 4)
            self.assertNotIn("emit-window:", printed)

    def test_overnight_gap_15m_feed(self) -> None:
        # ayusattv AAPL@15: the short placed on the 04-09 19:45 UTC bar fills
        # on 04-10 13:30 UTC.
        stamps = _rth_15m_stamps([(2025, 4, 8), (2025, 4, 9), (2025, 4, 10), (2025, 4, 11)])
        signal, fill = _utc_ms(2025, 4, 9, 19, 45), _utc_ms(2025, 4, 10, 13, 30)
        exit_ = _utc_ms(2025, 4, 11, 13, 30)
        tape = [(1, "Exit short", _taipei(exit_), "SL", "9.5"),
                (1, "Entry short", _taipei(fill), "S", "10")]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, stamps, tape, metrics={**WS_METRICS, "symbol": "NASDAQ:AAPL",
                                                         "interval": "15"})
            printed = _run_main(d, feed, out, [
                _trade(signal, fill, is_long=False),
                _trade(fill, exit_, is_long=False),
            ])
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], signal)
            self.assertEqual(_entry_rows(out), ["2025-04-10 13:30"])
            self.assertIn("widened from 2025-04-10 13:15 UTC", printed)

    def test_range_start_trim_is_the_loaded_feed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            # Trimmed at the fill bar: no earlier bar is loaded, the start is
            # the old one.
            feed, out = _probe(d, self.F_1D, self._tape(), inputs={"ohlcv_start_ms": self.MONDAY})
            printed = _run_main(d, feed, out, self._engine_trades())
            call = _FakeStrategy.calls[0]
            self.assertEqual(call["ohlcv_start_ms"], self.MONDAY)
            self.assertEqual(call["trade_start_time_ms"], self.MONDAY - DAY)
            self.assertIn("is on the loaded feed's first bar 2025-10-06 13:30 UTC", printed)
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30"])
            # Trimmed at the signal bar: it is the first loaded bar.
            feed, out = _probe(d, self.F_1D, self._tape(), inputs={"ohlcv_start_ms": self.FRIDAY})
            _run_main(d, feed, out, self._engine_trades())
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.FRIDAY)

    def test_no_tv_entries_falls_back_to_the_reference_window(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, [(1, "Exit long", _taipei(self.MONDAY), "Open", "10")])
            printed = _run_main(d, feed, out, self._engine_trades())
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertIsNone(_FakeStrategy.calls[0].get("ohlcv_end_ms"))
            self.assertNotIn("emit-window:", printed)
            self.assertNotIn("range-end:", printed)
            # The reference window (the feed's span here) reports every
            # trade entered inside it.
            self.assertEqual(_entry_rows(out), ["2025-10-10 13:30", "2025-10-06 13:30",
                                                "2025-10-03 13:30", "2025-09-30 13:30"])
            _run_main(d, feed, out, self._engine_trades(), "--disable-trading-before-window")
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.F_1D[0])
            # No tape at all: the same.
            (d / "tv_trades.csv").unlink()
            printed = _run_main(d, feed, out, self._engine_trades())
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertNotIn("emit-window:", printed)

    def test_first_tv_entry_before_the_feed(self) -> None:
        early = _utc_ms(2025, 9, 1, 13, 30)
        tape = [(1, "Exit long", _taipei(self.THURSDAY_EXIT), "X", "10.5"),
                (1, "Entry long", _taipei(early), "L", "10")]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, tape)
            printed = _run_main(d, feed, out, self._engine_trades())
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], early - DAY)
            self.assertIn("precedes the loaded feed: unchanged", printed)
            # Entries up to TV's last (= first) entry only: none of the
            # engine's trades, as before.
            self.assertEqual(_entry_rows(out), [])

    def test_explicit_emit_window_reports_what_it_gates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, self._tape())
            window = d / "window.csv"
            _write_feed(window, self.F_1D[4:8])   # 10-03 .. 10-08
            printed = _run_main(d, feed, out, self._engine_trades(),
                                "--emit-window-ohlcv", str(window),
                                "--disable-trading-before-window")
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.FRIDAY)
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30", "2025-10-03 13:30"])
            self.assertNotIn("emit-window:", printed)


if __name__ == "__main__":
    unittest.main()
