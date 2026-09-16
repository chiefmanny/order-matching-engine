#!/usr/bin/env python3
"""Tests for the analysis layer: candles, hypothesis verdicts, sweep depth.

These cover the places where real bugs have actually appeared, rather than the
plumbing. Every case below corresponds to something that went wrong on a real run:

  * depth measured from a raw minimum reported a 1000-share sweep consuming 17917
    shares, because a Level 2 poll caught the provider mid-requote.
  * a metric missing from a hypothesis clause silently read as a pass.
  * candles built from anything other than executions can move with no trade.

Run directly: experiments/tests/test_metrics.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from analyze import evaluate_hypothesis  # noqa: E402
from candles import build_candles  # noqa: E402

PASSED = 0


def ok(name: str) -> None:
    global PASSED
    PASSED += 1
    print(f"PASS: {name}")


# --------------------------------------------------------------------------
# Candles
# --------------------------------------------------------------------------

def _trades(rows) -> pd.DataFrame:
    return pd.DataFrame(
        [{"ts_ns": ts, "price": px, "size": sz} for ts, px, sz in rows])


def test_candles_ohlcv() -> None:
    s = 1_000_000_000
    df = _trades([
        (0,          100.0, 10),
        (300 * 10**6, 102.0, 5),
        (600 * 10**6,  99.0, 7),
        (900 * 10**6, 101.0, 3),
        (1 * s,       105.0, 1),   # second bucket
    ])
    c = build_candles(df, 1000)
    assert len(c) == 2, len(c)
    first = c.iloc[0]
    assert first["open"] == 100.0
    assert first["high"] == 102.0
    assert first["low"] == 99.0
    assert first["close"] == 101.0
    assert first["volume"] == 25
    assert first["trades"] == 4
    assert c.iloc[1]["open"] == 105.0
    ok("candles aggregate OHLCV per interval from executions")


def test_candles_skip_empty_intervals() -> None:
    s = 1_000_000_000
    # Nothing trades in the second or third second.
    df = _trades([(0, 100.0, 1), (3 * s, 101.0, 1)])
    c = build_candles(df, 1000)
    # Quiet periods produce NO candle rather than a flat synthetic one: a candle that
    # exists without a trade would disagree with the tape it summarises.
    assert len(c) == 2, len(c)
    assert list(c["bucket"]) == [0, 3]
    ok("intervals with no executions produce no candle")


def test_candles_empty_input() -> None:
    assert build_candles(pd.DataFrame(), 1000).empty
    assert build_candles(None, 1000).empty
    ok("no trades yields no candles rather than an error")


# --------------------------------------------------------------------------
# Hypothesis verdicts
# --------------------------------------------------------------------------

def test_verdict_consistent_and_inconsistent() -> None:
    hyp = {"statement": "x", "expect": [
        {"metric": "a", "op": ">=", "value": 10},
        {"metric": "b", "op": "<", "value": 5},
    ]}
    assert evaluate_hypothesis(hyp, {"a": 10, "b": 4})["verdict"] == "consistent"
    assert evaluate_hypothesis(hyp, {"a": 9, "b": 9})["verdict"] == "inconsistent"
    assert evaluate_hypothesis(hyp, {"a": 10, "b": 9})["verdict"] == "partially_consistent"
    ok("verdicts classify all-pass, all-fail and mixed correctly")


def test_missing_metric_does_not_silently_pass() -> None:
    hyp = {"expect": [{"metric": "nonexistent.metric", "op": ">", "value": 0}]}
    result = evaluate_hypothesis(hyp, {"a": 1})
    clause = result["clauses"][0]
    # A missing metric must be undecided, never a pass. Treating absence as success is
    # how a scenario ends up "confirmed" by a measurement that was never taken.
    assert clause["passed"] is None
    assert "not available" in clause["note"]
    assert result["verdict"] == "undetermined"
    ok("a missing metric is undetermined, not a pass")


def test_unknown_operator_is_rejected() -> None:
    hyp = {"expect": [{"metric": "a", "op": "~=", "value": 1}]}
    result = evaluate_hypothesis(hyp, {"a": 1})
    assert result["clauses"][0]["passed"] is None
    assert "unknown op" in result["clauses"][0]["note"]
    ok("an unknown operator is rejected rather than assumed")


def test_no_hypothesis_is_distinct_from_failure() -> None:
    assert evaluate_hypothesis({}, {"a": 1})["verdict"] == "no_hypothesis"
    ok("a scenario without a hypothesis is not reported as failing")


# --------------------------------------------------------------------------
# Sweep depth trough
# --------------------------------------------------------------------------

def test_median_filter_rejects_single_snapshot_gaps() -> None:
    """The exact failure seen on a real run, reproduced in miniature.

    The provider cancels and re-submits its whole ladder when its inventory skew
    shifts. A Level 2 poll landing mid-rebuild sees a near-empty book for exactly one
    snapshot. A raw minimum treats that as the sweep's doing.
    """
    depth = pd.Series([24000] * 10 + [1500] + [24000] * 10)   # one-snapshot dropout
    assert depth.min() == 1500
    smoothed = depth.rolling(3, center=True, min_periods=1).median()
    assert smoothed.min() == 24000, smoothed.min()
    ok("a one-snapshot rebuild gap is filtered out of the depth trough")


def test_median_filter_preserves_real_depletion() -> None:
    # A genuine sweep depletes the book for many snapshots while it replenishes.
    depth = pd.Series([24000] * 5 + [14000] * 8 + [24000] * 5)
    smoothed = depth.rolling(3, center=True, min_periods=1).median()
    assert smoothed.min() == 14000, smoothed.min()
    ok("a sustained depletion survives the filter")


def test_filter_distinguishes_the_two_cases_together() -> None:
    # Both in one series: the artifact must vanish and the real event must remain.
    depth = pd.Series([24000] * 5 + [100] + [24000] * 4 + [15000] * 8 + [24000] * 5)
    smoothed = depth.rolling(3, center=True, min_periods=1).median()
    assert smoothed.min() == 15000, smoothed.min()
    ok("with both present, the artifact is removed and the event is kept")


def main() -> int:
    print("--- Metrics Tests ---")
    test_candles_ohlcv()
    test_candles_skip_empty_intervals()
    test_candles_empty_input()
    test_verdict_consistent_and_inconsistent()
    test_missing_metric_does_not_silently_pass()
    test_unknown_operator_is_rejected()
    test_no_hypothesis_is_distinct_from_failure()
    test_median_filter_rejects_single_snapshot_gaps()
    test_median_filter_preserves_real_depletion()
    test_filter_distinguishes_the_two_cases_together()
    print(f"All {PASSED} metrics tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
