# Supported releases

| Release | Title ID | Version | Decrypted `EBOOT.elf` | TOC (r2) | Import stubs end |
|---|---|---|---|---|---|
| North America | BCUS98147 | 1.00 | 6,518,936 bytes | `0x56EB00` | `0x4EEB84` (167 imports) |
| Europe | BCES00050 | 1.00 | 6,518,912 bytes | `0x56EAF8` | `0x4EEB1C` (166 imports) |

A build is lifted from one release and runs only that release. `tools/relift.sh`
recognises the EBOOT by its size and writes `src/recomp/folklore_region.h`. The
engine code reads that header (`src/engine/engine_region.h`), and lifted code
from before it existed counts as US. `setup.ps1 -GameDir <dump of the other
release>` lifts that release instead, and `build.ps1` then rebuilds everything.

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
