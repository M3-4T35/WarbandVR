*This project was created with the help of Claude Opus 5.5 (Anthropic).*

# WarbandVR

*[Version française](README.fr.md)*

Engine-level virtual reality mod for **Mount & Blade: Warband**. It runs on 64-bit WSE2
(`mb_warband_wse2_x64.exe`) and makes **any module** playable in VR (Native, Viking Conquest,
Napoleonic Wars, community and Steam Workshop mods…). VR never goes through a module's files:
a DLL is injected into the game at launch. No game or WSE2 file is modified.

- Stereo rendering (alternate-eye or **true stereo**), head and hand tracking, sound in the
  headset.
- Physical combat with the controllers: gesture attacks, always-active blade and shield,
  two-handed grip for two-handed weapons and polearms, two-handed archery.
- **Immersive campaign map** in 3D around you; menus, inventory and dialogues on a virtual
  screen, with a laser pointer.
- Also playable with **mouse and keyboard** (headset only, no controllers).
- Troop orders through a radial menu, for every module.
- **Launcher**: module, game language, presets and every setting (VR and Warband graphics)
  before each game.
- OpenXR: any headset whose OpenXR runtime is active (Meta, SteamVR…), on Windows or on
  Linux / SteamOS with Proton (including the **standalone Steam Frame**).

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

- **Windows 10/11, 64-bit**, or **Linux / SteamOS** with Proton (Windows version of the game).
- Mount & Blade: Warband with **WSE2** (`mb_warband_wse2_x64.exe` in the game folder).
- A VR headset and its **OpenXR** runtime:
  - **Meta Quest** (Quest Link or Air Link): Meta Horizon / Oculus app, Meta OpenXR runtime
    active;
  - **Steam Frame, Valve Index, HTC Vive…**: SteamVR.
- In the game options, **attack direction: "by mouse movement"**. The mod picks the
  direction of attacks by simulating a mouse movement.

Tested headsets:

| Headset | Machine | Refresh rate |
|---|---|---|
| Meta Quest 2 | Windows PC (Quest Link) | 120 Hz |
| Steam Frame, streamed | Windows PC (SteamVR) | 90 Hz |
| Steam Frame, **standalone** | The headset itself (SteamOS ARM64, Proton 11) | 72 Hz |

Other OpenXR headsets should work, since everything headset-specific (resolution, field of
view, eye distance, refresh rate, audio) is read from the runtime. They are not tested.

## Installation

First install **WSE2** (Warband Script Enhancer 2) in the game folder, following its own
instructions: `mb_warband_wse2_x64.exe` must be next to `mb_warband.exe`. Check that the game
starts with WSE2 before adding WarbandVR.

Then download `WarbandVR-vX.Y.Z.zip` from the [Releases](../../releases) page and extract it. It
contains three files to keep **in the same folder**:

| File | Role |
|---|---|
| `warband_vr_injector.exe` | Starts the game (after the launcher) and injects the DLL |
| `warband_vr.dll` | The mod |
| `warband_vr_config.exe` | Launcher and configuration tool (window) |

The settings file `warband_vr.ini` is created in that folder at first launch, with the
default values. The log `warband_vr.log` is written at every session in the `warbandVRlog` folder, next to the DLL.

### Windows

**Recommended:** extract the three files **into the game folder**, next to
`mb_warband_wse2_x64.exe` (by default
`C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband`). The game is then found
automatically. Only new files are added: no game or WSE2 file is replaced.

They can also stay in any other folder: the game folder is then set in the launcher, **Launch**
tab.

At first launch, pick the **PCVR** preset in the launcher, and the **VR runtime**: *Windows
default* for a Meta Quest through Quest Link / Air Link (with the Meta runtime active),
*SteamVR* for a headset using SteamVR (streamed Steam Frame, Index, Vive…).

### Linux / SteamOS (Proton), standalone Steam Frame

