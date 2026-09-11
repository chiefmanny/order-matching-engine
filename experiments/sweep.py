#!/usr/bin/env python3
"""Sweep one parameter across values and compare the resulting markets.

    experiments/sweep.py experiments/scenarios/baseline_market.yaml \
        --param participants.lp1.config.inventorySkewTicksPerUnit \
        --values 0,1,2,4 \
        --report trades.rate_per_s,mid.range_ticks,book.bid_depth

One dimension at a time, on purpose. A grid search over a market we do not yet
understand mostly produces expensive noise; the point here is to see the shape of a
single relationship (quantity -> impact, arrival rate -> trade rate) clearly enough
to reason about it.

--repeat re-runs each value with different derived seeds, so a difference can be
separated from seed noise before anyone believes it.
"""
from __future__ import annotations

import argparse
import copy
import json
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib import scenario as scenario_mod  # noqa: E402

HERE = Path(__file__).resolve().parent


def analyse_isolated(run_dir: Path) -> dict:
    """Run analyze.py as a subprocess and read back its summary.

    In-process analysis accumulated pandas frames across every run of a sweep, which
    was enough to get a long sweep OOM-killed mid-flight -- and a killed sweep orphans
    a recorder, which then contaminates a run's tape with activity from a later period.
    A subprocess per run keeps peak memory flat no matter how many values are swept.
    """
    subprocess.run(
        [sys.executable, str(HERE / "analyze.py"), str(run_dir), "--no-plots"],
        capture_output=True, text=True, check=False,
    )
    summary_path = Path(run_dir) / "summary.json"
    if not summary_path.exists():
        return {}
    return json.loads(summary_path.read_text())


def set_path(doc: dict, dotted: str, value) -> None:
    """Set a value in the scenario document, understanding participant ids.

    `participants.<id>.config.<key>` addresses a participant by id rather than by
    list index, so a sweep does not silently retarget when the list order changes.
    """
    parts = dotted.split(".")
    if parts[0] == "participants":
        pid = parts[1]
        target = next((p for p in doc.get("participants", []) if p.get("id") == pid), None)
        if target is None:
            # The participant is inherited via `extends` and so does not appear in
            # this file's own list. Adding a stub with just the id is exactly how the
            # scenario loader expresses an override: merge-by-id will patch the
            # inherited definition rather than replace it.
            target = {"id": pid}
            doc.setdefault("participants", []).append(target)
        node = target
        for key in parts[2:-1]:
            node = node.setdefault(key, {})
        node[parts[-1]] = value
        return

    node = doc
    for key in parts[:-1]:
        node = node.setdefault(key, {})
    node[parts[-1]] = value


def coerce(raw: str):
    for cast in (int, float):
        try:
            return cast(raw)
        except ValueError:
            pass
    if raw.lower() in ("true", "false"):
        return raw.lower() == "true"
    return raw


def run_one(doc: dict, results_root: Path, backend: str, image: str) -> Path | None:
    """Materialise a scenario variant and run it. Returns its run directory."""
    # The variant must live under experiments/ so its `extends` paths still resolve.
    tmp_dir = HERE / "scenarios" / "_sweep"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".yaml", dir=tmp_dir, delete=False) as fh:
        yaml.safe_dump(doc, fh, sort_keys=False)
        variant_path = Path(fh.name)

    try:
        proc = subprocess.run(
            [sys.executable, str(HERE / "run.py"), str(variant_path),
             "--results-root", str(results_root), "--backend", backend, "--image", image],
            check=False,
        )
        scenario_dir = results_root / doc["name"]
        if not scenario_dir.exists():
            return None
        runs = sorted(p for p in scenario_dir.iterdir() if p.name.startswith("run_"))
        if not runs:
            return None
        run_dir = runs[-1]
        if proc.returncode != 0:
            print(f"[sweep] warning: run exited {proc.returncode} ({run_dir.name})")
        return run_dir
    finally:
        variant_path.unlink(missing_ok=True)


def summarise(samples: list) -> tuple[float | None, float | None]:
    """Mean and sample standard deviation of the numeric samples, if any."""
    nums = [v for v in samples if isinstance(v, (int, float))]
    if not nums:
        return None, None
    mean = statistics.fmean(nums)
    spread = statistics.stdev(nums) if len(nums) > 1 else 0.0
    return mean, spread


