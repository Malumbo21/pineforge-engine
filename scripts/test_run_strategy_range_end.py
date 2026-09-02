#!/usr/bin/env python3
"""The engine's measurement ends where TradingView's range ends.

The engine books a position still open after its FINAL bar as TradingView's
range-end close (open_at_end, at that bar's close). That final bar must be
TV's last bar, not the feed's: the BINANCE:ETHUSDT.P 15 chart feed runs on
to 2026-05-04 15:00 UTC while every ETH tape's range ends 2026-05-01 (228 of
397 scrapper tapes with an Open row at 05-01 08:00 (+8) = 05-01 00:00 UTC @
2261.44 = that bar's close). Unbounded, the row would be booked on 05-04
15:00 @ 2365.09, a different bar ~4.6% off TV's, and fail exit/pnl against
TV's Open row where it was previously simply absent.

For an UNMARKED tape (the ws-report-v1 shape: the range-end position is an
ordinary closed trade with an empty Signal — every 1D lane, most 15m)
run_strategy.py therefore bounds the feed (``ohlcv_end_ms``) to
TradingView's RANGE END, read from the tape's metrics.json — the bars
opening at or before ``to`` as 00:00 UTC (``wsProvenance.requestedRange.to``
when the ws exporter recorded it, else the ``to`` date) — and writes the
range-end close rows. Not the tape's last trade row: a first cut did that
and the full-population gate failed hard.regression on
waranyutrkm-asian-box-breakout-eda-tuned (110 -> 109 trades) — its last
trade closes 04-29 18:00 UTC, the feed was cut mid-day, and the strategy
reads the 04-29 daily close/EMA through a lookahead_on projection that sees
the day's FINAL value; the partial day flipped the trend filter and TV's
trade 110 (entered 04-29 09:45) never happened. The tape's last row is only
the fallback for a metrics.json with no range. Under this (v2) behaviour
every unmarked lane measured was clean: f-1d 10/10 engine-0 -> excellent,
waranyutrkm 110/110 at 100%, the 8 unmarked range-end ETH tapes unchanged.

For a MARKED tape (the browser export writes the same row with Signal
"Open"; 227 of the 396 scraped ETH tapes) run_strategy.py keeps the
BASELINE behaviour: no bound — the feed as exported, three days past the
range — and the open_at_end rows withheld from the CSV. The baseline traded
past TV's range end and the canonical grader (scripts/verify_corpus.py)
pairs those post-range closes with TV's Open rows on entry time; v2 (bound
+ emit) paired the marks exactly but moved pnlP90 on the three 3commas
grid bots by the cent-rounding of TV's Net PnL (22/22/28 open lots each;
gate v2 4gfpn), which the hard gate counts as a regression, and v3 (bound +
withhold) lost the pairing on ~186 marked probes (3commas-xlm 94.3 -> 93.7,
alexgrover 100 -> 99.8; spark pre-check, 2026-09-02). Until
verify_corpus.py pairs the marks itself, a marked tape is measured exactly
as the baseline measured it. main() logs which regime applied and why.
"""

from __future__ import annotations

import contextlib
import csv
import io
import json
import re
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

import run_strategy
from run_strategy import (
    EXPECTED_PF_ABI,
    TradeC,
    _apply_range_end_regime,
    _load_bars,
    _load_tv_range_end,
    _load_tv_range_end_ms,
    _trim_ohlcv_csv,
    _tv_metrics_range_end,
    _tv_metrics_range_end_ms,
    _tv_tape_marks_open_rows,
    _withhold_open_at_end_trades,
)

FIFTEEN_MIN = 15 * 60 * 1000
DAY = 24 * 60 * 60 * 1000


def _utc_ms(y: int, m: int, d: int, hh: int = 0, mm: int = 0) -> int:
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


# Every campaign tape: from 2025-04-01 to 2026-05-01, i.e. the ws exporter's
# requestedRange {from: 1743465600000, to: 1777593600000}.
RANGE_TO_MS = 1777593600000
assert RANGE_TO_MS == _utc_ms(2026, 5, 1)

# The ETH chart feed: 15m bars from 04-29 00:00 UTC past the range end to
# 05-04 15:00 UTC. TV's last bar is 05-01 00:00 UTC (close 2261.44).
FEED_START = _utc_ms(2026, 4, 29, 0, 0)
TV_LAST_BAR = RANGE_TO_MS
FEED_END = _utc_ms(2026, 5, 4, 15, 0)
# The asian-box shape: the tape's last trade closes 04-29 18:00 UTC.
LAST_EXIT = _utc_ms(2026, 4, 29, 18, 0)


def _write_eth_feed(path: Path) -> list[int]:
    stamps = list(range(FEED_START, FEED_END + 1, FIFTEEN_MIN))
    with path.open("w", encoding="utf-8") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for ts in stamps:
            close = 2261.44 if ts == TV_LAST_BAR else (2365.09 if ts == FEED_END else 2300.0)
            f.write(f"{ts},2300,2400,2200,{close},1\n")
    return stamps


