#!/usr/bin/env python3
"""Run one experiment: engine + recorder + participants, on a timeline.

    experiments/run.py experiments/scenarios/static_book.yaml

Python orchestrates and observes. It never simulates market behavior -- the matching
engine and every participant are the real C++ binaries, and all market state lives in
the engine.

Exit status is the run's verdict: 0 means the run completed and passed its structural
checks, non-zero means the artifacts should not be trusted.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib import artifacts, processes, scenario as scenario_mod  # noqa: E402

DEFAULT_IMAGE = "ome-cpp:latest"
DEFAULT_CONTAINER = "ome-experiment"
# Host port for recording runs. Deliberately NOT 50051: the repo's docker-compose
# demo stack publishes that, and a recording run would fail with "port is already
# allocated". The container still listens on 50051; only the host side moves.
FINCHART_HOST_PORT = 50061


def log(message: str) -> None:
    print(f"[runner] {message}", flush=True)


def wait_for_engine(launcher, symbol: str, timeout: float = 30.0) -> bool:
    """Poll CheckSymbol until the engine answers for our symbol.

    Starting participants before the engine is ready produces a confusing cascade of
    connection failures, so the run refuses to proceed until this succeeds.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            probe = launcher.run_sync("participant_runner", ["--list-strategies"])
            if probe.returncode == 0:
                return True
        except subprocess.TimeoutExpired:
            pass
        time.sleep(0.5)
    return False


