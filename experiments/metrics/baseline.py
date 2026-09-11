"""Baseline market character: spread, depth, trade rate, volatility, candles."""
from __future__ import annotations

import numpy as np
import pandas as pd

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from candles import build_candles  # noqa: E402


def compute(run, tick: float) -> dict:
    out: dict = {}

    # --- trades ---------------------------------------------------------
    t = run.trades
    out["trades.count"] = int(len(t))
    if not t.empty:
        span = (t["ts_ns"].iloc[-1] - t["ts_ns"].iloc[0]) / 1e9
        out["trades.span_seconds"] = round(float(span), 3)
        out["trades.rate_per_s"] = round(len(t) / span, 3) if span > 0 else 0.0
        out["trades.volume"] = int(t["size"].sum())
        out["trades.mean_size"] = round(float(t["size"].mean()), 1)
        out["price.first"] = round(float(t["price"].iloc[0]), 4)
        out["price.last"] = round(float(t["price"].iloc[-1]), 4)
        out["price.min"] = round(float(t["price"].min()), 4)
        out["price.max"] = round(float(t["price"].max()), 4)
        out["price.range_ticks"] = int(round((t["price"].max() - t["price"].min()) / tick))
        first = float(t["price"].iloc[0])
        out["price.total_drift_pct"] = round(
            abs(float(t["price"].iloc[-1]) - first) / first * 100.0, 4) if first else 0.0
    else:
        for k in ("trades.rate_per_s", "trades.volume", "trades.mean_size",
                  "price.range_ticks", "price.total_drift_pct"):
            out[k] = 0

    # --- BBO / spread ---------------------------------------------------
    b = run.bbo
    if not b.empty:
        two = b[b["spread"].notna()]
        out["book.two_sided_fraction"] = round(len(two) / len(b), 4)
        out["book.bbo_samples"] = int(len(b))
        if not two.empty:
            out["spread.mean"] = round(float(two["spread"].mean()), 4)
            out["spread.median"] = round(float(two["spread"].median()), 4)
            out["spread.max"] = round(float(two["spread"].max()), 4)
            out["spread.mean_ticks"] = round(float(two["spread"].mean()) / tick, 2)
            out["spread.max_ticks"] = int(round(float(two["spread"].max()) / tick))
            out["mid.min"] = round(float(two["mid"].min()), 4)
            out["mid.max"] = round(float(two["mid"].max()), 4)
            out["mid.range_ticks"] = int(round(
                (float(two["mid"].max()) - float(two["mid"].min())) / tick))
            # Short-window realised volatility of the mid, annotated in basis points
            # so it is comparable across reference prices.
            rets = np.diff(np.log(two["mid"].to_numpy()))
            rets = rets[np.isfinite(rets)]
            out["mid.realized_vol_bps"] = round(float(np.std(rets) * 1e4), 3) if len(rets) else 0.0

    # --- Level 2 depth --------------------------------------------------
    l2 = run.level2
    if not l2.empty:
        per_snap = l2.groupby(["snapshot_id", "side"]).agg(
            depth=("size", "sum"), levels=("rank", "count")).reset_index()
        bids = per_snap[per_snap["side"] == "BID"]
        asks = per_snap[per_snap["side"] == "ASK"]
        if not bids.empty:
            out["book.bid_depth"] = int(round(bids["depth"].mean()))
            out["book.bid_levels"] = round(float(bids["levels"].mean()), 2)
            out["book.bid_depth_min"] = int(bids["depth"].min())
        if not asks.empty:
            out["book.ask_depth"] = int(round(asks["depth"].mean()))
            out["book.ask_levels"] = round(float(asks["levels"].mean()), 2)
            out["book.ask_depth_min"] = int(asks["depth"].min())
        if not bids.empty and not asks.empty:
            merged = bids.merge(asks, on="snapshot_id", suffixes=("_bid", "_ask"))
            total = merged["depth_bid"] + merged["depth_ask"]
            imb = (merged["depth_bid"] - merged["depth_ask"]) / total.replace(0, np.nan)
            out["book.depth_imbalance_mean"] = round(float(imb.mean()), 4)
            out["book.depth_imbalance_abs_mean"] = round(float(imb.abs().mean()), 4)

    # --- candles from real executions -----------------------------------
    for label, ms in (("250ms", 250), ("1s", 1000), ("5s", 5000)):
        c = build_candles(run.trades, ms)
        if c.empty:
            out[f"candles.{label}.non_empty"] = 0
            continue
        out[f"candles.{label}.non_empty"] = int(len(c))
        out[f"candles.{label}.mean_range"] = round(float((c["high"] - c["low"]).mean()), 4)
        out[f"candles.{label}.mean_range_ticks"] = round(
            float((c["high"] - c["low"]).mean()) / tick, 2)
        out[f"candles.{label}.max_range_ticks"] = int(round(
            float((c["high"] - c["low"]).max()) / tick))
    return out