def _write_tape(path: Path, rows: list[tuple[int, str, str, str, str]]) -> None:
    with path.open("w", encoding="utf-8-sig") as f:
        f.write("Trade number,Type,Date and time,Signal,Price USDT\n")
        for n, typ, when, signal, price in rows:
            f.write(f"{n},{typ},{when},{signal},{price}\n")


def _write_metrics(path: Path, metrics: dict) -> None:
    path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")


# The scrapper's browser export (ETH, AAPL 15, EURUSD): the range as dates.
BROWSER_METRICS = {
    "strategy": "waranyutrkm-asian-box-breakout-eda-tuned",
    "symbol": "BINANCE:ETHUSDT.P", "interval": "15",
    "from": "2025-04-01", "to": "2026-05-01",
    "appliedRange": "Apr 1, 2025 — May 1, 2026 DEEP",
    "deepBacktesting": True, "trades": 110,
}

# The ws-report-v1 export (F, BTC, ES, NQ, XAU, NIFTY, AAPL 1D): the dates
# plus the exact window sent, and the range TV returned.
WS_METRICS_F_1D = {
    "validationEligible": True, "strategy": "orb-lite",
    "symbol": "NYSE:F", "interval": "1D",
    "from": "2025-04-01", "to": "2026-05-01",
    "deepBacktesting": True, "tapeChannel": "ws-report-v1",
    "wsProvenance": {
        "schemaVersion": 1,
        "requestedRange": {"from": 1743465600000, "to": RANGE_TO_MS},
        "returnedRange": {"from": 1743514200000, "to": _utc_ms(2026, 4, 30, 13, 30)},
        "rangeProof": "covered",
    },
}

# Taipei-stamped (+8) tapes, as the campaign's verifier writes them.
TZ8 = {"tv_trades_csv_tz": "utc_plus_8"}

ASIAN_BOX_TAPE = [
    (109, "Exit long", "2026-04-27 05:15", "X", "2250.0"),
    (109, "Entry long", "2026-04-26 09:45", "L", "2240.0"),
    (110, "Exit long", "2026-04-30 02:00", "X", "2248.45"),   # 04-29 18:00 UTC
    (110, "Entry long", "2026-04-29 17:45", "L", "2342.15"),  # 04-29 09:45 UTC
]


class MetricsRangeEndTests(unittest.TestCase):
    """The range end is ``to`` at 00:00 UTC, whichever spelling carries it."""

    def test_ws_requested_range_to_is_the_bound(self) -> None:
        self.assertEqual(_tv_metrics_range_end_ms(WS_METRICS_F_1D), RANGE_TO_MS)

    def test_browser_to_date_is_utc_midnight(self) -> None:
        self.assertEqual(_tv_metrics_range_end_ms(BROWSER_METRICS), RANGE_TO_MS)

    def test_ws_integer_wins_over_the_date(self) -> None:
        m = json.loads(json.dumps(WS_METRICS_F_1D))
        m["to"] = "2026-04-15"
        self.assertEqual(_tv_metrics_range_end_ms(m), RANGE_TO_MS)

    def test_no_range(self) -> None:
        self.assertIsNone(_tv_metrics_range_end_ms({"symbol": "NYSE:F", "interval": "1D"}))
        self.assertIsNone(_tv_metrics_range_end_ms({"to": "May 1, 2026"}))
        self.assertIsNone(_tv_metrics_range_end_ms({"wsProvenance": {"requestedRange": {"to": "x"}}}))
        self.assertIsNone(_tv_metrics_range_end_ms(None))  # type: ignore[arg-type]

    def test_rule_matches_every_lanes_returned_range(self) -> None:
        # TradingView's own returnedRange.to on the ws tapes (survey of the
        # scrapper's lane directories, 2026-09-02) is the open of the last
        # bar at or before requestedRange.to = 05-01 00:00 UTC: the bar
        # OPENING at `to` is inside the range, the next one is not, and
        # `to` is UTC midnight even on CME's America/Chicago chart.
        returned_to = {
            "BINANCE:BTCUSDT 15": _utc_ms(2026, 5, 1, 0, 0),
            "CME_MINI:ES1! 15": _utc_ms(2026, 5, 1, 0, 0),
            "CME_MINI:NQ1! 15": _utc_ms(2026, 5, 1, 0, 0),
            "OANDA:XAUUSD 15": _utc_ms(2026, 5, 1, 0, 0),
            "NYSE:F 15": _utc_ms(2026, 4, 30, 19, 45),
            "NYSE:F 1D": _utc_ms(2026, 4, 30, 13, 30),
            "CME_MINI:ES1! 1D": _utc_ms(2026, 4, 30, 22, 0),
            "OANDA:XAUUSD 1D": _utc_ms(2026, 4, 30, 21, 0),
            "NSE:NIFTY 15": _utc_ms(2026, 4, 30, 9, 45),
            "NSE:NIFTY 1D": _utc_ms(2026, 4, 30, 3, 45),
        }
        for lane, last_bar in returned_to.items():
            with self.subTest(lane=lane):
                self.assertLessEqual(last_bar, RANGE_TO_MS)
        # A CME 15m bar after `to` (00:15 UTC) and the NYSE:F 05-01 session
        # bar (13:30 UTC, which exists on TV) are outside.
        self.assertGreater(_utc_ms(2026, 5, 1, 0, 15), RANGE_TO_MS)
        self.assertGreater(_utc_ms(2026, 5, 1, 13, 30), RANGE_TO_MS)


