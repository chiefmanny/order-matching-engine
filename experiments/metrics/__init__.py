"""Metrics computed from a run's artifacts.

Every metric is exposed under a flat dotted name (`trades.rate_per_s`,
`book.bid_depth`, ...) so a scenario's `hypothesis.expect` clauses can reference them
directly by string and analyze.py can evaluate them without special-casing.
"""
from __future__ import annotations

from . import baseline, order_flow, participant, sweep


def compute_all(run, tick_size: float | None = None) -> dict:
    tick = tick_size or run.metadata.get("tick_size", 0.01)
    out: dict = {}
    out.update(baseline.compute(run, tick))
    out.update(order_flow.compute(run, tick))
    out.update(participant.compute(run, tick))
    out.update(sweep.compute(run, tick))
    return out
