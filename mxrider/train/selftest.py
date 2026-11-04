#!/usr/bin/env python3
"""End-to-end check of the training half, without the game.

Generates synthetic run logs in exactly the format src/telemetry.cpp writes, runs the real loader
and trainer over them, exports weights, and re-reads them with the same rules src/policy.cpp uses.
It also checks that a damaged file is rejected rather than silently accepted, because the DLL's
failure policy depends on that rejection being real.
"""
from __future__ import annotations

import os
import tempfile

import numpy as np

import mxrider_learn as ml

HEADER = ("episode,frame,t,dt,source,valid,airborne,px,py,pz,vx,vy,vz,speed,fspeed,accelG,"
          "pitch,roll,yawRate,upright,progress,lap,throttle,brakeFront,brakeRear,steer,lean,clutch,"
          "reward,done,reason")


def write_synthetic(path: str, episodes: int = 3, frames: int = 240) -> None:
    rows = [HEADER]
    row_index = 0
    for episode in range(1, episodes + 1):
        speed = 20.0 + 5.0 * episode
        for frame in range(frames):
            t = frame / 60.0
            steer = float(np.sin(t * 2.0)) * 0.4
            throttle = 0.6 + 0.3 * float(np.cos(t))
            # A hand-driven episode is source 3, a policy drive is source 2.
            source = 3 if episode == 1 else 2
            reward = 0.02 * speed / 30.0
            rows.append(",".join(str(v) for v in [
                episode, row_index, round(t, 4), round(1 / 60.0, 5), source, 1, 0,
                10.0 * frame, 0.5, 5.0, speed * 0.7, 0.0, speed * 0.7, speed, speed * 0.9,
                round(-0.2 + 0.1 * np.sin(t), 4), round(3.0 * np.sin(t * 3), 3),
                round(4.0 * np.sin(t * 2), 3), round(20.0 * np.cos(t * 2), 3), round(0.98, 4),
                round(frame / frames, 4), 0,
                round(throttle, 4), 0.0, 0.0, round(steer, 4), 0.0, 0.0,
                round(reward, 4), 0, "-",
            ]))
            row_index += 1
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(rows) + "\n")


def main() -> int:
    failures = 0
    workdir = tempfile.mkdtemp(prefix="mxrider_selftest_")
    log_path = os.path.join(workdir, "mxrider_run_20260101_000000.csv")
    weights = os.path.join(workdir, "mxrider_policy.txt")
    write_synthetic(log_path)

    # 1. The loader accepts the DLL's own column layout.
    episodes = ml.load_runs([log_path], ref_speed=30.0)
    assert len(episodes) == 3, episodes
    assert episodes[0].obs.shape[1] == len(ml.OBS_NAMES), episodes[0].obs.shape
    print(f"PASS load: {ml.summarise(episodes).splitlines()[0]}")

    # 2. Both imitation sources are picked up.
    x, y, w = ml.build_dataset(episodes, use_human=True, use_rider=True, reward_filter=True)
    assert x.shape[1] == len(ml.OBS_NAMES) and y.shape[1] == len(ml.ACT_NAMES), (x.shape, y.shape)
    print(f"PASS dataset: {x.shape[0]} frames, human={int(np.sum(episodes[0].source == 3))}, "
          f"rider={int(np.sum(episodes[1].source == 2))}")

    # 3. Training actually reduces the loss.
    model = ml.MLP(x.shape[1], (32, 32), y.shape[1], seed=3)
    history = ml.train(model, x, y, w, epochs=12, lr=3e-3, batch=64, l2=1e-4, seed=3)
    assert history["train"][-1] < history["train"][0], history["train"][:2] + history["train"][-2:]
    print(f"PASS train: loss {history['train'][0]:.4f} -> {history['train'][-1]:.4f}")

    # 4. Export and re-import: the C++ parser's rules, applied to the file we just wrote.
    model.save(weights)
    with open(weights, encoding="utf-8") as handle:
        text = handle.read()
    reloaded = ml.MLP.from_text(text)
    delta = float(np.max(np.abs(model.predict(x[:32]) - reloaded.predict(x[:32]))))
    assert delta < 1e-6, delta
    tokens = text.split()
    assert tokens[0] == "1" and tokens[1] == "obs" and tokens[3] == "hidden", tokens[:6]
    print(f"PASS export: {len(tokens)} tokens, round trip |delta| = {delta:.2e}")

    # 5. Truncation is rejected, which is what lets the DLL fall back to the baseline.
    try:
        ml.MLP.from_text(" ".join(tokens[: len(tokens) // 2]))
    except ValueError as error:
        print(f"PASS reject: truncated file refused ({error})")
    else:
        print("FAIL reject: a truncated weights file was accepted")
        failures += 1

    # 6. Numbers the C++ loader would refuse on sight (non-finite) are refused here too.
    poisoned = text.replace(tokens[9], "nan", 1)  # tokens[9] is w1's first number
    try:
        ml.MLP.from_text(poisoned)
    except ValueError as error:
        print(f"PASS reject: non-finite weight refused ({error})")
    else:
        print("FAIL reject: a NaN weight was accepted")
        failures += 1

    # 7. CLI smoke test on the same log.
    code = ml.main(["--data", log_path, "--out", weights, "--epochs", "3", "--ref-speed", "30"])
    if code != 0:
        print(f"FAIL cli: exit {code}")
        failures += 1
    else:
        print("PASS cli: train + export via argparse path")

    print(f"\n{'FAILED' if failures else 'ALL PASSED'} ({failures} failure(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
