#!/usr/bin/env python3
"""Compare runs: tabulate metrics side by side and plot a relationship.

    experiments/compare.py experiments/results/_sweeps/*/run_001 \
        --metrics sweep.peak_impact_ticks,sweep.depth_consumed \
        --curve sweep.peak_impact_ticks

Written because comparing runs was the thing done most often by hand during Phase 6 —
once per sweep, re-typed each time, with the label column filled in from memory. That
is exactly how a mislabelled comparison gets published as a finding.

Two things it does that a hand-written table does not:

  * Works out for itself which parameter actually varies across the runs, by diffing
    their recorded configs, and labels rows with that. No hand-maintained legend, and
    no way to mislabel which run was which.
  * Groups repeats of the same configuration and reports mean +/- spread, so a
    difference that is inside run-to-run noise is visibly inside run-to-run noise.
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

HERE = Path(__file__).resolve().parent


def load_summary(run_dir: Path) -> dict | None:
    """Read a run's summary, analysing it first if that has not happened yet."""
    summary = run_dir / "summary.json"
    if not summary.exists():
        subprocess.run(
            [sys.executable, str(HERE / "analyze.py"), str(run_dir), "--no-plots"],
            capture_output=True, text=True, check=False,
        )
    if not summary.exists():
        return None
    return json.loads(summary.read_text())


def flatten_config(meta: dict) -> dict:
    """Scenario-level and per-participant settings as one flat dotted mapping."""
    flat: dict = {}
    for key in ("symbol", "tick_size", "duration_seconds", "random_seed"):
        if key in meta:
            flat[key] = meta[key]
    for participant in meta.get("participants", []) or []:
        pid = participant.get("id")
        flat[f"{pid}.strategy"] = participant.get("strategy")
        flat[f"{pid}.start_at"] = participant.get("start_at")
        for name, value in (participant.get("config") or {}).items():
            if isinstance(value, (int, float, str, bool)) or value is None:
                flat[f"{pid}.{name}"] = value
    return flat


def varying_keys(configs: list[dict], ignore_seed: bool = True) -> list[str]:
    """Config keys that are not identical across every run.

    random_seed is excluded by default: it varies between repeats of the SAME
    configuration, so treating it as the independent variable would split every group
    into singletons and hide the noise the repeats were run to measure.
    """
    keys = set()
    for cfg in configs:
        keys |= set(cfg)
    out = []
    for key in sorted(keys):
        if ignore_seed and key == "random_seed":
            continue
        values = {json.dumps(cfg.get(key), sort_keys=True, default=str) for cfg in configs}
        if len(values) > 1:
            out.append(key)
    return out


