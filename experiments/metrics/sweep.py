"""Sweep metrics: market impact and liquidity recovery.

Division of labour with the C++ strategy is deliberate. The sweep participant knows
its OWN execution (what it sent, what filled, its VWAP and slippage) and reports that
in its metrics. It cannot credibly report what happened to the MARKET, because it only
sees its own fills. So everything here is computed from the recorder's independent
trade/BBO/Level 2 artifacts, using the sweep's start and end timestamps from the
event log to locate the event in time.

That separation is what makes "the sweep moved the price" a measurement rather than
the sweeper's own claim.
"""
from __future__ import annotations

import numpy as np
import pandas as pd


def _sweep_window(run) -> tuple[str, int, int] | None:
    """Find the sweep participant and the ns window over which it executed."""
    ev = run.events
    if ev.empty or "type" not in ev:
        return None

    # Identify sweep participants from the scenario, not by guessing at ids.
    # Either registered name: `sweep` is an alias for the executor's immediate mode.
    sweep_ids = [p["id"] for p in run.metadata.get("participants", [])
                 if p.get("strategy") in ("sweep", "parent_executor")]
    if not sweep_ids:
        return None
    pid = sweep_ids[0]

    mine = ev[ev["participant_id"] == pid]
    if mine.empty:
        return None

    # "armed" marks the arrival instant; the last fill (or the stop) marks the end.
    armed = mine[(mine["type"] == "strategy_note") &
                 (mine.get("message", pd.Series(dtype=str)) == "sweep: armed")]
    start_ns = int(armed["ts_ns"].iloc[0]) if not armed.empty else int(mine["ts_ns"].iloc[0])

    fills = mine[mine["type"] == "fill"]
    end_ns = int(fills["ts_ns"].iloc[-1]) if not fills.empty else start_ns
    return pid, start_ns, end_ns


def _mid_at(bbo: pd.DataFrame, ts_ns: int) -> float | None:
    """Most recent two-sided mid at or before ts_ns."""
    if bbo.empty:
        return None
    prior = bbo[(bbo["ts_ns"] <= ts_ns) & bbo["mid"].notna()]
    if prior.empty:
        return None
    return float(prior["mid"].iloc[-1])


def _depth_at(level2: pd.DataFrame, ts_ns: int, side: str) -> float | None:
    """Total displayed depth on `side` in the snapshot at or before ts_ns."""
    if level2.empty:
        return None
    prior = level2[(level2["ts_ns"] <= ts_ns) & (level2["side"] == side)]
    if prior.empty:
        return None
    snap = int(prior["snapshot_id"].iloc[-1])
    rows = level2[(level2["snapshot_id"] == snap) & (level2["side"] == side)]
    return float(rows["size"].sum())