class LoadTvRangeEndTests(unittest.TestCase):
    def test_closed_last_trade_bounds_at_the_range_end_not_the_tape(self) -> None:
        # The asian-box shape: the tape's last row is a closed exit 30 h
        # before the range end; the bound is the range end.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", ASIAN_BOX_TAPE)
            _write_metrics(d / "metrics.json", BROWSER_METRICS)
            self.assertEqual(_load_tv_range_end_ms(d, TZ8), RANGE_TO_MS)
            self.assertNotEqual(_load_tv_range_end_ms(d, TZ8), LAST_EXIT)

    def test_open_position_row_sits_on_the_range_end(self) -> None:
        # Browser export: the Open row at 05-01 08:00 (+8) IS the range end.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit long", "2026-04-29 10:15", "X", "2310.5"),
                (1, "Entry long", "2026-04-28 09:00", "A", "2290.0"),
                (2, "Exit long", "2026-05-01 08:00", "Open", "2261.44"),
                (2, "Entry long", "2026-04-30 20:45", "B", "2280.0"),
            ])
            _write_metrics(d / "metrics.json", BROWSER_METRICS)
            self.assertEqual(_load_tv_range_end_ms(d, TZ8), TV_LAST_BAR)

    def test_ws_tape_bound_is_the_requested_to(self) -> None:
        # orb-lite NYSE:F 1D: the range-end row (no Signal) sits on 04-30
        # 13:30 UTC, TV's returnedRange.to; the bound is requestedRange.to,
        # 05-01 00:00 UTC, which keeps that bar and excludes 05-01 13:30.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit short", "2026-04-30 21:30", "", "12.08"),
                (1, "Entry short", "2026-03-16 21:30", "Short", "11.82"),
            ])
            _write_metrics(d / "metrics.json", WS_METRICS_F_1D)
            self.assertEqual(_load_tv_range_end_ms(d, TZ8), RANGE_TO_MS)

    def test_metrics_without_a_range_falls_back_to_the_tapes_last_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", ASIAN_BOX_TAPE)
            _write_metrics(d / "metrics.json", {"symbol": "BINANCE:ETHUSDT.P", "interval": "15"})
            self.assertEqual(_load_tv_range_end_ms(d, TZ8), LAST_EXIT)
            # An unreadable metrics.json falls back the same way.
            (d / "metrics.json").write_text("{not json", encoding="utf-8")
            self.assertEqual(_load_tv_range_end_ms(d, TZ8), LAST_EXIT)

    def test_no_metrics_falls_back_to_the_tapes_last_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            # Browser tape ending on its Open row: the fallback is the range end.
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit long", "2026-04-29 10:15", "X", "2310.5"),
                (1, "Entry long", "2026-04-28 09:00", "A", "2290.0"),
                (2, "Exit long", "2026-05-01 08:00", "Open", "2261.44"),
                (2, "Entry long", "2026-04-30 20:45", "B", "2280.0"),
            ])
            self.assertEqual(_load_tv_range_end_ms(d, TZ8), TV_LAST_BAR)
            # ws tape (orb-lite, UTC-stamped): the range-end row's bar.
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit short", "2026-04-30 13:30", "", "12.08"),
                (1, "Entry short", "2026-03-16 13:30", "Short", "11.82"),
            ])
            self.assertEqual(_load_tv_range_end_ms(d, {"tv_trades_csv_tz": "utc"}),
                             _utc_ms(2026, 4, 30, 13, 30))

    def test_no_tape_or_no_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_metrics(d / "metrics.json", BROWSER_METRICS)
            self.assertIsNone(_load_tv_range_end_ms(d, {}))
            _write_tape(d / "tv_trades.csv", [])
            # A tape of no rows still has a range.
            self.assertEqual(_load_tv_range_end_ms(d, TZ8), RANGE_TO_MS)
            (d / "metrics.json").unlink()
            self.assertIsNone(_load_tv_range_end_ms(d, TZ8))

    def test_meta_names_the_tape_and_the_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            probe = d / "probe"
            probe.mkdir()
            # The verifier's inputs.json names the tape by absolute path;
            # metrics.json is read from beside it.
            _write_tape(probe / "tv_trades.csv", ASIAN_BOX_TAPE)
            _write_metrics(probe / "metrics.json", BROWSER_METRICS)
            meta = {"tv_trades_csv": str(probe / "tv_trades.csv"), **TZ8}
            self.assertEqual(_load_tv_range_end_ms(d, meta), RANGE_TO_MS)
            # An explicit tv_metrics_json (relative to the strategy dir) wins.
            _write_metrics(d / "other-metrics.json", {**BROWSER_METRICS, "to": "2026-04-30"})
            meta["tv_metrics_json"] = "other-metrics.json"
            self.assertEqual(_load_tv_range_end_ms(d, meta), _utc_ms(2026, 4, 30))


