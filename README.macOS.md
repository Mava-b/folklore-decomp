# Folklore — static recompilation for PC (macOS)

🌐 **English** · [Português (BR)](README.macOS.pt-BR.md)

A native port of **Folklore** (PS3, disc `BCUS98147` / `BCES00050`) to
**macOS/arm64**, built with static recompilation: the game's PowerPC and SPU
code is translated to C/C++ and compiled into a native executable, with
graphics on Metal. It is not an emulator. This page is about the Mac build;
the Windows build, where the port started, is in [README.md](README.md).

**A new game starts and plays on the Mac**: the opening, cutscenes, Doolin,
the story pages and Folk captures. Tested on an M4 Pro with macOS 27, built
from a `BCUS98147` v1.00 disc image. There is no download yet: you build the
game from your own disc with one script ([how](#building-from-source)), or
with the app in [`launcher/`](launcher/README.md).

The port itself is [Mava-b](https://github.com/Mava-b/folklore-decomp)'s
Windows port of Folklore, built on
[ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet. The Mac
build adds the macOS toolkit of the author's
[inFAMOUS 2](https://github.com/erikomis/infamous2-recomp) and
[Resistance](https://github.com/erikomis/resistance-recomp) ports, and fixes
first made in the Demon's Souls, Resistance 2 and PES 2013 ports (see
[Fixes made here](#fixes-made-here)).

> [!WARNING]
> **Early, not played to the end on the Mac yet.** A new game runs into the
> story and captures work, only on Apple-silicon macOS. Motion controls are
> not read yet (captures complete by themselves), and most of the Windows
> build's PC extras are not on the Mac. What is missing is in
> [ROADMAP.macOS.md](ROADMAP.macOS.md).

> **This repository contains no game data, no firmware, no Sony keys and
> nothing translated from them.** Everyone generates all of that locally from
> their own legitimate disc; the EBOOT is decrypted by the port's own
> decrypter, with no RPCS3 and no key files. A fan project, not affiliated
> with Sony Interactive Entertainment or Game Republic.

## Status

| | |
| --- | --- |
| Build on macOS (Apple silicon) from your disc with `setup_mac.sh` | ✅ about 2.5 minutes for the build itself on an M4 Pro |
| Boot, first-boot install, opening logos, movies (FFmpeg) | ✅ |
| Title screen, Options, Load Game, back to the title | ✅ since the empty-path fix (the "HDD access error" black screen) |
| New game → cutscenes → Doolin → the story pages | ✅ |
| Folk captures, boss captures too | ✅ completed by themselves (auto-capture): motion sensors are not read on the Mac yet |
| The PS3's yes/no and OK prompts | ✅ answered with Cross/A or Circle/B, Return/Escape or the mouse |
| Graphics: grass and fences, cliffs and rocks, bloom, blend colours | ✅ fixed on Metal (see [Fixes made here](#fixes-made-here)) |
| The host window: MetalFX, windowed / macOS fullscreen / borderless, a display of choice, vsync | ✅ since 3b5d899; tested at 1.5x internal resolution with MetalFX to 2560x1440 |
| Internal resolution (`PS3_RES_SCALE`, 1x to 4x of 1280x720) | 🧪 1.5x tested; the others not measured |
| In-game menu (F1 / fn+F1 / Cmd+, / L3+R3): image, audio, controls, performance HUD, Quit game | ✅ saved to `overlay.ini` |
| Controller through SDL (PlayStation pads; Xbox pads read as HID) | ✅ buttons and sticks (played with an Xbox Series X controller) |
| Keyboard and mouse (the toolkit's layout, remappable) | 🧪 mapped, not play-tested in Folklore |
| Rumble | ⏳ Folklore rumbles only in the 1.10 build, which the Mac scripts do not make yet |
| Motion controls (capturing by moving the controller) | ⏳ not read on the Mac yet |
| App bundle with Game Mode (`folklore.app`) | ✅ |
| Performance | ✅ 60 fps in the intro and Doolin on an M4 Pro (1.5x internal resolution, MetalFX, SMAA); the fps is in the window title and the HUD |
| The engine menu and the PC extras of the Windows build (ultrawide, soft shadows, ...) | ⏳ Windows only (Direct3D 12) |
| The app (`launcher/`): install, rebuild, play, settings, saves | 🧪 built from source, no release yet |
| Whole game | ❓ not played to the end on the Mac |

Details and next steps in [ROADMAP.macOS.md](ROADMAP.macOS.md).

## Download and play

There is no release to download yet. You need an Apple-silicon Mac, your own
**Folklore** disc, version 1.00, North American `BCUS98147` or European
`BCES00050` (the Japanese release does not work), as a disc image (`.iso`) or
a dumped folder that contains `PS3_GAME`, and [Homebrew](https://brew.sh).

Two ways to get from the disc to Play:

1. **The app:** build [`launcher/`](launcher/README.md) (Node, Rust and the
   Xcode Command Line Tools), open it and press **Install the game**, choose
   your `.iso` or folder, then **Play**. It runs this repository's own
   scripts, below, and shows their progress.
2. **The terminal:** `./setup_mac.sh <your .iso or folder>`, then
   `./run_mac.sh` ([Building from source](#building-from-source)).

Your disc is only ever read: a disc image is mounted read only at
`gamedisc/`.

## The app

[`launcher/`](launcher/README.md) is a desktop app (Tauri), adapted from the
Resistance port's launcher, which was made from the inFAMOUS 2 one. It does
not do the port's work again: it runs `setup_mac.sh` and `run_mac.sh`, keeps
the settings and starts the game with them. Pages: Home, Graphics,
Performance, Game, Controls, Audio and Advanced.

- **Install / rebuild** with a progress bar, a plain-language reason when a
  step fails, and **Cancel**; pressing it again continues where it stopped.
- **Graphics:** internal resolution, FXAA or SMAA, sharpening, anisotropic
  filtering, gamma, vibrance, contrast, vignette, grain, a colour LUT,
  texture packs, and ready-made profiles (Performance / Balanced / Quality).
- **Performance:** the game's own frame-rate cap (from
  `folklore_settings.ini`), the fps counter, the HUD, the vertex and shader
  caches.
- **Game:** your disc, opening logos, the language (European release), the
  movies, the in-game menu's saved settings, and a **saves manager** (back
  up, restore, import and export as `.zip`).
- **Controls:** remap keyboard, mouse and controller, with a button test;
  mouse and vibration options. **Audio:** volume, mute, output device.
- **Advanced:** what this Mac needs, the Folklore folder, rebuild options,
  updates from git, the game log, a logs `.zip` for a report, extra
  variables.
- **Play shortcut** on the Desktop and **Add to Steam**.

The window options of the host window (MetalFX, display mode, window size,
display, vsync) are on the Graphics page and in Home's quick settings, and
Advanced has a switch "use the game's own window" (`FOLKLORE_HOST_WINDOW=0`).
The app has no release feed, so it does not
update itself; it is rebuilt from this folder. Its builds are not signed: the
first time, right-click > Open.

**In the game,** F1 (fn+F1), Insert, Cmd+, or L3+R3 opens the **in-game
menu** (from the inFAMOUS 2 port): image, audio volume, controls with live
remapping, a performance HUD and **Quit game**. Details
[below](#in-game-menu).

## Known problems

- **Motion controls:** the controllers' motion sensors (SDL's gyro) are not
  read on the Mac yet, so every capture completes by itself (auto-capture),
  boss captures too. The Windows build reads them directly.
- **The Windows extras are not on the Mac:** the engine menu (Dear ImGui on
  Direct3D 12) and most of README.md's "What's better than on the PS3" list
  (ultrawide, ambient occlusion, soft shadows, the smooth depth of field and
  bloom, the cleaner Id souls, ...) live in the Direct3D 12 renderer
  (`rsx_live_draw.c`), which is Windows only. On the Mac the image options
  are the toolkit's (`PS3_*` variables, the in-game menu).
- **Downloadable content:** importing DLC (`.rap` licences, **Add DLC from
  RPCS3...**) is in the Windows engine menu only.
- **The 1.10 update** is not wired into the Mac scripts yet: the Mac build is
  the disc's 1.00, so no rumble and not the newer Folk table.
- **First-use hitches:** the first time an effect shows up its shader is
  translated, which can stall a moment. The shader cache (on in `run_mac.sh`)
  keeps them, so each happens once.
- **The PPU's vector code (VMX)** runs through the portable path, with no
  NEON version yet.
- **Windows:** the merged runtime (ps3recomp v0.19.0+3 with the macOS toolkit)
  has not been built or run on Windows yet.

## Work in progress

What is being worked on now (details in [ROADMAP.macOS.md](ROADMAP.macOS.md)):

- **Playing further into the story** on the Mac, writing down what breaks.
- **The app:** the host window's options and the "use the game's own window"
  switch, above.
- **Motion controls** through SDL's gyro, so captures can be played as on
  the PS3.
- **The 1.10 update** in `setup_mac.sh` and `run_mac.sh`.
- **A Windows build of the merged runtime**, to check nothing broke there.

## Building from source

The app does all of this for you; by hand, from a terminal:

### What you need

- An Apple Silicon Mac (tested on an M4 Pro, macOS 27).
- Your own Folklore disc, version 1.00, `BCUS98147` or `BCES00050`, as an
  `.iso` or a dumped folder that contains `PS3_GAME`.
- The Xcode Command Line Tools (`xcode-select --install`) and
  [Homebrew](https://brew.sh). `setup_mac.sh` installs the Homebrew packages
  it needs (`cmake ninja sdl2 glslang spirv-cross ffmpeg`) and makes its own
  Python environment.

### Building

```sh
git clone https://github.com/erikomis/folklore-decomp.git
cd folklore-decomp
git checkout macos-port
./setup_mac.sh ~/path/to/Folklore.iso      # or the folder that contains PS3_GAME
./run_mac.sh
```

`setup_mac.sh` saves your disc's path in `folklore_mac.env`, then, each step
skipped when it is already done:

1. installs the Homebrew packages;
2. makes a Python environment for the lifter (`tools/venv-mac`);
3. decrypts `PS3_GAME/USRDIR/EBOOT.BIN` with the port's own decrypter into
   `decrypted/<hash>/EBOOT.elf` and checks it is a supported 1.00 EBOOT;
4. lifts the game code (`folklore/tools/relift.sh`, a few minutes);
5. promotes guest registers and builds `folklore/build-mac/folklore.app`
   (about 2.5 minutes on an M4 Pro), with your disc's `ICON0.PNG` as its
   icon.

Run it again with no argument to rebuild with the same disc. `JOBS=n` sets
the parallel compiles (default 4), `NO_PROMOTE=1` builds the lifted code as
it is, `RELIFT=1` lifts again. Each step writes a log in `logs/`
(`relift-mac.log`, `promote-mac.log`, `configure-mac.log`, `build-mac.log`).

## Playing from the terminal

```sh
./run_mac.sh
./run_mac.sh PS3_AA=smaa PS3_ANISO=16       # extra arguments are environment overrides
```

`run_mac.sh` mounts the disc if needed and starts the game through the host
window (`folklore.app/Contents/MacOS/folklore-window`), with the switches
`run.ps1` sets on Windows (`GCM_WAITFLIP_DEPTH=1` among them: without it every
frame waits out the vblank and dialogue openings stall). The game's install
data and cache go to `hdd0/` and `hdd1/` here, saves to
`gamedata/dev_hdd0/home/00000001/savedata`, the log to `logs/run-mac.log`, and
the game's own options are read from `folklore_settings.ini`, as on Windows.

A controller works out of the box through SDL. Keyboard and mouse add on top
of it, with the toolkit's built-in layout (Folklore's own Z/X/A/S keys stay on
Windows):

| Key | PS3 |
| --- | --- |
| W A S D | left stick |
| I J K L or mouse | right stick (camera) |
| Arrow keys | D-pad |
| Space | ✕ |
| C, Control, Backspace | ○ |
| R, E | □ |
| Q | △ |
| Z / X | L1 / R1 |
| G / 1 | L2 / R2 |
| Shift / V | L3 / R3 |
| Return, keypad Enter, Esc, P / Tab, M | START / SELECT |
| Left / right / middle mouse button | R1 / L1 / R2 |
| Mouse buttons 4 / 5, wheel | L2 / □, △ |

Every binding can be changed: in the in-game menu's **Controls** tab (live),
in the app's Controls page, or in a controls file (`PS3_INPUT_MAP`, the
toolkit's `input.toml` format, the same as in the inFAMOUS 2 port). The
mouse has the toolkit's options (`PS3_MOUSE=0`, `PS3_MOUSE_SENS`,
`PS3_MOUSE_INVERT_Y`, `PS3_MOUSE_CAPTURE=0`); a click in the window captures
it, and Esc, Cmd or leaving the window releases it.

The PS3's yes/no and OK prompts (saving and loading among them) are drawn by the in-game menu: Cross/A or Return for yes/OK, Circle/B or
Escape for no, or the mouse on the buttons. The press that opened a prompt has
to be released first. With `FOLKLORE_HOST_WINDOW=0` they are a sheet on the
game's own window instead, answered the same way.

| Shortcut | What it does |
| --- | --- |
| Cmd+F | fullscreen on/off (borderless with `PS3_WINDOW_MODE=borderless`, macOS fullscreen otherwise) |
| Cmd+Ctrl+F | macOS fullscreen on/off |
| Cmd+Option+F | borderless fullscreen on/off |
| Esc / Cmd | release the mouse |
| F1 (fn+F1 on a Mac keyboard), Insert or Cmd+, ; L3+R3 on a controller | the in-game menu (below) |
| Cmd+Q, closing the window | quit the game |

### In-game menu

F1 (fn+F1 on a Mac keyboard, where F1 is the brightness key), Insert (Help on
older Mac keyboards) or Cmd+, opens and closes it; on a controller, press L3
and R3 together. Esc or ○ also closes it. While it is open the game gets no
input, but it keeps running: pause it first with START if needed.
`PS3_OVERLAY_PAD=0` turns the controller combo off, `PS3_OVERLAY=0` the whole
menu. It is the inFAMOUS 2 port's menu, adapted to this toolkit.

| Tab | What it changes |
| --- | --- |
| Image | gamma, sharpening, vibrance, contrast, vignette, grain, the LUT's strength, FXAA or SMAA, MetalFX, vsync and the display mode (window, macOS fullscreen, borderless), live; anisotropic filtering and internal resolution are shown and apply at the next start |
| Audio | master volume (movies included) |
| Controls | play with controller + keyboard and mouse, controller only, or keyboard and mouse only; vibration; the mouse camera; a table of every PS3 button with its key, mouse button and controller button: click a cell, press the new input, it applies at once |
| Performance | a small HUD in a corner that stays when the menu is closed: fps, frame time (with a graph), CPU cores in use and busy threads, the RSX's Metal encode time and the GPU wait |

**Quit game** asks first, then ends the game as closing the window does, after
any save in progress has been written.

What the menu changes is saved when it closes, to `overlay.ini` beside
`run_mac.sh` (`PS3_OVERLAY_INI`), and its remapping to `input.toml` beside it.
Precedence at start: a variable set when the game starts wins (the shell, or
the app, which passes only the settings it has away from their defaults);
otherwise the menu's line in `overlay.ini`; otherwise the built-in default.
The menu marks a setting whose variable was set with "env".

**Game Mode.** The build makes an app bundle, `folklore/build-mac/folklore.app`
(`folklore/build-mac/folklore` links to its executable, and `run_mac.sh`
starts it directly, so the environment and the log work as before). Its
Info.plist (from the inFAMOUS 2 port) marks it as a game, so macOS 14+ turns
on Game Mode while it is fullscreen and in front. The game also keeps macOS
from App-Napping it while it runs.

| Variable | What it does |
| --- | --- |
| `PS3_RES_SCALE=1.5\|2\|3` | Internal resolution: the game's render targets at 1.5x (1080p), 2x (1440p) or 3x (4K) of 1280x720. Any value 1..4 in quarter steps, or `720p`, `1080p`, `1440p`, `4k`. Unset or 1: the PS3's own size. What the PS3 code reads back stays at 720p. Read at start |
| `PS3_UPSCALE=metalfx` | MetalFX spatial upscaling to the window's pixel size, instead of a plain stretch (host window only). Default off |
| `PS3_WINDOW_MODE=windowed\|fullscreen\|borderless` | how the game opens: a window (default), macOS fullscreen (a Space of its own) or borderless fullscreen (covers the screen in place) (host window only) |
| `PS3_WINDOW_SIZE=<w>x<h>` | the window's size in points, e.g. `1920x1080` (default 1280x720, shrunk to fit the screen) (host window only) |
| `PS3_DISPLAY=<n>\|<name>` | the screen the game opens on: `1` is the one with the menu bar, `2` the next, or a screen's name (any case, or a part of it). Default: the main screen (host window only) |
| `PS3_VSYNC=0` | presents frames without waiting for the screen's refresh (less latency, may tear). Default on |
| `PS3_AA=fxaa\|smaa` | anti-aliasing on the game's image: FXAA (cheapest, slightly soft) or SMAA 1x (sharper). Default off |
| `PS3_SHARPEN=<0..1>` | contrast-adaptive sharpening; 0 (default) is off |
| `PS3_ANISO=2\|4\|8\|16` | anisotropic filtering on the game's mipmapped textures; lookup tables, render targets and the UI keep their filtering. Default off |
| `PS3_VIBRANCE=-1..1` | vibrance: saturation raised most on dull colours (negative mutes). Default 0 (off) |
| `PS3_CONTRAST=0.5..1.5` | contrast around mid-grey. Default 1 (off) |
| `PS3_LUT=<file.png>` | a colour LUT in ReShade's `LUT.png` layout (N·N wide, N high, e.g. 1024x32), your own file. Default none |
| `PS3_LUT_STRENGTH=0..1` | how much of the LUT is mixed in. Default 1 |
| `PS3_VIGNETTE=0..1`, `PS3_GRAIN=0..1` | darker corners; film grain. Default 0 (off) |
| `PS3_MODS=<pack>,<pack>` | texture packs: folders in `PS3_MODS_DIR` (or full paths) of PNGs named by the key of the texture they replace; the first pack wins. `PS3_MODS_DUMP=<dir>` writes the game's colour textures there under those names. Default none |
| `PS3_INPUT_MAP=<file>` | the controls file. Unset: the `input.toml` the in-game menu saved beside `overlay.ini`, if there is one, else the built-in layout above. Empty: the built-in layout |
| `PS3_AUDIO_VOLUME=<0..100>` | master volume (default 100), on an even-sounding curve (50 is about −12 dB); the movies too |
| `PS3_AUDIO_MUTE=1` | no sound (any value mutes; leave it unset to hear the game) |
| `PS3_AUDIO_DEVICE=<name>` | the audio output, by its name or a part of it (`AirPods`); not found or unset, the system default |
| `PS3_OVERLAY=0` | no in-game menu and no HUD |
| `PS3_OVERLAY_HUD=1` | the performance HUD (fps, frame time, CPU/GPU). On by default: `run_mac.sh` writes it into `overlay.ini` on the first run, so the menu can turn it off; `0` hides it |
| `PS3_OVERLAY_INI=<file>` | where the in-game menu saves; `run_mac.sh` sets `overlay.ini` beside it. Empty: changes last only for the session |
| `PS3_SHADER_CACHE=<dir>` | keeps translated shaders between runs, and the compiled pipelines with them (`pipelines.metalar`): a scene stutters only the first time its shaders appear (translating one can take 160 ms). It is kept across rebuilds of the game and starts over only when the shader translator changes; `run_mac.sh` sets `folklore/build-mac/shader_cache`. Empty or `0`: off |
| `PS3_PIPELINE_CACHE=0` | no compiled-pipeline archive (the translations are still kept) |
| `PS3_PIPELINE_TIME=1` | logs how long each new shader takes to translate and compile |
| `PS3RECOMP_ENG_VCACHE=0` | turns off the cross-frame vertex cache (on by default) |
| `PS3RECOMP_METAL_FPS=1` | the fps in the window title and `[RSX FPS]` lines in the log, once a second. On by default in `run_mac.sh` (as in the Resistance ports); `0` turns it off |
| `PS3_VERBOSE=1` | the full runtime log in `logs/run-mac.log`; `run_mac.sh` sets 0 (quiet) |
| `MSGDIALOG_NATIVE=0` | the PS3's prompts answered automatically instead of shown to you |
| `FOLKLORE_HOST_WINDOW=0` | the game opens its own window instead of the host window: no MetalFX, display choice or window modes from the menu; the PS3's prompts are a sheet on that window. Default 1 |
| `FOLKLORE_EXE=<path>` | runs another build of `folklore` (set it in the shell: `FOLKLORE_EXE=... ./run_mac.sh`). Default `folklore/build-mac/folklore` |
| `PAD_FILE=<file>` | for testing: each poll, a button mask written to the file (`0x0040 4`) is pressed for that many reports and the file is emptied, so a script can drive the menus |

## Fixes made here

In ps3recomp (all in
[`patches/0001-folklore-on-upstream-de4f0903.patch`](patches/README.md), with
the earlier one-fix-per-patch series kept in `patches/v0.15-39a36296/`):

- **The toolkit, updated:** the included ps3recomp moved from v0.15
  (39a36296) to upstream v0.19.0+3 (de4f0903), 789 commits newer, with
  Folklore's 83 patches merged onto it. Upstream brings most of its macOS /
  Metal work.
- **The macOS toolkit of the Resistance and inFAMOUS 2 ports**, merged in:
  the in-game menu, internal resolution, MetalFX and window modes through the
  host window, colour LUTs, input remapping, audio volume, the vertex and
  shader caches, texture packs and the quit gate. Their SPU scheduler, lifter
  output and FPCR changes are left out (Folklore's lifted code and SPURS
  depend on its own).
- **The guest off the main thread:** the guest runs on its own 256 MB thread
  and the main thread turns the run loop (from the Demon's Souls port): with
  the guest on the main thread the Metal backend never drained the FIFO.
- **A SIGTRAP one start in three:** NSApp is set up on the main thread before
  the guest starts, so AppKit never binds its event queue to another thread.
- **The black screen at the title:** the game stats `""` each time it returns
  to the title; off Windows that named a real folder, and the game ran its
  download installer over garbage until "An HDD access error occurred.". An
  empty path is now ENOENT, as on Windows.
- **`cellFsUnlink`** never removes a directory (EISDIR, as lv2 does).
- **The PS3's prompts** go to the player (a sheet, or the in-game menu in the
  host window), and the pad always comes back to the game after one.
- **Holes in the rocks:** the Metal draw engine kept only 256 draw packets per
  BEGIN/END, so cliffs and rocks lost about a quarter of their triangles.
  Contiguous packets are merged and up to 4096 ranges queued, as patch 0019
  did for Direct3D 12.
- **Grass and fences as solid cards:** alpha-to-coverage emulated as an alpha
  test, as patch 0020 did for Direct3D 12.
- **Bloom as a grid of squares:** the RSX convolution filter applied in the
  shader (patches 0035/0036), and **the blend colour** passed to Metal for the
  constant blend factors (patch 0038).
- **From the Resistance 2, PES 2013 and inFAMOUS 2 ports, for long sessions:**
  bigger Metal and draw-engine tables (in Resistance 2 a full pipeline table
  dropped every new draw), an autorelease pool around each frame-clock pass
  (it leaked about 2 GB in three hours), NV308A inline uploads over the whole
  colour range, a linear mipmapped texture keeping its pitch at every level,
  and App Nap and timer coalescing off while the game runs.

In the port: `GCM_WAITFLIP_DEPTH` and the other switches `run.ps1` sets, now
in `run_mac.sh`; the settings file, DLC decryption and the capture assist
built for macOS (`_NSGetExecutablePath`, CoreGraphics, CommonCrypto,
`mach_vm_region`); the capture assist on a thread of its own, so captures
complete without the engine menu; the app bundle and its icon; `setup_mac.sh`,
`run_mac.sh` and the app.

## Layout

| Path | What it is |
| --- | --- |
| `setup_mac.sh`, `run_mac.sh`, `mac_common.sh` | Build from your disc; play; what both share |
| `folklore_mac.env` | Made by `setup_mac.sh`: the path to your disc |
| [`folklore/`](folklore/) | The port: CMake project, Folklore's HLE and engine code; `folklore/macos/` has the bundle's Info.plist and icon script |
| [`ps3recomp/`](ps3recomp/) | The recompiler and runtime, with the patch below already applied |
| [`patches/`](patches/README.md) | This port's changes to ps3recomp, as one patch on upstream de4f0903 |
| [`launcher/`](launcher/README.md) | The app (Tauri) |
| [README.md](README.md) | The Windows build |

## License

The port's own code is MIT ([LICENSE](LICENSE)); ps3recomp keeps its own
licence (MIT). Folklore is © Sony Interactive Entertainment / Game Republic;
this project is not affiliated with Sony or Game Republic, and no part of the
game is included.
