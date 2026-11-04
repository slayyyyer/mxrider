# MXRider

An offline AI rider for **MX Simulator**. It reads bike state through a cosmetic `opengl32.dll`
proxy, decides what to press, and drives the game with a **virtual Xbox 360 pad**. Single-player,
local, no network.

It is a fork of [Voaks/MXCam](https://github.com/Voaks/MXCam) (MIT) with the helmet camera removed:
what remains of the original is the export-stub proxy, the per-draw uniform capture, and the bike
tracker. Everything about driving, logging and learning is new.

## Hard rules, and how they are enforced

| Rule | Where it is enforced |
| --- | --- |
| Read-only hook; never alters physics or game data | `uniforms.cpp` observes `glUniformMatrix4fv` and passes the game's own pointer to the real driver unchanged. There is no matrix correction, no shader swap, no memory write, no input hook. The only things this DLL changes are its own log files and the virtual pad. |
| Never crash the game; on failure do nothing and log why | Every subsystem has one failure answer: release the controls. Missing `ViGEmClient.dll`, no ViGEmBus driver, bike not tracked, no line, missing/corrupt weights, NaN in a pose — all log once and stop driving. The policy loader validates every shape and every number before use. |

## What is verified, and what needs the game

Verified on this machine (Windows, VS2019 Build Tools, x64):

* the proxy builds and exports a complete opengl32 surface — 368 exports forwarded, 4 hooked, 0 missing;
* loading it into a bare process runs `DllMain`, finds the system driver, reads `mxrider.ini` and logs;
* the Python trainer: load → dataset → train → export → re-import, with rejection of truncated and
  non-finite weight files (`train/selftest.py`);
* the **weights contract**: a file exported by the trainer evaluated by the *real C++* `Policy`
  class matches numpy to ~1e-7 (`tests/run_policy_test.bat` + `tests/parity.py`).

Not verified here, because it needs the game running — these are the first things to check in-game:

* that `u_modelview` / `u_heights` still carry the meaning the reference mod relied on (the log
  prints every uniform the game looks up, so a rename is obvious);
* that the bike locks on (log: `bike: locked on an object of program …`);
* that the game sees the virtual pad, and that `SteerSign` / `LeanSign` have the right polarity;
* that laps count (signalled by `line: lap N completed`).

## Install

Copy these next to `mx.exe` (normally `C:\Program Files (x86)\MX Simulator\`):

```
opengl32.dll        the mod (the game loads it instead of the system one)
ViGEmClient.dll     the virtual pad's client library
mxrider.ini         settings
```

`build.bat` stages all three into `bin\x64\Release\`. The game folder needs administrator rights to
write to; run your editor or copy step elevated once. If `ViGEmBus` is not installed, the mod still
loads and logs but will not drive.

Uninstall by deleting those three files (`opengl32.dll` is the only one that must go).

## Controls

| Key | Action |
| --- | --- |
| F7 | rider on / off |
| F6 | cycle mode: off → baseline → record-line → policy |
| F5 | start / stop recording the racing line |
| F9 | reload `mxrider.ini`, the line and the weights |

The virtual pad presents a plain Xbox 360 layout. Bind the game's controller settings to match, or
change the `[Pad]` section to match what the game already expects:

| Bike input | Virtual pad | ini key |
| --- | --- | --- |
| throttle | right trigger | — (always RT) |
| front brake | left trigger | `FrontBrakeTrigger=0` (`1` swaps the triggers) |
| rear brake | B | `RearBrakeButton=0x2000` |
| clutch | A | `ClutchButton=0x1000` |
| steering | left stick X | — |
| lean forward / back | left stick Y | — |

**One pad at a time.** The game reads every attached controller, so unplug a real pad while the
rider drives, or it will fight the rider for control. The mod does not hide real devices.

## Workflow

1. **Record the line.** In a race, stand on the track and press F6 twice (to `record-line`), ride one
   clean lap, then F5. The centreline is written to `mxrider_line.csv`. This is what makes laps
   measurable and what the baseline steers by.
2. **Baseline.** F6 → `baseline`. Pure pursuit along the line plus a speed hold. It is deliberately
   simple: it establishes a reward and lap-time baseline, and it exercises the whole loop
   (state → action → pad → state) without any learning.
3. **Human logging.** While in `record-line` the mod also reads the *real* controller and logs it
   next to the bike state. That is the imitation data the first policy is trained from.
4. **Train.** From `train/`:

   ```bat
   py -3 mxrider_learn.py --data "..\bin\x64\Release" --out mxrider_policy.txt --epochs 40
   ```

   It reports episode returns and frame counts by source, trains, and refuses to export a shape the
   DLL would reject. Drop `mxrider_policy.txt` next to the DLL and press F9.
5. **Evaluate.** F6 → `policy`. Each run writes `mxrider_run_YYYYMMDD_HHMMSS.csv`. Move the logs into
   one folder, retrain, and compare lap counts and returns in `mxrider.log`:
   `episode N ended (crash): 812 frames, 41.2 s, return 18.7, laps 1`.

The training loop is **behavioural cloning from human laps, then reward-weighted refits on the
rider's own episodes**: rows from episodes that beat the mean return are up-weighted, worse episodes
are dropped. That is a cheap offline stand-in for a policy gradient that needs no live rollouts, and
it is honest about what it is — a small supervised model, not deep RL.

## Observation and action

16 observations, all derived from the pose (the hook cannot see throttle, suspension, grip, engine
state or crash flags):

| # | Observation | Scale |
| --- | --- | --- |
| 0–4 | forward speed, speed, velocity x/y/z | ÷ `RefSpeed` |
| 5 | vertical acceleration | g |
| 6–7 | pitch (nose up +), roll (leaning right +) | ÷ 45° |
| 8 | yaw rate | ÷ 180°/s |
| 9 | airborne estimate | 0/1 |
| 10–14 | previous throttle, front brake, rear brake, steer, lean | as sent |
| 15 | progress along the line | 0..1 |

6 actions: throttle, front brake, rear brake, steer, lean, clutch. The network output is `tanh`, so
every channel is inside [-1, 1]; pedals use the positive half and a zero output is "hands off".

These two layouts must stay in step between `src/rider.cpp` and `train/mxrider_learn.py`; the DLL
refuses weights whose observation width doesn't match its own.

## Files

| Path | What it is |
| --- | --- |
| `mxrider.log` | the only diagnostic. Rewritten each launch. |
| `mxrider_run_*.csv` | one per session: state, action, reward, episode bookkeeping. |
| `mxrider_line.csv` | the recorded racing line (`x,y,z`). |
| `mxrider_policy.txt` | exported weights (plain text, diffable). |

## Source map

| File | Role |
| --- | --- |
| `src/proxy.cpp` | export stubs, loads the real driver, patches 368 jumps |
| `src/hooks.cpp` | frame boundary (swap / clear), hotkeys |
| `src/uniforms.cpp` | observes `u_modelview` uploads; finds the camera view; hands objects to the tracker |
| `src/bike.cpp` | tracks the bike against the sprung camera, world pose, least-squares velocity / acceleration |
| `src/line.cpp` | the recorded line, projection, lookahead, lap wrap |
| `src/rider.cpp` | modes, baseline controller, policy evaluation, episodes and reward |
| `src/pad.cpp` | ViGEm virtual pad (runtime-loaded) and the read-only human pad |
| `src/policy.cpp` | the network loader / evaluator |
| `src/telemetry.cpp` | state derivation and the CSV log |
| `train/` | numpy trainer, self-test, and the C++ ↔ Python parity check |

## Limitations

* **Units are not metres.** The camera moves about 65 units/s at a displayed ~39. `RefSpeed` and
  `TargetSpeed` are in game units; treat speeds as relative, not physical.
* **Laps are line progress, not the game's timing.** Lap detection is a wrap of the projection onto
  the recorded line. The GL hook cannot see the game's lap timer. A line recorded over more than one
  lap, or a lap that cuts the line, will mis-count.
* **No crash flag exists.** Crashes are inferred (bike on its side for 0.5 s, or a respawn-sized
  jump). A crash that leaves the bike upright and moving will look like a bad lap, not a crash.
* **The bike must be tracked.** Everything depends on the tracker locking onto your own bike; if the
  log says `LOOKING FOR THE BIKE`, the rider does nothing. `BikeSearchRadius` and the uniform names
  in `[Detection]` are the knobs.
* **One process, one pad.** The virtual pad is per-machine, not per-process: a second game instance
  or another ViGEm user will see it too.
* **No track geometry.** The rider knows the line it was shown and the bike's motion, nothing else.
  It cannot plan around obstacles, sight a jump it was not shown, or recover from leaving the line
  by more than a few units.

## Credits

* Proxy technique, per-draw `u_modelview` capture and bike tracking: [Voaks/MXCam](https://github.com/Voaks/MXCam) (MIT).
* Virtual pad: [ViGEm](https://github.com/nefarius/ViGEmClient) (MIT), loaded at runtime.
* Shader/uniform behaviour cross-checked against [JSC500/mxs-shaders](https://github.com/JSC500/mxs-shaders) (MIT).

MX Simulator itself is not modified, decompiled, or redistributed by this project.
