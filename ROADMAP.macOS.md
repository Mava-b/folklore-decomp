# Roadmap (macOS)

🌐 **English** · [Português (BR)](ROADMAP.macOS.pt-BR.md)

Status of the native Folklore port to macOS/arm64 (disc version 1.00,
`BCUS98147` and `BCES00050`; the Mac build was tested with `BCUS98147`) and
what comes next. The Windows build is in [README.md](README.md) and its
engineering log in [`folklore/docs/STATUS.md`](folklore/docs/STATUS.md).
`[x]` = done and verified; `[ ]` = to do.

## Status at a glance (2026-11-07)

- **Runs:** the build from your own disc with `setup_mac.sh`; boot, the
  opening logos and movies, the title screen, Options and Load Game, a new
  game into cutscenes, Doolin and the story pages, with Folk captures (boss
  captures too) completing by themselves. Tested on an M4 Pro with macOS 27.
- **The window:** since 3b5d899 the game plays through ps3recomp's host
  window (`folklore-window`, inside `folklore.app`): MetalFX, windowed /
  macOS fullscreen / borderless, a display of choice, vsync. Tested at 1.5x
  internal resolution with MetalFX to 2560x1440. `FOLKLORE_HOST_WINDOW=0`
  opens the game's own window as before.
- **Toolkit:** ps3recomp updated to upstream v0.19.0+3 (de4f0903) with
  Folklore's patches merged onto it, then the macOS toolkit of the
  Resistance and inFAMOUS 2 ports merged in (in-game menu, internal
  resolution, remapping, caches). The whole of it is one patch,
  `patches/0001-folklore-on-upstream-de4f0903.patch` (regenerated in 731fb78).
- **Speed:** not measured in numbers on the Mac yet.
- **What is left for "playable on Mac":** play the story further and fix what
  that turns up, a long session for speed and stability, and motion controls.
- **Not yet:** motion sensors, the 1.10 update in the Mac scripts, the
  Windows engine menu and PC extras on Metal, DLC import on the Mac, a
  Windows build of the merged runtime, a release of the app.

---

## What is missing to be 100% playable

### Blockers

- [ ] Play the story on the Mac past the first chapters, writing down what
  breaks. So far: a new game into Doolin and the story pages.
- [ ] A long windowed session (an hour or more) for the frame rate and any
  stall or leak. The long-session fixes from the other ports are in
  (bbce67d), not yet checked here over hours.
