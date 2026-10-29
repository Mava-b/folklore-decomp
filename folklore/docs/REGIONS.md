# Supported releases

| Release | Title ID | Version | Decrypted `EBOOT.elf` | TOC (r2) | Import stubs end |
|---|---|---|---|---|---|
| North America | BCUS98147 | 1.00 | 6,518,936 bytes | `0x56EB00` | `0x4EEB84` (167 imports) |
| Europe | BCES00050 | 1.00 | 6,518,912 bytes | `0x56EAF8` | `0x4EEB1C` (166 imports) |
| North America | BCUS98147 | 1.10 update | 6,520,504 bytes | `0x56EC48` | `0x4F32B4` (170 imports) |
| Europe | BCES00050 | 1.10 update | 6,520,480 bytes | `0x56EC48` | `0x4F326C` (169 imports) |

A build is lifted from one executable and runs only that one. `tools/relift.sh`
recognises the EBOOT by its size and writes `src/recomp/folklore_region.h`
(region, title ID, version, size, TOC). The engine code reads that header
(`src/engine/engine_region.h`); lifted code from before it existed counts as
the US disc, and a header without a version as v1.00. `setup.ps1 -GameDir
<dump of the other release>` lifts that release instead, `setup.ps1 -GameDir
<dump> -UpdateDir <installed 1.10 update>` lifts the update, and `build.ps1`
then rebuilds everything.

## How the two executables differ

The comparison was made on 2026-10-26 from a BCES00050 v1.00 dump. Both
executables were split into functions with ps3recomp's `ppu_loader.py` and
`find_functions.py`: 13,298 functions in the US one and 13,297 in the EU one.
The function lists were then aligned in order, comparing each function's
instructions with the address-carrying fields masked out: branch
displacements and 16-bit immediates.

- **13,284 functions are the same code.** Most sit a few bytes away (-0x48,
  0, -0x40, +4 ...), and their TOC offsets and data addresses differ.
- **Ten places differ in content**:
  - US `0x13F9F8`, `0x172F4C`, `0x178C68`, `0x17A350`/`0x17A3A8`;
  - `0x1A2844`/`0x1A289C`, the movie player set-up, 0x23C bytes longer in the
    EU build;
  - `0x1FFA7C`/`0x200000`, `0x20C320`, `0x216280`, `0x21FC6C`, `0x26AEC8`;
  - one import stub fewer: the EU build does not import `sceNp` NID
    `0x29DD45DC`.
- **The nine SPURS job binaries are byte-identical**, at the same addresses.
  `src/spu_gen` and `src/spu_gen_lle` come out the same for both releases, and
  the native SPURS (docs/SPURS.md) needs nothing per release.
- **The TOC is laid out a few entries differently**: most entries the code
  uses moved by 0, -4, -8, -0x10 or -0x20 bytes. The two the movie-table fix
  reads are -0x20C0 → -0x20E0 and -0x263C → -0x265C. The seven camera
  constants the aspect-ratio setting changes (all 16:9 in both) are
  -28188 → -28188, -16228 → -16232, -13536 → -13544, -13364 → -13372,
  -6676 → -6692, 316 → 284 and 4360 → 4328. These were found by pairing
  every TOC-relative load in the aligned functions.

## Where each fix goes

| Fix | US | EU |
|---|---|---|
| Movie hold wait (`game_patches.py`) | `func_001A4780` @ `0x1A48C4` | `func_001A4784` @ `0x1A48C8` |
| Movie AU size table, two sites | `func_001A289C` @ `0x1A3124`, `0x1A33F4` (calls returning to `0x1A3128`..`0x1A314C`, `0x1A33F8`..`0x1A341C`) | `func_001A2664` @ `0x1A30E0`, `0x1A33F8` (returns `0x1A30E4`..`0x1A3108`, `0x1A33FC`..`0x1A3420`) |
| Room for the table after the player | `func_001A3B78` @ `0x1A3BA4` | `func_001A3B7C` @ `0x1A3BA8` |
| Havok step by frame time | `func_003C913C` (labels `3C9184` / `3C9170`, exit `3C91A0`) | `func_003C90F4` (`3C913C` / `3C9128`, exit `3C9158`) |
| Switch-table hint (`relift.sh`) | `34D3C8:34D3CC:19` | `34D380:34D384:19` |
| Frame limiter (`patch_limiter.py`) | 10 sites, found by pattern | the same 10 |

## The 1.10 update

Folklore's 1.10 update replaces the executable; RPCS3 (like a PS3) installs
it in `dev_hdd0/game/<title id>/` and starts that EBOOT, which reads the rest
of the game from the disc. The comparison below was made on 2026-10-29 from
both regions' updates, decrypted with RPCS3 from the user's own installs.

**What changed.** Aligned against v1.00 the same way (13,336 functions in US
1.10, 13,335 in EU 1.10):

- 12,359 functions are the same code. About 230 of the game's own functions
  changed and 14 are new; most of the rest of the difference (~600
  functions above `0x480000`) is the SDK libraries linked into the
  executable, rebuilt with a newer SDK.
- **Rumble**: a *Vibration* on/off option in the options menu (texts in all
  five languages) and two new imports, `cellPadSetActDirect` and
  `cellPadGetCapabilityInfo`. patches/0054 passes the motors on (XInput,
  DualShock 4, DualSense).
