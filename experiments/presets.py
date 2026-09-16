#!/usr/bin/env python3
"""List, verify and index the calibrated presets.

    experiments/presets.py list
    experiments/presets.py verify [--only NAME ...]
    experiments/presets.py manifest

Presets are the end product of the calibration work: scenarios tuned until they
produce the market behavior they claim to, so that something launching them -- a UI,
a demo script, a person -- gets a known result rather than having to invent market
conditions on the fly.

"Known-good" has to mean checked. `verify` runs each preset end to end, analyses it,
and records whether its stated hypothesis held; `manifest` writes an index carrying
those verdicts, so a consumer can tell which presets are currently trustworthy rather
than taking the word of a filename.

The distinction from experiments/scenarios/ is deliberate. Those are investigative --
controls, ladders, A/B arms, deliberately broken cases. Presets are the subset anyone
would want to press a button on.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib import scenario as scenario_mod  # noqa: E402

HERE = Path(__file__).resolve().parent
PRESET_DIR = HERE / "presets"
MANIFEST = PRESET_DIR / "manifest.json"

# Display order, since a UI reading the manifest wants a sensible sequence rather
# than whatever the filesystem returns. Roughly: ordinary market first, then single
# events, then conditions, then the specialised demonstrations.
ORDER = [
    "normal_market",
    "thin_liquidity",
    "buy_sweep",
    "large_buy_sweep",
    "selloff",
    "liquidity_crisis",
    "volatility_shock",
    "market_maker_inventory_pressure",
    "execution_immediate",
    "execution_sliced",
    "spoofing_detection_demo",
]


def preset_files() -> list[Path]:
    found = {p.stem: p for p in PRESET_DIR.glob("*.yaml")}
    ordered = [found.pop(name) for name in ORDER if name in found]
    return ordered + sorted(found.values())


def describe(path: Path) -> dict:
    scen = scenario_mod.load(path)
    return {
        "preset": path.stem,
        "scenario_name": scen.name,
        "file": str(path.relative_to(HERE.parent)),
        "description": " ".join((scen.description or "").split()),
        "symbol": scen.symbol,
        "duration_seconds": scen.duration_seconds,
        "participants": [
            {"id": p.id, "strategy": p.strategy, "start_at": p.start_at}
            for p in scen.participants
        ],
        "markers": scen.markers,
        "hypothesis": " ".join((scen.hypothesis.get("statement") or "").split()),
    }


def cmd_list() -> int:
    for path in preset_files():
        info = describe(path)
        actors = ", ".join(sorted({p["strategy"] for p in info["participants"]}))
        print(f"{info['preset']:<34} {info['duration_seconds']:>5.0f}s  {actors}")
        print(f"{'':<34} {info['description'][:96]}")
    return 0


def cmd_verify(only: list[str] | None) -> int:
    results = []
    failures = 0
    for path in preset_files():
        name = path.stem
        if only and name not in only:
            continue
        print(f"\n=== verifying {name} ===", flush=True)

        run = subprocess.run(
            [sys.executable, str(HERE / "run.py"), str(path)],
            capture_output=True, text=True, check=False,
        )
        tail = [ln for ln in run.stdout.splitlines() if ln.startswith("[runner]")][-1:]
        print(tail[0] if tail else run.stdout[-400:])

        run_dir = None
        for line in run.stdout.splitlines():
            if "RUN OK —" in line or "RUN FAILED" in line:
                pass
            if "run directory:" in line:
                run_dir = Path(line.split("run directory:")[1].strip())

        entry = {"preset": name, "ran": run.returncode == 0, "run_dir": str(run_dir)}
        if run_dir and run_dir.exists():
            analysis = subprocess.run(
                [sys.executable, str(HERE / "analyze.py"), str(run_dir), "--no-plots"],
                capture_output=True, text=True, check=False,
            )
            summary_path = run_dir / "summary.json"
            if summary_path.exists():
                summary = json.loads(summary_path.read_text())
                verdict = summary.get("hypothesis", {}).get("verdict", "unknown")
                entry["verdict"] = verdict
                entry["failed_clauses"] = [
                    c["metric"] for c in summary.get("hypothesis", {}).get("clauses", [])
                    if c.get("passed") is False
                ]
                print(f"    hypothesis: {verdict}")
            else:
                entry["verdict"] = "not_analysed"
        else:
            entry["verdict"] = "no_run"

        if not entry["ran"] or entry["verdict"] not in ("consistent", "no_hypothesis"):
            failures += 1
        results.append(entry)

    print("\n" + "=" * 70)
    print("PRESET VERIFICATION")
    print("=" * 70)
    for entry in results:
        mark = "ok " if entry["ran"] and entry["verdict"] in ("consistent", "no_hypothesis") else "NO "
        print(f"  [{mark}] {entry['preset']:<34} {entry['verdict']}"
              + (f"  ({', '.join(entry['failed_clauses'])})" if entry.get("failed_clauses") else ""))

    write_manifest(results)
    if failures:
        print(f"\n{failures} preset(s) did not verify cleanly.")
        return 1
    print(f"\nAll {len(results)} preset(s) verified.")
    return 0


def write_manifest(verifications: list[dict] | None = None) -> Path:
    # Merge with whatever is already recorded rather than replacing it. Verification
    # is normally run in batches (eleven presets is ten minutes of real market time),
    # and overwriting would silently erase the verdicts for every preset not in the
    # current batch, leaving a manifest that looks authoritative and is mostly blank.
    by_name: dict = {}
    if MANIFEST.exists():
        try:
            existing = json.loads(MANIFEST.read_text())
            for entry in existing.get("presets", []):
                if entry.get("verification"):
                    by_name[entry["preset"]] = {
                        "verdict": entry["verification"].get("verdict"),
                        "ran": entry["verification"].get("ran"),
                        "failed_clauses": entry["verification"].get("failed_clauses", []),
                        "verified_at_utc": entry["verification"].get("verified_at_utc"),
                    }
        except (json.JSONDecodeError, OSError):
            pass
    now = datetime.now(timezone.utc).isoformat()
    for v in (verifications or []):
        by_name[v["preset"]] = {**v, "verified_at_utc": now}
    entries = []
    for path in preset_files():
        info = describe(path)
        check = by_name.get(info["preset"])
        if check:
            info["verification"] = {
                "verdict": check.get("verdict"),
                "ran": check.get("ran"),
                "failed_clauses": check.get("failed_clauses", []),
                "verified_at_utc": check.get("verified_at_utc"),
            }
        entries.append(info)

    doc = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "note": ("Calibrated, launchable market scenarios. Each is tuned to produce the "
                 "behavior it describes; `verification` records whether its stated "
                 "hypothesis held the last time experiments/presets.py verify was run. "
                 "Presets without a recent verification should be re-verified before "
                 "being relied on, since calibration is specific to the participants "
                 "and engine behavior at the time it was done."),
        "presets": entries,
    }
    MANIFEST.write_text(json.dumps(doc, indent=2) + "\n")
    return MANIFEST


def main() -> int:
    ap = argparse.ArgumentParser(description="Calibrated market presets.")
    ap.add_argument("command", choices=("list", "verify", "manifest"))
    ap.add_argument("--only", nargs="*", default=None)
    args = ap.parse_args()

    if args.command == "list":
        return cmd_list()
    if args.command == "verify":
        return cmd_verify(args.only)
    path = write_manifest()
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