1. In Steam, Warband properties → **Compatibility**: force Proton (Proton 11 ARM64 on the
   Steam Frame). The native Linux version of the game has no WSE2: the Windows version is
   needed.
2. Install WSE2 in the game folder, as on Windows.
3. Copy the three WarbandVR files **into the game folder**
   (`~/.local/share/Steam/steamapps/common/MountBlade Warband`).
4. Also copy `d3dx9_43.dll` and `D3DCompiler_43.dll` from a Windows installation (DirectX)
   into the game folder. Without them, WSE2 crashes while compiling its post effects under
   Proton.
5. Steam launch options of the game:
   ```
   export WINEDLLOVERRIDES="d3dx9_43,d3dcompiler_43=n,b"; eval $(echo "%command%" | sed "s|mb_warband.exe|warband_vr_injector.exe|")
   ```
6. At first launch, pick the **Steam Frame (standalone)** preset in the launcher.

The OpenXR runtime is Steam's (SteamVR through `wineopenxr`): keep the "VR runtime (OpenXR)"
setting on "Windows default".

## Launching the game in VR

1. Plug in and turn on the headset, then start its software (Quest Link or SteamVR). For a
   wireless headset (streamed Steam Frame…), connect it to the PC. Standalone, there is
   nothing to do.
2. Run **`warband_vr_injector.exe`** (under Proton: start the game from Steam). The
   **launcher** opens: pick the module, the game language, a preset, adjust the settings if
   needed, then click **PLAY**. *Quit* cancels the launch.
3. Put the headset on: the menus appear on a virtual screen in front of you.

The mod waits for the headset if it is not ready yet, so the order does not matter. To start
without the launcher, untick **"Show the launcher at startup"** (Launch tab).

### Injector options

```
warband_vr_injector.exe [--game-dir <folder>] [--steamvr | --runtime <json> | --default-runtime]
                        [--launcher | --no-launcher] [-- <game arguments>]
```

| Option | Effect |
|---|---|
| `--game-dir <folder>` | Game folder. Default: the injector's folder if it contains the game, otherwise the "Game folder" setting |
| `--steamvr` | Uses SteamVR for this launch, without changing Windows' OpenXR runtime |
| `--runtime <json>` | Uses another runtime's OpenXR manifest |
| `--default-runtime` | Forces Windows' OpenXR runtime |
| `--launcher` / `--no-launcher` | Opens (or not) the launcher, whatever the setting |
| `-- <arguments>` | Arguments passed to the game (no launcher); default `--module <Module setting> --no-intro` |

### Playing another module

Pick it in the launcher (**Launch → Module**): the list holds the modules of the game's
`Modules` folder and the **Steam Workshop** modules you are subscribed to, as
`Title (Steam Workshop)`. The launcher asks Steam for their titles (internet needed), and Steam
must be running when you play them. From the command line:

```
warband_vr_injector.exe -- --module "Viking Conquest" --no-intro
```

## Configuration

The launcher (opened at startup) and **`warband_vr_config.exe`** (run on its own, with a
*Save* button instead of *PLAY*) are the same window. It is drawn by the tool itself, large
and dark-themed, to stay readable in a headset: **− / +** change the size of the interface,
the language button switches between English and French. Lists open inside the window; point
at a setting to see its explanation at the bottom.

The game reloads the VR settings **live** (about 1 s): no restart needed, except for the VR
runtime. Warband graphics and the game language apply at the next launch.

### Presets

