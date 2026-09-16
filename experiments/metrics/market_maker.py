"""Market-maker metrics: inventory, spread capture, and adverse selection.

The interesting question about a market maker is not how much it traded but whether
its quoting was profitable, and *why*. Two makers with identical fill counts can have
opposite outcomes depending on who was hitting them.

Adverse selection is the measure that answers this, and it cannot come from the maker
itself. The maker knows only its own fills; whether those fills were good requires
knowing where the market went next, which only the recorder's independent BBO series
can say. So the maker reports its execution and this module judges it.
"""
from __future__ import annotations

import pandas as pd


def _maker_ids(run) -> list[str]:
    return [p["id"] for p in run.metadata.get("participants", []) or []
            if p.get("strategy") == "market_maker"]


def _mid_at(bbo: pd.DataFrame, ts_ns: int) -> float | None:
    prior = bbo[bbo["ts_ns"] <= ts_ns]
    if prior.empty:
        return None
    return float(prior["mid"].iloc[-1])


def compute(run, tick: float) -> dict:
    makers = _maker_ids(run)
    if not makers or run.events.empty:
        return {}

    bbo = run.bbo[run.bbo["mid"].notna()].sort_values("ts_ns") if not run.bbo.empty \
        else pd.DataFrame()
    out: dict = {}

    for pid in makers:
        prefix = f"mm.{pid}"
        fills = run.events[(run.events["participant_id"] == pid) &
                           (run.events["type"] == "fill")].sort_values("ts_ns")
        out[f"{prefix}.fills"] = int(len(fills))
        if fills.empty:
            continue

        signed = fills["size"].where(fills["buy"], -fills["size"])
        inventory = signed.cumsum()
        out[f"{prefix}.inventory_final"] = int(inventory.iloc[-1])
        out[f"{prefix}.inventory_min"] = int(inventory.min())
        out[f"{prefix}.inventory_max"] = int(inventory.max())
        out[f"{prefix}.inventory_range"] = int(inventory.max() - inventory.min())
        # Mean absolute inventory says how much risk the maker carried on average,
        # which a final position of zero would completely hide.
        out[f"{prefix}.mean_abs_inventory"] = round(float(inventory.abs().mean()), 1)

        if bbo.empty:
            continue

        # --- adverse selection -------------------------------------------
        # For each fill, where did the mid go afterwards, signed so positive means the
        # market moved AGAINST the maker: bought and the price fell, or sold and it
        # rose. A maker that is being picked off shows a consistently positive value
        # even while its quoted spread looks healthy.
        for horizon in (1, 5):
            moves = []
            for _, fill in fills.iterrows():
                ts = int(fill["ts_ns"])
                now_mid = _mid_at(bbo, ts)
                later_mid = _mid_at(bbo, ts + horizon * 1_000_000_000)
                if now_mid is None or later_mid is None:
                    continue
                drift = later_mid - now_mid
                moves.append(-drift if fill["buy"] else drift)
            if moves:
                mean_move = sum(moves) / len(moves)
                out[f"{prefix}.adverse_selection_{horizon}s_ticks"] = round(
                    mean_move / tick, 3)
                out[f"{prefix}.adverse_fill_fraction_{horizon}s"] = round(
                    sum(1 for m in moves if m > 0) / len(moves), 3)

        # --- realised spread capture -------------------------------------
        # Each fill measured against the mid prevailing at that instant: a maker
        # earns by buying below the mid and selling above it. Averaged over fills
        # this is the per-share edge before any inventory outcome.
        edges = []
        for _, fill in fills.iterrows():
            mid = _mid_at(bbo, int(fill["ts_ns"]))
            if mid is None:
                continue
            edges.append((mid - fill["price"]) if fill["buy"] else (fill["price"] - mid))
        if edges:
            out[f"{prefix}.mean_edge_ticks"] = round(sum(edges) / len(edges) / tick, 3)

    return out
