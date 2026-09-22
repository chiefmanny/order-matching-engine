#!/usr/bin/env python3
"""Tests for the demo-bundle writer.

Run directly: experiments/tests/test_record_demo.py
No pytest dependency, matching the plain-assert style of the other suites.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import record_demo  # noqa: E402

PASSED = 0


def ok(name: str) -> None:
    global PASSED
    PASSED += 1
    print(f"PASS: {name}")


def test_non_finite_floats_become_null() -> None:
    bundle = {
        "metrics": {
            "surveil.sweep1.lifetime_cv": float("nan"),
            "diverged": float("inf"),
            "negative": float("-inf"),
            "ordinary": 1.5,
        }
    }
    out = record_demo.json_safe(bundle)
    assert out["metrics"]["surveil.sweep1.lifetime_cv"] is None
    assert out["metrics"]["diverged"] is None
    assert out["metrics"]["negative"] is None
    assert out["metrics"]["ordinary"] == 1.5
    ok("non-finite floats become null")


def test_nested_lists_are_sanitised() -> None:
    bundle = {"clauses": [{"actual": float("nan")}, {"actual": 3}]}
    out = record_demo.json_safe(bundle)
    assert out["clauses"][0]["actual"] is None
    assert out["clauses"][1]["actual"] == 3
    ok("non-finite floats inside lists become null")


def test_the_result_is_strict_json() -> None:
    # The point of the exercise: every JSON parser that is not Python must be able
    # to read the bundle. json.dumps with allow_nan=False is that guarantee.
    bundle = {"metrics": {"cv": float("nan")}}
    text = json.dumps(record_demo.json_safe(bundle), allow_nan=False)
    assert "NaN" not in text, text
    assert json.loads(text)["metrics"]["cv"] is None
    ok("the serialised bundle is strict JSON")


def main() -> int:
    test_non_finite_floats_become_null()
    test_nested_lists_are_sanitised()
    test_the_result_is_strict_json()
    print(f"\n{PASSED} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
