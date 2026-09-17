#!/usr/bin/env python3
"""Record one preset into a website demo bundle.

    experiments/record_demo.py experiments/presets/normal_market.yaml \
        --out-dir /tmp/ome-demos --finchart-bin /path/to/examples-matching-engine

One run produces both halves of a demo: the finchart MarketEvent recording the
browser replays, and the measured metrics and plots the essay quotes. They come
from the same run deliberately -- the numbers on the page then describe exactly
the market the visitor is watching, rather than a different run that happened to
be similar.

Unlike `presets.py verify`, this analyses WITH plots, because the essays embed
them.

Exit status is the bundle's verdict: 0 means the recording and the analysis are
both trustworthy, non-zero means do not ship it.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# A recording with fewer events than this is a capture that went wrong -- an
# adapter that attached late, or a run that died early.
MIN_EVENTS = 200


def log(message: str) -> None:
    print(f"[record-demo] {message}", flush=True)


def find_run_dir(stdout: str) -> Path | None:
    """Pull the run directory out of run.py's log, as presets.py does."""
    run_dir = None
    for line in stdout.splitlines():
        if "run directory:" in line:
            run_dir = Path(line.split("run directory:")[1].strip())
    return run_dir


def validate_recording(path: Path, adapter: str) -> list[str]:
    """Check a recording is worth shipping. Returns a list of problems."""
    problems: list[str] = []

    if not path.exists():
        return [f"recording was never written: {path}"]

    lines = [ln for ln in path.read_text().splitlines() if ln]
    trades = sum(1 for ln in lines if ln.startswith("T "))
    snapshots = sum(1 for ln in lines if ln.startswith("S "))

    if len(lines) < MIN_EVENTS:
        problems.append(f"only {len(lines)} events (expected >= {MIN_EVENTS})")
    if trades == 0:
        # A snapshot-only capture renders an empty candle series: the chart looks
        # alive because depth moves, but the price panel is decorative.
        problems.append("no trades recorded -- the candle series would be empty")
    if snapshots == 0:
        problems.append("no depth snapshots recorded -- the book panel would be empty")

    # Replay it offline through the adapter itself. This needs no engine, and it is
    # the check that the file actually decodes rather than merely existing.
    replay = subprocess.run([adapter, "--replay", str(path)],
                            capture_output=True, text=True, check=False)
    if replay.returncode != 0:
        problems.append(f"offline replay failed: {replay.stderr.strip()[:200]}")
    elif not replay.stdout.strip():
        problems.append("offline replay rendered an empty frame")

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description="Record one preset into a demo bundle.")
    ap.add_argument("preset", help="path to a preset YAML file")
    ap.add_argument("--out-dir", required=True, help="where the bundle is written")
    ap.add_argument("--finchart-bin", required=True,
                    help="path to the finchart examples-matching-engine binary")
    args = ap.parse_args()

    preset_path = Path(args.preset).resolve()
    if not preset_path.exists():
        log(f"FAILED: no such preset: {preset_path}")
        return 2

    adapter = str(Path(args.finchart_bin).resolve())
    if not Path(adapter).exists():
        log(f"FAILED: no such finchart binary: {adapter}")
        return 2

    name = preset_path.stem
    out_dir = Path(args.out_dir).resolve()
    (out_dir / "plots" / name).mkdir(parents=True, exist_ok=True)
    recording = out_dir / f"{name}.txt"

    # --- run -----------------------------------------------------------------
    log(f"running {name}")
    run = subprocess.run(
        [sys.executable, str(HERE / "run.py"), str(preset_path),
         "--finchart-record", str(recording), "--finchart-bin", adapter],
        capture_output=True, text=True, check=False,
    )
    sys.stdout.write(run.stdout)
    if run.returncode != 0:
        log(f"FAILED: run.py exited {run.returncode}")
        sys.stderr.write(run.stderr)
        return 1

    run_dir = find_run_dir(run.stdout)
    if run_dir is None or not run_dir.exists():
        log("FAILED: could not locate the run directory in run.py's output")
        return 1

    # --- analyse, with plots -------------------------------------------------
    log("analysing")
    analysis = subprocess.run([sys.executable, str(HERE / "analyze.py"), str(run_dir)],
                              capture_output=True, text=True, check=False)
    summary_path = run_dir / "summary.json"
    if not summary_path.exists():
        log("FAILED: analyze.py wrote no summary.json")
        sys.stderr.write(analysis.stderr)
        return 1
    summary = json.loads(summary_path.read_text())

    # --- validate the recording ----------------------------------------------
    log("validating the recording")
    problems = validate_recording(recording, adapter)
    if problems:
        for problem in problems:
            log(f"FAILED: {problem}")
        return 1

    events = [ln for ln in recording.read_text().splitlines() if ln]

    # --- assemble ------------------------------------------------------------
    plots = []
    src_plots = run_dir / "plots"
    if src_plots.is_dir():
        for png in sorted(src_plots.glob("*.png")):
            shutil.copy(png, out_dir / "plots" / name / png.name)
            plots.append(png.name)

    # Scenario facts come from metadata.json, which records them verbatim; summary.json
    # carries only the derived metrics and the hypothesis verdict.
    metadata = json.loads((run_dir / "metadata.json").read_text())

    hypothesis = summary.get("hypothesis", {})
    bundle = {
        "preset": name,
        "scenario_name": metadata.get("scenario_name"),
        "description": metadata.get("description", ""),
        "symbol": metadata.get("symbol"),
        "tick_size": metadata.get("tick_size"),
        "duration_seconds": metadata.get("duration_seconds"),
        "participants": metadata.get("participants", []),
        "markers": metadata.get("markers", []),
        "recording": recording.name,
        "event_count": len(events),
        "trade_count": sum(1 for ln in events if ln.startswith("T ")),
        "hypothesis": hypothesis.get("statement", "").strip(),
        "verdict": hypothesis.get("verdict", "undetermined"),
        "clauses": hypothesis.get("clauses", []),
        "metrics": summary.get("metrics", {}),
        "plots": plots,
        "run_dir": str(run_dir),
    }
    bundle_path = out_dir / f"{name}.demo.json"
    bundle_path.write_text(json.dumps(bundle, indent=2) + "\n")

    log(f"OK  {recording.name}: {len(events)} events, "
        f"{bundle['trade_count']} trades, verdict={bundle['verdict']}, "
        f"{len(plots)} plot(s)")
    if bundle["verdict"] != "consistent":
        log(f"NOTE: hypothesis verdict is '{bundle['verdict']}', not 'consistent'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
