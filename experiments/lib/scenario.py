"""Scenario definitions: load, validate, and resolve into per-participant configs.

A scenario is authored in YAML (readable timelines, comments, inheritance) and
resolved into the JSON participant configs that ParticipantConfig::fromFile already
reads. Nothing about the C++ side changes to support a new scenario.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

# Directory containing scenarios/ and baselines/. `extends` is resolved against this
# so a scenario can say `extends: baselines/normal_market.yaml` regardless of where
# the referring file itself lives -- including generated sweep variants written into
# a temporary subdirectory.
EXPERIMENTS_ROOT = Path(__file__).resolve().parent.parent


class ScenarioError(ValueError):
    """A scenario file is malformed. The message is meant for the user."""


@dataclass
class Participant:
    id: str
    strategy: str
    start_at: float = 0.0
    stop_at: float | None = None
    config: dict[str, Any] = field(default_factory=dict)
    # Runtime-level overrides; fall back to scenario defaults when absent.
    decision_interval_ms: int | None = None
    seed: int | None = None


@dataclass
class Scenario:
    name: str
    symbol: str
    duration_seconds: float
    tick_size: float = 0.01
    random_seed: int = 0
    description: str = ""
    participants: list[Participant] = field(default_factory=list)
    recorder: dict[str, Any] = field(default_factory=dict)
    engine: dict[str, Any] = field(default_factory=dict)
    markers: list[dict[str, Any]] = field(default_factory=list)
    hypothesis: dict[str, Any] = field(default_factory=dict)
    source_path: Path | None = None

    @property
    def l2_depth(self) -> int:
        return int(self.recorder.get("l2_depth", 15))

    @property
    def l2_interval_ms(self) -> int:
        return int(self.recorder.get("l2_interval_ms", 100))

    @property
    def bbo_interval_ms(self) -> int:
        """Engine-side WatchSymbol cadence. Bounds how finely any event can be
        observed, so scenarios containing a discrete event should lower it."""
        return int(self.engine.get("bbo_interval_ms", 100))

    def participant_seed(self, participant: Participant) -> int:
        """Deterministic per-participant seed derived from the master seed.

        Derived rather than shared so two identically configured noise traders do not
        make identical decisions, while the run as a whole stays reproducible from a
        single `random_seed`.
        """
        if participant.seed is not None:
            return participant.seed
        digest = hashlib.sha256(f"{self.random_seed}:{participant.id}".encode()).digest()
        # Keep it inside int32: the participant config carries it as a signed int.
        return int.from_bytes(digest[:4], "big") & 0x7FFFFFFF


def _deep_merge(base: dict, override: dict) -> dict:
    out = dict(base)
    for key, value in override.items():
        if key in out and isinstance(out[key], dict) and isinstance(value, dict):
            out[key] = _deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def _merge_participants(base: list[dict], override: list[dict]) -> list[dict]:
    """Merge participant lists by id, so a child scenario can tune one actor.

    An entry with `remove: true` drops an inherited participant, which is how a
    scenario like `noise_only` reuses a baseline minus its liquidity provider.
    """
    by_id = {p["id"]: dict(p) for p in base}
    order = [p["id"] for p in base]
    for entry in override:
        pid = entry.get("id")
        if not pid:
            raise ScenarioError("every participant needs an 'id'")
        if entry.get("remove"):
            by_id.pop(pid, None)
            if pid in order:
                order.remove(pid)
            continue
        if pid in by_id:
            by_id[pid] = _deep_merge(by_id[pid], entry)
        else:
            by_id[pid] = dict(entry)
            order.append(pid)
    return [by_id[pid] for pid in order]


def _load_raw(path: Path, seen: set[Path]) -> dict:
    path = path.resolve()
    if path in seen:
        raise ScenarioError(f"circular 'extends' chain at {path}")
    seen.add(path)

    if not path.exists():
        raise ScenarioError(f"scenario not found: {path}")
    with path.open() as handle:
        raw = yaml.safe_load(handle)
    if not isinstance(raw, dict):
        raise ScenarioError(f"{path} does not contain a YAML mapping")

    parent_ref = raw.pop("extends", None)
    if parent_ref is None:
        return raw

    candidates = [
        EXPERIMENTS_ROOT / parent_ref,     # canonical: relative to experiments/
        path.parent / parent_ref,          # convenience: alongside the referring file
        path.parent.parent / parent_ref,
    ]
    parent_path = next((c.resolve() for c in candidates if c.exists()), None)
    if parent_path is None:
        raise ScenarioError(
            f"{path}: 'extends: {parent_ref}' not found (looked in "
            f"{EXPERIMENTS_ROOT} and alongside the file)")
    parent = _load_raw(parent_path, seen)

    participants = _merge_participants(
        parent.get("participants", []), raw.get("participants", [])
    )
    merged = _deep_merge(parent, raw)
    merged["participants"] = participants
    return merged


def load(path: str | Path) -> Scenario:
    path = Path(path)
    raw = _load_raw(path, set())

    for required in ("name", "symbol", "duration_seconds"):
        if required not in raw:
            raise ScenarioError(f"{path}: missing required field '{required}'")

    participants: list[Participant] = []
    seen_ids: set[str] = set()
    for entry in raw.get("participants", []):
        if "id" not in entry or "strategy" not in entry:
            raise ScenarioError(f"{path}: each participant needs 'id' and 'strategy'")
        pid = str(entry["id"])
        if pid in seen_ids:
            raise ScenarioError(f"{path}: duplicate participant id '{pid}'")
        seen_ids.add(pid)

        start_at = float(entry.get("start_at", 0.0))
        stop_at = entry.get("stop_at")
        stop_at = float(stop_at) if stop_at is not None else None
        if stop_at is not None and stop_at <= start_at:
            raise ScenarioError(f"{path}: participant '{pid}' has stop_at <= start_at")
        if start_at >= float(raw["duration_seconds"]):
            raise ScenarioError(
                f"{path}: participant '{pid}' starts at {start_at}s, "
                f"at or after the {raw['duration_seconds']}s experiment end"
            )

        participants.append(
            Participant(
                id=pid,
                strategy=str(entry["strategy"]),
                start_at=start_at,
                stop_at=stop_at,
                config=dict(entry.get("config", {})),
                decision_interval_ms=entry.get("decision_interval_ms"),
                seed=entry.get("seed"),
            )
        )

    if not participants:
        raise ScenarioError(f"{path}: a scenario needs at least one participant")

    return Scenario(
        name=str(raw["name"]),
        symbol=str(raw["symbol"]),
        duration_seconds=float(raw["duration_seconds"]),
        tick_size=float(raw.get("tick_size", 0.01)),
        random_seed=int(raw.get("random_seed", 0)),
        description=str(raw.get("description", "")),
        participants=participants,
        recorder=dict(raw.get("recorder", {})),
        engine=dict(raw.get("engine", {})),
        markers=list(raw.get("markers", [])),
        hypothesis=dict(raw.get("hypothesis", {})),
        source_path=path,
    )


def participant_config(scenario: Scenario, participant: Participant, run_id: str) -> dict:
    """Build the JSON that ParticipantConfig::fromFile consumes."""
    # A participant runs until the experiment ends, or until its own stop_at. The
    # runtime's own max-runtime is the backstop if the harness fails to stop it.
    end = participant.stop_at if participant.stop_at is not None else scenario.duration_seconds
    max_runtime = max(1, int(round(end - participant.start_at)) + 2)

    return {
        "participantId": participant.id,
        "runId": run_id,
        "symbol": scenario.symbol,
        "strategy": participant.strategy,
        "randomSeed": scenario.participant_seed(participant),
        "maxRuntimeSeconds": max_runtime,
        "shutdownTimeoutMs": 5000,
        "tickSize": scenario.tick_size,
        "decisionIntervalMs": int(participant.decision_interval_ms or 100),
        "level2Depth": scenario.l2_depth,
        "level2IntervalMs": scenario.l2_interval_ms,
        "parameters": participant.config,
    }
