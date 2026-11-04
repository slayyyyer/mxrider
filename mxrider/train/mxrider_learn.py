#!/usr/bin/env python3
"""MXRider offline trainer.

Reads the run logs the mod writes (mxrider_run_*.csv), trains the small network the DLL evaluates,
and exports it as plain text in the format src/policy.cpp parses.

Two things are trained from, and the difference matters:

  * source 3 (human): the person's own throttle / brake / steer, logged while riding in
    record-line mode. This is imitation data - the starting point.
  * source 1/2 (baseline / policy): episodes driven by the DLL, with their logged rewards. These
    are used for reward-weighted imitation: rows from better-than-average episodes count more, so
    the network drifts towards whatever the current rider does well.

Everything is numpy; there is no deep-learning dependency to install.
"""
from __future__ import annotations

import argparse
import csv
import glob
import math
import os
import sys
from dataclasses import dataclass, field

import numpy as np

# Must match rider.cpp's observation construction, in this order.
OBS_NAMES = [
    "forward_speed",
    "speed",
    "vel_x",
    "vel_y",
    "vel_z",
    "accel_g",
    "pitch",
    "roll",
    "yaw_rate",
    "airborne",
    "prev_throttle",
    "prev_brake_front",
    "prev_brake_rear",
    "prev_steer",
    "prev_lean",
    "progress",
]

# Must match policy.cpp: tanh outputs, pedals take the positive half.
ACT_NAMES = ["throttle", "brake_front", "brake_rear", "steer", "lean", "clutch"]
PEDAL_ACTIONS = {0, 1, 2, 5}  # indices clamped to [0, 1] by max(0, out)

REQUIRED_COLUMNS = {
    "episode", "frame", "source", "valid",
    "vx", "vy", "vz", "speed", "fspeed", "accelG", "pitch", "roll", "yawRate", "airborne",
    "progress", "throttle", "brakeFront", "brakeRear", "steer", "lean", "clutch", "reward", "done",
}


@dataclass
class Episode:
    file: str
    episode: int
    obs: np.ndarray = field(default_factory=lambda: np.zeros((0, len(OBS_NAMES))))
    act: np.ndarray = field(default_factory=lambda: np.zeros((0, len(ACT_NAMES))))
    reward: np.ndarray = field(default_factory=lambda: np.zeros((0,)))
    source: np.ndarray = field(default_factory=lambda: np.zeros((0,), dtype=int))
    laps: int = 0

    @property
    def length(self) -> int:
        return int(self.obs.shape[0])

    @property
    def total_reward(self) -> float:
        return float(self.reward.sum())


def _row_to_obs(row: dict, ref_speed: float) -> list[float]:
    return [
        float(row["fspeed"]) / ref_speed,
        float(row["speed"]) / ref_speed,
        float(row["vx"]) / ref_speed,
        float(row["vy"]) / ref_speed,
        float(row["vz"]) / ref_speed,
        float(row["accelG"]),
        float(row["pitch"]) / 45.0,
        float(row["roll"]) / 45.0,
        float(row["yawRate"]) / 180.0,
        1.0 if int(row["airborne"]) else 0.0,
        float(row["throttle"]),
        float(row["brakeFront"]),
        float(row["brakeRear"]),
        float(row["steer"]),
        float(row["lean"]),
        float(row["progress"]),
    ]