def print_table(rows: list, values: list, report_keys: list, param: str, repeat: int) -> None:
    """Tabulate the sweep, aggregating repeats so noise is visible.

    With a single run per value there is no way to tell a real effect from seed noise,
    and small differences get read as findings when they are not. With repeats, each
    cell shows mean +/- stdev, and the footer flags any metric whose variation ACROSS
    values is no larger than its variation WITHIN a value -- i.e. not yet a result.
    """
    width = 26
    print("\n" + "=" * 78)
    print(f"SWEEP  {param}" + (f"   ({repeat} runs per value)" if repeat > 1 else ""))
    print("=" * 78)
    header = f"{'value':>12} " + " ".join(f"{k[-width + 2:]:>{width}}" for k in report_keys)
    print(header)
    print("-" * len(header))

    means: dict[str, list] = {k: [] for k in report_keys}
    within: dict[str, list] = {k: [] for k in report_keys}

    for value in values:
        group = [r for r in rows if r["value"] == value]
        if not group:
            continue
        cells = []
        for key in report_keys:
            mean, spread = summarise([r.get(key) for r in group])
            if mean is None:
                cells.append(f"{'-':>{width}}")
                continue
            means[key].append(mean)
            within[key].append(spread or 0.0)
            cells.append(f"{mean:>{width - 9}.3g} +/-{spread:>5.2g}" if repeat > 1
                         else f"{mean:>{width}.4g}")
        print(f"{str(value):>12} " + " ".join(cells))

    if repeat > 1:
        inconclusive = []
        for key in report_keys:
            if len(means[key]) < 2:
                continue
            across = statistics.stdev(means[key])
            typical_within = statistics.fmean(within[key]) if within[key] else 0.0
            if across <= typical_within:
                inconclusive.append(key)
        if inconclusive:
            print("\n  NOT CONCLUSIVE (variation across values <= variation within a value):")
            for key in inconclusive:
                print(f"    - {key}")
        else:
            print("\n  All reported metrics vary more across values than within them.")


def main() -> int:
    ap = argparse.ArgumentParser(description="Sweep one scenario parameter.")
    ap.add_argument("scenario")
    ap.add_argument("--param", required=True, help="dotted path, e.g. participants.lp1.config.X")
    ap.add_argument("--values", required=True, help="comma-separated values")
    ap.add_argument("--repeat", type=int, default=1, help="runs per value, with varied seeds")
    ap.add_argument("--report", default="trades.rate_per_s,spread.mean_ticks,book.bid_depth",
                    help="comma-separated metric names to tabulate")
    ap.add_argument("--results-root", default=None)
    ap.add_argument("--backend", default="docker", choices=("docker", "local"))
    ap.add_argument("--image", default="ome-cpp:latest")
    ap.add_argument("--out", default=None, help="write the comparison table as JSON")
    args = ap.parse_args()

    source = Path(args.scenario)
    base_doc = yaml.safe_load(source.read_text())
    # Resolve `extends` relative to the original file's directory.
    if "extends" in base_doc:
        base_doc = yaml.safe_load(source.read_text())

    values = [coerce(v.strip()) for v in args.values.split(",")]
    report_keys = [k.strip() for k in args.report.split(",")]
    results_root = Path(args.results_root) if args.results_root else HERE / "results" / "_sweeps"
    results_root.mkdir(parents=True, exist_ok=True)

    base_seed = int(base_doc.get("random_seed", 0))
    sweep_name = args.param.split(".")[-1]
    rows = []

    for value in values:
        for rep in range(args.repeat):
            doc = copy.deepcopy(base_doc)
            doc["name"] = f"sweep_{sweep_name}_{value}" + (f"_r{rep}" if args.repeat > 1 else "")
            if "extends" in doc:
                # `extends` was resolved relative to the scenario file; make it
                # relative to experiments/ so the variant still finds its parent.
                doc["extends"] = doc["extends"]
            doc["random_seed"] = base_seed + rep
            set_path(doc, args.param, value)

            print(f"\n[sweep] {args.param} = {value}"
                  + (f"  (repeat {rep + 1}/{args.repeat})" if args.repeat > 1 else ""))
            run_dir = run_one(doc, results_root, args.backend, args.image)
            if run_dir is None:
                print("[sweep] run produced no directory; skipping")
                continue

            summary = analyse_isolated(run_dir)
            metrics = summary.get("metrics", {})
            row = {"value": value, "repeat": rep, "run": str(run_dir)}
            row.update({k: metrics.get(k) for k in report_keys})
            rows.append(row)

    print_table(rows, values, report_keys, args.param, args.repeat)

    if args.out:
        Path(args.out).write_text(json.dumps(rows, indent=2, default=str) + "\n")
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
