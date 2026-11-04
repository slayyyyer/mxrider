# MXRider

An offline AI rider for **MX Simulator**. It reads the bike's state from inside the game's own
rendering calls, decides what to press, and drives the game through a **virtual Xbox 360 pad**.
Single-player, local, no network, no training during play.

The game is never modified: the DLL only observes, and every failure ends with the controls released.

```
you/recorded lap ──► opengl32.dll ──► bike state ──► rider ──► virtual pad ──► MX Simulator
       ▲                                                                         │
       └──────────────────────── mxrider_run_*.csv  ◄───────────────────────────┘
```

## What you need

* **MX Simulator**, 64-bit (that is the only build that exists; a 32-bit proxy would be ignored).
* **ViGEmBus driver** — <https://github.com/nefarius/ViGEmBus/releases>. This is what creates the
  virtual pad. Without it the mod still loads, logs `pad: ViGEm unavailable`, and never drives.
* Windows 10/11.

## Install

Grab the three files from [`dist/`](dist) and copy them next to `mx.exe` — normally
`C:\Program Files (x86)\MX Simulator\`. That folder needs administrator rights, so copy as admin.

| File | What it is |
| --- | --- |
| `opengl32.dll` | the mod; the game loads this instead of the system OpenGL DLL |
| `ViGEmClient.dll` | the virtual pad's client library, loaded by the mod at runtime |
| `mxrider.ini` | settings |

That is the whole install. No patcher, no launcher, no game files changed.

**Check it loaded.** Start the game, then open `%LOCALAPPDATA%\MXRider\mxrider.log`. You want:

```
config: loaded ...\mxrider.ini (mode=baseline, ...)
rider: ready (mode=baseline, enabled=0, line=..., pad=VIRTUAL PAD)
```

If it says `pad: none`, ViGEmBus is missing or the driver is not running.

**Run the game before the mod can see a controller.** MX Simulator enumerates pads once, at startup,
and never rescans. The mod creates the virtual pad from `DllMain` for that reason — so the pad exists
before the game looks for it. If you start the game *without* ViGEmBus and plug the driver in later,
restart the game.

## Bind the controls

MX Simulator binds controls **per device name**, so the virtual pad needs its own entries. It appears
as `XInputGamepad0`. In the game's controller settings, bind:

| Bike input | Virtual pad |
| --- | --- |
| throttle | right trigger |
| front brake | left trigger |
| rear brake | B |
| clutch | A |
| steering | left stick X |
| lean forward/back | left stick Y (always centred — see `LeanGain`) |

The game writes those bindings to `%LOCALAPPDATA%\MX Simulator\config`. If your own controller uses a
different slot name, bind both — the game remembers one set per device.

## Drive

1. Load a race or free ride, and sit on the bike at the start.
2. **Record the line** — this is what the rider follows. Press **F5**, ride one clean lap yourself,
   press **F5** again. It writes `mxrider_line.csv` (only the last recorded lap is kept).
3. Press **F7**. The rider takes over: pure-pursuit steering onto the line plus a speed hold.
4. Press **F7** again to hand it back. It releases every control when disabled, so the bike rolls to a
   stop rather than being driven into a wall.

| Key | Action |
| --- | --- |
| **F5** | start / stop recording the racing line |
| **F6** | cycle mode: off → baseline → record-line → policy |
| **F7** | rider on / off |
| **F9** | reload `mxrider.ini`, the line and the policy (does **not** reload the pad) |

## Settings

Everything lives in `mxrider.ini`, next to the DLL. **The file is read at startup only** — F9 reloads
the line and policy but not the settings, so a change means restarting the game.

The values that matter when it is not driving well:

| Key | Meaning | Shipping value |
| --- | --- | --- |
| `TargetSpeed` | speed the throttle/brake loop holds, game units/s | `27.0` |
| `SpeedGain` | throttle/brake per unit of speed error | `0.30` |
| `Lookahead` | how far up the line the rider aims | `28.0` |
| `SteerGain` | steering per radian of heading error | `0.30` |
| `CrossGain` | steering per unit of sideways offset from the line | `0.04` |
| `YawDamp` | counter-steer per rad/s of yaw — this is the anti-slide term | `0.15` |
| `LeanGain` | rider weight shift; `0` keeps the rider from dabbing and sitting | `0.00` |
| `ResumeUpright` | how upright the bike must be before the rider drives again after a crash | `0.90` |
| `ResumeSettle` | how long it must stay that upright, seconds | `0.30` |
| `BikeSearchRadius` | how far from the camera the tracker looks for the bike | `20.0` |

Signs are in `[General]`/`[Pad]`: `SteerSign` flips the steering polarity, `FrontBrakeTrigger=1` swaps
the triggers, and the button bits in `[Pad]` change which button is the rear brake.

### It is sliding

Lower `TargetSpeed` first — grip is the limit, not the controller. Then raise `YawDamp` (0.25 arrests a
slide harder; 0.08 is looser if the bike fights its own turn-in). If the bike is dabbing and sitting,
keep `LeanGain` at `0`.

### It is not driving at all

Read `mxrider.log` first — it names the failure:

| Log line | Meaning |
| --- | --- |
| `LOOKING FOR THE BIKE` | the tracker has not locked on; check `BikeSearchRadius` and the uniform names in `[Detection]` |
| `pad: none` / `pad: ViGEm unavailable` | ViGEmBus is not installed or not running |
| `line: no line loaded` | no `mxrider_line.csv`; record one with F5 |
| `rider: waiting` (repeated) | it crashed and is waiting for the bike to be upright — reset with the game's own reset key |

## What it writes

| Path | What it is |
| --- | --- |
| `%LOCALAPPDATA%\MXRider\mxrider.log` | the only diagnostic; rewritten each launch |
| `%LOCALAPPDATA%\MXRider\mxrider_line.csv` | the recorded line, `x,y,z` per point |
| `%LOCALAPPDATA%\MXRider\mxrider_run_*.csv` | one per session: state, action, reward, episode bookkeeping |
| `%LOCALAPPDATA%\MXRider\mxrider_policy.txt` | exported weights, if you train one (plain text, diffable) |

## Uninstall

Delete `opengl32.dll` from the game folder. That alone restores the stock game — it loads the system
OpenGL DLL again. `ViGEmClient.dll` and `mxrider.ini` are inert without it; the log folder in
`%LOCALAPPDATA%\MXRider` is yours to delete.

## Build from source

Visual Studio 2019 Build Tools (or 2022, passing `v143`) with the C++ x64 toolset:

```bat
build.bat
```

It stages `opengl32.dll` and `ViGEmClient.dll` into `bin\x64\Release\`. The full build instructions,
the design notes, the observation/action layout and the trainer are in
[`mxrider/README.md`](mxrider/README.md).

`build.bat` expects the ViGEm client checkout at `reference/vigem`; see that README for the layout.

## Limits worth knowing

* **Units are not metres.** The rider's speeds are game units; treat `TargetSpeed` as relative.
* **Laps are line progress**, not the game's timer — the hook cannot see the timing gates.
* **No crash flag exists.** A crash is inferred (bike on its side, or a respawn-sized jump), so a fall
  that leaves the bike upright just looks like a bad lap.
* **It follows one recorded line.** No track geometry, no planning around obstacles or jumps.
* **It is a learner's baseline**, not a fast rider: it holds a line at a modest pace. It will not beat
  you, and on tight or rough sections it may need a lower `TargetSpeed`.

## Credits

Proxy technique and bike tracking follow [Voaks/MXCam](https://github.com/Voaks/MXCam) (MIT). The
virtual pad is [ViGEm](https://github.com/nefarius) (MIT), loaded at runtime. Uniform behaviour was
cross-checked against [JSC500/mxs-shaders](https://github.com/JSC500/mxs-shaders) (MIT).

MX Simulator is not modified, decompiled or redistributed by this project.
