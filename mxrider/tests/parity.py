#!/usr/bin/env python3
"""Checks that the C++ evaluator and the Python trainer agree on the same weights file.

Usage: parity.py <weights> <cpp-harness-output>

policy_test.cpp prints "SHAPE obs out hidden0" and "OUT <values...>" for a fixed observation;
this script evaluates the same observation with the numpy model and fails if they differ.
"""
from __future__ import annotations

import os
import sys

import numpy as np

# The trainer lives next door in train/; make the import work from any working directory.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "train"))

import mxrider_learn as ml  # noqa: E402  (path set up above)

FIXED_OBS = np.array([
    0.70, -0.30, 0.25, -0.15, 0.05, -0.40, 0.30, -0.20,
    0.10, 1.00, 0.60, 0.00, 0.20, -0.50, 0.15, 0.33,
], dtype=float)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: parity.py <weights> <cpp-output>", file=sys.stderr)
        return 2
    weights, cpp_output = argv[1], argv[2]

    with open(weights, encoding="utf-8") as handle:
        model = ml.MLP.from_text(handle.read())
    expected = model.predict(FIXED_OBS)[0]

    shape = None
    actual = None
    for line in cpp_output.splitlines():
        if line.startswith("SHAPE "):
            shape = [int(v) for v in line.split()[1:]]
        elif line.startswith("OUT "):
            actual = np.array([float(v) for v in line.split()[1:]])

    if shape is None or actual is None:
        print(f"FAIL parity: harness produced no result:\n{cpp_output}")
        return 1
    if shape != [model.obs, model.out, model.hidden[0]]:
        print(f"FAIL parity: shape {shape} != numpy {(model.obs, model.out, model.hidden[0])}")
        return 1
    if actual.shape[0] != model.out:
        print(f"FAIL parity: {actual.shape[0]} outputs, expected {model.out}")
        return 1

    delta = float(np.max(np.abs(actual - expected)))
    print(f"  numpy:  {' '.join(f'{v:+.6f}' for v in expected)}")
    print(f"  C++:    {' '.join(f'{v:+.6f}' for v in actual)}")
    if delta > 1e-6:
        print(f"FAIL parity: max |delta| = {delta:.3e} (the DLL would not reproduce training)")
        return 1
    print(f"PASS parity: C++ and numpy agree to {delta:.2e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