class HarnessAbiMirrorTests(unittest.TestCase):
    """The harness's ABI guard and its pf_trade_t mirror follow the header.

    verify-engine-local.py runs every probe through run_strategy.py, so a
    guard left at the previous ABI is a RuntimeError on every slug rather
    than a wrong number on one: the f-1d spark pre-check of the range-end
    change (2026-09-02) failed all 64 probes with ".so reports 3, harness
    expects 2" because only the docker / tutorial / benchmark mirrors had
    been bumped. Pin the constant to PF_ABI_VERSION as the header declares
    it, and the mirror's tail to the v3 field.
    """

    HEADER = Path(__file__).resolve().parents[1] / "include" / "pineforge" / "pineforge.h"

    def test_expected_abi_is_the_headers_macro(self) -> None:
        text = self.HEADER.read_text(encoding="utf-8")
        m = re.search(r"^#define PF_ABI_VERSION (\d+)\s*$", text, re.M)
        self.assertIsNotNone(m)
        assert m is not None
        self.assertEqual(EXPECTED_PF_ABI, int(m.group(1)))
        self.assertEqual(EXPECTED_PF_ABI, 3)

    def test_trade_mirror_ends_with_open_at_end(self) -> None:
        names = [name for name, _ in TradeC._fields_]
        self.assertEqual(names[-3:], ["entry_bar_index", "exit_bar_index", "open_at_end"])


class FeedBoundTests(unittest.TestCase):
    def test_closed_last_trade_keeps_the_final_htf_period(self) -> None:
        # The asian-box shape on the ETH feed: bounded at the range end the
        # engine sees every 04-29 bar (a complete daily bar for the
        # lookahead_on projection) and every bar through 05-01 00:00 UTC;
        # bounded at the tape's last row it would see 04-29 only to 18:00.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            stamps = _write_eth_feed(d / "feed.csv")
            _write_tape(d / "tv_trades.csv", ASIAN_BOX_TAPE)
            _write_metrics(d / "metrics.json", BROWSER_METRICS)
            end = _load_tv_range_end_ms(d, TZ8)
            self.assertEqual(end, TV_LAST_BAR)
            bars, n, sha = _load_bars(d / "feed.csv", ohlcv_end_ms=end)
            self.assertEqual(bars[n - 1].timestamp, TV_LAST_BAR)
            self.assertAlmostEqual(bars[n - 1].close, 2261.44)
            self.assertEqual(n, stamps.index(TV_LAST_BAR) + 1)
            day_0429 = [i for i in range(n)
                        if FEED_START <= bars[i].timestamp < FEED_START + DAY]
            self.assertEqual(len(day_0429), 96)
            # Contrast: the tape-row bound truncates the day at 18:00 (73 bars).
            cut, n_cut, _ = _load_bars(d / "feed.csv", ohlcv_end_ms=LAST_EXIT)
            self.assertEqual(cut[n_cut - 1].timestamp, LAST_EXIT)
            self.assertEqual(n_cut, 73)
            # The source identity is the full feed either way.
            full, n_full, sha_full = _load_bars(d / "feed.csv")
            self.assertEqual(n_full, len(stamps))
            self.assertEqual(full[n_full - 1].timestamp, FEED_END)
            self.assertEqual(sha, sha_full)

    def test_open_position_is_marked_on_the_ranges_last_bar(self) -> None:
        # The ETH Open-row shape: the last bar the engine sees is 05-01
        # 00:00 UTC, close 2261.44 — the bar and price of TV's Open row —
        # not the feed's 05-04 15:00 @ 2365.09.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_eth_feed(d / "feed.csv")
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit long", "2026-05-01 08:00", "Open", "2261.44"),
                (1, "Entry long", "2026-04-30 22:15", "A", "2300.0"),
            ])
            _write_metrics(d / "metrics.json", BROWSER_METRICS)
            end = _load_tv_range_end_ms(d, TZ8)
            bars, n, _ = _load_bars(d / "feed.csv", ohlcv_end_ms=end)
            self.assertEqual(bars[n - 1].timestamp, TV_LAST_BAR)
            self.assertAlmostEqual(bars[n - 1].close, 2261.44)

    def test_daily_feed_keeps_the_bar_before_to_and_drops_the_one_after(self) -> None:
        # NYSE:F 1D: the 04-30 13:30 UTC bar (TV's returnedRange.to, where
        # orb-lite's range-end row sits @ 12.08) is inside; a 05-01 13:30
        # session bar is not.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            stamps = [_utc_ms(2026, 4, 28, 13, 30), _utc_ms(2026, 4, 29, 13, 30),
                      _utc_ms(2026, 4, 30, 13, 30), _utc_ms(2026, 5, 1, 13, 30)]
            with (d / "feed.csv").open("w", encoding="utf-8") as f:
                f.write("timestamp,open,high,low,close,volume\n")
                for ts, close in zip(stamps, (12.0, 12.1, 12.08, 12.3)):
                    f.write(f"{ts},12,12.5,11.5,{close},1\n")
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit short", "2026-04-30 21:30", "", "12.08"),
                (1, "Entry short", "2026-03-16 21:30", "Short", "11.82"),
            ])
            _write_metrics(d / "metrics.json", WS_METRICS_F_1D)
            end = _load_tv_range_end_ms(d, TZ8)
            self.assertEqual(end, RANGE_TO_MS)
            bars, n, _ = _load_bars(d / "feed.csv", ohlcv_end_ms=end)
            self.assertEqual(n, 3)
            self.assertEqual(bars[n - 1].timestamp, _utc_ms(2026, 4, 30, 13, 30))
            self.assertAlmostEqual(bars[n - 1].close, 12.08)

    def test_docker_pretrim_applies_both_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            stamps = _write_eth_feed(d / "feed.csv")
            start = stamps[3]
            trimmed = _trim_ohlcv_csv(d / "feed.csv", start, TV_LAST_BAR)
            self.assertIsNotNone(trimmed)
            assert trimmed is not None
            try:
                kept, n, _ = _load_bars(trimmed)
                self.assertEqual(kept[0].timestamp, start)
                self.assertEqual(kept[n - 1].timestamp, TV_LAST_BAR)
                self.assertEqual(n, stamps.index(TV_LAST_BAR) - 3 + 1)
            finally:
                trimmed.unlink()
            end_only = _trim_ohlcv_csv(d / "feed.csv", None, TV_LAST_BAR)
            assert end_only is not None
            try:
                kept, n, _ = _load_bars(end_only)
                self.assertEqual(kept[0].timestamp, FEED_START)
                self.assertEqual(kept[n - 1].timestamp, TV_LAST_BAR)
            finally:
                end_only.unlink()
            # No bound: no copy is made, the feed is used as given.
            self.assertIsNone(_trim_ohlcv_csv(d / "feed.csv", None, None))


