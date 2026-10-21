# Prototype & Debug Builds as Ground Truth

Retail PS3 EBOOTs are encrypted (real SELF, needs keys) and stripped (no symbols).
**Prototype and debug builds are neither.** They ship `EBOOT.BIN` as a *debug fSELF*
(`SCE\0` header, `key_revision 0x8000`): the payload is not encrypted, only zlib-deflated,
so it unwraps with an inflate and no keys. And SDK debug builds are *unstripped* — they
keep `.symtab`/`.strtab` and full DWARF, which the SELF stores verbatim in its tail at a
constant offset delta from the section-header table.

That gives the recompiler something it otherwise never has: **ground truth**. Exact
function boundaries, real firmware struct layouts, source line numbers, and named SPU
jobs — all recoverable offline, with no keys, from a build that leaked or shipped as a
demo/beta.

This page documents the tools that exploit it and what they found.

---

## The toolchain

Each tool takes the *unwrapped* ELF (run `unfself.py` first).

| Tool | What it does |
|------|--------------|
| **`unfself.py`** | Rebuild the original ELF from a debug fSELF (inflate segments + recover the non-alloc tail). Refuses retail (encrypted) SELFs instead of mis-decoding them. |
| **`elf_symbols.py`** | Pull the `STT_FUNC` table — exact start/size/name per function. Emits the `find_functions --seed-json` shape, and can `--score` a detection run against it. |
| **`nid_harvest.py`** | Hash every firmware-shaped symbol name into a NID→name gazetteer and intersect it with the NIDs the build imports → definitive `(library, name, NID)` triples. |
| **`dwarf_abi.py`** | Firmware struct layouts + function signatures from DWARF. `--diff-headers` flags any repo struct whose layout disagrees with the ABI. |
| **`spu_ident.py`** | Name the embedded SPU images (via `_binary_*` symbols) and classify them middleware-vs-custom; optional fingerprint DB to identify the same blobs in stripped builds. |
| **`dwarf_line.py`** | Resolve a code address to `source.cpp:line` via the DWARF line-number program — turns a recomp crash address into a source location. |
| **`dwarf_common.py`** | Shared minimal DWARF2 reader (abbrev/forms/DIE-tree walk). |

Typical flow:

```bash
python tools/unfself.py EBOOT.BIN -o EBOOT.elf
python tools/elf_symbols.py EBOOT.elf -o syms.json
python tools/find_functions.py EBOOT.elf --seed-json syms.json -o funcs.json
python tools/dwarf_abi.py EBOOT.elf --diff-headers
python tools/spu_ident.py EBOOT.elf
python tools/dwarf_line.py EBOOT.elf 0x004F4A98
```

Self-checks (no binary needed): `test_unfself.py`, `test_dwarf.py`, `test_nid_from_protos.py`.

---

## What the ground truth found

**Function detection.** Scoring `find_functions.py` against the symbol tables (43,788
real functions in Dungeon Siege III, 59,192 in *What if*) gave the detector its first
real accuracy measurement — and exposed a silent recall bug: because a function is
scanned only to its first `blr`, every `bl` after an early return sat in an "uncovered"
region and its callee was never seeded (1,723 functions lost on DS3, including a 15 KB
PhysX routine, each swallowed into a neighbour by the lifter). Trusting `bl` sources
across the `.opd` code span raised recall 96.1% → 98.9% on both titles.

**NID resolution.** A NID is `SHA-1(name+suffix)[:4]` and can't be inverted, so an import
NID is only resolvable with a candidate name to hash. The symbol tables supply the names.
Across 30 proto import tables (1,389 distinct NIDs): resolution 43.5% → 70.2%, +371
definitive names (`compute_nid(name)` == the imported NID), zero collisions — heaviest on
the sparsely-documented online/media libraries (`sceNp*` 130+, SPURS 39, cellFont, Sail).
Committed as `tools/nid_from_protos.json`, auto-loaded by the NID database.

**Struct ABI.** The debug builds compiled the SDK headers *with* debug info, so DWARF
carries the authoritative layout of every `Cell*`/`Sce*` struct — the exact bug class the
project keeps hand-fixing. `dwarf_abi.py --diff-headers` re-derived the `CellFsStat`
52-byte 4-aligned packing and the `CellGcmContextData` "callback at +0xC" fixes
automatically, and flags any header still disagreeing. 139 firmware struct layouts and
~30 inlined-SDK function signatures (including the 12-arg `cellSpursJobChainAttributeInitialize`).

**SPU strategy.** `.debug_scespuversion` and `_binary_*` symbols name every embedded SPU
image. The strategic finding: **most SPU work is prebuilt middleware, not custom code** —
Sony Edge `zlib` in *every* title, plus Bink video, Havok (`hka*`), Granny, MP3/Vorbis
codecs, and *What if*'s ~40 Wwise `Ak*FXJob` audio tasks. That reframes the SPU frontier
from "lift arbitrary SPU" to "identify and HLE known middleware." Fingerprints (`.text`
SHA-1) are stable across games built at the same middleware version (identical Edge blob
in Paragons and Pirates), so `spu_ident.py --update-db` / `--db` can carry identification
into stripped retail builds — within a version.

**Source lines.** `dwarf_line.py` maps a code address to `file:line` (validated:
`0x1F107C` → `AkAudioMgr.cpp:361`, matching the symbol `CAkAudioMgr::ProcessMsgQueue`),
and correctly reports "no line info" for prebuilt-library gaps (PhysX ships without DWARF).

---

## Corpus notes

13 of the ~50 sampled proto binaries carry usable symbol tables — NFS Rivals (114,315
functions across three build configs), *What if* (UE3, 59,192), Dungeon Siege III
(SNC/PhysX, 43,788), Paragons, Pirates, Puppeteer, Ragdoll Kung Fu. ~35 are debug fSELF
(unwrappable, no keys) even when stripped. Retail demos (FIFA, Killzone 2, GT5, Demon's
Souls) are genuinely encrypted (`key_revision != 0x8000`) and are refused, not mis-parsed.

Provide your own legally obtained builds; this toolkit ships no game data.
