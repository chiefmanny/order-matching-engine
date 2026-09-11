"""Order-flow metrics: aggressive volume, signed imbalance, order-flow imbalance."""
from __future__ import annotations

import numpy as np
import pandas as pd


def compute(run, tick: float) -> dict:
    out: dict = {}
    t = run.trades
    if not t.empty and "aggressor_is_buy" in t:
        buy = t[t["aggressor_is_buy"] == 1]
        sell = t[t["aggressor_is_buy"] == 0]
        bq, sq = int(buy["size"].sum()), int(sell["size"].sum())
        out["flow.aggressive_buy_qty"] = bq
        out["flow.aggressive_sell_qty"] = sq
        out["flow.buy_trade_count"] = int(len(buy))
        out["flow.sell_trade_count"] = int(len(sell))
        total = bq + sq
        # Signed imbalance in [-1, 1]: +1 is all buyer-initiated volume.
        out["flow.signed_imbalance"] = round((bq - sq) / total, 4) if total else 0.0

    # Order-flow imbalance (Cont-Kukanov-Stoikov) from consecutive BBO observations.
    # OFI counts additions to the bid and removals from the ask as positive pressure,
    # which is what actually predicts short-horizon price moves -- unlike raw trade
    # imbalance, it also captures pressure applied by quoting rather than trading.
    b = run.bbo
    if not b.empty:
        two = b[b["spread"].notna()].reset_index(drop=True)
        if len(two) > 1:
            pb = two["best_bid"].to_numpy()
            qb = two["bid_size"].to_numpy()
            pa = two["best_ask"].to_numpy()
            qa = two["ask_size"].to_numpy()
            e = np.zeros(len(two) - 1)
            for i in range(1, len(two)):
                if   pb[i] >  pb[i-1]: e[i-1] += qb[i]
                elif pb[i] == pb[i-1]: e[i-1] += qb[i] - qb[i-1]
                else:                  e[i-1] -= qb[i-1]

                if   pa[i] <  pa[i-1]: e[i-1] -= qa[i]
                elif pa[i] == pa[i-1]: e[i-1] -= qa[i] - qa[i-1]
                else:                  e[i-1] += qa[i-1]
            out["flow.ofi_sum"] = int(np.sum(e))
            out["flow.ofi_abs_mean"] = round(float(np.mean(np.abs(e))), 2)
    return out