def summarise(samples: list) -> tuple[float | None, float | None, int]:
    nums = [v for v in samples if isinstance(v, (int, float)) and not isinstance(v, bool)]
    if not nums:
        return None, None, 0
    return (statistics.fmean(nums),
            statistics.stdev(nums) if len(nums) > 1 else 0.0,
            len(nums))


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare experiment runs.")
    ap.add_argument("runs", nargs="+", help="run directories")
    ap.add_argument("--metrics", default="trades.rate_per_s,spread.mean_ticks,book.bid_depth",
                    help="comma-separated metric names")
    ap.add_argument("--curve", default=None,
                    help="metric to plot against the varying parameter")
    ap.add_argument("--label", default=None,
                    help="config key to group by (default: auto-detected)")
    ap.add_argument("--out", default=None, help="write the table as JSON")
    ap.add_argument("--plot-out", default=None, help="path for the curve image")
    args = ap.parse_args()

    loaded = []
    for raw in args.runs:
        run_dir = Path(raw)
        summary = load_summary(run_dir)
        if summary is None:
            print(f"[compare] skipping {run_dir}: no analysable artifacts", file=sys.stderr)
            continue
        meta_path = run_dir / "metadata.json"
        meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
        loaded.append({"dir": run_dir, "summary": summary, "config": flatten_config(meta),
                       "status": meta.get("status", "?")})

    if not loaded:
        print("[compare] nothing to compare", file=sys.stderr)
        return 1

    configs = [r["config"] for r in loaded]
    if args.label:
        label_keys = [args.label]
    else:
        label_keys = varying_keys(configs)
        if not label_keys:
            label_keys = []

    def label_of(entry) -> str:
        if not label_keys:
            return entry["dir"].parent.name
        return ", ".join(f"{k}={entry['config'].get(k)}" for k in label_keys)

    groups: dict[str, list] = {}
    for entry in loaded:
        groups.setdefault(label_of(entry), []).append(entry)

    metrics = [m.strip() for m in args.metrics.split(",")]
    if args.curve and args.curve not in metrics:
        metrics.append(args.curve)

    # --- table ---------------------------------------------------------
    repeats = max(len(g) for g in groups.values())
    label_width = max(28, min(52, max(len(k) for k in groups)))
    col = 24
    print("=" * (label_width + 2 + (col + 1) * len(metrics)))
    print(f"COMPARE  {len(loaded)} run(s) in {len(groups)} group(s)"
          + (f", {repeats} per group" if repeats > 1 else ""))
    if label_keys:
        print(f"varying: {', '.join(label_keys)}")
    print("=" * (label_width + 2 + (col + 1) * len(metrics)))
    header = f"{'':<{label_width}} " + " ".join(f"{m[-col + 2:]:>{col}}" for m in metrics)
    print(header)
    print("-" * len(header))

    rows = []
    for name in sorted(groups, key=lambda k: (len(groups[k]), k)):
        entries = groups[name]
        cells, record = [], {"label": name, "runs": [str(e["dir"]) for e in entries]}
        for metric in metrics:
            mean, spread, n = summarise(
                [e["summary"].get("metrics", {}).get(metric) for e in entries])
            record[metric] = {"mean": mean, "stdev": spread, "n": n}
            if mean is None:
                cells.append(f"{'-':>{col}}")
            elif n > 1:
                cells.append(f"{mean:>{col - 9}.4g} +/-{spread:>5.2g}")
            else:
                cells.append(f"{mean:>{col}.5g}")
        rows.append(record)
        print(f"{name[:label_width]:<{label_width}} " + " ".join(cells))

    bad = [e for e in loaded if e["status"] not in ("ok", "?")]
    if bad:
        print(f"\n  WARNING: {len(bad)} run(s) did not pass their structural checks:")
        for entry in bad:
            print(f"    {entry['dir']}  ({entry['status']})")

    # --- curve ---------------------------------------------------------
    if args.curve:
        plot_curve(rows, args.curve, label_keys, args.plot_out)

    if args.out:
        Path(args.out).write_text(json.dumps(rows, indent=2, default=str) + "\n")
        print(f"\nwrote {args.out}")
    return 0


def plot_curve(rows: list, metric: str, label_keys: list, out_path: str | None) -> None:
    """Plot the metric against the varying parameter, with error bars from repeats."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    xs, ys, errs = [], [], []
    for row in rows:
        entry = row.get(metric) or {}
        if entry.get("mean") is None:
            continue
        # Use the numeric part of the label as the x value when there is one, so the
        # curve is ordered by the parameter rather than alphabetically by label.
        value = row["label"].split("=")[-1] if "=" in row["label"] else row["label"]
        try:
            xs.append(float(value))
        except ValueError:
            xs.append(len(xs))
        ys.append(entry["mean"])
        errs.append(entry.get("stdev") or 0.0)

    if len(xs) < 2:
        print("\n[compare] not enough comparable points to plot a curve")
        return

    order = sorted(range(len(xs)), key=lambda i: xs[i])
    xs = [xs[i] for i in order]; ys = [ys[i] for i in order]; errs = [errs[i] for i in order]

    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.errorbar(xs, ys, yerr=errs, marker="o", capsize=4, color="#0f766e", linewidth=1.5)
    ax.set_xlabel(", ".join(label_keys) if label_keys else "run")
    ax.set_ylabel(metric)
    ax.set_title(f"{metric} vs {', '.join(label_keys) if label_keys else 'run'}")
    ax.grid(alpha=0.3)
    fig.tight_layout()

    path = Path(out_path) if out_path else Path("comparison.png")
    fig.savefig(path, dpi=110)
    plt.close(fig)
    print(f"\nwrote {path}")


if __name__ == "__main__":
    sys.exit(main())
