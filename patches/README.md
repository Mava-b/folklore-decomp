# patches/

The changes this port made to [ps3recomp](https://github.com/sp00nznet/ps3recomp)
while bringing Folklore up.

`0001-folklore-on-upstream-de4f0903.patch` is all of them, applied on top of
upstream commit `de4f090328ea08bef6b2f15caec0b85e343a7ece` (v0.19.0 + 3,
2026-11-06). It was made by three-way merging the earlier series (below) with
upstream, which gained 789 commits since the old base, among them most of its
macOS / Metal work. Things the merge had to decide (where both sides fixed
the same thing, or a Folklore fix had to move onto upstream's newer code) are
commented in the code; the notable ones:

- the command ring (cellGcmSys.c): Folklore's back-pressure, laps and drain
  positions sit beside upstream's wrap-aware backlog and recycle wait; the
  ring's end is now upstream's (the wrap JUMP's slot), and Folklore's
  back-pressure is not set up when `GCM_FIFO_SEGMENT` is on;
- vertex fetch: upstream's 4-argument `rsx_vertex_fetch_many` (with its
  28-bit address wrap) replaces Folklore's batched version;
- callback stacks: upstream's per-host-thread slots replace Folklore's
  `PS3_CB_SCRATCH_STACK`;
- the boot harness runs the guest on its own 256 MB thread on macOS, with the
  main thread turning the Cocoa run loop (from the Demon's Souls port): with
  the guest on the main thread the Metal backend never drained the FIFO;
  and NSApp is set up (and finishes launching) on the main thread before the
  guest starts, or AppKit could bind its event queue to another thread and
  stop the game with SIGTRAP a second into some runs;
- `cellFsUnlink` never removes a directory, and a path with no file name is
  ENOENT;
- `cellFsStat`/`cellFsOpen`/`cellFsOpendir` answer ENOENT for an empty path.
  Folklore stats "" while looking for downloaded data each time it returns to
  the title. Off Windows the empty path mapped to a host folder that exists,
  so the game ran its installer over a list it never loaded and stopped on
  "An HDD access error occurred." (a black screen);
- macOS: the PS3's yes/no and OK dialogs appear as a sheet on the game window,
  answered with the controller (Cross/A, Circle/B), Return/Escape or the mouse
  (`MSGDIALOG_NATIVE=0` keeps the automatic answers);
- the Metal draw engine emulates alpha-to-coverage as an alpha test, as patch
  0020 did for D3D12 (grass and wire fences were solid cards);
- the Metal draw engine merges contiguous draw packets and queues up to 4096
  ranges per BEGIN/END, as patch 0019 did for D3D12 (at 256 the cliff and
  rock meshes lost about a quarter of their triangles);
- the Metal draw engine samples convolution-filtered textures bilinearly and
  applies the kernel in the shader (patches 0035/0036: the bloom), and passes
  SET_BLEND_COLOR on for the CONSTANT_* blend factors (patch 0038);
- the macOS toolkit of the Resistance and inFAMOUS 2 ports, merged in three
  ways from its own base (a6d76c8, v0.17.0): the in-game menu, internal
  resolution, MetalFX and window modes through the host window, colour LUTs,
  input remapping, audio volume, the vertex cache and the encoder state
  cache, texture packs, the shader cache and the quit gate. Its SPU
  scheduler, lifter output and FPCR changes are left out (Folklore's lifted
  code and SPURS depend on this side's), and VSCR[NJ] at thread start, the
  policy module's kernel writes and the event-queue retry stay as before;
- macOS: the PS3's yes/no and OK prompts are drawn by the in-game menu when
  the game runs inside the host window.

`v0.15-39a36296/` keeps the earlier series (0001-0107), one fix per patch,
in the order they applied on top of upstream commit
`39a36296511c445707adabcd312f0b740cfbd694`; 0084 and 0085 were carried onto
the merged code by hand, and 0086-0107 came in with git merges of this
port's main (they touch rsx_live_draw.c, cellGcmSys.c, cellSaveData.c,
rsx_null_backend.c and cellPad.c; the three HDR functions got no-op
versions for the builds without D3D12, and 0105's keyboard and mouse sits
in cellPad.c beside the merged toolkit's input code, Windows-only like the
rest of its Windows key reading, with no-op versions elsewhere). Each still explains its fix, and
`folklore/docs/STATUS.md` tells the story behind most of them.

The `ps3recomp/` folder in this repository already has the patch applied, so
setup.ps1 (and setup_mac.sh) use it as it is. If `ps3recomp/` is instead a git
clone of upstream (an older working folder), setup.ps1 resets it to the
pinned commit and applies `patches/*.patch`.