- **The table of Folks** is `LMonS2.bi5_` instead of `LMonS.bi5_`. It comes
  with the update as data (below); the later downloadable packs ship their
  own copy.
- The "game data is corrupt" messages print the title from PARAM.SFO
  instead of a fixed "Folklore".
- `sysPrxForUser` `_sys_process_atexitspawn` (NID `0x2C847572`, checked
  against the NID hash) is a new import from the newer SDK's start-up code;
  `folklore_hle.cpp` answers it.
- US 1.10 and EU 1.10 differ from each other in 11 functions, as the two
  v1.00 discs do, and EU 1.10 again leaves out `sceNp` NID `0x29DD45DC`.
- The nine SPURS job binaries are the same bytes at the same addresses and
  file offsets as in v1.00.

**The update's data.** Besides the EBOOT the update brings three files,
delivered the way the downloadable packs are: in `USRDIR/bin/17/` with a list
`download.dat_` (a big-endian count, then per file a pack-number byte and a
NUL-terminated path under `bin/`). On its next start the game copies them
into `USRDIR/bin/` and adds them to `install.dat_` (the same format). Both
regions' updates carry the same three: `LMonS2.bi5_`, `cmc/sp/LCmc301_sp.bi5_`
(Spanish) and `download.dat_`. The game opens `LMonS2.bi5_` before it looks
for packs to install, so `engine_portable.cpp` (installUpdateData) puts the
files where the game's installer would and records them in `install.dat_`,
from the update folder as installed (`bin/`) or not yet (`bin/17/`), on every
start; it leaves a file alone when `install.dat_` says a later pack replaced
it. The update folder itself is only read.

**Running it.** A 1.10 build needs the disc dump (game data, PARAM.SFO) and the
update folder (its decrypted `USRDIR/EBOOT.elf` is what is loaded). The
`update_dir` setting, `run.ps1 -UpdateDir` and a second folder picker supply
it; the disc's own EBOOT is not used.

| Fix | US 1.10 | EU 1.10 |
|---|---|---|
| Movie hold wait | `func_001A59CC` @ `0x1A5B10` | `func_001A59EC` @ `0x1A5B30` |
| Movie AU size table, two sites | `func_001A3AE8` @ `0x1A4370`, `0x1A4640` (returns `0x1A4374`..`0x1A4398`, `0x1A4644`..`0x1A4668`) | `func_001A38CC` @ `0x1A4348`, `0x1A4660` (returns `0x1A434C`..`0x1A4370`, `0x1A4664`..`0x1A4688`) |
| Room for the table after the player | `func_001A4DC4` @ `0x1A4DF0` | `func_001A4DE4` @ `0x1A4E10` |
| Havok step by frame time | `func_003CD8C4` (labels `3CD90C` / `3CD8F8`, exit `3CD928`) | `func_003CD89C` (`3CD8E4` / `3CD8D0`, exit `3CD900`) |
| Switch-table hint | `351B50:351B54:19` | `351B28:351B2C:19` |
| Movie-table TOC entries | -0x2020, -0x259C | -0x2038, -0x25B4 |
| Camera aspect constants (TOC) | -28180, -16084, -13392, -13220, -6516, 508, 4552 | -28180, -16084, -13396, -13224, -6524, 484, 4528 |
| Language query | `func_0020D8B4` | `func_0020D8D4` |
| Frame limiter | 10 sites, found by pattern | the same 10 |

Every function these fixes touch is unchanged from v1.00, only moved. The
addresses come from mapping each v1.00 site through the function alignment
(US 1.00 → US 1.10, EU 1.00 → EU 1.10); the TOC offsets from pairing every
TOC-relative load in the unchanged functions, which gave one answer for each
(the seven camera slots hold 16:9 in both updates). Both updates were lifted
with these and all five game patches applied, with 10 limiter sites each.

## Languages

Both executables ask for the console language once, as they start
(`cellSysutilGetSystemParamInt(CELL_SYSUTIL_SYSTEMPARAM_ID_LANG)`, in
`func_0020C324` in the EU executable and `func_0020C320` in the US one). They
keep it in a global (EU `0x107FA170`) that the loaders use as an index into a
table of file suffixes at `0x530A44` (EU):

| Value | 0 | 1 | 2 | 3 | 4 | 5 | 6-8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Suffix | `_jp` | `_uk` | `_fr` | `_sp` | `_ge` | `_it` | (none) | `_ko` | `_tr` | `_si` | `_en` |

- **EU executable:** keeps 1-5 (the CELL_SYSUTIL_LANG codes for English,
  French, Spanish, German and Italian) and turns anything else into 1. Its
  disc has those five: `movie_00N_<lang>.dat2_` subtitles,
  `movie_996_<lang>.m2v_`, and `_fr`/`_ge`/`_it`/`_sp` cutscene sound
  (`mov_9xx`, `rtd_0xx`, `BgmHead`/`EnvHead`), with the unsuffixed files as
  English.
- **US executable:** keeps 2-5 and turns anything else into 12 (`_en`). Its
  disc only has the `_en` subtitle files.

The port tells the game the engine menu's **Language** setting on the EU
build, and English on the US build (`folklore_hle.cpp`,
`folklore_system_language` in `engine_settings.cpp`).

## Notes

The movie set-up function is one of the ten that differ. In the EU build the
code around both table allocations is the same; the extra code is elsewhere
in the function.
