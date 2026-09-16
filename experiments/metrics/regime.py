"""Before/after comparison across a scenario marker.

Some market events are dramatic in the order book and nearly invisible in the trade
tape. Liquidity withdrawal is the clearest case: cancelling orders produces no trades
at all, so price and candles can look entirely normal while displayed depth collapses
and the spread blows out. Judged on candles alone, nothing happened.

This module splits a run at its marker and reports both spaces side by side, so that
claim becomes a measurement rather than an assertion about a picture.

It is deliberately generic rather than withdrawal-specific: the same before/after
framing applies to a volatility shock, a regime change, or anything else a scenario
marks with a timestamp.
"""
from __future__ import annotations

import numpy as np
import pandas as pd


def _marker(run) -> tuple[str, float] | None:
    """The marker to split on.

    A scenario can nominate one explicitly with `split: true`, which matters when the
    event takes time: splitting at the START of a 4-second withdrawal averages the
    transition into the "after" window and understates the change. Marking the point
    where the event has finished compares two settled regimes instead.
    """
    markers = run.markers or []
    for m in markers:
        if m.get("split") and m.get("at") is not None:
            return str(m.get("label", "event")), float(m["at"])
    for m in markers:
        if m.get("at") is not None:
            return str(m.get("label", "event")), float(m["at"])
    return None


def _warmup_seconds(run) -> float:
    """When every participant is finally running.

    Anything earlier is startup, not a market, and including it in the "before"
    window drags the comparison in ways that have nothing to do with the event.
    """
    starts = [float(p.get("start_at", 0) or 0)
              for p in run.metadata.get("participants", []) or []]
    return max(starts) if starts else 0.0


def _window_stats(bbo: pd.DataFrame, l2: pd.DataFrame, trades: pd.DataFrame,
                  candles: pd.DataFrame, tick: float) -> dict:
    out: dict = {}

    two = bbo[bbo["mid"].notna()] if not bbo.empty else pd.DataFrame()
    if not two.empty:
        out["spread_ticks"] = round(float(two["spread"].mean()) / tick, 2)
        out["spread_max_ticks"] = round(float(two["spread"].max()) / tick, 1)
        out["two_sided_fraction"] = round(len(two) / max(len(bbo), 1), 3)
        rets = np.diff(np.log(two["mid"].to_numpy()))
        rets = rets[np.isfinite(rets)]
        out["realized_vol_bps"] = round(float(np.std(rets) * 1e4), 3) if len(rets) else 0.0
    if not bbo.empty:
        out["two_sided_fraction"] = round(
            float(bbo["mid"].notna().sum()) / len(bbo), 3)

    if not l2.empty:
        per = l2.groupby(["snapshot_id", "side"])["size"].sum().unstack(fill_value=0)
        for side, name in (("BID", "bid_depth"), ("ASK", "ask_depth")):
            if side in per:
                out[name] = int(round(float(per[side].mean())))
        levels = l2.groupby(["snapshot_id", "side"])["rank"].count().unstack(fill_value=0)
        for side, name in (("BID", "bid_levels"), ("ASK", "ask_levels")):
            if side in levels:
                out[name] = round(float(levels[side].mean()), 1)

    if not trades.empty:
        span = (trades["ts_ns"].max() - trades["ts_ns"].min()) / 1e9
        out["trades"] = int(len(trades))
        out["trade_rate_per_s"] = round(len(trades) / span, 3) if span > 0 else 0.0
        out["volume"] = int(trades["size"].sum())
    else:
        out["trades"] = 0
        out["trade_rate_per_s"] = 0.0
        out["volume"] = 0

    if not candles.empty:
        out["candle_range_ticks"] = round(
            float((candles["high"] - candles["low"]).mean()) / tick, 2)
        out["non_empty_candles"] = int(len(candles))

    return out


def compute(run, tick: float) -> dict:
    marker = _marker(run)
    if marker is None:
        return {}
    label, at = marker

    t0 = run.t0_ns
    split_ns = t0 + int(at * 1e9)

    # Symmetric windows either side of the split, as wide as both sides allow. Equal
    # durations keep rate-based metrics (trades/s) and averages comparable; an
    # unbalanced split would make the longer side look quieter purely by averaging.
    warmup = _warmup_seconds(run)
    duration = float(run.metadata.get("duration_seconds", 0) or 0)
    width = min(at - warmup, duration - at) if duration > at else (at - warmup)
    if width <= 0:
        width = max(at - warmup, 1.0)
    lo_ns = t0 + int((at - width) * 1e9)
    hi_ns = t0 + int((at + width) * 1e9)

    def split(df: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
        if df is None or df.empty or "ts_ns" not in df:
            return pd.DataFrame(), pd.DataFrame()
        return (df[(df["ts_ns"] >= lo_ns) & (df["ts_ns"] < split_ns)],
                df[(df["ts_ns"] >= split_ns) & (df["ts_ns"] < hi_ns)])

    bbo_b, bbo_a = split(run.bbo)
    l2_b, l2_a = split(run.level2)
    tr_b, tr_a = split(run.trades)

    from candles import build_candles
    c_b = build_candles(tr_b, 1000)
    c_a = build_candles(tr_a, 1000)

    before = _window_stats(bbo_b, l2_b, tr_b, c_b, tick)
    after = _window_stats(bbo_a, l2_a, tr_a, c_a, tick)

    out: dict = {"regime.marker": label, "regime.split_at_s": at,
                 "regime.window_s": round(width, 2)}
    for key, value in before.items():
        out[f"regime.before.{key}"] = value
    for key, value in after.items():
        out[f"regime.after.{key}"] = value

    # Ratios make "collapsed" vs "barely moved" directly comparable between the book
    # and the tape, which is the whole point of the split.
    for key in ("bid_depth", "ask_depth", "spread_ticks", "trade_rate_per_s",
                "candle_range_ticks", "realized_vol_bps"):
        b, a = before.get(key), after.get(key)
        if isinstance(b, (int, float)) and isinstance(a, (int, float)) and b:
            out[f"regime.ratio.{key}"] = round(a / b, 3)
    return out
