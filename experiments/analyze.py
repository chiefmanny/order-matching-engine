#!/usr/bin/env python3
"""Analyse a run: compute metrics, build candles, judge the stated hypothesis.

    experiments/analyze.py experiments/results/baseline_market/run_001

Writes candles_*.csv and summary.json into the run directory, and prints a report.

The hypothesis is judged against expectations written into the scenario BEFORE the
run. That ordering is the whole point: it is far too easy to look at a synthetic
market afterwards and narrate whatever it happened to do as the intended result.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from candles import write_candles  # noqa: E402
from lib import loaders  # noqa: E402
import metrics as metrics_mod  # noqa: E402

OPS = {
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
    ">":  lambda a, b: a > b,
    ">=": lambda a, b: a >= b,
    "<":  lambda a, b: a < b,
    "<=": lambda a, b: a <= b,
}


def evaluate_hypothesis(hypothesis: dict, values: dict) -> dict:
    """Check each expect clause and classify the result as a whole."""
    clauses = hypothesis.get("expect", []) or []
    if not clauses:
        return {"verdict": "no_hypothesis", "statement": hypothesis.get("statement", ""),
                "clauses": []}

    results = []
    for clause in clauses:
        name, op, expected = clause.get("metric"), clause.get("op"), clause.get("value")
        actual = values.get(name)
        if actual is None:
            results.append({"metric": name, "op": op, "expected": expected,
                            "actual": None, "passed": None, "note": "metric not available"})
            continue
        if op not in OPS:
            results.append({"metric": name, "op": op, "expected": expected,
                            "actual": actual, "passed": None, "note": f"unknown op '{op}'"})
            continue
        results.append({"metric": name, "op": op, "expected": expected,
                        "actual": actual, "passed": bool(OPS[op](actual, expected))})

    decided = [r for r in results if r["passed"] is not None]
    passed = [r for r in decided if r["passed"]]
    if not decided:
        verdict = "undetermined"
    elif len(passed) == len(decided):
        verdict = "consistent"
    elif passed:
        verdict = "partially_consistent"
    else:
        verdict = "inconsistent"

    return {"verdict": verdict, "statement": hypothesis.get("statement", ""),
            "clauses": results}


HEADLINE = [
    ("trades.count", "trades"),
    ("trades.rate_per_s", "trades/s"),
    ("trades.mean_size", "mean trade size"),
    ("spread.mean_ticks", "mean spread (ticks)"),
    ("spread.max_ticks", "max spread (ticks)"),
    ("book.two_sided_fraction", "two-sided fraction"),
    ("book.bid_depth", "mean bid depth"),
    ("book.ask_depth", "mean ask depth"),
    ("book.bid_depth_min", "min bid depth"),
    ("book.bid_levels", "mean bid levels"),
    ("mid.range_ticks", "mid range (ticks)"),
    ("mid.realized_vol_bps", "mid realised vol (bps)"),
    ("price.range_ticks", "trade price range (ticks)"),
    ("price.total_drift_pct", "price drift (%)"),
    ("candles.1s.non_empty", "non-empty 1s candles"),
    ("candles.1s.mean_range_ticks", "mean 1s candle range (ticks)"),
    ("flow.signed_imbalance", "signed flow imbalance"),
    ("orders.fill_ratio", "fill ratio"),
    ("orders.cancel_ratio", "cancel ratio"),
]

SWEEP_HEADLINE = [
    ("sweep.side", "sweep side"),
    ("sweep.pre_mid", "arrival mid"),
    ("sweep.levels_consumed", "levels eliminated"),
    ("sweep.levels_reduced", "levels reduced"),
    ("sweep.pre_levels", "levels before"),
    ("sweep.depth_consumed", "depth consumed"),
    ("sweep.volume_during", "volume during sweep"),
    ("sweep.impact_ticks", "price impact (ticks)"),
    ("sweep.peak_impact_ticks", "peak impact (ticks)"),
    ("sweep.impact_after_1s_ticks", "impact +1s (ticks)"),
    ("sweep.impact_after_5s_ticks", "impact +5s (ticks)"),
    ("sweep.depth_recovery_s", "depth recovery (s)"),
]


def report(run, values: dict, verdict: dict) -> None:
    print(f"=== {run.name} ===")
    desc = run.metadata.get("description", "")
    if desc:
        print(f"    {desc.strip()}")
    print()
    for key, label in HEADLINE:
        if key in values:
            print(f"  {label:<30} {values[key]}")

    if any(k in values for k, _ in SWEEP_HEADLINE):
        print("\n  sweep event:")
        for key, label in SWEEP_HEADLINE:
            if key in values:
                print(f"    {label:<28} {values[key]}")

    print("\n  participants:")
    for pid in sorted(run.participant_metrics()):
        p = f"participant.{pid}"
        print(f"    {pid:<10} sub={values.get(p+'.orders_submitted', 0):<5}"
              f" filled={values.get(p+'.orders_filled', 0):<4}"
              f" shares={values.get(p+'.shares_filled', 0):<6}"
              f" inv={values.get(p+'.inventory', 0):<6}"
              f" pnl={values.get(p+'.realized_pnl', 0.0):>10.2f}")

    if verdict["verdict"] != "no_hypothesis":
        print(f"\n  hypothesis: {verdict['verdict'].upper()}")
        if verdict["statement"]:
            print(f"    {' '.join(verdict['statement'].split())}")
        for c in verdict["clauses"]:
            mark = "ok " if c["passed"] else ("?? " if c["passed"] is None else "NO ")
            print(f"    [{mark}] {c['metric']} {c['op']} {c['expected']}  (actual: {c['actual']})")


def analyse(run_path: Path, make_plots: bool = True) -> dict:
    run = loaders.load(run_path)
    values = metrics_mod.compute_all(run)
    write_candles(run.path, run.trades)
    verdict = evaluate_hypothesis(run.hypothesis, values)

    summary = {
        "run_id": run.name,
        "scenario": run.metadata.get("scenario_name"),
        "status": run.metadata.get("status"),
        "failures": run.metadata.get("failures", []),
        "metrics": values,
        "hypothesis": verdict,
    }
    (run.path / "summary.json").write_text(json.dumps(summary, indent=2, default=str) + "\n")

    report(run, values, verdict)

    if make_plots:
        try:
            import plots
            written = plots.render(run, values)
            print(f"\n  plots: {len(written)} written to {run.path / 'plots'}")
        except Exception as exc:  # plotting must never invalidate an analysis
            print(f"\n  [warn] plots skipped: {exc}")

    return summary


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyse an experiment run.")
    ap.add_argument("run", help="path to a run directory")
    ap.add_argument("--no-plots", action="store_true")
    ap.add_argument("--fail-on-inconsistent", action="store_true",
                    help="exit non-zero if the hypothesis was not supported")
    args = ap.parse_args()

    summary = analyse(Path(args.run), make_plots=not args.no_plots)
    if args.fail_on_inconsistent and summary["hypothesis"]["verdict"] in (
            "inconsistent", "partially_consistent"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
