*This project was created with the help of Claude Opus 5.5 (Anthropic).*

# WarbandVR

*[Version française](README.fr.md)*

Engine-level virtual reality mod for **Mount & Blade: Warband**. It runs on 64-bit WSE2
(`mb_warband_wse2_x64.exe`) and makes **any module** playable in VR (Native, Viking Conquest,
Napoleonic Wars, community mods…). VR never goes through a module's files: a DLL is injected
into the game at launch. No game or WSE2 file is modified.

- Stereo rendering, head and hand tracking, sound in the headset.
- Physical combat with the controllers: gesture attacks, always-active blade and shield,
  two-handed archery.
- Menus, world map, inventory and dialogues on a virtual screen, with a laser pointer.
- Troop orders through a radial menu, for every module.
- OpenXR: any headset whose OpenXR runtime is active (Meta, SteamVR…).

> **Designed for single player.** Using it in public multiplayer games is not recommended: it
> has only been tested in single player.

## Contents
1. [Requirements](#requirements)
2. [Installation](#installation)
3. [Launching the game in VR](#launching-the-game-in-vr)
4. [Configuration](#configuration)
5. [Controls](#controls)
6. [Features](#features)
7. [Known limitations](#known-limitations)
8. [Troubleshooting](#troubleshooting)
9. [Building from source](#building-from-source)
10. [License and credits](#license-and-credits)

## Requirements

- Windows 10/11, 64-bit.
- Mount & Blade: Warband with **WSE2** (`mb_warband_wse2_x64.exe` in the game folder).
- A VR headset and its **OpenXR** runtime:
  - **Meta Quest** (Quest Link or Air Link): Meta Horizon / Oculus app, Meta OpenXR runtime
    active;
  - **Steam Frame, Valve Index, HTC Vive…**: SteamVR.
- In the game options, **attack direction: "by mouse movement"**. The mod picks the
  direction of attacks by simulating a mouse movement.

Tested headsets: **Meta Quest 2** (Quest Link, 120 Hz) and **Steam Frame** (SteamVR, 90 Hz).
Other OpenXR headsets should work, since everything headset-specific (resolution, field of
view, eye distance, refresh rate, audio) is read from the runtime. They are not tested.

## Installation

Download `WarbandVR-vX.Y.Z.zip` from the [Releases](../../releases) page and extract it.
Keep these three files **in the same folder**, anywhere (the game folder is not required):

| File | Role |
|---|---|
| `warband_vr_injector.exe` | Launcher: starts the game and injects the DLL |
| `warband_vr.dll` | The mod |
| `warband_vr_config.exe` | Configuration tool (window) |

The settings file `warband_vr.ini` is created in that folder at first launch, with the
default values. The log `warband_vr.log` is written there at every session.

## Launching the game in VR

1. Plug in and turn on the headset, then start its software (Quest Link or SteamVR). For a
   wireless headset (Steam Frame…), connect it to the PC.
2. Run **`warband_vr_injector.exe`**. It starts the game with the module chosen in the
   configuration tool (Native by default) and injects the mod.
3. Put the headset on: the menus appear on a virtual screen in front of you.

The mod waits for the headset if it is not ready yet, so the order does not matter.

### Launcher options

```
warband_vr_injector.exe [--game-dir <folder>] [--steamvr | --runtime <json> | --default-runtime]
                        [-- <game arguments>]
```

| Option | Effect |
|---|---|
| `--game-dir <folder>` | Game folder, if not the default Steam one (`C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband`) |
| `--steamvr` | Uses SteamVR for this launch, without changing Windows' OpenXR runtime |
| `--runtime <json>` | Uses another runtime's OpenXR manifest |
| `--default-runtime` | Forces Windows' OpenXR runtime |
| `-- <arguments>` | Arguments passed to the game; default `--module <Module setting> --no-intro` |

Without these options, the launcher follows the configuration tool: **"Game folder"**,
**"Module"** and **"VR runtime (OpenXR)"**.

### Playing another module

Pick it in the configuration tool (**Launch → Module**, a list of the modules installed in
the game's `Modules` folder), save, and run the launcher. **Steam Workshop** modules you are
subscribed to also appear in the list, as `Title (Steam Workshop)`; the tool asks Steam for
their titles (internet needed), and Steam must be running when you play them. From the
command line:

```
warband_vr_injector.exe -- --module "Viking Conquest" --no-intro
```

## Configuration

Run **`warband_vr_config.exe`**. It is in English by default; French can be selected at the
top of the window. Every setting is explained in the window. The defaults are the values
validated in the headset, and the *Defaults* button restores them. The game reloads the
settings **live** (about 1 s): no restart needed, except for the VR runtime.

| Group | Setting | Default |
|---|---|---|
| Launch | Game folder | `C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband` |
| | **Module** (list of the installed modules) | Native |
| Display | Roll mode (head tilt) | Inverted compositor |
| | Virtual screen: distance / width | 2 m / 2.4 m |
| | HUD: distance / width | 4 m / 46.9° |
| | Game window at headset resolution | Yes |
| | **VR runtime (OpenXR)**: Windows default / SteamVR | Windows default |
| Controls | Stick turning (degrees/s) | 143 |
| | **Stick turning mode**: smooth / snap | Smooth |
| | Snap turning: step | 30° |
| | Turning stick deadzone | 0.2 |
| | Jump threshold (right stick up) | 0.7 |
| | Crosshair source at start: gaze / left hand / right hand | Gaze |
| Combat | Gesture attacks | Yes |
| | Gesture: trigger speed / measuring time / minimal movement | 1 m/s / 50 ms / 5 cm |
| | Thrust: forward dominance | 1.5 |
| | Melee weapon always active | Yes |
| | Active weapon: start / end speed | 1.5 / 0.8 m/s |
| | Power per m/s of hand / maximum power | 1 / 4 |
| | Shield always active / protection radius | Yes / 35 cm |
| | Bow: full draw (distance between hands) | 0.5 m |
| | Bow: maximum precision | 110 % |
| Advanced | Aim: servo gain | 0.5 |

## Controls

Controls are given for **Meta Touch** controllers. Valve Index, HTC Vive and Windows Mixed
Reality controllers have equivalent bindings. SteamVR presents the Steam Frame controllers as
Touch controllers, and also lets you remap the buttons.

### Menus, world map, inventory (virtual screen)

| Button | Action |
|---|---|
| Trigger (either hand) | Left click; the hand that clicks becomes the pointing hand |
| Y (left) / B (right) | Right click |
| Stick up / down | Mouse wheel (scrolling, map zoom) |
| Stick left / right | Rotate the world map camera |
| Grip (either hand) | Hold Ctrl (Ctrl+click in the inventory) |
| Menu (left), short press | Escape |
| Menu (left), long press (~1 s) | **Recenter the view** |
| X | Closes the order map |

When the game asks for text (character name, for instance), a **virtual keyboard** appears
under the screen, in your Windows keyboard layout.

### Combat, towns, walking

| Left hand | Action |
|---|---|
| Stick | Move |
| Stick click | Crosshair source (gaze / left hand / right hand); in the radial menu: back |
| Trigger | Block |
| Grip | Use (talk, open, pick up…) |
| X | **Radial order menu** |
| Y | Tab menu (leave the battle…) |
| Menu | Short press: Escape; long press: recenter |

| Right hand | Action |
|---|---|
| Stick left / right | Turn the body (smooth or snap) |
| Stick up | Jump |
| Stick click | First / third person view |
| A | Next weapon |
| B | Equip or put away the shield |
| Trigger | Draw the bow / shoot (ranged weapons) |
| Grip | Bow and ranged weapons: same as the trigger; other weapons: fallback attack |
| **Gesture** | Melee attacks: left swing, right swing, overhead, thrust |

### Attacking with each weapon

| Weapon | How |
|---|---|
| Melee (sword, mace, spear…) | A quick right-hand gesture attacks in its direction. The blade is also **always active**: it hits as soon as it goes through an enemy fast enough, with a power that depends on the speed |
| Shield | Follows the left hand; it blocks whatever touches it, without holding block |
| Bow | Hold the right trigger or grip to draw, release to shoot. The arrow follows the axis of your **two hands**; the further apart they are, the more accurate the shot |
| Crossbow, pistol, musket, throwing weapons | Hold the right trigger or grip to aim, release to shoot or throw. The crosshair automatically moves to the right hand. No gesture firing |

### Radial order menu (X)

1. **X** opens a disc in front of your eyes; press it again to close it.
2. **Push the left stick** towards an entry, then **release it** to confirm. **Stick
   click**: previous level.
3. First level: *Everyone*, *Infantry*, *Archers*, *Cavalry*, *Order map*.
4. Next levels: **the game's own orders** (movement, formation, firing…), with its labels, in
   its language and for any module.
5. **Hold this position**: the disc closes, the game shows its flag where you look, and **X**
   confirms the position.

The *order map* (strategic view) only exists if the module provides it: in Native, in the
campaign, not in custom battles.

### Debug keys (keyboard)

| Key | Effect |
|---|---|
| F9 | Cycles the roll mode (for the session) |
| F10 | Toggles the always-active melee weapon |
| F11 | Screenshot (scene and interface) as PNG next to the DLL |

## Features

- **Stereo rendering**: each eye has its own camera (headset position and field of view),
  with alternate-eye rendering. The game frame rate locks to the headset: two frames per
  refresh, one per eye (180 frames/s at 90 Hz). If the PC cannot keep up, it falls back to
  one frame per refresh.
- **Resolution**: the game window takes the resolution recommended by the headset.
- **HUD** (health, ammo, names) on a head-locked plane, readable and without double vision.
- **Menus**: fixed virtual screen, laser pointer, controllers shown in 3D, virtual keyboard.
- **VR body**: in first person (the default view at the start of every mission), the
  character's arms follow your hands. The held weapon follows the controller's grip; ranged
  weapons take the game's pose while aiming, shooting and reloading.
- **Physical combat**: always-active weapon and shield, gesture attacks, power from speed,
  servo aiming (gaze, left hand or right hand).
- **VR archery**: aim along the axis of both hands, accuracy from the draw length.
- **Orders**: universal radial menu, "hold this position" with the flag.
- **Sound**: sent to the headset's audio output, given by the runtime (Meta) or Windows'
  default output (SteamVR).
- **Comfort**: recentering, smooth or snap turning.
- **Robustness**: no hard-coded memory address, everything is found by signature. If a WSE2
  update breaks a signature, the related feature is disabled and the log says so, without
  crashing the game.

## Known limitations

- **Two-handed weapons, polearms**: wielded with gestures, without a realistic two-handed
  grip.
- **Alternate-eye rendering**: each eye is refreshed every other frame. It is smooth at a
  steady cadence, but less so than true simultaneous rendering of both eyes.
- **Crouching**: not bound (disabled in Native).
- **Order map**: depends on the module (see above). The radial menu works everywhere.
- **Multiplayer**: not supported; not recommended in public multiplayer games.
- **Linux (Proton / Wine)**: **untested**. The mod is built for Windows. Under Proton, the
  uncertain parts are Direct3D 9 → Direct3D 11 frame sharing under DXVK and OpenXR through
  `wineopenxr`. Feedback welcome, with the `warband_vr.log` file.

## Troubleshooting

Everything is recorded in **`warband_vr.log`**, next to the DLL. Lines are prefixed by
module (`[xr]`, `[render]`, `[engine]`, `[input]`).

| Problem | What to check |
|---|---|
| No image in the headset | Is the headset software running? The log says "runtime OpenXR pas prêt" until the headset is connected |
| The game starts on the wrong headset / runtime | "VR runtime (OpenXR)" setting, or the `--steamvr` / `--default-runtime` options |
| No sound in the headset | `son :` lines in the log: list of FMOD outputs and the selected one |
| Blurry image when turning the head | The log should say "casque à N Hz : le jeu vise 2N images/s". If the game cannot keep up, lower the graphics options |
| Attacks go in the wrong direction | Game option "attack direction: by mouse movement"; *Gesture* settings |
| A feature stopped working after a WSE2 update | Look for "signature" in the log |

The log is written in French.

## Building from source

You need Visual Studio 2022 (MSVC, C++20), CMake and vcpkg. Dependencies (MinHook, OpenXR
loader) use manifest mode with the `x64-windows-static` triplet: the DLL depends on no
third-party DLL.

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The three files are produced in `build/Release/`.

Code layout:

| Folder | Contents |
|---|---|
| `injector/` | 64-bit launcher (CreateProcess + LoadLibrary injection) |
| `src/render/` | Direct3D 9Ex hooks (PresentEx, render targets), frame sharing to D3D11, game window |
| `src/xr/` | OpenXR session, controllers, controller rendering, virtual keyboard, radial menu |
| `src/engine/` | Game engine: stereo camera, skeleton (VR arms), combat, orders, text input, sound, frame pacing |
| `src/input/` | DirectInput injection (keyboard, mouse), menu pointer |
| `src/util/` | Log, signature scanning, settings |
| `config_tool/` | Configuration tool (Win32) |
| `tools/` | Analysis scripts (signatures, references) |

## License and credits

WarbandVR is released under the [MIT license](LICENSE).

- Third-party libraries linked into the binaries: **MinHook** (BSD 2-Clause), **OpenXR SDK
  loader** (Apache 2.0), **JsonCpp** (MIT) — see `THIRD_PARTY_NOTICES.txt` in the release.
- Architecture inspired by **WSE** (Warband Script Enhancer, WTFPL); WarbandVR runs on top of
  **WSE2**, which it does not modify.
- Mount & Blade: Warband is a game by TaleWorlds Entertainment. This project is not
  affiliated with or endorsed by TaleWorlds or the WSE2 authors.