def run(args: argparse.Namespace) -> int:
    if args.finchart_record and not args.finchart_bin:
        log("FAILED: --finchart-record requires --finchart-bin")
        return 2

    scen = scenario_mod.load(args.scenario)
    log(f"scenario '{scen.name}' — {scen.symbol}, {scen.duration_seconds}s, "
        f"{len(scen.participants)} participant(s)")

    run_dir = artifacts.create(scen.name, args.results_root)
    log(f"run directory: {run_dir.path}")

    # The engine registers exactly the scenario's symbol and observes at the
    # scenario's resolution, rather than relying on the image's defaults.
    engine_args = ["--symbols", scen.symbol,
                   "--bbo-interval-ms", str(scen.bbo_interval_ms)]
    if args.backend == "docker":
        launcher = processes.DockerLauncher(
            args.image, run_dir.path, args.container, engine_args=engine_args,
            publish_port=FINCHART_HOST_PORT if args.finchart_record else None)
    else:
        launcher = processes.Launcher(Path(args.bin_dir))

    # Preserve the exact scenario that produced this run, including any inheritance
    # already resolved, so the directory is self-contained.
    shutil.copy(scen.source_path, run_dir.path / "scenario.yaml")

    meta = artifacts.base_metadata(scen, run_dir, args.backend)
    if meta["git"]["dirty"]:
        log("WARNING: working tree is dirty — this run is not reproducible from the commit alone")

    supervisor = processes.Supervisor()
    finchart_proc: subprocess.Popen | None = None
    failures: list[str] = []
    started_wall = time.time()
    # Phase timings, so a run that takes far longer than its window can be diagnosed
    # from the artifacts instead of by re-running it with a stopwatch.
    phases: dict[str, float] = {}
    mark = time.monotonic()

    def phase(name: str) -> None:
        nonlocal mark
        now = time.monotonic()
        phases[name] = round(now - mark, 2)
        mark = now

    try:
        launcher.setup(run_dir.path)
        phase("container_start")

        if not wait_for_engine(launcher, scen.symbol):
            failures.append("engine_did_not_start")
            log("FAILED: engine did not become reachable")
            if isinstance(launcher, processes.DockerLauncher):
                (run_dir.logs / "engine.stderr").write_text(launcher.logs())
            return finish(run_dir, meta, supervisor, scen, failures, started_wall, launcher)
        phase("engine_ready")
        log(f"engine is up ({phases['engine_ready']:.1f}s)")

        # The recorder attaches BEFORE any participant trades, so the tape has no
        # gap at the start of the run.
        rec_args = [
            "--symbol", scen.symbol,
            "--out-dir", launcher.container_path(run_dir.path),
            "--l2-depth", str(scen.l2_depth),
            "--l2-interval-ms", str(scen.l2_interval_ms),
        ]
        recorder = supervisor.add(
            launcher.start("recorder", "market_recorder", rec_args, run_dir.logs))
        time.sleep(1.0)
        if recorder.poll() is not None:
            failures.append("recorder_failed_to_start")
            log("FAILED: recorder exited immediately")
            return finish(run_dir, meta, supervisor, scen, failures, started_wall,
                          launcher, phases)
        phase("recorder_attach")
        log("recorder attached")

        # The finchart adapter is a HOST binary, so it cannot go through the launcher
        # (which `docker exec`s into the container). It attaches here, alongside the
        # recorder and before any participant trades; the engine's per-subscriber
        # fan-out means both observers see the complete tape.
        if args.finchart_record:
            out_path = Path(args.finchart_record).resolve()
            out_path.parent.mkdir(parents=True, exist_ok=True)
            fc_args = [
                args.finchart_bin,
                "--endpoint", f"http://localhost:{FINCHART_HOST_PORT}",
                "--symbol", scen.symbol,
                "--tick-size", str(scen.tick_size),
                "--interval-secs", "1",
                # Far finer than the adapter's 1000ms defaults. At one snapshot a
                # second a 45s run yields ~47 book updates, which renders a coarse,
                # steppy depth panel and heatmap. A whole preset is ~18KB at the
                # default cadence, so resolution is effectively free here.
                "--depth-interval-ms", "250",
                "--heartbeat-ms", "500",
                "--heatmap",
                "--headless",
                # Outlive participant teardown so the tape captures the cancellations.
                "--duration", str(int(scen.duration_seconds) + 2),
                "--record", str(out_path),
            ]
            finchart_log = (run_dir.logs / "finchart.stderr").open("w")
            finchart_proc = subprocess.Popen(fc_args, stdout=subprocess.DEVNULL,
                                             stderr=finchart_log)
            time.sleep(1.0)
            if finchart_proc.poll() is not None:
                failures.append("finchart_adapter_failed_to_start")
                log("FAILED: finchart adapter exited immediately "
                    f"(see {run_dir.logs / 'finchart.stderr'})")
                finchart_proc = None
            else:
                log(f"finchart adapter attached -> {out_path}")
            phase("finchart_attach")

        # --- timeline ------------------------------------------------------
        schedule = []
        for participant in scen.participants:
            config = scenario_mod.participant_config(scen, participant, run_dir.run_id)
            config_path = run_dir.configs / f"{participant.id}.json"
            config_path.write_text(json.dumps(config, indent=2) + "\n")
            schedule.append((participant.start_at, "start", participant, config_path))
            if participant.stop_at is not None:
                schedule.append((participant.stop_at, "stop", participant, config_path))
        schedule.sort(key=lambda item: (item[0], item[1] == "start"))

        running: dict[str, processes.ManagedProcess] = {}
        t0 = time.monotonic()
        next_event = 0

        while True:
            elapsed = time.monotonic() - t0
            if elapsed >= scen.duration_seconds:
                break

            while next_event < len(schedule) and schedule[next_event][0] <= elapsed:
                at, action, participant, config_path = schedule[next_event]
                next_event += 1
                if action == "start":
                    p_args = [
                        "--config", launcher.container_path(config_path),
                        "--server", "localhost:50051",
                    ]
                    proc = supervisor.add(launcher.start(
                        participant.id, "participant_runner", p_args, run_dir.logs))
                    running[participant.id] = proc
                    log(f"T+{elapsed:5.1f}s  start {participant.id} ({participant.strategy})")
                else:
                    proc = running.pop(participant.id, None)
                    if proc:
                        supervisor.stop(proc)
                        log(f"T+{elapsed:5.1f}s  stop  {participant.id}")

            for crashed in supervisor.crashed():
                marker = f"crashed:{crashed.name}"
                if marker not in failures:
                    failures.append(marker)
                    log(f"FAILED: {crashed.name} exited with {crashed.poll()}")

            time.sleep(0.05)

        phase("experiment_window")
        log(f"T+{scen.duration_seconds:5.1f}s  experiment window complete")

    except processes.LaunchError as exc:
        log(f"FAILED: {exc}")
        failures.append("launch_error")
    except KeyboardInterrupt:
        log("interrupted — stopping everything")
        failures.append("interrupted")

    return finish(run_dir, meta, supervisor, scen, failures, started_wall, launcher,
                  phases, finchart_proc)


