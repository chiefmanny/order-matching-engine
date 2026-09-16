"""Directional-pressure metrics: did sustained one-sided flow move price, and did the
move last?

The regime before/after split cannot answer this. It compares two windows, so a move
that happens during the pressure and reverts once it stops nets out to roughly zero —
measured on a real run, the mid excursion rose 3.5 ticks while the net change came out
at -1.0. "Price moved and came back" and "price never moved" look identical that way,
and they are the two answers we most need to distinguish.

So this module locates the pressure episode itself and decomposes its effect the way
Almgren-Chriss frames execution cost:

    temporary impact   the move while pressure is being applied, which decays
    permanent impact   what is still there long after it stops

Everything is signed so positive means "price moved in the direction of the pressure",
making buy and sell episodes directly comparable.
"""
from __future__ import annotations

import pandas as pd


def _actors(run) -> list[dict]:
    return [p for p in run.metadata.get("participants", []) or []
            if p.get("strategy") == "directional_trader"]


def _mid_at(bbo: pd.DataFrame, ts_ns: int) -> float | None:
    prior = bbo[bbo["ts_ns"] <= ts_ns]
    if prior.empty:
        return None
    return float(prior["mid"].iloc[-1])


def compute(run, tick: float) -> dict:
    actors = _actors(run)
    if not actors or run.events.empty or run.bbo.empty:
        return {}

    bbo = run.bbo[run.bbo["mid"].notna()].sort_values("ts_ns")
    if bbo.empty:
        return {}

    out: dict = {}
    for actor in actors:
        pid = actor["id"]
        side = (actor.get("config") or {}).get("side", "BUY")
        sign = 1.0 if side == "BUY" else -1.0
        prefix = f"flow.{pid}"

        mine = run.events[run.events["participant_id"] == pid]
        orders = mine[mine["type"] == "submit"].sort_values("ts_ns")
        if orders.empty:
            continue

        start_ns = int(orders["ts_ns"].iloc[0])
        end_ns = int(orders["ts_ns"].iloc[-1])
        out[f"{prefix}.side"] = side
        out[f"{prefix}.episode_seconds"] = round((end_ns - start_ns) / 1e9, 2)

        mid_start = _mid_at(bbo, start_ns)
        if mid_start is None:
            continue
        out[f"{prefix}.mid_at_start"] = round(mid_start, 4)

        during = bbo[(bbo["ts_ns"] >= start_ns) & (bbo["ts_ns"] <= end_ns)]
        if not during.empty:
            extreme = float(during["mid"].max()) if sign > 0 else float(during["mid"].min())
            out[f"{prefix}.peak_move_ticks"] = round(sign * (extreme - mid_start) / tick, 2)

        mid_end = _mid_at(bbo, end_ns)
        if mid_end is not None:
            out[f"{prefix}.move_at_end_ticks"] = round(sign * (mid_end - mid_start) / tick, 2)

        # How much of the move survives once the pressure is gone.
        for horizon in (5, 10):
            mid_later = _mid_at(bbo, end_ns + horizon * 1_000_000_000)
            last_ns = int(bbo["ts_ns"].iloc[-1])
            if mid_later is None or end_ns + horizon * 1_000_000_000 > last_ns:
                continue  # the run ended before this horizon; reporting it would lie
            out[f"{prefix}.move_after_{horizon}s_ticks"] = round(
                sign * (mid_later - mid_start) / tick, 2)

        peak = out.get(f"{prefix}.peak_move_ticks")
        lasting = out.get(f"{prefix}.move_after_10s_ticks",
                          out.get(f"{prefix}.move_after_5s_ticks"))
        if isinstance(peak, (int, float)) and isinstance(lasting, (int, float)) and peak:
            # 1.0 means the move fully persisted; 0.0 that it entirely reverted.
            out[f"{prefix}.persistence"] = round(lasting / peak, 3)

        fills = mine[mine["type"] == "fill"]
        if not fills.empty:
            out[f"{prefix}.shares_filled"] = int(fills["size"].sum())
            out[f"{prefix}.fills"] = int(len(fills))

        # Share of all volume in the episode that this actor was responsible for:
        # the difference between nudging a market and dominating it.
        trades = run.trades
        if not trades.empty:
            window = trades[(trades["ts_ns"] >= start_ns) & (trades["ts_ns"] <= end_ns)]
            total = int(window["size"].sum())
            out[f"{prefix}.market_volume_during"] = total
            filled = out.get(f"{prefix}.shares_filled", 0)
            if total:
                out[f"{prefix}.volume_share"] = round(filled / total, 3)
    return out
