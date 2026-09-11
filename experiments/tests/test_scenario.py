#!/usr/bin/env python3
"""Tests for scenario loading, inheritance and validation.

Run directly: experiments/tests/test_scenario.py
No pytest dependency, matching the plain-assert style of the C++ unit tests.
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from lib import scenario as scenario_mod  # noqa: E402

PASSED = 0


def ok(name: str) -> None:
    global PASSED
    PASSED += 1
    print(f"PASS: {name}")


def write(tmp: Path, name: str, text: str) -> Path:
    path = tmp / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


BASE = """
name: baseline
symbol: AAPL
tick_size: 0.01
duration_seconds: 30
random_seed: 7
recorder:
  l2_depth: 15
  l2_interval_ms: 100
participants:
  - id: lp1
    strategy: liquidity_provider
    start_at: 0
    config:
      referencePrice: 500.0
      averageQuantity: 500
  - id: noise1
    strategy: noise_trader
    start_at: 2
    config:
      orderIntervalMs: 300
"""


def test_loads_basic_scenario(tmp: Path) -> None:
    path = write(tmp, "base.yaml", BASE)
    scen = scenario_mod.load(path)
    assert scen.name == "baseline"
    assert scen.symbol == "AAPL"
    assert scen.duration_seconds == 30
    assert len(scen.participants) == 2
    assert scen.l2_depth == 15 and scen.l2_interval_ms == 100
    ok("loads a basic scenario")


def test_extends_merges_and_overrides(tmp: Path) -> None:
    write(tmp, "base.yaml", BASE)
    child = write(tmp, "child.yaml", """
extends: base.yaml
name: child
duration_seconds: 45
participants:
  - id: lp1
    config:
      averageQuantity: 1000
  - id: sweep
    strategy: sweep
    start_at: 20
    config:
      targetQuantity: 5000
""")
    scen = scenario_mod.load(child)
    assert scen.name == "child"
    assert scen.duration_seconds == 45
    assert scen.symbol == "AAPL"                    # inherited

    by_id = {p.id: p for p in scen.participants}
    assert set(by_id) == {"lp1", "noise1", "sweep"}
    # A partial override must patch the inherited config, not replace it wholesale.
    assert by_id["lp1"].config["averageQuantity"] == 1000
    assert by_id["lp1"].config["referencePrice"] == 500.0
    assert by_id["lp1"].strategy == "liquidity_provider"
    assert by_id["sweep"].start_at == 20
    ok("extends merges participants and deep-merges config")


def test_extends_can_remove_a_participant(tmp: Path) -> None:
    write(tmp, "base.yaml", BASE)
    child = write(tmp, "noise_only.yaml", """
extends: base.yaml
name: noise_only
participants:
  - id: lp1
    remove: true
""")
    scen = scenario_mod.load(child)
    assert [p.id for p in scen.participants] == ["noise1"]
    ok("extends can remove an inherited participant")


def test_seeds_are_derived_and_distinct(tmp: Path) -> None:
    path = write(tmp, "base.yaml", BASE)
    scen = scenario_mod.load(path)
    lp, noise = scen.participants[0], scen.participants[1]

    a, b = scen.participant_seed(lp), scen.participant_seed(noise)
    # Two participants must not share a seed, or identically configured actors would
    # make identical decisions and stop being independent sources of flow.
    assert a != b
    assert 0 <= a <= 0x7FFFFFFF and 0 <= b <= 0x7FFFFFFF
    # ...but the derivation must be stable, so a run is reproducible.
    assert scenario_mod.load(path).participant_seed(lp) == a
    ok("participant seeds are derived, distinct, stable and int32-safe")


def test_master_seed_changes_all_participant_seeds(tmp: Path) -> None:
    a = scenario_mod.load(write(tmp, "a.yaml", BASE))
    b = scenario_mod.load(write(tmp, "b.yaml", BASE.replace("random_seed: 7", "random_seed: 8")))
    assert a.participant_seed(a.participants[0]) != b.participant_seed(b.participants[0])
    ok("changing the master seed changes every participant seed")


def test_participant_config_shape(tmp: Path) -> None:
    scen = scenario_mod.load(write(tmp, "base.yaml", BASE))
    cfg = scenario_mod.participant_config(scen, scen.participants[1], "baseline/run_001")

    for key in ("participantId", "runId", "symbol", "strategy", "randomSeed",
                "maxRuntimeSeconds", "tickSize", "decisionIntervalMs", "parameters"):
        assert key in cfg, f"missing {key}"
    assert cfg["participantId"] == "noise1"
    assert cfg["symbol"] == "AAPL"
    assert cfg["parameters"]["orderIntervalMs"] == 300
    # noise1 starts at 2s in a 30s run -> ~28s of work plus a safety margin.
    assert cfg["maxRuntimeSeconds"] >= 28
    ok("participant config has the shape ParticipantConfig::fromFile expects")


def test_stop_at_shortens_max_runtime(tmp: Path) -> None:
    scen = scenario_mod.load(write(tmp, "s.yaml", """