| Preset | Contents |
|---|---|
| **PCVR (default)** | VR settings validated on the Quest 2, true stereo; Warband graphics on **Ultra**, without depth of field blur |
| **Steam Frame (standalone)** | True stereo, render scale 0.85; Warband graphics on **Low**, with maximum blood (72 image pairs per second validated in the headset) |
| Your own | *Save as…* stores them in `warband_vr_presets\` (next to the `.ini`); *Export…* writes a file anywhere; *Import…* adds one and applies it; *Delete* |

Picking a preset applies it at once in the window; *PLAY* or *Save* writes it. A preset does
not change the game folder, the module, the game language or the VR runtime. The built-in
presets are never applied automatically: you choose.

### Game graphics tab (Warband)

The game's graphics settings, written to its `rgl_config.ini` file
(`Documents\Mount&Blade Warband WSE2`); only the changed values are written. The **overall
quality** Low / Medium / High / Ultra uses exactly the values of the presets of the game's
video options. Then the settings one by one: shaders, textures, filtering, shadows (scene,
characters, vegetation), grass, vegetation, character detail, HDR, exposure, depth of field,
antialiasing, water reflections, particles, dynamic lighting, sun flare, corpses, ragdolls,
blood, weapon trails, battle size (in troops).

In VR every image is drawn twice: lower graphics give a smoother image. Close the game before
changing these settings: it rewrites its file when it quits.

### Game language

Launch tab, below the module: the languages installed in the game's `languages` folder. It is
written to `%APPDATA%\Mount&Blade Warband WSE2\language.txt` and applies at the next launch,
for the modules that provide that language.

### VR settings

| Tab | Setting | Default |
|---|---|---|
| Launch | Game folder | `C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband` (under Proton: the tool's folder) |
| | **Module** (installed and Steam Workshop modules) | Native |
| | Show the launcher at startup | Yes |
| | **VR runtime (OpenXR)**: Windows default / SteamVR | Windows default |
| Display | Roll mode (head tilt) | Inverted compositor |
| | Virtual screen: distance / width | 2 m / 2.4 m |
| | HUD: distance / width | 4 m / 46.9° |
| | Game window at headset resolution | Yes |
| | **Render scale** (resolution per eye / recommended resolution) | 1 |
| | Game frame rate limit (0 = automatic, locked to the headset) | 0 |
| | **Immersive campaign map** / its distance / interface size | Yes / 1 m / 1.5 |
| Controls | **Input**: auto / VR controllers / mouse and keyboard | Auto |
| | Stick turning (degrees/s) | 143 |
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
| | Torso facing forward (melee weapons) | Yes |
| | Bow: full draw (distance between hands) | 0.5 m |
| | Bow: maximum precision | 110 % |
| Advanced | Aim: servo gain | 0.5 |
| | **True stereo** | Yes |
| | Virtual screen drawn with the controllers: auto (SteamVR) / always / never | Auto |
| | Diagnostics: right eye first, left image in both eyes, frame timings in the log | No |

## Controls

Controls are given for **Meta Touch** controllers. Valve Index, HTC Vive and Windows Mixed
Reality controllers have equivalent bindings. SteamVR presents the Steam Frame controllers as
Touch controllers, and also lets you remap the buttons.

### Menus, inventory (virtual screen)

| Button | Action |
|---|---|
| Trigger (either hand) | Left click; the hand that clicks becomes the pointing hand |
| Y (left) / B (right) | Right click |
| Stick up / down | Mouse wheel (scrolling, map zoom) |
| Stick left / right | Rotate the world map camera |
| Grip (either hand) | Hold Ctrl (Ctrl+click in the inventory) |
| X or Menu (left), short press | Escape |
| X or Menu (left), long press (~1 s) | **Recenter the view** |
| Left stick click | Closes the order map |

When the game asks for text (character name, for instance), a **virtual keyboard** appears
under the screen, in your Windows keyboard layout.

### World map (immersive)

The campaign map is shown **in 3D around you**, from the game camera: looking straight ahead
gives the game's usual view, turn your head to look around, lean in to get closer (the map looks
like a scale model; its size follows the game zoom). The map interface (buttons, date, tooltips)
floats in front of you, aligned with the map: **point at a place on the map** and pull the trigger
to travel there, exactly as with the mouse. The same buttons as the virtual screen apply (stick up
/ down = zoom, left / right = rotate the camera). Off in the settings (*Immersive campaign map*):
the map stays on the virtual screen. *Interface size* widens the map camera's field of view: the
interface and the clickable area get bigger and stay aligned with the map.

### Mouse and keyboard (no controllers)

You can play with the headset on and the mouse and keyboard, without controllers (setting
*Input*; in *Auto*, the last device used wins: moving the mouse switches to it, picking up a
controller switches back).

- The game keys work as usual (WASD, Tab, Escape, R, F1-F4, number keys...).
- **The mouse turns the body** (left / right) and picks the **attack direction** as in the normal
  game; the buttons attack, block, draw the bow, shoot.
- **The gaze aims**: the crosshair stays in the centre of your view, also for the bow and the
  crossbow. The vertical aim follows your head a moment after a vertical mouse movement.
- In menus and on the map, the mouse moves a cursor on the virtual screen.
- **Home** recenters the view.

### Combat, towns, walking

| Left hand | Action |
|---|---|
| Stick | Move |
| Stick click | **Radial order menu** |
| Trigger | Block |
| Grip | Use (talk, open, pick up…) |
| X | Short press: Escape; held 1 s: crosshair source (gaze / left hand / right hand); in the radial menu: back |
| Y | Tab menu (leave the battle…) |
| Menu | Short press: Escape; long press: recenter (under SteamVR this button opens the SteamVR dashboard: use X) |

| Right hand | Action |
|---|---|
| Stick left / right | Turn the body (smooth or snap) |
| Stick up | Jump |
| Stick click | Kick (short press); first / third person view (hold 1 s) |
| A | Next weapon (short press); weapon mode (long press: bayonet, one / two hands, couched lance..., the game's X key) |
| B | Equip or put away the shield |
| Trigger | Draw the bow / shoot (ranged weapons); two-handed weapons and polearms: hold to **grip with both hands** |
| Grip | Bow and ranged weapons: same as the trigger; other weapons: fallback attack |
| **Gesture** | Melee attacks: left swing, right swing, overhead, thrust |

### Attacking with each weapon

| Weapon | How |
|---|---|
| Melee (sword, mace, spear…) | A quick right-hand gesture attacks in its direction. The blade is also **always active**: it hits as soon as it goes through an enemy fast enough (once per movement), with a power that depends on the speed of the blade. The kind of blow (thrust, overhead, left or right swing) follows the real movement of the blade |
| Two-handed sword, polearm | Same, plus: **hold the right trigger** to grip with both hands. The weapon then follows the axis of your two controllers (pointing where the right controller points), the left hand lies on the handle |
| Shield | Follows the left hand; it blocks whatever touches it, without holding block |
| Bow | Hold the right trigger or grip to draw, release to shoot. The arrow follows the axis of your **two hands**; the further apart they are, the more accurate the shot |
| Crossbow, pistol, musket, throwing weapons | Hold the right trigger or grip to aim, release to shoot or throw. The crosshair automatically moves to the right hand. No gesture firing |

### Radial order menu (left stick click)

1. **Clicking the left stick** opens a disc in front of your eyes; click again to close it.
2. **Push the left stick** towards an entry, then **release it** to confirm. **X**:
   previous level.
3. First level: *Everyone*, *Infantry*, *Archers*, *Cavalry*, *Order map*.
4. Next levels: **the game's own orders** (movement, formation, firing…), with its labels, in
   its language and for any module.
5. **Hold this position**: the disc closes, the game shows its flag where your left hand points, and a **left
   stick click** confirms the position.

The *order map* (strategic view) only exists if the module provides it: in Native, in the
campaign, not in custom battles.

### Debug keys (keyboard)

| Key | Effect |
|---|---|
| F9 | Cycles the roll mode (for the session) |
| F10 | Toggles the always-active melee weapon |
| F11 | Screenshot of both eyes and the interface as PNG in `warbandVRlog` (freezes the game a few seconds) |

## Features

- **Stereo rendering**, each eye with its own camera (headset position and field of view):
  - **true stereo** (default, *True stereo* setting): both eyes in every game frame. Simulation,
    shadows and interface are computed once per pair: correct depth while moving, and up to
    twice as many images when the game is CPU-limited;
  - **alternate-eye rendering** (*True stereo* off): one eye per game frame, two frames per
    headset refresh, with the same head pose for both eyes of a pair.
- **Frame pacing** locked to the headset: one image pair per refresh; if the machine cannot
  keep up, one pair every 2 or 3 refreshes, with regular attempts to go back up as soon as
  possible. The frame rate limit of the game options is ignored in VR.
- **Resolution**: the game window takes the resolution recommended by the headset, times the
  *render scale*.
- **HUD** (health, ammo, names) on a head-locked plane, readable and without double vision.
- **Menus**: fixed virtual screen, laser pointer, controllers shown in 3D, virtual keyboard.
- **VR body**: in first person (the default view at the start of every mission), the
  character's arms follow your hands. The held weapon follows the controller's grip; ranged
  weapons take the game's pose while aiming, shooting and reloading. With a melee weapon, the
  upper body faces forward instead of the game's side-on stances.
- **Physical combat**: always-active weapon and shield (blow type from the blade's movement,
  one hit per movement), two-handed grip, gesture attacks, power from the blade's speed,
  servo aiming (gaze, left hand or right hand), kick, weapon mode (bayonet…).
- **VR archery**: aim along the axis of both hands, accuracy from the draw length.
- **Orders**: universal radial menu, "hold this position" with the flag.
- **Sound**: sent to the headset's audio output, given by the runtime (Meta) or the default
  output (SteamVR).
- **Comfort**: recentering, smooth or snap turning.
- **Launcher**: module (Steam Workshop included), game language, presets, VR settings and
  Warband graphics.
- **Robustness**: no hard-coded memory address, everything is found by signature. If a WSE2
  update breaks a signature, the related feature is disabled and the log says so, without
  crashing the game.

## Known limitations

- **Two-handed grip**: the left hand is placed on the weapon only while the right trigger is
  held; the game still plays its own one-handed or two-handed animations for gesture attacks.
- **True stereo** (on by default): validated on the standalone Steam Frame and on PC with the
  streamed Steam Frame. If an eye looks wrong, turn it off (alternate-eye rendering).
- **Pair-based pacing and true stereo**: validated on the standalone and streamed Steam Frame;
  not yet revalidated on the Quest 2.
- **Crouching**: not bound (disabled in Native).
- **Order map**: only exists if the module provides it (in Native: in the campaign, not in
  custom battles). The radial menu works everywhere.
- **Linux**: tested on the standalone Steam Frame (SteamOS ARM64, Proton 11). Other Linux PCs
  and the Steam Deck are not tested.
- **Multiplayer**: not supported; not recommended in public multiplayer games.

## Troubleshooting

Everything is recorded in **`warbandVRlog\warband_vr.log`**, next to the DLL (written in French). Lines are
prefixed by module (`[xr]`, `[render]`, `[engine]`, `[input]`). The injector writes
`warband_vr_injector.log` in the same place.

| Problem | What to check |
|---|---|
| No image in the headset | Is the headset software running? The log says "runtime OpenXR pas prêt" until the headset is connected |
| The game starts on the wrong headset / runtime | "VR runtime (OpenXR)" setting, or the `--steamvr` / `--default-runtime` options |
| No sound in the headset | `son :` lines in the log: list of FMOD outputs and the selected one |
| Choppy image | The log shows the pacing ("une paire d'images ... par frame du casque", "le jeu ne tient pas...", "cadence remontee"). Lower the Warband graphics or the *render scale*; enable *Diagnostic: frame timings* to see what limits (game, renderer, graphics card) |
| Blurry or pixelated image | "resolution de rendu par oeil" line in the log; *render scale* too low |
| The launcher does not open | "Show the launcher at startup" setting, or `warband_vr_injector.exe --launcher` |
| Empty module list | Launch tab: "Game folder"; Workshop titles need internet |
| The game language does not change | The module must provide that language (its `languages` folder) |
| Attacks go in the wrong direction | Game option "attack direction: by mouse movement"; *Gesture* settings |
| A feature stopped working after a WSE2 update | Look for "signature" in the log |
| `warband_vr_injector.exe` disappeared, or an antivirus warning | False positive: Windows Defender may flag the injector (`Behavior:Win32/DefenseEvasion.A!ml`) because it loads a DLL into the game, which is how the mod works. Restore it from *Windows Security → Protection history* and add the mod folder to the exclusions |
| An eye looks wrong, or the HUD is missing | Enable *Diagnostic: capture both eyes* (Advanced tab), reproduce, and attach the contents of the `warbandVRlog` folder (PNG files and logs) to your report |

## Building from source

The code is C++20, built with CMake. The dependencies (MinHook, OpenXR loader) are linked
statically, along with the CRT: the DLL depends on no third-party DLL. The three files are the
same whatever the build machine.

### On Windows (MSVC + vcpkg)

You need Visual Studio 2022, CMake and vcpkg (manifest mode, `x64-windows-static` triplet).

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The files are produced in `build/Release/`.

### On Linux (clang-cl cross-compilation)

Works on x86_64 and ARM64 PCs, including directly on the Steam Frame (SteamOS). You need
`clang` (clang-cl), `lld` (lld-link), `cmake`, `ninja`, `git` and `curl`.

1. Once, the Windows SDK and the MSVC CRT (about 630 MB, in `~/.local/opt/winsdk`), downloaded
   by [xwin](https://github.com/Jake-Shadle/xwin). The script accepts Microsoft's license
   (the Visual Studio Build Tools one):
   ```sh
   tools/setup_linux_toolchain.sh
   ```
2. Build (MinHook and the OpenXR loader are downloaded and built from source, in the same
   versions as vcpkg):
   ```sh
   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake
   cmake --build build-linux
   ```

The files are produced in `build-linux/`. Only the Release configuration is available (the
debug CRT is not extracted). The SDK folder can be changed with the `XWIN_DIR` variable.

### Code layout

| Folder | Contents |
|---|---|
| `injector/` | 64-bit injector (launcher, CreateProcess + LoadLibrary injection) |
| `src/render/` | Direct3D 9 hooks (Present, render targets), frame sharing to D3D11, game window, timing measurements |
| `src/xr/` | OpenXR session, pacing, controllers, controller rendering, virtual keyboard, radial menu |
| `src/engine/` | Game engine: stereo camera, true stereo, skeleton (VR arms), combat, orders, text input, sound, frame pacing |
| `src/input/` | DirectInput injection (keyboard, mouse), menu pointer |
| `src/util/` | Log, signature scanning, settings and presets, hooks (MinHook, virtual tables) |
| `config_tool/` | Launcher and configuration tool (GDI interface), Warband graphics and language |
| `cmake/` | Cross-compilation from Linux (clang-cl, lld-link, xwin) |
| `tools/` | Linux build setup, exploring the game's PDB without cdb (`pdbtool.py`), analysis scripts |

## License and credits

WarbandVR is released under the [MIT license](LICENSE).

- Third-party libraries linked into the binaries: **MinHook** (BSD 2-Clause), **OpenXR SDK
  loader** (Apache 2.0), **JsonCpp** (MIT) — see `THIRD_PARTY_NOTICES.txt` in the release.
- Architecture inspired by **WSE** (Warband Script Enhancer, WTFPL); WarbandVR runs on top of
  **WSE2**, which it does not modify.
- Mount & Blade: Warband is a game by TaleWorlds Entertainment. This project is not
  affiliated with or endorsed by TaleWorlds or the WSE2 authors.
