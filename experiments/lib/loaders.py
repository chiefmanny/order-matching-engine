"""Load a run directory into dataframes.

One place that knows the artifact layout, so metrics, plots and checks all read the
same thing the same way.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import pandas as pd


@dataclass
class Run:
    path: Path
    trades: pd.DataFrame
    bbo: pd.DataFrame
    level2: pd.DataFrame
    events: pd.DataFrame
    metadata: dict = field(default_factory=dict)
    recorder_meta: dict = field(default_factory=dict)

    @property
    def name(self) -> str:
        return self.metadata.get("run_id", self.path.name)

    @property
    def markers(self) -> list[dict]:
        return self.metadata.get("markers", []) or []

    @property
    def hypothesis(self) -> dict:
        return self.metadata.get("hypothesis", {}) or {}

    def participant_metrics(self) -> dict[str, dict]:
        """Final metrics event per participant, keyed by participant id."""
        if self.events.empty:
            return {}
        rows = self.events[self.events["type"] == "metrics"]
        return {r["participant_id"]: r.to_dict() for _, r in rows.iterrows()}

    def elapsed(self, ts_ns: pd.Series) -> pd.Series:
        """Seconds since the recorder attached — the common clock for every plot."""
        return (ts_ns - self.t0_ns) / 1e9

    @property
    def t0_ns(self) -> int:
        # The recorder starts before any participant, so its first observation is the
        # earliest common reference point across all artifacts.
        candidates = []
        if not self.bbo.empty:
            candidates.append(int(self.bbo["ts_ns"].iloc[0]))
        if not self.level2.empty:
            candidates.append(int(self.level2["ts_ns"].iloc[0]))
        if self.recorder_meta.get("started_ts_ns"):
            candidates.append(int(self.recorder_meta["started_ts_ns"]))
        return min(candidates) if candidates else 0


def _read_csv(path: Path, **kwargs) -> pd.DataFrame:
    if not path.exists() or path.stat().st_size == 0:
        return pd.DataFrame()
    try:
        return pd.read_csv(path, **kwargs)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()


def load(run_path: str | Path) -> Run:
    path = Path(run_path)
    if not path.is_dir():
        raise FileNotFoundError(f"not a run directory: {path}")

    trades = _read_csv(path / "trades.csv")
    bbo = _read_csv(path / "bbo.csv")
    level2 = _read_csv(path / "level2.csv")

    # The recorder writes EMPTY marker rows so an empty book is distinguishable from
    # a missed poll. They are not price levels, so drop them here but keep their
    # snapshot ids visible via the raw file for anyone who needs them.
    if not level2.empty:
        level2 = level2[level2["side"].isin(["BID", "ASK"])].copy()
        level2["price"] = pd.to_numeric(level2["price"], errors="coerce")
        level2["size"] = pd.to_numeric(level2["size"], errors="coerce")

    events_path = path / "participant_events.jsonl"
    records = []
    if events_path.exists():
        for line in events_path.read_text().splitlines():
            if line.strip():
                records.append(json.loads(line))
    events = pd.DataFrame(records) if records else pd.DataFrame()

    def read_json(name: str) -> dict:
        p = path / name
        return json.loads(p.read_text()) if p.exists() else {}

    return Run(
        path=path,
        trades=trades,
        bbo=bbo,
        level2=level2,
        events=events,
        metadata=read_json("metadata.json"),
        recorder_meta=read_json("recorder_meta.json"),
    )
