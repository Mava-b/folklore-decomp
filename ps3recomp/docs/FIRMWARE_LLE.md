# Firmware LLE: Lifting a Decrypted Firmware Module

A method for running a real, lifted PS3 firmware module instead of hand-written HLE for it.
This document covers the SPURS kernel (`libsre.prx`) as the motivating case, but the pipeline
applies to any decrypted PRX from `dev_flash`, not just SPURS.

---

## Table of Contents

1. [Why LLE a firmware module](#why-lle-a-firmware-module)
2. [Prerequisite: you supply the firmware](#prerequisite-you-supply-the-firmware)
3. [Pipeline overview](#pipeline-overview)
4. [Step 1: obtain the decrypted PRX](#step-1-obtain-the-decrypted-prx)
5. [Step 2: relocate and lift with `lift_prx.py`](#step-2-relocate-and-lift-with-lift_prxpy)
6. [Step 3: lift the image to C](#step-3-lift-the-image-to-c)
7. [Step 4: load, patch, and bind at runtime](#step-4-load-patch-and-bind-at-runtime)
8. [What is proven vs. what is expected](#what-is-proven-vs-what-is-expected)
9. [SPU correctness is a prerequisite](#spu-correctness-is-a-prerequisite)
10. [Legal and redistribution](#legal-and-redistribution)
11. [Clean-room notes](#clean-room-notes)

---

## Why LLE a firmware module

ps3recomp's default approach to system libraries is HLE: reimplement each `cellXxx` /
`sys_xxx` call in C against a NID table, per [ARCHITECTURE.md](ARCHITECTURE.md) and
[GAME_PORTING_GUIDE.md](GAME_PORTING_GUIDE.md). That works well for most of the cell libraries.
It works poorly for `cellSpurs`.

SPURS is not a thin syscall wrapper, it is a scheduler: a cooperative task-dispatch kernel
that runs its own loop on the PPU and SPUs, manages workload queues, context-switches SPU
tasks, and coordinates through event flags and semaphores that the game's own code also
touches directly. Reimplementing that loop by hand means re-deriving its internal state
machine well enough to satisfy every title that depends on it, and in practice this has been
a source of endless problems on essentially every SPURS title, even minimal ones: the HLE
scheduler diverges from the real one in some corner (task readiness, priority, an event-flag
protocol variant) and a game hangs or livelocks in a way that is hard to trace back to the
scheduler itself.

The alternative in this document sidesteps the reimplementation problem: statically lift the
real SPURS kernel out of a decrypted firmware PRX and run that instead. The scheduler loop is
then authentic Sony code executing on our PPU/SPU recompiled runtime, so games launch their
tasks the same way they do on real hardware, because they are talking to the real kernel. This
is the same static-recompilation approach ps3recomp already applies to game code, applied one
layer down to a system module.

This is not SPURS-specific as a technique. Any firmware PRX that a game's HLE surface has
trouble emulating faithfully is a candidate: the tooling here relocates and lifts a decrypted
PRX regardless of which module it is.

## Prerequisite: you supply the firmware

**ps3recomp ships no firmware and no output derived from firmware.** To use this method you
need your own decrypted `dev_flash` PRX (for example `libsre.prx` for the SPURS kernel),
obtained from a PS3 firmware package you are entitled to decrypt. Decryption itself is out of
scope for this project; use your own tools and keys for that step.

Nothing produced from a firmware module, the decrypted PRX itself, the relocated image, the
generated JSON metadata, or the lifted C source, belongs in this repository or in any PR to
it. All of that is project-gitignored and must stay that way. If you are preparing a
contribution that involves this pipeline, only the game-agnostic tooling and documentation are
in scope; your own lifted kernel output stays local.

## Pipeline overview

```
  libsre.prx (decrypted, from your dev_flash)
       |
       v
  +-------------------+
  | lift_prx.py        |  Step 2: relocate + emit metadata
  +-------+-----------+
          |  <name>_image.bin, _functions.json, _exports.json, _imports.json
          v
  +-------------------+
  | ppu_lifter.py       |  Step 3: lift the image to C (--raw --base mode)
  | --raw --base        |
  +-------+-----------+
          |  C source, compiled into the runtime like any other module
          v
  +-------------------+
  | runner (per-game)   |  Step 4: load image at --base, patch import stubs,
  | import binding       |  bind the game's SPURS NIDs to the lifted exports
  +-------------------+
          |
          v
     game calls the real, lifted SPURS kernel
```

## Step 1: obtain the decrypted PRX

Extract the module you want to LLE from your own decrypted `dev_flash`. For SPURS this is
`libsre.prx`. The file must be a plain decrypted ELF64 big-endian PRX (`e_type` 0xFFA4, PPC64),
not the encrypted SELF-wrapped form Sony ships on disc; if you only have the encrypted form,
decrypt it first with your own tooling (this is the same class of step as decrypting a game's
`EBOOT.BIN`, see [GAME_PORTING_GUIDE.md](GAME_PORTING_GUIDE.md), Phase 2).

## Step 2: relocate and lift with `lift_prx.py`

```bash
python tools/lift_prx.py libsre.prx --base 0x02000000 --output out/
```

`--base` is the guest address the module's LOAD segments are placed at; pick a region that
does not collide with your game's own memory map. `lift_prx.py` reads the PRX's ELF headers
and its `SCE_PPURELA` relocation section, places each LOAD segment at `base + p_vaddr`, and
applies every relocation in place (types ADDR32, ADDR16_LO/HI/HA, REL64; an unrecognized type
raises rather than silently mis-relocating).

It writes four files into `--output`:

| File | Contents |
|------|----------|
| `<name>_image.bin` | The relocated flat image; load this at `--base` in the runner. |
| `<name>_functions.json` | Function seeds for `ppu_lifter.py` in `--raw --base` mode: every exported function's code address plus internal functions found by scanning the image for `{code, toc}` OPD pairs (needed because some entry points, such as PPU handler threads SPURS hands to `sys_ppu_thread_create`, are referenced only through data, never through a direct branch, and would otherwise be invisible to branch-following discovery). |
| `<name>_exports.json` | `{library: {fnid_hex: opd_ea_hex}}`, the module's own exported functions by NID, at their real relocated addresses. This is what a game's import calls get bound to. |
| `<name>_imports.json` | `{library: {fnid_hex: stub_slot_ea_hex}}`, the module's own imports (SPURS itself calls into other firmware libraries): the stub slot addresses a runner would patch if it also wants to satisfy those imports. |

The tool prints a summary (module name, base, TOC, relocation counts by type, export/import
counts, function seed count) so you can sanity-check the lift before proceeding.

## Step 3: lift the image to C

Feed `<name>_functions.json` to `ppu_lifter.py` as the function boundary list, in the lifter's
raw-image mode (load a flat binary at a fixed base rather than parsing an ELF with its own
entry point):

```bash
python tools/ppu_lifter.py out/libsre_image.bin --raw --base 0x02000000 \
    --functions out/libsre_functions.json --output recomp_prx/
```

This produces ordinary lifted C source, the same kind [PPU_RECOMP.md](PPU_RECOMP.md) and
[SPU_LIFTER.md](SPU_LIFTER.md) describe for game code, compiled into your project like any
other translation unit. The seeds in `_functions.json` are region-bounded; the lifter's normal
discovery pass (branch targets, tail entries) fills in interior functions within each region.

## Step 4: load, patch, and bind at runtime

This step is per-game integration and is intentionally not covered by generic tooling here,
the same way ps3recomp's existing game runner pattern (see the `host_main` / runner code a
port project builds per [GAME_PORTING_GUIDE.md](GAME_PORTING_GUIDE.md)) is per-game. At
a high level, a runner that wants to use a lifted firmware module needs to:

1. **Load the image.** Copy `<name>_image.bin` into guest memory at `--base` (or map it there),
   the same way the runner already places the game's own ELF segments.
2. **Patch the module's own import stub slots.** Walk `<name>_imports.json` and write your
   HLE bridge descriptor (whatever the runtime's NID dispatch convention is, see
   `import_stubs.cpp` / `nid_dispatch()` as described in
   [GAME_PORTING_GUIDE.md](GAME_PORTING_GUIDE.md)'s HLE bridge section) into each stub slot
   address, exactly as you would for a game's own unresolved imports.
3. **Bind the game's imports to the lifted module.** For every NID in the game's own import
   table that belongs to the firmware module's library (for SPURS: the `cellSpurs`/SPURS NIDs),
   look up that NID in `<name>_exports.json` and point the game's import stub at that address
   instead of an HLE bridge. From here on, the game's calls into that library reach the real,
   lifted kernel code instead of a hand-written stand-in.

After this, the game's own SPURS calls run the actual scheduler loop: task creation, dispatch,
and the SPU-side context switch machinery are all the lifted firmware code, not an
approximation of it.

## What is proven vs. what is expected

**Proven (validated on one title so far, Yakuza: Dead Souls):** the lifted SPURS scheduler
loop runs stably under this project's PPU/SPU recompiled runtime and dispatches a real game's
tasks, including SPU-side task context switches driven by the real kernel rather than an HLE
approximation.

**Expected, not yet generalized:** that this unblocks every title stuck on the SPU task
pipeline. The technique is not tied to SPURS or to this one game (the tooling relocates and
lifts any decrypted PRX the same way), but it has only been exercised end to end on this one
title and one firmware module so far. Treat "this will fix your SPURS hang" as a hypothesis to
test on your own title, not a guarantee.

## SPU correctness is a prerequisite

The lifted SPURS kernel includes real SPU code, the scheduler's SPU-side dispatcher and
context-switch logic, which now actually executes on ps3recomp's SPU recompiled runtime
instead of being bypassed by HLE. That surfaced SPU instruction decode and semantic bugs that
existing game SPU workloads had not exercised (rare opcodes, floating-point estimate
instructions, condition-handling edge cases). Getting the lifted kernel to run correctly
therefore doubles as a stress test of the SPU lifter itself; fixing what it turns up is a
practical prerequisite for this method to work, not an unrelated side project.

## Legal and redistribution

This is not legal advice, but the risk contours are worth stating plainly, because static
recompilation differs from emulation in one way that matters here.

The tooling and this document are the safe part. `lift_prx.py` is a general-purpose relocator
that transforms a PRX the user supplies; it contains no firmware and does no decryption, the
same footing as any disassembler or an emulator's module loader. Running it on firmware you
dumped from your own console, for your own use, is the same bring-your-own-firmware model that
established emulators operate under.

The exposure is in what gets distributed, and static recompilation raises the stakes versus an
emulator. An emulator loads firmware at runtime, so the firmware bytes stay on the user's
machine and the emulator itself ships clean. Lifting a firmware module to C and compiling it
into a shipped binary is different: that binary now contains a translation, a derivative work,
of the module. A standalone executable with lifted firmware baked in is closer to
redistributing the firmware than a general emulator is. Firmware is licensed system software,
not something owned outright the way a purchased game disc is, so distributing a built artifact
that embeds lifted firmware is legally fraught and is the user's own responsibility. Keep the
lifted output local; do not distribute binaries that embed it without understanding that step.

Decryption is deliberately outside this pipeline. `lift_prx.py` takes an already-decrypted PRX,
which keeps the tooling clear of anti-circumvention questions that are separate from, and
touchier than, copyright.

## Clean-room notes

This method touches firmware, so the project's clean-room rules apply without exception:

- No Sony SDK material is used anywhere in this pipeline or its tooling.
- No firmware bytes, decrypted or otherwise, and no output derived from lifting firmware
  (image, JSON metadata, generated C) are ever committed to this repository.
- The tooling (`lift_prx.py`) only implements the public PRX/ELF relocation format as
  documented by third-party PS3 research (the same class of format-level knowledge the rest of
  `tools/` already relies on for ELF and PRX parsing), it contains no Sony code or data.