# --- the range-end row and the tape's marker --------------------------

# The 3commas grid-bot shape (browser export): closed lots, then the lots
# still open at the range end, each marked "Open" on its exit row at
# 05-01 08:00 (+8) @ 2261.44.
BROWSER_OPEN_TAPE = [
    (3, "Exit long", "2026-05-01 08:00", "Open", "2261.44"),
    (3, "Entry long", "2026-04-30 20:45", "B", "2189.13"),
    (2, "Exit long", "2026-05-01 08:00", "Open", "2261.44"),
    (2, "Entry long", "2026-04-30 18:00", "B", "2200.0"),
    (1, "Exit long", "2026-04-29 10:15", "X", "2310.5"),
    (1, "Entry long", "2026-04-28 09:00", "A", "2290.0"),
]

# The same position on a ws-report-v1 tape: the exit row has no Signal.
WS_OPEN_TAPE = [
    (3, "Exit long", "2026-05-01 08:00", "", "2261.44"),
    (3, "Entry long", "2026-04-30 20:45", "B", "2189.13"),
    (2, "Exit long", "2026-05-01 08:00", "", "2261.44"),
    (2, "Entry long", "2026-04-30 18:00", "B", "2200.0"),
    (1, "Exit long", "2026-04-29 10:15", "X", "2310.5"),
    (1, "Entry long", "2026-04-28 09:00", "A", "2290.0"),
]

WS_METRICS_ETH = {**BROWSER_METRICS, "tapeChannel": "ws-report-v1",
                  "wsProvenance": {"schemaVersion": 1,
                                   "requestedRange": {"from": 1743465600000, "to": RANGE_TO_MS},
                                   "returnedRange": {"from": 1743465600000, "to": RANGE_TO_MS},
                                   "rangeProof": "covered"}}


def _trade(entry_ms: int, exit_ms: int, entry: float, exit_: float, qty: float,
           *, open_at_end: bool) -> dict:
    pnl = (exit_ - entry) * qty
    return {
        "is_long": True, "entry_time": entry_ms, "exit_time": exit_ms,
        "entry_price": entry, "exit_price": exit_, "qty": qty,
        "pnl": pnl, "pnl_pct": pnl / (entry * qty) * 100.0,
        "max_runup": max(pnl, 0.0), "max_drawdown": 0.0, "commission": 0.0,
        "entry_bar_index": 0, "exit_bar_index": 1, "open_at_end": open_at_end,
    }


# The engine's trades for either tape: one closed trade, two lots still open
# after the final bar and booked as range-end closes on 05-01 00:00 UTC.
ENGINE_TRADES = [
    _trade(_utc_ms(2026, 4, 28, 1, 0), _utc_ms(2026, 4, 29, 2, 15), 2290.0, 2310.5, 0.1,
           open_at_end=False),
    _trade(_utc_ms(2026, 4, 30, 10, 0), TV_LAST_BAR, 2200.0, 2261.44, 0.0912,
           open_at_end=True),
    _trade(_utc_ms(2026, 4, 30, 12, 45), TV_LAST_BAR, 2189.13, 2261.44, 0.0912,
           open_at_end=True),
]


