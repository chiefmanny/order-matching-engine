#!/usr/bin/env python3
"""Tests for launcher argv construction.

Run directly: experiments/tests/test_launcher.py
No pytest dependency, matching the plain-assert style of the C++ unit tests.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from lib import processes  # noqa: E402

PASSED = 0


def ok(name: str) -> None:
    global PASSED
    PASSED += 1
    print(f"PASS: {name}")


def test_publish_port_absent_by_default() -> None:
    launcher = processes.DockerLauncher("img", Path("/tmp"), "c")
    assert launcher.publish_port is None
    argv = launcher.docker_run_argv()
    assert "-p" not in argv, argv
    ok("publish port absent by default")


def test_publish_port_maps_the_port() -> None:
    launcher = processes.DockerLauncher("img", Path("/tmp"), "c", publish_port=50051)
    argv = launcher.docker_run_argv()
    assert "-p" in argv, argv
    assert "50051:50051" in argv, argv
    ok("publish port maps the port")


def test_publish_port_precedes_the_image() -> None:
    # Docker requires every option before the image name; a `-p` after it would be
    # passed to the container as an argument instead of publishing a port.
    launcher = processes.DockerLauncher("img", Path("/tmp"), "c", publish_port=50051)
    argv = launcher.docker_run_argv()
    assert argv.index("-p") < argv.index("img"), argv
    ok("publish port precedes the image")


def test_engine_args_still_follow_the_image() -> None:
    launcher = processes.DockerLauncher("img", Path("/tmp"), "c",
                                        engine_args=["--symbols", "AAPL"],
                                        publish_port=50051)
    argv = launcher.docker_run_argv()
    assert argv.index("img") < argv.index("/app/matching_engine_server"), argv
    assert argv[-2:] == ["--symbols", "AAPL"], argv
    ok("engine args still follow the image")


def main() -> int:
    test_publish_port_absent_by_default()
    test_publish_port_maps_the_port()
    test_publish_port_precedes_the_image()
    test_engine_args_still_follow_the_image()
    print(f"\n{PASSED} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
