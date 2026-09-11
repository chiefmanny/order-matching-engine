#!/usr/bin/env python3
"""Diagnostic plots for a run.

Development tools, not the production chart — finchart owns real charting. These
exist to answer "what actually happened?" quickly enough to keep the tune/rerun loop
tight.

Deliberately includes BOTH order-book space and trade/candle space. A market event
can be dramatic in one and invisible in the other: liquidity withdrawal collapses
depth while producing no trades at all, and judging it from candles alone would miss
it entirely.
"""
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from candles import build_candles  # noqa: E402
from lib import loaders  # noqa: E402


def _mark(ax, run) -> None:
    """Draw scenario trigger times so an event is visible in every panel."""
    for m in run.markers:
        at = m.get("at")
        if at is None:
            continue
        ax.axvline(at, color="crimson", linestyle="--", linewidth=1.1, alpha=0.8)
        ax.annotate(m.get("label", ""), xy=(at, 1.0), xycoords=("data", "axes fraction"),
                    xytext=(3, -12), textcoords="offset points",
                    color="crimson", fontsize=8, fontweight="bold")


def _save(fig, out_dir: Path, name: str, written: list) -> None:
    path = out_dir / name
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    written.append(path)


def render(run, values: dict | None = None) -> list[Path]:
    out_dir = run.path / "plots"
    out_dir.mkdir(exist_ok=True)
    written: list[Path] = []

    bbo = run.bbo[run.bbo["spread"].notna()].copy() if not run.bbo.empty else pd.DataFrame()
    if not bbo.empty:
        bbo["t"] = run.elapsed(bbo["ts_ns"])
    trades = run.trades.copy()
    if not trades.empty:
        trades["t"] = run.elapsed(trades["ts_ns"])

    # 1. Price: mid over time with executions overlaid, coloured by aggressor.
    if not bbo.empty or not trades.empty:
        fig, ax = plt.subplots(figsize=(11, 4))
        if not bbo.empty:
            ax.plot(bbo["t"], bbo["mid"], color="#334155", linewidth=1.0, label="mid")
            ax.fill_between(bbo["t"], bbo["best_bid"], bbo["best_ask"],
                            color="#94a3b8", alpha=0.35, label="bid/ask")
        if not trades.empty:
            buys = trades[trades["aggressor_is_buy"] == 1]
            sells = trades[trades["aggressor_is_buy"] == 0]
            ax.scatter(buys["t"], buys["price"], s=14, color="#16a34a",
                       label="buyer-initiated", zorder=3)
            ax.scatter(sells["t"], sells["price"], s=14, color="#dc2626",
                       label="seller-initiated", zorder=3)
        ax.set_xlabel("seconds"); ax.set_ylabel("price")
        ax.set_title(f"{run.name} — price and executions")
        ax.legend(loc="upper left", fontsize=8); ax.grid(alpha=0.25)
        _mark(ax, run)
        _save(fig, out_dir, "price.png", written)

    # 2. Spread over time.
    if not bbo.empty:
        fig, ax = plt.subplots(figsize=(11, 2.8))
        ax.plot(bbo["t"], bbo["spread"], color="#7c3aed", linewidth=1.0)
        ax.set_xlabel("seconds"); ax.set_ylabel("spread")
        ax.set_title(f"{run.name} — spread"); ax.grid(alpha=0.25)
        _mark(ax, run)
        _save(fig, out_dir, "spread.png", written)

    # 3. Depth and depth imbalance — the panel that shows liquidity events which
    #    leave no trace in the candles.
    if not run.level2.empty:
        per = run.level2.groupby(["snapshot_id", "side"]).agg(
            depth=("size", "sum"), ts_ns=("ts_ns", "first")).reset_index()
        bids = per[per["side"] == "BID"].set_index("snapshot_id")
        asks = per[per["side"] == "ASK"].set_index("snapshot_id")
        idx = bids.index.union(asks.index)
        bd = bids["depth"].reindex(idx, fill_value=0)
        ad = asks["depth"].reindex(idx, fill_value=0)
        ts = bids["ts_ns"].reindex(idx).fillna(asks["ts_ns"].reindex(idx))
        t = run.elapsed(ts)

        fig, axes = plt.subplots(2, 1, figsize=(11, 5.5), sharex=True)
        axes[0].plot(t, bd, color="#16a34a", label="bid depth")
        axes[0].plot(t, ad, color="#dc2626", label="ask depth")
        axes[0].set_ylabel("shares"); axes[0].legend(fontsize=8); axes[0].grid(alpha=0.25)
        axes[0].set_title(f"{run.name} — displayed depth")

        total = (bd + ad).replace(0, np.nan)
        axes[1].plot(t, (bd - ad) / total, color="#0891b2")
        axes[1].axhline(0, color="#64748b", linewidth=0.8)
        axes[1].set_ylim(-1, 1); axes[1].set_ylabel("depth imbalance")
        axes[1].set_xlabel("seconds"); axes[1].grid(alpha=0.25)
        for ax in axes:
            _mark(ax, run)
        _save(fig, out_dir, "depth.png", written)

    # 4. Candlesticks from real executions, with volume.
    candles = build_candles(run.trades, 1000)
    if not candles.empty:
        fig, axes = plt.subplots(2, 1, figsize=(11, 5.5), sharex=True,
                                 gridspec_kw={"height_ratios": [3, 1]})
        ax = axes[0]
        width = 0.6
        for _, c in candles.iterrows():
            up = c["close"] >= c["open"]
            color = "#16a34a" if up else "#dc2626"
            ax.plot([c["t_seconds"], c["t_seconds"]], [c["low"], c["high"]],
                    color=color, linewidth=1.0)
            lo = min(c["open"], c["close"])
            height = abs(c["close"] - c["open"])
            ax.add_patch(plt.Rectangle((c["t_seconds"] - width / 2, lo), width,
                                       height if height > 0 else 1e-9,
                                       facecolor=color, edgecolor=color))
        ax.set_ylabel("price"); ax.grid(alpha=0.25)
        ax.set_title(f"{run.name} — 1s candles from executions")

        axes[1].bar(candles["t_seconds"], candles["volume"], width=width, color="#64748b")
        axes[1].set_ylabel("volume"); axes[1].set_xlabel("seconds"); axes[1].grid(alpha=0.25)
        for a in axes:
            _mark(a, run)
        _save(fig, out_dir, "candles.png", written)

    # 5. Per-participant inventory over time, from the structured event log.
    ev = run.events
    if not ev.empty and "type" in ev and (ev["type"] == "fill").any():
        fills = ev[ev["type"] == "fill"].copy()
        fig, ax = plt.subplots(figsize=(11, 3.2))
        plotted = False
        for pid, grp in fills.groupby("participant_id"):
            grp = grp.sort_values("ts_ns")
            signed = np.where(grp["buy"].to_numpy(), grp["size"].to_numpy(),
                              -grp["size"].to_numpy())
            ax.step(run.elapsed(grp["ts_ns"]), np.cumsum(signed), where="post", label=str(pid))
            plotted = True
        if plotted:
            ax.axhline(0, color="#64748b", linewidth=0.8)
            ax.set_xlabel("seconds"); ax.set_ylabel("inventory (shares)")
            ax.set_title(f"{run.name} — participant inventory")
            ax.legend(fontsize=8); ax.grid(alpha=0.25)
            _mark(ax, run)
            _save(fig, out_dir, "inventory.png", written)
        else:
            plt.close(fig)

    return written


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: plots.py <run_dir>")
        return 1
    run = loaders.load(sys.argv[1])
    written = render(run)
    for path in written:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
