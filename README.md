# Folklore (PS3) — native PC recompilation

Folklore (Game Republic / SCE, 2007) statically recompiled from its PS3
executable to native x86-64 Windows code with
[ps3recomp](https://github.com/sp00nznet/ps3recomp). There is no emulator: the
game's PowerPC and SPU code is translated to C++ and runs on a runtime that
stands in for the PS3's OS, with Direct3D 12 for graphics.

**This repository contains no game code and no Sony code.** You build the game
yourself from **your own disc** and **your own PS3 firmware file**; setup does
the translation on your PC.

**Status:** playable on the author's PC (Windows 11, Ryzen 7 7800X3D): it boots,
does the first-boot install, and plays, including capturing Folks with
the original motion controls. Expect rough edges.
[`folklore/docs/STATUS.md`](folklore/docs/STATUS.md) is the full engineering
log.

Extras over the PS3: 60 fps or unlimited frame rate (with physics stepped to
match), up to 3x resolution, ultrawide, FXAA, anisotropic filtering,
sharpening, borderless fullscreen, save backups.

---

## Requirements — read this first

You need **all three** of these. None of them are in this repository and none
can be downloaded from it.

### 1. Your own Folklore disc, North American release, v1.00

- **Title ID BCUS98147, disc version 1.00.** The code is translated from that
  exact executable; the European/Japanese releases and the 1.10 update will
  not work.
- Dumped from your disc to a **folder** (the "JB folder" format RPCS3 uses): a
  folder that contains `PS3_GAME\`, with `PS3_GAME\PARAM.SFO` and
  `PS3_GAME\USRDIR\EBOOT.BIN` in it. Your dump is only ever read, never
  written to.

### 2. The game's EBOOT, decrypted

The dumped `PS3_GAME\USRDIR\EBOOT.BIN` is encrypted. Decrypt it with
[RPCS3](https://rpcs3.net/): **Utilities → Decrypt PS3 Binaries**, choose that
`EBOOT.BIN`. RPCS3 writes **`EBOOT.elf`** beside it — leave it there.

The decrypted file must be **6,518,936 bytes**; setup checks this.

### 3. Your libsre.prx (Sony's SPURS library), decrypted

Folklore runs its SPU work through Sony's SPURS library, which lives in the
PS3 firmware, not in the game. You provide your own copy:

1. Install the PS3 firmware into RPCS3 (**File → Install Firmware**, with the
   official `PS3UPDAT.PUP` from PlayStation's website).
2. In RPCS3's folder, find **`dev_flash\sys\external\libsre.sprx`**.
3. Decrypt it the same way: **Utilities → Decrypt PS3 Binaries** →
   `libsre.sprx`. That writes **`libsre.prx`** beside it.

### Also needed

- **Windows 10 or 11, 64-bit**, a Direct3D 12 graphics card, and a CPU with
  AVX2 (x86-64-v3: Intel Haswell / AMD Zen or newer).
- About **30 GB** free on the drive you clone to (tools, build files, and the
  game's own first-boot install of several GB).
- An internet connection for setup (it downloads its tools).
- A controller: any XInput pad, or a **DualShock 3** with
  [DsHidMini](https://github.com/nefarius/DsHidMini) in **SXS** mode for the
  original motion controls.

---

## Building

Clone this repository (or download it as a ZIP and unzip it) somewhere with
the space free — a short path such as `D:\Folklore` is best. Open
**PowerShell** in that folder and run these one at a time:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1
powershell -ExecutionPolicy Bypass -File .\build.ps1
powershell -ExecutionPolicy Bypass -File .\run.ps1
```

1. **setup.ps1** (15–30 minutes, once)
   - **First it asks for your files**: a folder picker for your dump (the
     folder that contains `PS3_GAME`), then a file picker for your decrypted
     `libsre.prx`. Each is checked; if something is wrong (still encrypted,
     wrong version, wrong file) it says what and asks again. You can also pass
     them: `.\setup.ps1 -GameDir D:\Games\BCUS98147 -LibsrePrx D:\RPCS3\dev_flash\sys\external\libsre.prx`
   - Then it runs on its own: downloads Git, CMake, Ninja, Python and FFmpeg
     into `tools\`, and — only if you have no Visual Studio with clang-cl —
     Visual Studio 2022 Build Tools (about 6 GB, one UAC prompt; the Visual
     Studio Installer always goes on C:).
   - Translates the game's code from your `EBOOT.elf` into
     `folklore\src\recomp\` (about a minute) and Sony's SPURS library from your
     `libsre.prx` into `folklore\firmware_lle\`. Both stay on your PC.
   - Builds the ps3recomp runtime library.
2. **build.ps1** compiles about 1.5 million lines of translated C++: roughly
   10–30 minutes the first time on a modern 8-core CPU, incremental after
   that. Result: `folklore\build\folklore.exe`.
3. **run.ps1** starts the game and writes `logs\run-<date-time>.log`. It
   uses the game folder you gave setup (saved in `folklore_settings.ini`), and
   the game's first boot copies several GB of data into `hdd1\`.

After the first build you can also start `folklore\build\folklore.exe`
directly (see *Running folklore.exe on its own*).

### If something fails

- Every step writes a log in `logs\` (`setup-*.log`, `build.log`,
  `run-*.log`); the error message names the one to look at.
- **setup can't find clang-cl after installing Build Tools**: open *Visual
  Studio Installer* → Modify → Individual components → tick *C++ Clang Compiler
  for Windows* and *MSBuild support for LLVM (clang-cl)*, then run setup.ps1
  again.
- **"EBOOT.BIN is still encrypted" / "libsre.sprx is still encrypted"**:
  decrypt it with RPCS3 as described above and choose the `.elf` / `.prx` it
  writes.
- **"not the BCUS98147 v1.00 EBOOT"**: your disc is another region or the
  EBOOT is from the 1.10 update. Only the North American v1.00 disc works.
- setup and build are safe to re-run; each step skips what is already done.

---

## Playing

### Engine menu

Press **L3 + R3 together** (or **F1**) in game. D-pad / left stick or the arrow
keys move, Cross / A or Enter changes a setting, Circle / B, Start or Esc
closes. The game ignores the controller while the menu is open. Settings are
saved to `folklore_settings.ini`.

- **Frame rate**: 30 fps (the game's own limiter), 60 fps or unlimited.
  Above 30 is experimental — the game was built for 30.
- **Precise frame limiter**: wakes every 1 ms instead of every 10 ms, so
  frames end on time (on by default).
- **Controls**: motion controls for capturing Folks — **Automatic** (motion when
  a DualShock 3 is connected, otherwise captures complete by themselves),
  **SIXAXIS (original)** or **Auto-capture**.
- **Physics**: step physics by the frame time, which keeps cloth and hair
  stable above 30 fps (on by default).
- **Graphics**
  - *Fullscreen* (borderless; **Alt+Enter** toggles it any time).
  - *Render resolution*: 1x (1280×720, the PS3's), 2x or 3x. Applies on the
    next start.
  - *Aspect ratio*: 16:9, match display, 21:9 or 32:9 — a wider field of view,
    not a stretched picture (menus and HUD are stretched). Next start.
  - *Anti-aliasing* (FXAA), *Anisotropic filtering* (up to 16x), *Sharpening*.
  - *Build new shaders in the background* (no hitch when an effect first
    appears).
- **Game files**: the game folder in use (change it here; it applies on the
  next start) and where the installed data and saves are.
- **Performance overlay**: fps, frame time and where the time goes.
- **Save data snapshots**: copy the game's saves to `save_snapshots\` and put
  any snapshot back later, then load it from the game's own Load menu.

Escape closes the menu; it does not quit the game (use the window's close
button or Alt+F4).

### DualShock 3 / SIXAXIS

Folklore captures Folks with motion gestures. With
[DsHidMini](https://github.com/nefarius/DsHidMini) set to **SXS** mode
(change it in DSHMC, the DsHidMini control app), the game reads the DualShock
3 directly — buttons, sticks, pressure and the motion sensors — on controller
port 0. XInput mode has no motion data; without a DS3, use *Automatic* or
*Auto-capture*.

### run.ps1 options

```powershell
.\run.ps1 -GameDir D:\other\BCUS98147     # a different dump (otherwise the saved one)
.\run.ps1 -DevFlash D:\RPCS3\dev_flash    # firmware fonts (optional; or dev_flash= in the ini)
.\run.ps1 -ResetInstall                   # wipe hdd0/hdd1 and redo the first-boot install
.\run.ps1 -Verbose                        # very detailed logging (slow)
.\run.ps1 -Profile                        # report where the CPU time goes (every 10 s)
.\run.ps1 -NoLibsre                       # ps3recomp's own SPURS stand-in instead (crashes; testing)
.\run.ps1 -InterpJobs                     # SPU jobs in the interpreter (A/B, slower)
.\build.ps1 -Clean                        # full rebuild
$env:AUDIO_LATENCY_MS = '48'; .\run.ps1   # more audio buffering if sound breaks up (default 32)
```

### Running folklore.exe on its own

`folklore\build\folklore.exe` also starts without run.ps1 (double-click it or
a shortcut). It asks once for the game folder and remembers it, sets the same
runtime options run.ps1 sets, keeps `hdd0\`, `hdd1\`, `gamedata\` (saves), the
shader cache and `logs\` beside the exe (or in `data_dir` from
`folklore_settings.ini`), and writes its log to `logs\run-<date-time>.log`.

### Render capture (for bug reports)

When something draws wrong, press **F2** (or the PS button on a DualShock 3)
while it is on screen, or double-click `capture.bat`. The next four seconds of
draws go to `logs\capture-<date-time>\`.

---

## Sharing what you built

```powershell
.\package.ps1 -Kind BuildKit     # dist\Folklore-buildkit-<date>.zip: this repository's contents
.\package.ps1                    # dist\Folklore-<date>.zip: your built game, ready to run
```

The build kit contains nothing from the game or the firmware — the same as
this repository. The **binaries** package contains your `folklore.exe`, which
*is* Folklore's code translated and compiled, plus Sony's libsre lifted from
your firmware: giving that to someone else distributes their code. Point
people at this repository instead.

---

## Repository layout

| Path | What it is |
|---|---|
| `setup.ps1`, `build.ps1`, `run.ps1` | Set up, build, run (see above) |
| `package.ps1`, `capture.bat` | Packaging; render capture |
| `common.ps1` | Paths and helpers the scripts share |
| `folklore/` | The port: CMake project, Folklore-specific HLE, the engine menu and settings (`src/engine`), the lifting scripts (`tools/`) and the engineering log (`docs/STATUS.md`) |
| `folklore/src/recomp/` | Empty here; setup fills it with the game's code translated from **your** EBOOT |
| `folklore/firmware_lle/` | Not in the repository; setup creates it from **your** libsre.prx |
| `ps3recomp/` | The recompiler and runtime: [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp) at commit `39a3629`, with everything in `patches/` already applied |
| `patches/` | The changes this port made to ps3recomp, one patch per fix, kept as a record (and so they can go upstream) |

Created by the scripts and ignored by git: `tools/`, `logs/`, `hdd0/`,
`hdd1/`, `gamedata/`, `dist/`, `folklore/build/`, `ps3recomp/build/`, the
lifted sources and `folklore_settings.ini`.

## Credits and licences

- **ps3recomp** © Ned Heller / sp00nznet, MIT licence (`ps3recomp/LICENSE`).
- **Dear ImGui** © Omar Cornut, MIT licence (`folklore/src/third_party/imgui`).
- **FFmpeg** (LGPL) is downloaded by setup for the cutscenes; it is not part
  of this repository.
- Folklore is © Sony Interactive Entertainment / Game Republic. This project
  is not affiliated with or endorsed by Sony or Game Republic, contains none
  of their code or data, and requires your own legally obtained copy of the
  game and firmware.