class _FakeStrategy:
    """Stands in for Strategy: records the run kwargs, returns ``trades``
    (ENGINE_TRADES unless _run_main was handed another list)."""
    calls: list[dict] = []
    trades: list[dict] = ENGINE_TRADES

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


def _run_main(strategy_dir: Path, feed: Path, out: Path,
              trades: list[dict] | None = None) -> str:
    """Drive run_strategy.main() end to end with the engine stubbed out;
    returns everything it printed."""
    _FakeStrategy.calls.clear()
    _FakeStrategy.trades = ENGINE_TRADES if trades is None else trades
    argv = ["run_strategy.py", str(strategy_dir), "--ohlcv", str(feed), "-o", str(out)]
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


def _csv_trades(path: Path) -> list[dict]:
    with path.open(encoding="utf-8") as f:
        return list(csv.DictReader(f))


class TapeMarksOpenRowsTests(unittest.TestCase):
    def test_browser_tape_with_open_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", BROWSER_OPEN_TAPE)
            self.assertTrue(_tv_tape_marks_open_rows(d, TZ8))
            # Case and whitespace are not the marker.
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit long", "2026-05-01 08:00", " OPEN ", "2261.44"),
                (1, "Entry long", "2026-04-30 20:45", "B", "2189.13"),
            ])
            self.assertTrue(_tv_tape_marks_open_rows(d, TZ8))

    def test_ws_tape_has_no_marker(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", WS_OPEN_TAPE)
            self.assertFalse(_tv_tape_marks_open_rows(d, TZ8))
            # A signal that merely CONTAINS the word is a comment, not the marker.
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit long", "2026-05-01 08:00", "Open box", "2261.44"),
                (1, "Entry long", "2026-04-30 20:45", "B", "2189.13"),
            ])
            self.assertFalse(_tv_tape_marks_open_rows(d, TZ8))

    def test_no_tape_or_no_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.assertFalse(_tv_tape_marks_open_rows(d, TZ8))
            _write_tape(d / "tv_trades.csv", [])
            self.assertFalse(_tv_tape_marks_open_rows(d, TZ8))

    def test_meta_names_the_tape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            probe = d / "probe"
            probe.mkdir()
            _write_tape(probe / "tv_trades.csv", BROWSER_OPEN_TAPE)
            self.assertTrue(_tv_tape_marks_open_rows(
                d, {"tv_trades_csv": str(probe / "tv_trades.csv"), **TZ8}))
            self.assertFalse(_tv_tape_marks_open_rows(d, TZ8))


class WithholdOpenAtEndTests(unittest.TestCase):
    def test_drops_only_the_flagged_rows_in_order(self) -> None:
        kept, withheld = _withhold_open_at_end_trades(ENGINE_TRADES)
        self.assertEqual(withheld, 2)
        self.assertEqual(kept, [ENGINE_TRADES[0]])
        # Rows without the key (an older report dict) are closed trades.
        legacy = [{k: v for k, v in t.items() if k != "open_at_end"} for t in ENGINE_TRADES]
        self.assertEqual(_withhold_open_at_end_trades(legacy), (legacy, 0))
        self.assertEqual(_withhold_open_at_end_trades([]), ([], 0))


# The marked regime's engine shape: the feed ran unbounded to 05-04 15:00
# UTC, so lot 2 CLOSED for real past the range (the row the grader pairs
# with TV's Open row on entry time) and lot 3 is still open at the FEED's
# end and marked there.
ENGINE_TRADES_UNBOUNDED = [
    ENGINE_TRADES[0],
    _trade(_utc_ms(2026, 4, 30, 10, 0), _utc_ms(2026, 5, 2, 6, 30), 2200.0, 2330.0, 0.0912,
           open_at_end=False),
    _trade(_utc_ms(2026, 4, 30, 12, 45), FEED_END, 2189.13, 2365.09, 0.0912,
           open_at_end=True),
]


