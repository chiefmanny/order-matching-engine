"""Per-participant order-flow behavior, read from the structured event log."""
from __future__ import annotations


def compute(run, tick: float) -> dict:
    out: dict = {}
    metrics = run.participant_metrics()
    out["participants.reporting"] = len(metrics)

    total_submitted = total_filled = total_canceled = total_rejected = 0
    for pid, m in metrics.items():
        sub = int(m.get("orders_submitted", 0) or 0)
        fil = int(m.get("orders_filled", 0) or 0)
        can = int(m.get("orders_canceled", 0) or 0)
        rej = int(m.get("orders_rejected", 0) or 0)
        total_submitted += sub; total_filled += fil
        total_canceled += can;  total_rejected += rej

        p = f"participant.{pid}"
        out[f"{p}.orders_submitted"] = sub
        out[f"{p}.orders_filled"] = fil
        out[f"{p}.orders_canceled"] = can
        out[f"{p}.shares_filled"] = int(m.get("shares_filled", 0) or 0)
        out[f"{p}.inventory"] = int(m.get("inventory", 0) or 0)
        out[f"{p}.realized_pnl"] = round(float(m.get("realized_pnl", 0.0) or 0.0), 4)
        out[f"{p}.fill_ratio"] = round(fil / sub, 4) if sub else 0.0
        out[f"{p}.cancel_ratio"] = round(can / sub, 4) if sub else 0.0
        out[f"{p}.mean_order_lifetime_s"] = round(
            float(m.get("mean_order_lifetime_seconds", 0.0) or 0.0), 4)
        out[f"{p}.ownership_violations"] = int(m.get("ownership_violations", 0) or 0)
        strat = m.get("strategy") or {}
        for k, v in strat.items():
            if isinstance(v, (int, float)):
                out[f"{p}.strategy.{k}"] = round(v, 4) if isinstance(v, float) else v

    out["orders.submitted"] = total_submitted
    out["orders.filled"] = total_filled
    out["orders.canceled"] = total_canceled
    out["orders.rejected"] = total_rejected
    out["orders.fill_ratio"] = round(total_filled / total_submitted, 4) if total_submitted else 0.0
    out["orders.cancel_ratio"] = round(total_canceled / total_submitted, 4) if total_submitted else 0.0
    return out
