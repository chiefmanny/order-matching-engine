"""Run directories and the metadata needed to reproduce a run."""
from __future__ import annotations

import json
import platform
import re
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

RESULTS_ROOT = Path(__file__).resolve().parent.parent / "results"

_RUN_DIR_RE = re.compile(r"^run_(\d+)$")


@dataclass
class RunDir:
    path: Path
    run_id: str

    @property
    def configs(self) -> Path:
        return self.path / "configs"

    @property
    def logs(self) -> Path:
        return self.path / "logs"


def create(scenario_name: str, results_root: Path | None = None) -> RunDir:
    """Allocate the next run_NNN directory for a scenario."""
    root = Path(results_root or RESULTS_ROOT) / scenario_name
    root.mkdir(parents=True, exist_ok=True)

    highest = 0
    for child in root.iterdir():
        match = _RUN_DIR_RE.match(child.name)
        if child.is_dir() and match:
            highest = max(highest, int(match.group(1)))

    run_id = f"run_{highest + 1:03d}"
    path = root / run_id
    path.mkdir()
    (path / "configs").mkdir()
    (path / "logs").mkdir()
    return RunDir(path=path, run_id=f"{scenario_name}/{run_id}")


def _git(*args: str) -> str:
    repo = Path(__file__).resolve().parent.parent.parent
    try:
        proc = subprocess.run(["git", "-C", str(repo), *args],
                              capture_output=True, text=True, check=False)
        return proc.stdout.strip()
    except OSError:
        return ""


def git_info() -> dict[str, Any]:
    """Commit and dirty-state, so a run can be tied back to the code that produced it.

    `dirty` matters more than the commit: results from an uncommitted tree cannot be
    reproduced from the hash alone, and quietly recording only the hash would imply
    a reproducibility the run does not have.
    """
    status = _git("status", "--porcelain")
    return {
        "commit": _git("rev-parse", "HEAD"),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(status),
        "dirty_files": [line[3:] for line in status.splitlines()][:50],
    }


def write_metadata(run: RunDir, payload: dict[str, Any]) -> Path:
    path = run.path / "metadata.json"
    with path.open("w") as handle:
        json.dump(payload, handle, indent=2, default=str)
        handle.write("\n")
    return path


def base_metadata(scenario, run: RunDir, backend: str) -> dict[str, Any]:
    return {
        "run_id": run.run_id,
        "scenario_name": scenario.name,
        "scenario_path": str(scenario.source_path),
        "description": scenario.description,
        "symbol": scenario.symbol,
        "tick_size": scenario.tick_size,
        "duration_seconds": scenario.duration_seconds,
        "random_seed": scenario.random_seed,
        "backend": backend,
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "git": git_info(),
        "platform": {"system": platform.system(), "release": platform.release(),
                     "python": platform.python_version()},
        "participants": [
            {
                "id": p.id,
                "strategy": p.strategy,
                "start_at": p.start_at,
                "stop_at": p.stop_at,
                "seed": scenario.participant_seed(p),
                "config": p.config,
            }
            for p in scenario.participants
        ],
        "recorder": {"l2_depth": scenario.l2_depth, "l2_interval_ms": scenario.l2_interval_ms},
        "markers": scenario.markers,
        "hypothesis": scenario.hypothesis,
    }


def merge_participant_events(run: RunDir) -> Path:
    """Concatenate each participant's JSONL stdout into one time-ordered stream."""
    events = []
    for log in sorted(run.logs.glob("*.stdout")):
        if log.stem in ("engine", "recorder"):
            continue
        with log.open() as handle:
            for line_no, line in enumerate(handle, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError:
                    # Keep going: one bad line must not cost us the whole run's
                    # events. run.py reports the count as a run-quality signal.
                    events.append({
                        "ts_ns": 0, "type": "malformed_event",
                        "participant_id": log.stem, "line_no": line_no, "raw": line[:500],
                    })

    events.sort(key=lambda e: e.get("ts_ns", 0))
    path = run.path / "participant_events.jsonl"
    with path.open("w") as handle:
        for event in events:
            handle.write(json.dumps(event) + "\n")
    return path