class RangeEndRegimeTests(unittest.TestCase):
    """_apply_range_end_regime: the marker picks the regime, the bound is
    set only for an unmarked tape, and the reason names its source."""

    def _probe(self, d: Path, tape, metrics=None) -> None:
        _write_tape(d / "tv_trades.csv", tape)
        if metrics is not None:
            _write_metrics(d / "metrics.json", metrics)

    def test_marked_tape_is_the_baseline_regime(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self._probe(d, BROWSER_OPEN_TAPE, BROWSER_METRICS)
            kwargs: dict = {}
            marks, why = _apply_range_end_regime(d, TZ8, kwargs)
            self.assertTrue(marks)
            self.assertEqual(kwargs, {})
            self.assertIn("baseline regime", why)
            self.assertIn("feed unbounded", why)
            self.assertIn("open_at_end rows kept out of the CSV", why)
            # A probe's own bound is left alone too.
            kwargs = {"ohlcv_end_ms": LAST_EXIT}
            _apply_range_end_regime(d, TZ8, kwargs)
            self.assertEqual(kwargs, {"ohlcv_end_ms": LAST_EXIT})

    def test_unmarked_ws_tape_is_bounded_at_the_requested_to(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self._probe(d, WS_OPEN_TAPE, WS_METRICS_ETH)
            kwargs: dict = {}
            marks, why = _apply_range_end_regime(d, TZ8, kwargs)
            self.assertFalse(marks)
            self.assertEqual(kwargs, {"ohlcv_end_ms": RANGE_TO_MS})
            self.assertIn("range-end regime", why)
            self.assertIn("2026-05-01 00:00 UTC", why)
            self.assertIn("wsProvenance.requestedRange.to", why)
            self.assertIn("rows written", why)

    def test_unmarked_browser_metrics_tape_is_bounded_at_the_to_date(self) -> None:
        # A browser-export metrics.json (dates only) beside a tape with no
        # Open rows: unmarked, so bounded — at `to` as UTC midnight.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self._probe(d, ASIAN_BOX_TAPE, BROWSER_METRICS)
            kwargs: dict = {}
            marks, why = _apply_range_end_regime(d, TZ8, kwargs)
            self.assertFalse(marks)
            self.assertEqual(kwargs, {"ohlcv_end_ms": RANGE_TO_MS})
            self.assertIn("(metrics.json to)", why)

    def test_unmarked_tape_without_a_range_falls_back_to_its_last_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self._probe(d, ASIAN_BOX_TAPE)
            kwargs: dict = {}
            marks, why = _apply_range_end_regime(d, TZ8, kwargs)
            self.assertFalse(marks)
            self.assertEqual(kwargs, {"ohlcv_end_ms": LAST_EXIT})
            self.assertIn("the tape's last row", why)

    def test_unmarked_tape_keeps_the_probes_own_bound(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self._probe(d, WS_OPEN_TAPE, WS_METRICS_ETH)
            kwargs = {"ohlcv_end_ms": LAST_EXIT}
            marks, why = _apply_range_end_regime(d, TZ8, kwargs)
            self.assertFalse(marks)
            self.assertEqual(kwargs, {"ohlcv_end_ms": LAST_EXIT})
            self.assertIn("probe's own ohlcv_end_ms", why)
            self.assertIn("2026-04-29 18:00 UTC", why)

    def test_sources_are_named(self) -> None:
        self.assertEqual(_tv_metrics_range_end(WS_METRICS_ETH),
                         (RANGE_TO_MS, "metrics.json wsProvenance.requestedRange.to"))
        self.assertEqual(_tv_metrics_range_end(BROWSER_METRICS),
                         (RANGE_TO_MS, "metrics.json to"))
        self.assertIsNone(_tv_metrics_range_end({"strategy": "x"}))
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", ASIAN_BOX_TAPE)
            self.assertEqual(_load_tv_range_end(d, TZ8),
                             (LAST_EXIT, "the tape's last row (metrics.json carries no range)"))
            _write_tape(d / "tv_trades.csv", [])
            self.assertIsNone(_load_tv_range_end(d, TZ8))
            self.assertIsNone(_load_tv_range_end_ms(d, TZ8))


class RangeEndRowSymmetryTests(unittest.TestCase):
    """main(): an unmarked tape bounds the feed at the range end and writes
    the range-end rows; a marked tape runs the feed unbounded, withholds the
    open_at_end rows and keeps the closed rows; a header-only tape (no TV
    window) is measured as given with every row written; and one log line
    says which regime applied and why."""

    def _probe(self, d: Path, tape, metrics) -> tuple[Path, Path]:
        _write_eth_feed(d / "feed.csv")
        _write_tape(d / "tv_trades.csv", tape)
        _write_metrics(d / "metrics.json", metrics)
        (d / "inputs.json").write_text(json.dumps(TZ8), encoding="utf-8")
        return d / "feed.csv", d / "engine_trades.csv"

    def test_browser_tape_withholds_the_open_at_end_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = self._probe(d, BROWSER_OPEN_TAPE, BROWSER_METRICS)
            printed = _run_main(d, feed, out)
            rows = _csv_trades(out)
            # The closed trade is intact, renumbered from 1, exit then entry.
            self.assertEqual([(r["Trade #"], r["Type"]) for r in rows],
                             [("1", "Exit long"), ("1", "Entry long")])
            self.assertEqual(rows[0]["Price"], "2310.500000")
            self.assertEqual(rows[1]["Price"], "2290.000000")
            self.assertEqual(rows[0]["Net PnL"], f"{20.5 * 0.1:.6f}")
            self.assertEqual(rows[0]["Cumulative PnL"], f"{20.5 * 0.1:.6f}")
            # The count and the reason are logged, once.
            self.assertEqual(printed.count("withheld 2 open_at_end trade(s)"), 1)
            self.assertIn("Signal 'Open'", printed)
            self.assertIn("1 trades (3 raw) (2 open_at_end withheld)", printed)
            # The marked regime: NO feed bound — the baseline's measurement.
            self.assertIsNone(_FakeStrategy.calls[0].get("ohlcv_end_ms"))
            self.assertEqual(printed.count("range-end: tape marks its open rows"), 1)
            self.assertIn("baseline regime", printed)

    def test_browser_tape_keeps_the_post_range_close_and_withholds_the_feed_end_mark(self) -> None:
        # The realistic marked shape on the unbounded ETH feed: lot 2 closed
        # for real on 05-02 (past TV's range; written, as the baseline wrote
        # it — the grader pairs it with TV's Open row on entry time) and lot
        # 3 marked at the FEED's last bar 05-04 15:00 (withheld).
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = self._probe(d, BROWSER_OPEN_TAPE, BROWSER_METRICS)
            printed = _run_main(d, feed, out, trades=ENGINE_TRADES_UNBOUNDED)
            rows = _csv_trades(out)
            self.assertEqual([(r["Trade #"], r["Type"], r["Date and time"]) for r in rows],
                             [("2", "Exit long", "2026-05-02 06:30"),
                              ("2", "Entry long", "2026-04-30 10:00"),
                              ("1", "Exit long", "2026-04-29 02:15"),
                              ("1", "Entry long", "2026-04-28 01:00")])
            self.assertNotIn("2026-05-04", "".join(r["Date and time"] for r in rows))
            self.assertIn("withheld 1 open_at_end trade(s)", printed)
            self.assertIn("2 trades (3 raw) (1 open_at_end withheld)", printed)
            self.assertIsNone(_FakeStrategy.calls[0].get("ohlcv_end_ms"))

    def test_ws_tape_writes_the_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = self._probe(d, WS_OPEN_TAPE, WS_METRICS_ETH)
            printed = _run_main(d, feed, out)
            rows = _csv_trades(out)
            self.assertEqual([(r["Trade #"], r["Type"]) for r in rows],
                             [("3", "Exit long"), ("3", "Entry long"),
                              ("2", "Exit long"), ("2", "Entry long"),
                              ("1", "Exit long"), ("1", "Entry long")])
            # The range-end rows are ordinary exits on TV's last bar.
            self.assertEqual(rows[0]["Date and time"], "2026-05-01 00:00")
            self.assertEqual(rows[0]["Price"], "2261.440000")
            self.assertEqual(rows[2]["Date and time"], "2026-05-01 00:00")
            self.assertNotIn("withheld", printed)
            self.assertIn("3 trades,", printed)
            # The unmarked regime: bounded at TV's range end, source named.
            self.assertEqual(_FakeStrategy.calls[0]["ohlcv_end_ms"], RANGE_TO_MS)
            self.assertEqual(printed.count("range-end: tape unmarked"), 1)
            self.assertIn("feed bounded at TV's range end 2026-05-01 00:00 UTC "
                          "(metrics.json wsProvenance.requestedRange.to)", printed)

    def test_unmarked_tape_with_browser_metrics_is_bounded_at_the_to_date(self) -> None:
        # A tape with no Open rows beside a dates-only metrics.json (no
        # wsProvenance): unmarked, so bounded at `to` as UTC midnight and
        # the rows written.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = self._probe(d, WS_OPEN_TAPE, BROWSER_METRICS)
            printed = _run_main(d, feed, out)
            self.assertEqual(_FakeStrategy.calls[0]["ohlcv_end_ms"], RANGE_TO_MS)
            self.assertIn("(metrics.json to)", printed)
            self.assertNotIn("withheld", printed)
            rows = _csv_trades(out)
            # Every engine trade entered inside the window is written, the
            # two range-end rows as ordinary exits on TV's last bar.
            self.assertEqual([r["Date and time"] for r in rows if r["Type"] == "Exit long"],
                             ["2026-05-01 00:00", "2026-05-01 00:00", "2026-04-29 02:15"])

    def test_tape_without_rows_is_unchanged(self) -> None:
        # Header only: no TV window, so no regime, no bound, nothing
        # withheld — the run measures the feed it was given and writes
        # every trade.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = self._probe(d, [], BROWSER_METRICS)
            printed = _run_main(d, feed, out)
            rows = _csv_trades(out)
            # The emit window falls back to the reference feed's span (the
            # feed here starts 04-29, so the 04-28 entry is outside it, as
            # before); both range-end rows are written as ordinary exits.
            self.assertEqual([(r["Trade #"], r["Type"], r["Date and time"]) for r in rows],
                             [("2", "Exit long", "2026-05-01 00:00"),
                              ("2", "Entry long", "2026-04-30 12:45"),
                              ("1", "Exit long", "2026-05-01 00:00"),
                              ("1", "Entry long", "2026-04-30 10:00")])
            self.assertNotIn("withheld", printed)
            self.assertNotIn("range-end:", printed)
            self.assertIsNone(_FakeStrategy.calls[0].get("ohlcv_end_ms"))

    def test_browser_tape_with_a_flat_engine_logs_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = self._probe(d, BROWSER_OPEN_TAPE, BROWSER_METRICS)
            with mock.patch.object(run_strategy, "_filter_trades_to_window",
                                   lambda trades, window: [ENGINE_TRADES[0]]):
                printed = _run_main(d, feed, out)
            self.assertEqual(len(_csv_trades(out)), 2)
            self.assertNotIn("withheld", printed)
            # The regime line is still logged; only the withhold line is not.
            self.assertEqual(printed.count("range-end:"), 1)


if __name__ == "__main__":
    unittest.main()