- [ ] Measure the frame rate on the Mac (`PS3RECOMP_METAL_FPS=1`, the
  in-game menu's HUD), at 1x and with `PS3_RES_SCALE`, and with the game's
  own frame-rate cap above 30 (`folklore_settings.ini`'s `frame_rate_cap`,
  read on the Mac; not measured).

### Graphics

- [ ] Compare against the Windows build, scene by scene, for anything the
  Metal renderer still draws differently. Fixed so far: grass and fences,
  holes in cliffs and rocks, the bloom grid, the blend colour (see Done).
- [ ] The PC extras of the Windows build (ultrawide, ambient occlusion, soft
  shadows, the smooth depth of field and bloom, the cleaner Id souls, finer
  distant textures) live in the Direct3D 12 renderer (`rsx_live_draw.c`);
  none of them is on Metal. Each needs porting on its own.
- [ ] First-use shader hitches: the shader cache keeps them to once per
  shader; `PS3_SHADER_ASYNC=1` (builds in the background) is untried in
  Folklore.

### Stability

- [x] The SIGTRAP about one start in three (AppKit's event queue bound to
  another thread): fixed in b1b5c35, six starts in a row ran.
- [x] The black screen after Options or Load Game ("An HDD access error
  occurred."): fixed in 65ff3ed (an empty path is ENOENT).
- [x] The pad kept from the game after a prompt (the title stopped taking
  START): fixed in c585e18, the pad comes back within 1.5 s.
- [ ] Hours-long sessions: see Blockers.

### Saves

- [x] Saves through the toolkit's cellSaveData, in
  `gamedata/dev_hdd0/home/00000001/savedata` beside `run_mac.sh`, the same
  layout as the Windows build; the app lists, backs up and restores them.
- [ ] Saving and loading over a long session on the Mac: to check.
- [ ] Moving saves between the Windows and Mac builds: same layout, not
  tried.

### Controls

- [x] Controller through SDL: buttons and sticks.
- [x] The PS3's yes/no and OK prompts answered by the player: a sheet on
  the game's window (f8ced14), drawn by the in-game menu in the host window
  (3b5d899).
- [x] Remapping: the in-game menu's Controls tab (live), the app, or
  `PS3_INPUT_MAP`.
- [ ] **Motion controls:** read the controllers' gyro through SDL, so a
  capture is played by moving the controller as on the PS3. Until then the
  capture assist completes every capture by itself.
- [ ] Keyboard and mouse: the toolkit's layout (Space = Cross, WASD) works
  through the window; not play-tested in Folklore. A layout chosen for
  Folklore (the Windows build polls Z/X/A/S) would be better than the
  shooter layout the toolkit ships.
- [ ] Rumble: comes with the 1.10 build (next item).

### Rebuild

- [x] One script from the disc to the game: `setup_mac.sh` (Homebrew
  packages, Python environment, decrypting with the port's own decrypter,
  lifting, register promotion, the build). The build itself takes about
  2.5 minutes on an M4 Pro.
- [ ] **The 1.10 update** in the Mac scripts: `setup_mac.sh` recognizes the
  1.10 EBOOT's size, but takes no update folder (`setup.ps1 -UpdateDir` on
  Windows), so a Mac build is always the disc's 1.00. The 1.10 build brings rumble and the newer
  Folk table that later DLC uses.
- [ ] **A Windows build of the merged runtime:** ps3recomp v0.19.0+3 with the
  macOS toolkit has not been built or run on Windows yet.

## After it is playable (improvements)

### Graphics and resolution

- [x] Internal resolution (`PS3_RES_SCALE`, 1x to 4x), MetalFX spatial
  upscaling, window modes, display choice and vsync, through the host window.
- [x] FXAA / SMAA, sharpening, anisotropic filtering, vibrance, contrast,
  vignette, grain, colour LUTs, texture packs: from the toolkit merge.
- [ ] Ultrawide and the other Windows extras on Metal (see Graphics above).
- [ ] MetalFX temporal upscaling, HDR (EDR): not started.

### App to install and play (launcher)

- [x] The app in `launcher/` (Tauri, from the Resistance and inFAMOUS 2
  launchers): install and rebuild with progress, settings, controls editor,
  saves manager, game log, a logs `.zip`, Play shortcut, Add to Steam.
- [ ] (being done now) The host window's options back on the Graphics page
  (MetalFX, display mode, window size, display, vsync), and an Advanced
  switch "use the game's own window" (`FOLKLORE_HOST_WINDOW=0`).
- [ ] A release: a signed `.dmg` and a feed, so the app can update itself.
- [ ] DLC import on the Mac: the Windows engine menu's **Add DLC from
  RPCS3...** (`.rap` licences); `engine_dlc.cpp` already builds on macOS
  (CommonCrypto), the menu around it does not.

### Performance

- [ ] A NEON path for the PPU's vector code (VMX): today `ppu_vmx.h` takes
  its portable path on arm64.
- [ ] Profile a busy scene (Doolin's streets, a boss) once the frame rate is
  measured.

### Other platforms

- [ ] The Windows build stays the main one (Mava-b's); the Mac work should
  keep it building (see Rebuild).

### Out of scope for now

- [ ] The Windows engine menu itself (Dear ImGui on Direct3D 12): on the Mac
  its settings are the toolkit's in-game menu and `PS3_*` variables, plus
  `folklore_settings.ini` for the game's own options.

## Done and finished

### Build and pipeline

- [x] The game builds for macOS on Apple silicon (1e356fd): the settings file
  found with `_NSGetExecutablePath`, the display size from CoreGraphics, DLC
  decryption with CommonCrypto, no x86 `-march` on arm64, FFmpeg from
  Homebrew for the movies, the same 256 MB main stack Windows gets.
- [x] `setup_mac.sh`, `run_mac.sh`, `mac_common.sh` (78d80a8): a disc image
  mounted read only at `gamedisc/`, the EBOOT decrypted with the port's own
  decrypter, the switches `run.ps1` sets (`GCM_WAITFLIP_DEPTH` among them).
- [x] `folklore.app` with Game Mode (853ee36), its icon made from your disc's
  `ICON0.PNG` at setup (never shipped); the in-game menu's settings in
  `overlay.ini`, compiled shaders in `folklore/build-mac/shader_cache`,
  `FOLKLORE_EXE` to run another build.
- [x] The host window (3b5d899): `folklore-window` presents the game's
  frames with MetalFX and the window modes; closing it ends the game cleanly.

### From the title to gameplay

- [x] Boot, opening logos, movies, the title screen.
- [x] Options and Load Game, and back to the title (65ff3ed).
- [x] New game, cutscenes, Doolin, the story pages.
- [x] Captures complete by themselves on macOS, boss captures too: the
  capture assist runs on a thread of its own there (ef35b63); the capture
  finder (F5-F9) reads guest memory with `mach_vm_region`.

### Graphics

- [x] Grass and wire fences as solid cards: alpha-to-coverage as an alpha
  test (f8ced14, as patch 0020 for Direct3D 12).
- [x] Holes in cliffs and rocks: no 256-packet cap on one draw (f628bd5, as
  patch 0019).
- [x] Bloom as a grid of squares: the convolution filter in the shader;
  the blend colour passed to Metal (25c4701, as patches 0035/0036 and 0038).
- [x] Small mip levels drawn as stripes (a linear mipmapped texture's pitch),
  NV308A inline uploads over the whole colour range (bbce67d).

### Stability

- [x] The guest on its own 256 MB thread, the main thread turning the run
  loop (from the Demon's Souls port; ac8c6cd).
- [x] NSApp set up on the main thread before the guest (b1b5c35).
- [x] `cellFsUnlink` never removes a directory (f91c283).
- [x] Bigger Metal and draw-engine tables, an autorelease pool per
  frame-clock pass (about 2 GB leaked in three hours before), App Nap and
  timer coalescing off (bbce67d, from the Resistance 2, PES 2013 and
  inFAMOUS 2 ports).

### Toolkit

- [x] ps3recomp from v0.15 (39a36296) to upstream v0.19.0+3 (de4f0903), 789
  commits, Folklore's 83 patches merged onto it (ac8c6cd).
- [x] The Resistance / inFAMOUS 2 macOS toolkit merged in (76b64d4): 341
  conflicting hunks in 64 files; what Folklore runs by default unchanged.
- [x] One patch for all of it on de4f0903 (0efc356, regenerated in 731fb78);
  the old 85-patch series kept in `patches/v0.15-39a36296/`.

### Credits

- [Mava-b](https://github.com/Mava-b/folklore-decomp): the Folklore port
  itself, its SPURS, renderer fixes and the Windows build.
- [sp00nznet](https://github.com/sp00nznet/ps3recomp): ps3recomp.
- The author's Demon's Souls, Resistance 1 and 2, inFAMOUS 2 and PES 2013
  ports: the macOS toolkit and the fixes named above.
