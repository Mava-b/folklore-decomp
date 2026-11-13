# Folklore Launcher (Tauri)

A desktop app that takes the player from their own Folklore disc to "Play"
on an Apple-silicon Mac without a terminal. It does not do the port's work
again: it runs the repository's own scripts, `setup_mac.sh` (install and
rebuild) and `run_mac.sh` (play), with zsh, shows their progress, keeps the
game's settings and starts the game with them. It ships no game code or
files: the art on Home is read at runtime from the player's own disc
(`PS3_GAME/ICON0.PNG` and `PIC1.PNG`, from the image mounted at
`gamedisc/` or the dumped folder).

It is adapted from the [Resistance: Fall of Man port's
launcher](https://github.com/erikomis/resistance-recomp/tree/main/launcher),
itself made from the inFAMOUS 2 port's, and keeps their look, their layout
and most of their code (the Play shortcut, Add to Steam, the Dock icon from
the player's copy, the saves manager, the controls editor, the log viewer).

Supported: Folklore disc version 1.00, **BCUS98147** (North America) and
**BCES00050** (Europe), as a disc image (`.iso`) or a dumped folder that
holds `PS3_GAME`. The 1.10 update is not used by the Mac flow yet.

## Building

Needs Node 20+, Rust (stable) and the Xcode Command Line Tools.

```sh
cd launcher
npm install
npm run tauri build          # .app and .dmg in src-tauri/target/release/bundle/
npm run tauri build -- --debug   # a debug .app in src-tauri/target/debug/bundle/macos/
npm run tauri dev            # the app, with the frontend hot-reloaded
npm run typecheck            # frontend only (tsc --noEmit)
(cd src-tauri && cargo test)
```

`src-tauri/Cargo.toml` keeps debug info in release build scripts and proc
macros (`[profile.release.build-override]`): without it, on macOS 27 with
Rust 1.94, `tauri build`'s proc-macro dylibs were refused by dyld
("mis-aligned LINKEDIT string pool") and the release build failed.

The repository's `.gitignore` keeps `launcher/node_modules/`,
`launcher/dist/`, `launcher/src-tauri/target/` and `launcher/src-tauri/gen/`
out. The builds are not code-signed: the first time, right-click the app and
choose Open (Gatekeeper). `python3 scripts/check_bundle.py <.app|.dmg>` fails
when a bundle carries anything from the game (names, PS3 file headers,
PowerPC/SPU ELFs, big files). The icon is an original placeholder (a
crescent moon; `python3 scripts/make_icon.py src-tauri/icons/app-icon.png`,
then `npx tauri icon src-tauri/icons/app-icon.png`); the font is Oswald (SIL
Open Font License, `src/assets/fonts/`).

## The Folklore folder

The launcher works on a checkout of this repository (it needs
`setup_mac.sh`, `run_mac.sh` and `folklore/`). It finds it on its own, in
order: `FOLKLORE_ROOT`; a folder above the running executable (the launcher
lives in `<repo>/launcher`, its builds in `launcher/src-tauri/target/…`, so
an app built here, and `npm run tauri dev`, find the parent folder); the
repository the launcher was compiled from; then a folder one level under
`~/Desktop`, `~/Developer`, `~/Documents`, `~/Projects`, `~/src`, `~/code`,
`~/git` or `~` that has `setup_mac.sh`. **Advanced > Install / rebuild > The
Folklore folder** sets another one (or Search lists the ones found).

Its own files live in `~/Library/Application Support/folklore-recomp/`:
`launcher.json` (the settings), `input.toml` (the controls), `mods/` and
`texture-dump/` (texture packs), `save-backups/`, `reports/` (log archives),
`install.log` (the last install's output), the Dock icon cache and the Steam
shortcut.

## What the player does

1. **Home > Install the game** (shown while the game is not built): choose
   the disc image (`.iso`) or the folder that holds `PS3_GAME`, and press the
   button. The launcher checks the free disk space (3 GB asked) and says how
   long it takes (10 to 30 minutes), opens Apple's Command Line Tools
   installer when they are missing (`xcode-select --install`) and waits for
   it (polling every 5 s). Homebrew asks for the user's password, so it is
   not installed from the app: the window says so and Advanced > Install /
   rebuild opens brew.sh or copies the official install command. Then it
   runs `zsh setup_mac.sh <disc>`, which saves the disc in
   `folklore_mac.env`, installs `cmake ninja sdl2 glslang spirv-cross
   ffmpeg` with Homebrew, makes `tools/venv-mac`, decrypts `EBOOT.BIN` with
   the port's own decrypter (no RPCS3, no keys), lifts the game code,
   promotes guest registers and builds `folklore/build-mac/folklore`. One
   progress bar with the step's name and a time estimate (from setup_mac.sh's
   `==>` lines), **Cancel**; on a failure a plain-language reason by step
   with **Copy log**, **Show the log file** and **Report a problem** (the
   log's end copied, a new GitHub issue opened). Pressing it again continues
   where it stopped: setup_mac.sh skips each step it finds done.
2. **Play** on Home.

## Pages

Every setting is saved the moment it changes and applies the next time the
game starts. Every setting is passed only when it differs from the runtime's
default: with the defaults, Play runs exactly `./run_mac.sh`.

- **Home / Início**: a hero with the player's own disc art (`PIC1.PNG`
  behind, `ICON0.PNG` as the cover), else a plain styled title; the title
  ID, region and disc version, the build date and the folder; **Play** (Stop
  while the game runs). A bar for what blocks Play, with its fix: no Folklore
  folder (choose it), the saved disc is gone (an external disk?), a disc that
  is not BCUS98147/BCES00050. The Install the game card (above). A bar when
  Updates has news. Cards: **Quick settings** (display mode, window size,
  upscaling, frame-rate cap, fps counter), **Last save**, **Status** (state, folder,
  your copy, disc, build; links to the log, Updates, **Play shortcut on the
  Desktop**, **Add to Steam** / **Remove from Steam**). Play backs up the
  saves first.
- **Graphics / Gráficos**: the window, which is the window host's
  (`folklore-window`, ps3recomp's `tools/host_window`; values in its
  `host_display.h`): display mode `PS3_WINDOW_MODE=fullscreen|borderless`
  (windowed is the default and passes nothing), window size
  `PS3_WINDOW_SIZE=WxH`, screen `PS3_DISPLAY` (by the names `NSScreen` gives,
  listed through `osascript`; a saved one that is not connected stays in the
  list), vsync `PS3_VSYNC=0` when off; your disc's icon in the Dock and as
  the app's Finder icon. Image quality with ready-made profiles
  (**Performance**: the PS3's own picture; **Balanced**: `PS3_RES_SCALE=1.5`,
  FXAA, sharpening 0.2, 4× anisotropic; **Quality**: `PS3_RES_SCALE=2`,
  MetalFX, SMAA, sharpening 0.3, 16×, as the inFAMOUS 2 launcher's Quality
  uses MetalFX; Custom otherwise), MetalFX upscaling `PS3_UPSCALE=metalfx`,
  internal resolution (`PS3_RES_SCALE`), `PS3_AA`
  (fxaa/smaa), `PS3_SHARPEN`, `PS3_ANISO`, gamma override
  (`PS3_DISPLAY_GAMMA`); filters (`PS3_VIBRANCE`, `PS3_CONTRAST`,
  `PS3_VIGNETTE`, `PS3_GRAIN`, a colour LUT file `PS3_LUT` and
  `PS3_LUT_STRENGTH`); texture packs (the folders in the launcher's `mods/`,
  ticked and ordered: `PS3_MODS_DIR`, `PS3_MODS`, first wins; **Dump the
  game's textures**: `PS3_MODS_DUMP`).
- **Performance / Desempenho**: the frame-rate cap (30 fps original, 60 fps,
  unlimited), stable cloth and hair above 30 fps and the precise limiter
  (`folklore_settings.ini`, below); the fps counter in the window title
  (`PS3RECOMP_METAL_FPS=1`) and the in-game menu's performance overlay
  (`PS3_OVERLAY_HUD=1`, `PS3_OVERLAY_HUD_CORNER`); the vertex cache
  (`PS3RECOMP_ENG_VCACHE=0` / `PS3RECOMP_ENG_VCACHE_SPLIT=0` when off), bind
  only changed state (`PS3RECOMP_METAL_BIND_ALL=1` when off), the shader
  cache (run_mac.sh's `folklore/build-mac/shader_cache`, with its size and
  **Clear**; off passes an empty `PS3_SHADER_CACHE`), async shaders
  (`PS3_SHADER_ASYNC=1`); keep the Mac awake (`caffeinate -i`).
- **Game / Jogo**: your disc (the path saved in `folklore_mac.env`, where it
  is read from, its title ID and version; **Mount now**; **Install from
  another disc…**; "Mount the image when the launcher opens", on by default:
  `hdiutil attach -readonly -nobrowse` at `<repo>/gamedisc`, exactly as
  mac_common.sh does, so Home shows the art); the game's own options in
  `folklore_settings.ini`: opening logos (`skip_logos`: all / START skips /
  never), motion controls (`motion_controls`; the Mac build does not read
  motion sensors yet, so captures complete by themselves either way), the
  language (`language`: English, French, Spanish, German or Italian, the
  European release only; disabled with a note for the North American one),
  the movies (`PS3RECOMP_FFMPEG=0` when off); the in-game menu (`PS3_OVERLAY=0`
  when off; what `overlay.ini` holds, and **Forget the menu's settings**,
  which moves it to `overlay.ini.bak`); the **saves manager** (below).
- **Controls / Controles**: the bindings table, PS3 control by keyboard,
  mouse button and controller, labelled by the PS3 control only (✕, ○, □,
  △, L1…, sticks): the repository's README and docs do not say what each
  does in Folklore beyond Cross/Circle answering the yes/no prompts, so no
  action is named. Click a cell and press the input; it is written at once to
  `input.toml` (the toolkit's `libs/input/pad_input_map.h` format), which
  Play passes as `PS3_INPUT_MAP`. Until a binding changes there is no file
  and the built-in layout applies; **Restore defaults** moves the file to
  `input.toml.bak`. Pressing keys, mouse buttons or controller buttons
  lights the rows they drive, and the controller test shows the pad and both
  sticks. Then `PS3_INPUT_MODE` (controller/keyboard; both is the default),
  the mouse (`PS3_MOUSE=0`, `PS3_MOUSE_SENS`, `PS3_MOUSE_INVERT_Y`,
  `PS3_MOUSE_INVERT_X`, `PS3_MOUSE_DEADZONE`, `PS3_MOUSE_CAPTURE=0`) and
  vibration (`PS3_RUMBLE=0`, `PS3_RUMBLE_STRENGTH` 0-200 %).
- **Audio / Áudio**: volume (`PS3_AUDIO_VOLUME` 0-100, passed below 100),
  mute (`PS3_AUDIO_MUTE=1`) and the output device (`PS3_AUDIO_DEVICE`, part
  of its name; empty = the system's default).
- **Advanced / Avançado**, five tabs:
  - **Install / rebuild**: 1. *What this computer needs*: Xcode Command Line
    Tools (Apple's installer), Homebrew (brew.sh, the install command
    copied), the Homebrew packages setup_mac.sh adds, Python 3, the Folklore
    folder, your disc, the disk space, the build. 2. *The Folklore folder*
    (found on its own, chosen, or picked from a search). 3. *Install /
    rebuild*: the disc (the saved one, or another chosen for the next run),
    `JOBS`, `NO_PROMOTE=1` and `RELIFT=1`, **Install** / **Rebuild**
    (`setup_mac.sh [<disc>]`), **Cancel** (stops its whole process group),
    the steps and the log.
  - **Updates**: the repository's git state (branch, commit, `git fetch`,
    commits behind its upstream, **Get the new sources** = `git pull
    --ff-only`, refused with local changes) and "the sources changed after
    the build" (the newest commit touching `folklore/`, `ps3recomp/`,
    `patches/`, `setup_mac.sh` or `mac_common.sh` against the build's time)
    with **Rebuild the game**. The launcher card: see "Self-update" below.
  - **Game log**: the end of `logs/run-mac.log` (at most 512 KB), followed
    every 2 s, a filter, warnings and errors only, copy, show in Finder.
  - **Debug / Depuração**: **Play with the full log** (`PS3_VERBOSE=1` for
    that run), **Open the logs folder**, and **Create .zip of the logs**:
    every `logs/*.log` (the last 8 MB of each: `run-mac.log`,
    `launcher-play.log`, `relift-mac.log`, `promote-mac.log`,
    `configure-mac.log`, `build-mac.log`), the launcher's `install.log`, a
    `system.txt` (Mac model, macOS, CPU, memory, the commit, the build, the
    command Play runs) and the optional description as `notes.txt`, with the
    home folder written as `~`, into `reports/folklore-logs-<time>.zip`.
  - **Environment**: "Use the game's own window (no MetalFX; for
    troubleshooting)" passes `FOLKLORE_HOST_WINDOW=0`: run_mac.sh then starts
    the game directly and it opens its own window; the launcher maps the
    display mode to `PS3RECOMP_FULLSCREEN=1` and the size to
    `PS3RECOMP_METAL_WINDOW` (borderless becomes macOS fullscreen; MetalFX
    and the screen choice are disabled). Extra variables (`KEY=VALUE`, passed last on run_mac.sh's
    command line, so they win), the quiet log (off passes `PS3_VERBOSE=1`),
    the equivalent command line, Restore defaults.
- UI in Brazilian Portuguese (default) and English (sidebar); English is the
  fallback for a missing string.

## How it fits together

```text
launcher (Rust, src-tauri/src/lib.rs)
 ├─ Install / Rebuild → nice -n 10 zsh <repo>/setup_mac.sh [<disc>]   (cwd <repo>;
 │                      JOBS, NO_PROMOTE, RELIFT; "==> step" lines: src/progress.ts;
 │                      the output also in <data>/install.log)
 └─ Play             → [caffeinate -i] zsh <repo>/run_mac.sh KEY=VALUE …   (cwd <repo>;
                        run_mac.sh → folklore.app/Contents/MacOS/folklore-window -- folklore <elf>
                        (FOLKLORE_HOST_WINDOW=0: folklore <elf> on its own);
                        run_mac.sh's own output in logs/launcher-play.log,
                        the game's in logs/run-mac.log)
```

- **Play's command line** carries only the settings that differ from the
  runtime's defaults, then the extra variables; run_mac.sh sets the rest
  (the disc, `hdd0/`, `hdd1/`, `FOLKLORE_SETTINGS`, `PS3_OVERLAY_INI`, the
  shader and MSL caches, the port's fixed switches, Homebrew's FFmpeg). The
  launcher drops an inherited `PS3RECOMP_METAL_HEADLESS`,
  `PS3RECOMP_HOST_SURFACES`, `PS3RECOMP_WINDOW_STATE` and `FOLKLORE_EXE`. A
  GUI app starts with a bare `PATH`: Homebrew's folders go in front.
- **folklore_settings.ini** (run_mac.sh's `FOLKLORE_SETTINGS`, read by
  `folklore/src/engine/engine_settings.cpp`): the launcher owns
  `frame_rate_cap`, `fine_limiter_sleep`, `physics_frame_step`,
  `skip_logos`, `motion_controls` and `language`. It reads them from the file
  (the file wins over launcher.json) and writes them back on every change and
  before Play, rewriting those lines in place and appending missing ones;
  every other line (the Windows engine menu's graphics keys, `game_dir`,
  comments) stays as it is. The file's graphics keys (render scale,
  anti-aliasing, bloom, shadows, aspect ratio, `fullscreen`, …) drive the
  Direct3D 12 renderer only and are not offered: on the Mac the same things
  are the `PS3_*` variables above.
- **overlay.ini** (`PS3_OVERLAY_INI`): what the in-game menu (F1, Cmd+, or
  L3+R3) saves. The runtime applies it at start for every variable that is
  not set, so a menu choice holds unless the launcher passes that variable
  (which it does only when the setting is not the default).
- **Stop**: through the window host, SIGTERM to the `folklore-window` in the
  process group Play started (`pgrep -g <group> -x folklore-window`); the host
  closes the game itself (SIGTERM, SIGKILL 3 s later, as when its window is
  closed), exits, and run_mac.sh ends. Without a host in the group (the own
  window switch, or a build from before the host) SIGTERM goes to the whole
  group (zsh, run_mac.sh, the game), as the Resistance launcher stops its
  group. Either way the group gets SIGKILL 8 s later if anything of it is
  still there. Only processes in the launcher's own group are signalled; a
  `folklore` or `folklore-window` started from a terminal is only shown
  ("running, started outside the launcher") and keeps Play and the saves
  manager from acting.
- Quitting the app stops an install or a rebuild (its process group);
  setup_mac.sh continues where it stopped next time.

## Saves

`src-tauri/src/saves.rs`. Folklore saves through cellSaveData: one folder per
slot named after the title ID (`BCUS98147…`, `BCES00050…`) with PARAM.SFO,
ICON0.PNG and the game's files. On the Mac the toolkit writes them under the
working directory run_mac.sh uses (the repository):
`gamedata/dev_hdd0/home/00000001/savedata` (`PS3_GAMEDATA_ROOT` in the extra
variables moves it). `hdd0/home/00000001/savedata` (the `PS3_HDD0_ROOT`
side, which run_mac.sh creates) is listed too when slots are there. Each slot
shows its icon, title, subtitle, detail, size and date. Back up one or all,
delete, export to `.zip`, import a `.zip` (a zipped RPCS3 `savedata` works:
its `*<file>` keys become the port's PARAM.PFD), restore a backup. Backups go
to `save-backups/<UTC time>-<reason>/` with `backup.json`; one is made before
every Play (skipped when nothing changed; the newest N kept, default 10, 0 =
off) and before every restore, import and delete. A slot is built next to the
old one and swapped in by renaming. Everything refuses while the game runs.

## The Play shortcut and Add to Steam

As in the other launchers (`play_shortcut.rs`, `steam.rs`, `vdf.rs`,
`steam_art.rs`): `--play` on the command line starts the game once the
window is up (`open -b io.github.erikomis.folklorerecomp --args --play`);
Home > Status > **Play shortcut on the Desktop** writes `~/Desktop/Play
Folklore.app` running that line. **Add to Steam** (Steam installed, the
packaged app) writes a non-Steam entry "Folklore" into every account's
`shortcuts.vdf` (backed up first, other entries untouched; Steam must be
closed, the app offers to quit it), with library art drawn at that moment
from the player's own `ICON0.PNG` and `PIC1.PNG`; the entry runs `--play
--exit-with-game`, so Steam sees the game exactly as long as it runs.
**Remove from Steam** takes it out again. Both need the packaged `.app` (not
`tauri dev`).

## Self-update

The launcher has **no release feed yet**, so it does not update itself. The
code is kept (`src-tauri/src/update.rs`, `src/update.ts`): the manifest
check (`latest.json` in the shape tauri-plugin-updater reads, fetched with
`curl`), the "updated from" record and the restart. With `ENDPOINTS` empty
the check answers "not set up" without touching the network, the Updates
card says that the launcher is rebuilt from this folder (so **Get the new
sources** brings its new code too), and `install_update` refuses
(`blocked:nofeed`); the Download and install button never shows.
`tauri-plugin-updater` is not linked (it needs a public key in
`tauri.conf.json`, which only makes sense with signed releases). To turn it
on: publish signed releases with a `latest.json`, list its URLs in
`update::ENDPOINTS`, add the plugin back with `plugins.updater` (endpoints,
`pubkey`) in `tauri.conf.json`, and call its `download_and_install` in
`install_update` as the Resistance launcher does.
`FOLKLORE_NO_UPDATE_CHECK=1` turns the check off.

## Not done (from the Resistance launcher)

- **No game patches** of Resistance's kind (60 fps, field of view,
  RES_LEVEL, split-screen) and **no campaign / v1.7 / exitspawn / window
  host**: Folklore has none. Its frame-rate options are the game's own
  limiter keys above.
- **No RPCS3 side in the saves manager**, no RPCS3 check: the Mac flow does
  not use RPCS3. A zipped RPCS3 savedata folder can still be imported.
- **No disc mounting of its own beyond mac_common.sh's**: the disc is the one
  setup_mac.sh saved; another disc means installing from it (Advanced >
  Install / rebuild).
- **Debug-mode sessions** (frame captures, pass dumps, crash-report pickup,
  log triage) are replaced by the full-log Play and the logs .zip: run_mac.sh
  always writes `logs/run-mac.log` (so the session's log cannot be put in a
  folder of its own), and the capture trigger and dump paths would need
  plumbing through run_mac.sh. `PS3RECOMP_METAL_FRAME_DUMP` and the
  `RSX_ENGINE_PASS_DUMP*` variables still work through Extra variables.
- **Variables the Folklore runtime does not read on the Mac**, so not
  offered: `PS3_LUT_PRESET`,
  `PS3_FRAMECLOCK_US`, the tone curve and clarity are not in this toolkit;
  `PS3_LANG` is overridden by the game's own language hook
  (`folklore_system_language`), so the language is `folklore_settings.ini`'s.
- Screenshots and a mocked backend for them (`scripts/ui-mock.js` in the
  Resistance launcher), a CI workflow, a Windows build.