def compute(run, tick: float) -> dict:
    window = _sweep_window(run)
    if window is None:
        return {}
    pid, start_ns, end_ns = window

    out: dict = {"sweep.participant": pid,
                 "sweep.duration_ms": round((end_ns - start_ns) / 1e6, 1)}

    bbo = run.bbo[run.bbo["mid"].notna()] if not run.bbo.empty else pd.DataFrame()
    l2 = run.level2

    # Which side of the book the sweep consumed.
    side_cfg = next((p.get("config", {}).get("side", "BUY")
                     for p in run.metadata.get("participants", []) if p["id"] == pid), "BUY")
    consumed_side = "ASK" if side_cfg == "BUY" else "BID"
    out["sweep.side"] = side_cfg

    pre_mid = _mid_at(bbo, start_ns)
    if pre_mid is None:
        return out
    out["sweep.pre_mid"] = round(pre_mid, 4)

    # --- price impact ---------------------------------------------------
    post_mid = _mid_at(bbo, end_ns)
    if post_mid is not None:
        out["sweep.post_mid"] = round(post_mid, 4)
        # Signed so positive always means "moved against the sweeper".
        move = (post_mid - pre_mid) if side_cfg == "BUY" else (pre_mid - post_mid)
        out["sweep.impact_ticks"] = round(move / tick, 2)
        out["sweep.impact_bps"] = round(move / pre_mid * 1e4, 3)

    # Peak impact during the event, not just at its end.
    #
    # The window is extended past the sweep's own end because a sweep can complete
    # faster than the recorder samples the BBO (an unsliced sweep fills in a single
    # match, well inside the 100ms WatchSymbol cadence). Measuring strictly between
    # start and end would then find no samples at all and silently report nothing.
    lookahead_ns = 1_000_000_000
    during = bbo[(bbo["ts_ns"] >= start_ns) & (bbo["ts_ns"] <= end_ns + lookahead_ns)]
    if not during.empty:
        peak = during["mid"].max() if side_cfg == "BUY" else during["mid"].min()
        move = (peak - pre_mid) if side_cfg == "BUY" else (pre_mid - peak)
        out["sweep.peak_impact_ticks"] = round(move / tick, 2)

    # --- reversion: temporary vs persistent -----------------------------
    for horizon in (1, 5):
        mid = _mid_at(bbo, end_ns + horizon * 1_000_000_000)
        if mid is None:
            continue
        move = (mid - pre_mid) if side_cfg == "BUY" else (pre_mid - mid)
        out[f"sweep.mid_after_{horizon}s"] = round(mid, 4)
        out[f"sweep.impact_after_{horizon}s_ticks"] = round(move / tick, 2)

    # --- levels consumed -------------------------------------------------
    # Counted from the recorder's snapshots: how many distinct price levels present
    # on the consumed side before the sweep were gone (or reduced to nothing) after.
    if not l2.empty:
        pre_snap = l2[(l2["ts_ns"] <= start_ns) & (l2["side"] == consumed_side)]
        post_snap = l2[(l2["ts_ns"] >= end_ns) & (l2["side"] == consumed_side)]
        if not pre_snap.empty and not post_snap.empty:
            pre_id = int(pre_snap["snapshot_id"].iloc[-1])
            post_id = int(post_snap["snapshot_id"].iloc[0])
            pre_rows = l2[(l2["snapshot_id"] == pre_id) & (l2["side"] == consumed_side)]
            post_rows = l2[(l2["snapshot_id"] == post_id) & (l2["side"] == consumed_side)]
            pre_sizes = dict(zip(pre_rows["price"].round(6), pre_rows["size"]))
            post_sizes = dict(zip(post_rows["price"].round(6), post_rows["size"]))

            # Two distinct things, worth separating: a level can be wholly eliminated,
            # or merely eaten into. A sweep smaller than the resting size at the touch
            # reduces one level without removing any, and reporting only
            # levels_consumed would make that read as "the sweep did nothing".
            out["sweep.levels_consumed"] = int(
                len(set(pre_sizes) - set(post_sizes)))
            out["sweep.levels_reduced"] = int(sum(
                1 for price, size in pre_sizes.items()
                if post_sizes.get(price, 0) < size))
            out["sweep.pre_levels"] = int(len(pre_sizes))

            # Pre-sweep depth from a short median, not a single snapshot: the same
            # one-snapshot gaps described below can land immediately before the event.
            pre_window = l2[(l2["ts_ns"] <= start_ns) & (l2["side"] == consumed_side)]
            pre_series = (pre_window.groupby("snapshot_id")["size"].sum()
                          .tail(9)) if not pre_window.empty else pd.Series(dtype=float)
            pre_depth = (float(pre_series.median()) if not pre_series.empty
                         else float(pre_rows["size"].sum()))
            post_depth = float(post_rows["size"].sum())
            out["sweep.pre_depth"] = int(pre_depth)
            out["sweep.post_depth"] = int(post_depth)

            # Depth removed is measured at the TROUGH, not at the first snapshot after
            # the sweep ends. A fast sweep can complete and the provider can replenish
            # inside a single 100ms Level 2 poll, which made a sweep that ate the whole
            # book report "0 depth consumed". The trough over the event window is what
            # the sweep actually took out.
            window = l2[(l2["ts_ns"] >= start_ns) &
                        (l2["ts_ns"] <= end_ns + lookahead_ns) &
                        (l2["side"] == consumed_side)]
            if not window.empty:
                series = window.groupby("snapshot_id")["size"].sum().sort_index()
                # A plain minimum is not usable here. The liquidity provider cancels
                # and re-submits its whole ladder when its inventory skew shifts, so a
                # Level 2 poll landing mid-rebuild sees a near-empty book. Measured on
                # a real run: median ask depth 24493, minimum 1533 -- a 94% "collapse"
                # that lasted exactly one 25ms snapshot, and made a 1000-share sweep
                # report consuming 17917 shares. Raising the sampling rate made these
                # far more likely to be caught, not less.
                #
                # A genuine sweep's reduction persists until replenishment (hundreds
                # of ms, tens of snapshots), so a 3-wide rolling median erases the
                # single-snapshot artifacts while preserving real depletion.
                smoothed = series.rolling(3, center=True, min_periods=1).median()
                trough = float(smoothed.min())
                out["sweep.trough_depth"] = int(trough)
                out["sweep.depth_consumed"] = int(max(0.0, pre_depth - trough))
                out["sweep.raw_min_depth"] = int(series.min())
            else:
                out["sweep.depth_consumed"] = int(max(0.0, pre_depth - post_depth))

            # Note what depth_consumed means: the PEAK DRAWDOWN of displayed depth,
            # not the cumulative liquidity the sweep took. When the provider
            # replenishes while the sweep is still executing, the book refills as
            # fast as it is eaten and the drawdown understates total consumption --
            # e.g. a 2500-share sweep that fully filled showed only 714 of drawdown.
            # For "how much was taken", use filled_quantity or volume_during; this
            # metric answers "how thin did the book actually get".

            # --- depth recovery ---------------------------------------
            # Time until the consumed side climbs back to 90% of its pre-sweep depth.
            # This is the resilience measure: two markets can show identical price
            # impact and differ entirely in how fast liquidity comes back.
            if pre_depth <= 0:
                return out
            target = pre_depth * 0.9
            after = l2[(l2["ts_ns"] > end_ns) & (l2["side"] == consumed_side)]
            if not after.empty:
                by_snap = after.groupby("snapshot_id").agg(
                    depth=("size", "sum"), ts_ns=("ts_ns", "first")).reset_index()
                recovered = by_snap[by_snap["depth"] >= target]
                if not recovered.empty:
                    out["sweep.depth_recovery_s"] = round(
                        (int(recovered["ts_ns"].iloc[0]) - end_ns) / 1e9, 3)
                else:
                    out["sweep.depth_recovery_s"] = None  # never recovered in-run

    # --- what actually executed during the window ------------------------
    t = run.trades
    if not t.empty:
        during_t = t[(t["ts_ns"] >= start_ns) & (t["ts_ns"] <= end_ns)]
        if not during_t.empty:
            out["sweep.trades_during"] = int(len(during_t))
            out["sweep.volume_during"] = int(during_t["size"].sum())
            out["sweep.distinct_prices_during"] = int(during_t["price"].round(6).nunique())
    return out