def finish(run_dir, meta, supervisor, scen, failures, started_wall, launcher,
           phases: dict | None = None, finchart_proc=None) -> int:
    """Stop everything, gather artifacts, and judge the run."""
    # Participants first, then the recorder: it must still be listening while the
    # participants cancel their resting orders, or the tape misses the teardown.
    log("stopping participants...")
    phases = phases if phases is not None else {}
    teardown_start = time.monotonic()
    participant_procs = [p for p in supervisor.processes if p.name != "recorder"]
    for proc in reversed(participant_procs):
        began = time.monotonic()
        supervisor.stop(proc)
        phases[f"stop_{proc.name}"] = round(time.monotonic() - began, 2)

    recorder = next((p for p in supervisor.processes if p.name == "recorder"), None)
    if recorder:
        time.sleep(0.5)
        began = time.monotonic()
        supervisor.stop(recorder)
        phases["stop_recorder"] = round(time.monotonic() - began, 2)
    # It self-terminates on --duration, so this only collects it.
    if finchart_proc is not None:
        try:
            finchart_proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            finchart_proc.kill()
            failures.append("finchart_adapter_did_not_exit")
            log("FAILED: finchart adapter did not exit within 30s")

    phases["teardown"] = round(time.monotonic() - teardown_start, 2)
    log(f"stopped ({phases['teardown']:.1f}s)")

    exit_status = {}
    for proc in supervisor.processes:
        code = proc.poll()
        exit_status[proc.name] = {"exit_code": code, "killed": proc.killed}
        if proc.killed:
            failures.append(f"needed_sigkill:{proc.name}")
        if code not in (0, None) and proc.name != "recorder":
            marker = f"nonzero_exit:{proc.name}"
            if marker not in failures:
                failures.append(marker)

    if isinstance(launcher, processes.DockerLauncher):
        (run_dir.logs / "engine.stderr").write_text(launcher.logs())
    launcher.teardown()

    events_path = artifacts.merge_participant_events(run_dir)
    malformed = sum(
        1 for line in events_path.read_text().splitlines()
        if line and json.loads(line).get("type") == "malformed_event"
    )
    if malformed:
        failures.append("malformed_participant_events")

    checks = structural_checks(run_dir, scen, time.time() - started_wall,
                               sum(phases.values()))
    failures.extend(checks["failures"])

    meta["ended_at_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    meta["wall_seconds"] = round(time.time() - started_wall, 3)
    # Monotonic total, which excludes any time the host spent suspended. Wall clock
    # keeps advancing across a sleep and monotonic does not, so the difference between
    # them is exactly how long the machine was asleep.
    monotonic_total = round(sum(phases.values()), 3)
    meta["monotonic_seconds"] = monotonic_total
    suspended = meta["wall_seconds"] - monotonic_total
    if suspended > 30.0:
        meta["host_suspended_seconds"] = round(suspended, 1)
    meta["process_exit_status"] = exit_status
    meta["artifact_summary"] = checks["summary"]
    meta["engine_bbo_interval_ms"] = scen.bbo_interval_ms
    meta["phase_seconds"] = phases
    meta["malformed_event_lines"] = malformed
    meta["failures"] = failures
    meta["status"] = "ok" if not failures else "failed"
    artifacts.write_metadata(run_dir, meta)

    log("--- artifacts ---")
    for key, value in checks["summary"].items():
        log(f"  {key}: {value}")

    if failures:
        log(f"RUN FAILED: {', '.join(failures)}")
        return 1
    log(f"RUN OK — {run_dir.path}")
    return 0


def structural_checks(run_dir, scen, wall_seconds: float = 0.0,
                      monotonic_seconds: float = 0.0) -> dict:  # noqa: C901
    """Cheap checks that the artifacts are usable at all.

    Deliberately structural, not behavioral: whether the market did something
    interesting is the analysis step's job (Phase 5). This only answers "did we
    actually record a market?".
    """
    summary, failures = {"_wall_seconds": wall_seconds,
                         "_monotonic_seconds": monotonic_seconds}, []

    def line_count(name: str) -> int:
        path = run_dir.path / name
        if not path.exists():
            return -1
        with path.open() as handle:
            return max(0, sum(1 for _ in handle) - 1)  # minus header

    for name in ("trades.csv", "bbo.csv", "level2.csv"):
        count = line_count(name)
        summary[name] = count
        if count < 0:
            failures.append(f"missing:{name}")

    rec_meta_path = run_dir.path / "recorder_meta.json"
    if rec_meta_path.exists():
        rec_meta = json.loads(rec_meta_path.read_text())
        summary["level2_poll_failures"] = rec_meta.get("level2_poll_failures", 0)
        if rec_meta.get("trade_stream_error"):
            failures.append("trade_stream_error")
        if rec_meta.get("bbo_stream_error"):
            failures.append("bbo_stream_error")

        # The recorder must have observed roughly the scenario's window. A recorder
        # that outlives its run keeps appending to the same tape, so the artifacts
        # silently mix in market activity from a completely different period -- which
        # then shows up as nonsense derived metrics (a 45s run reporting a 1000s trade
        # span) rather than as an obvious failure.
        observed = float(rec_meta.get("duration_seconds", 0.0) or 0.0)
        summary["recorder_duration_seconds"] = round(observed, 1)
        budget = scen.duration_seconds + 30.0
        if observed > budget:
            # The recorder measures its own lifetime in wall clock, which keeps
            # advancing while the host is asleep. A suspended machine therefore
            # inflates it without anything being wrong with the recording -- observed
            # once at 468s of "recorder duration" for a run whose monotonic phases
            # summed to 48.6s and whose artifacts were entirely normal.
            #
            # Judge against monotonic time, which excludes suspension, and report a
            # sleep as its own thing rather than as data contamination.
            monotonic = summary.get("_monotonic_seconds", 0.0)
            wall = summary.get("_wall_seconds", 0.0)
            slept = wall - monotonic if wall and monotonic else 0.0
            effective = observed - slept if slept > 30.0 else observed

            if effective <= budget:
                summary["host_suspended_seconds"] = round(slept, 1)
            elif monotonic and effective <= monotonic + 5.0:
                failures.append("run_exceeded_window")
            else:
                failures.append("recorder_outlived_run")
        elif observed < scen.duration_seconds * 0.5:
            failures.append("recorder_stopped_early")
    else:
        failures.append("missing:recorder_meta.json")

    if summary.get("bbo.csv", 0) == 0:
        failures.append("no_bbo_observed")
    if summary.get("level2.csv", 0) == 0:
        failures.append("no_level2_observed")

    # Any participant leaving live orders behind poisons every later measurement, so
    # it is a run failure rather than a warning.
    #
    # Crucially, a participant that produced NO metrics event is also a failure: it
    # means the process was killed before it could clean up, and summing
    # orders_left_live over zero events would otherwise report a reassuring 0.
    events_path = run_dir.path / "participant_events.jsonl"
    left_behind = 0
    reported: set[str] = set()
    if events_path.exists():
        for line in events_path.read_text().splitlines():
            if not line:
                continue
            event = json.loads(line)
            if event.get("type") == "metrics":
                reported.add(event.get("participant_id", ""))
                left_behind += int(event.get("orders_left_live", 0) or 0)

    expected = {p.id for p in scen.participants}
    silent = sorted(expected - reported)
    summary["orders_left_live"] = left_behind
    summary["participants_reporting_metrics"] = f"{len(reported)}/{len(expected)}"
    if left_behind:
        failures.append("orders_left_behind")
    for pid in silent:
        failures.append(f"no_metrics_from:{pid}")

    summary.pop("_wall_seconds", None)
    summary.pop("_monotonic_seconds", None)
    return {"summary": summary, "failures": failures}


def main() -> int:
    parser = argparse.ArgumentParser(description="Run one matching-engine experiment.")
    parser.add_argument("scenario", help="path to a scenario YAML file")
    parser.add_argument("--backend", choices=("docker", "local"), default="docker",
                        help="how to launch the C++ binaries (default: docker)")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="docker image with the binaries")
    parser.add_argument("--container", default=DEFAULT_CONTAINER, help="engine container name")
    parser.add_argument("--bin-dir", default="build", help="binary directory for --backend local")
    parser.add_argument("--results-root", default=None, help="override experiments/results")
    parser.add_argument("--finchart-record", metavar="PATH", default=None,
                        help="also record a finchart MarketEvent stream to PATH")
    parser.add_argument("--finchart-bin", metavar="PATH", default=None,
                        help="path to the finchart examples-matching-engine binary")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