def load_runs(paths: list[str], ref_speed: float = 30.0, min_length: int = 20) -> list[Episode]:
    """Loads every CSV into episodes, one per (file, episode number)."""
    files: list[str] = []
    for path in paths:
        if os.path.isdir(path):
            files.extend(sorted(glob.glob(os.path.join(path, "mxrider_run_*.csv"))))
        else:
            files.extend(sorted(glob.glob(path)))
    if not files:
        raise SystemExit(f"no run logs found in {paths!r} (looking for mxrider_run_*.csv)")

    episodes: list[Episode] = []
    for path in files:
        with open(path, newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            missing = REQUIRED_COLUMNS - set(reader.fieldnames or [])
            if missing:
                raise SystemExit(f"{path}: not an MXRider log, missing columns {sorted(missing)}")

            groups: dict[int, Episode] = {}
            for row in reader:
                if int(row["valid"]) == 0:
                    continue  # no bike tracked that frame: nothing to imitate
                index = int(row["episode"])
                episode = groups.get(index)
                if episode is None:
                    episode = groups[index] = Episode(file=path, episode=index)
                episode.obs = np.vstack([episode.obs, _row_to_obs(row, ref_speed)])
                episode.act = np.vstack([episode.act, [
                    float(row["throttle"]), float(row["brakeFront"]), float(row["brakeRear"]),
                    float(row["steer"]), float(row["lean"]), float(row["clutch"]),
                ]])
                episode.reward = np.append(episode.reward, float(row["reward"]))
                episode.source = np.append(episode.source, int(row["source"]))
            for episode in groups.values():
                episode.laps = 0
                if episode.length >= min_length:
                    episodes.append(episode)
    if not episodes:
        raise SystemExit("no usable episodes (all shorter than %d frames?)" % min_length)
    return episodes


def summarise(episodes: list[Episode]) -> str:
    total = sum(e.total_reward for e in episodes)
    lines = [f"{len(episodes)} episodes, {sum(e.length for e in episodes)} frames, "
             f"total return {total:.1f}"]
    by_source: dict[int, int] = {}
    for episode in episodes:
        for code in episode.source:
            by_source[int(code)] = by_source.get(int(code), 0) + 1
    names = {0: "idle", 1: "baseline", 2: "policy", 3: "human"}
    parts = [f"{names.get(k, k)}={v}" for k, v in sorted(by_source.items())]
    lines.append("  frames by source: " + ", ".join(parts))
    best = max(episodes, key=lambda e: e.total_reward)
    lines.append(f"  best episode: {best.total_reward:.1f} return over {best.length} frames "
                 f"({os.path.basename(best.file)} #{best.episode})")
    return "\n".join(lines)


class MLP:
    """3-layer tanh network: obs -> hidden[0] -> hidden[1] -> out, all tanh."""

    def __init__(self, obs: int, hidden: tuple[int, int], out: int, seed: int = 0):
        self.obs, self.hidden, self.out = obs, tuple(hidden), out
        rng = np.random.default_rng(seed)
        def glorot(fan_in: int, fan_out: int) -> np.ndarray:
            limit = math.sqrt(6.0 / (fan_in + fan_out))
            return rng.uniform(-limit, limit, size=(fan_out, fan_in))
        self.w1 = glorot(obs, hidden[0]); self.b1 = np.zeros(hidden[0])
        self.w2 = glorot(hidden[0], hidden[1]); self.b2 = np.zeros(hidden[1])
        self.w3 = glorot(hidden[1], out); self.b3 = np.zeros(out)

    # ---- forward / backward -------------------------------------------------
    def forward(self, x: np.ndarray, cache: bool = False):
        z1 = x @ self.w1.T + self.b1
        a1 = np.tanh(z1)
        z2 = a1 @ self.w2.T + self.b2
        a2 = np.tanh(z2)
        z3 = a2 @ self.w3.T + self.b3
        y = np.tanh(z3)
        if not cache:
            return y
        return y, (x, a1, a2)

    def predict(self, x: np.ndarray) -> np.ndarray:
        return self.forward(np.atleast_2d(np.asarray(x, dtype=float)))

    def loss_and_grads(self, x: np.ndarray, target: np.ndarray, l2: float):
        n = x.shape[0]
        y, (x, a1, a2) = self.forward(x, cache=True)
        diff = y - target
        loss = float(np.mean(diff ** 2)) + l2 * float(
            np.sum(self.w1 ** 2) + np.sum(self.w2 ** 2) + np.sum(self.w3 ** 2))
        d3 = (2.0 / n) * diff * (1.0 - y ** 2)
        gw3 = d3.T @ a2 + 2.0 * l2 * self.w3
        gb3 = d3.sum(axis=0)
        d2 = (d3 @ self.w3) * (1.0 - a2 ** 2)
        gw2 = d2.T @ a1 + 2.0 * l2 * self.w2
        gb2 = d2.sum(axis=0)
        d1 = (d2 @ self.w2) * (1.0 - a1 ** 2)
        gw1 = d1.T @ x + 2.0 * l2 * self.w1
        gb1 = d1.sum(axis=0)
        return loss, {"w1": gw1, "b1": gb1, "w2": gw2, "b2": gb2, "w3": gw3, "b3": gb3}

    # ---- export / import ----------------------------------------------------
    def to_text(self) -> str:
        def numbers(array: np.ndarray) -> str:
            return " ".join(f"{v:.8g}" for v in np.asarray(array, dtype=float).ravel())

        return "\n".join([
            "1",                                   # format version
            f"obs {self.obs}",
            f"hidden {self.hidden[0]} {self.hidden[1]}",
            f"out {self.out}",
            "w1 " + numbers(self.w1),
            "b1 " + numbers(self.b1),
            "w2 " + numbers(self.w2),
            "b2 " + numbers(self.b2),
            "w3 " + numbers(self.w3),
            "b3 " + numbers(self.b3),
            "",                                    # trailing newline
        ])

    def save(self, path: str) -> None:
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(self.to_text())

    @staticmethod
    def from_text(text: str) -> "MLP":
        """Mirror of the C++ loader, so a round trip can be checked before shipping weights."""
        tokens = text.split()
        if not tokens:
            raise ValueError("empty policy file")
        index = 0

        def next_token() -> str:
            nonlocal index
            if index >= len(tokens):
                raise ValueError("policy file ended early")
            token = tokens[index]
            index += 1
            return token

        def expect(label: str) -> None:
            if next_token().lower() != label:
                raise ValueError(f"expected '{label}'")

        def read_int() -> int:
            return int(next_token())

        def read_float() -> float:
            value = float(next_token())
            if not math.isfinite(value):
                raise ValueError("non-finite weight")
            return value

        if read_int() != 1:
            raise ValueError("unsupported policy version")
        expect("obs"); obs = read_int()
        expect("hidden"); h0 = read_int(); h1 = read_int()
        expect("out"); out = read_int()
        model = MLP(obs, (h0, h1), out)
        for label, array in (("w1", model.w1), ("b1", model.b1), ("w2", model.w2),
                             ("b2", model.b2), ("w3", model.w3), ("b3", model.b3)):
            expect(label)
            flat = np.array([read_float() for _ in range(array.size)])
            array[...] = flat.reshape(array.shape)
        if index != len(tokens):
            raise ValueError("trailing data in policy file")
        return model


def discounted_returns(rewards: np.ndarray, gamma: float = 0.995) -> np.ndarray:
    out = np.zeros_like(rewards, dtype=float)
    running = 0.0
    for i in range(len(rewards) - 1, -1, -1):
        running = rewards[i] + gamma * running
        out[i] = running
    return out


def build_dataset(episodes: list[Episode], use_human: bool, use_rider: bool,
                  reward_filter: bool) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Assembles (obs, act, weight) from the episodes.

    Human rows get weight 1. Rider rows are weighted by their episode's advantage over the mean
    episode return, so good drives pull the policy and bad ones push it away - a one-step,
    offline stand-in for a policy-gradient update that needs no live rollouts.
    """
    xs, ys, ws = [], [], []
    rider_returns = [e.total_reward for e in episodes if 2 in set(e.source.tolist())]
    mean_return = float(np.mean(rider_returns)) if rider_returns else 0.0

    for episode in episodes:
        advantage = episode.total_reward - mean_return
        for i in range(episode.length):
            source = int(episode.source[i])
            target = episode.act[i]
            if target[3] > 1.0001 or target[3] < -1.0001:
                continue  # a human log from a wild pad sample; skip rather than learn it
            if source == 3 and use_human:
                weight = 1.0
            elif source in (1, 2) and use_rider:
                if reward_filter:
                    if advantage <= 0.0:
                        continue
                    weight = min(4.0, advantage / max(1e-6, abs(mean_return)) + 1.0)
                else:
                    weight = 1.0
            else:
                continue
            xs.append(episode.obs[i])
            ys.append(target)
            ws.append(weight)

    if not xs:
        raise SystemExit("nothing to train on: no rows matched the selected sources")
    return (np.asarray(xs, dtype=float), np.asarray(ys, dtype=float),
            np.asarray(ws, dtype=float))


def train(model: MLP, x: np.ndarray, y: np.ndarray, w: np.ndarray, epochs: int,
          lr: float, batch: int, l2: float, seed: int, holdout: float = 0.15) -> dict:
    rng = np.random.default_rng(seed)
    order = rng.permutation(len(x))
    split = int(len(x) * (1.0 - holdout)) if holdout > 0 else len(x)
    train_idx, val_idx = order[:split], order[split:]

    params = {k: getattr(model, k) for k in ("w1", "b1", "w2", "b2", "w3", "b3")}
    m = {k: np.zeros_like(v) for k, v in params.items()}
    v = {k: np.zeros_like(v) for k, v in params.items()}
    beta1, beta2, eps = 0.9, 0.999, 1e-8
    step = 0
    history = {"train": [], "val": []}

    for epoch in range(epochs):
        rng.shuffle(train_idx)
        for start in range(0, len(train_idx), batch):
            idx = train_idx[start:start + batch]
            xb, yb, wb = x[idx], y[idx], w[idx]
            loss, grads = model.loss_and_grads(xb, yb, l2)
            # Weighted by episode quality: scale each row's gradient instead of its loss.
            scale = (wb / max(1e-9, wb.mean())).reshape(-1, 1)
            step += 1
            for k in params:
                gk = grads[k]
                if gk.shape[0] == scale.shape[0]:
                    gk = gk * scale
                m[k] = beta1 * m[k] + (1 - beta1) * gk
                v[k] = beta2 * v[k] + (1 - beta2) * (gk ** 2)
                mhat = m[k] / (1 - beta1 ** step)
                vhat = v[k] / (1 - beta2 ** step)
                setattr(model, k, getattr(model, k) - lr * mhat / (np.sqrt(vhat) + eps))
                params[k] = getattr(model, k)
        train_loss, _ = model.loss_and_grads(x[train_idx], y[train_idx], 0.0)
        history["train"].append(train_loss)
        if len(val_idx):
            val_loss, _ = model.loss_and_grads(x[val_idx], y[val_idx], 0.0)
            history["val"].append(val_loss)
    return history


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Train the MXRider policy from its run logs.")
    parser.add_argument("--data", nargs="+", default=["."],
                        help="log file(s) or folder(s) of mxrider_run_*.csv (default: .)")
    parser.add_argument("--out", default="mxrider_policy.txt", help="weights file to write")
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--hidden", type=int, nargs=2, default=[32, 32])
    parser.add_argument("--lr", type=float, default=3e-3)
    parser.add_argument("--batch", type=int, default=128)
    parser.add_argument("--l2", type=float, default=1e-4)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--ref-speed", type=float, default=30.0,
                        help="must match RefSpeed in mxrider.ini")
    parser.add_argument("--human-only", action="store_true", help="imitate only the human rows")
    parser.add_argument("--no-reward-filter", action="store_true",
                        help="use rider rows regardless of how the episode scored")
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args(argv)

    episodes = load_runs(args.data, ref_speed=args.ref_speed)
    print(summarise(episodes))
    if args.report_only:
        return 0

    x, y, w = build_dataset(episodes,
                            use_human=True,
                            use_rider=not args.human_only,
                            reward_filter=not args.no_reward_filter)
    print(f"training on {len(x)} frames "
          f"({np.sum(w > 1.0)} reward-weighted), obs {x.shape[1]} -> {y.shape[1]}")

    model = MLP(x.shape[1], tuple(args.hidden), y.shape[1], seed=args.seed)
    history = train(model, x, y, w, args.epochs, args.lr, args.batch, args.l2, args.seed)
    print("loss: train %.5f -> %.5f, val %.5f -> %.5f" % (
        history["train"][0], history["train"][-1],
        history["val"][0] if history["val"] else float("nan"),
        history["val"][-1] if history["val"] else float("nan")))

    if x.shape[1] != len(OBS_NAMES) or y.shape[1] != len(ACT_NAMES):
        print(f"refusing to export: shape {x.shape[1]}->{y.shape[1]} does not match the DLL's "
              f"{len(OBS_NAMES)}->{len(ACT_NAMES)}; a mismatch is rejected by policy.cpp",
              file=sys.stderr)
        return 2

    model.save(args.out)
    reloaded = MLP.from_text(open(args.out, encoding="utf-8").read())
    probe = x[:16]
    delta = float(np.max(np.abs(model.predict(probe) - reloaded.predict(probe))))
    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes); reload check max |delta| = {delta:.2e}")
    if delta > 1e-6:
        print("round-trip mismatch: the exported file does not reproduce the trained network",
              file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