name: s
symbol: AAPL
duration_seconds: 60
participants:
  - id: sweep
    strategy: sweep
    start_at: 10
    stop_at: 20
"""))
    cfg = scenario_mod.participant_config(scen, scen.participants[0], "s/run_001")
    assert 10 <= cfg["maxRuntimeSeconds"] <= 14, cfg["maxRuntimeSeconds"]
    ok("stop_at bounds a participant's own max runtime")


def test_validation_rejects_bad_scenarios(tmp: Path) -> None:
    cases = {
        "missing required field": "name: x\nsymbol: AAPL\n",
        "no participants": "name: x\nsymbol: AAPL\nduration_seconds: 10\nparticipants: []\n",
        "duplicate id": """
name: x
symbol: AAPL
duration_seconds: 10
participants:
  - {id: a, strategy: echo}
  - {id: a, strategy: echo}
""",
        "participant lacking strategy": """
name: x
symbol: AAPL
duration_seconds: 10
participants:
  - {id: a}
""",
        "stop_at before start_at": """
name: x
symbol: AAPL
duration_seconds: 30
participants:
  - {id: a, strategy: echo, start_at: 10, stop_at: 5}
""",
        # A participant scheduled at or past the end would never run; silently
        # producing an empty result would be worse than refusing.
        "start_at past the end": """
name: x
symbol: AAPL
duration_seconds: 10
participants:
  - {id: a, strategy: echo, start_at: 30}
""",
    }
    for idx, (why, text) in enumerate(cases.items()):
        path = write(tmp, f"bad_{idx}.yaml", text)
        try:
            scenario_mod.load(path)
            raise AssertionError(f"should have rejected: {why}")
        except scenario_mod.ScenarioError:
            pass
    ok(f"validation rejects {len(cases)} malformed scenarios")


def test_circular_extends_is_detected(tmp: Path) -> None:
    write(tmp, "a.yaml", "extends: b.yaml\nname: a\nsymbol: AAPL\nduration_seconds: 5\n")
    write(tmp, "b.yaml", "extends: a.yaml\nname: b\nsymbol: AAPL\nduration_seconds: 5\n")
    try:
        scenario_mod.load(tmp / "a.yaml")
        raise AssertionError("should have detected the cycle")
    except scenario_mod.ScenarioError as exc:
        assert "circular" in str(exc)
    ok("circular extends chains are detected")


def test_extends_resolves_from_experiments_root(tmp: Path) -> None:
    """A generated variant in a subdirectory must still find its parent.

    sweep.py writes variants into experiments/scenarios/_sweep/, so `extends` cannot
    be resolved only relative to the referring file -- doing so silently produced
    "run produced no directory" for every value in a sweep.
    """
    root = Path(scenario_mod.EXPERIMENTS_ROOT)
    nested = root / "scenarios" / "_sweep_test"
    nested.mkdir(parents=True, exist_ok=True)
    variant = nested / "variant.yaml"
    variant.write_text(
        "extends: baselines/normal_market.yaml\n"
        "name: variant_probe\n"
        "duration_seconds: 10\n"
    )
    try:
        scen = scenario_mod.load(variant)
        assert scen.symbol == "AAPL"           # inherited from the baseline
        assert len(scen.participants) == 3     # inherited participants
        assert scen.duration_seconds == 10     # own override
        ok("extends resolves from the experiments root, not just the file's directory")
    finally:
        variant.unlink(missing_ok=True)
        nested.rmdir()


def test_missing_extends_is_reported(tmp: Path) -> None:
    path = write(tmp, "orphan.yaml",
                 "extends: nowhere/missing.yaml\nname: x\nsymbol: AAPL\nduration_seconds: 5\n")
    try:
        scenario_mod.load(path)
        raise AssertionError("should have reported the missing parent")
    except scenario_mod.ScenarioError as exc:
        assert "not found" in str(exc)
    ok("a missing 'extends' target is reported clearly")


def test_shipped_scenarios_all_load() -> None:
    root = Path(__file__).resolve().parent.parent
    files = sorted(root.glob("scenarios/*.yaml")) + sorted(root.glob("baselines/*.yaml"))
    assert files, "no scenarios found to validate"
    for path in files:
        scenario_mod.load(path)
    ok(f"all {len(files)} shipped scenario file(s) load and validate")


def main() -> int:
    print("--- Scenario Tests ---")
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        test_loads_basic_scenario(tmp)
        test_extends_merges_and_overrides(tmp)
        test_extends_can_remove_a_participant(tmp)
        test_seeds_are_derived_and_distinct(tmp)
        test_master_seed_changes_all_participant_seeds(tmp)
        test_participant_config_shape(tmp)
        test_stop_at_shortens_max_runtime(tmp)
        test_validation_rejects_bad_scenarios(tmp)
        test_circular_extends_is_detected(tmp)
        test_extends_resolves_from_experiments_root(tmp)
        test_missing_extends_is_reported(tmp)
    test_shipped_scenarios_all_load()
    print(f"All {PASSED} scenario tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
