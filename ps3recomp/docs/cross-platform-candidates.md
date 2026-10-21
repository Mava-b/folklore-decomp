# Cross-platform recompilation candidates (Xbox 360 ↔ PS3)

Shortlist of games shipped on **both** Xbox 360 and PS3 that are good targets for
parallel recompilation — 360 via `rexglue`/XenonRecomp, PS3 via **ps3recomp** —
so we can cross-reference function-by-function and fold improvements back into
ps3recomp.

> Compiled 2026-06-05. Emulator ratings change; **confirm the exact build
> (title id / region) on the live trackers** before buying/dumping:
> Xenia Canary compat list and RPCS3 compat list (links at bottom).

## Selection criteria (all four weighted)

| # | Criterion | Why it matters |
|---|-----------|----------------|
| 1 | **Small / native binary** | Smaller code = faster to lift fully. Must be **native C++** on *both* sides. |
| 2 | **Shared engine / middleware** | Engine we can learn once and reuse (Hedgehog, PhyreEngine, UE3, Criterion). |
| 3 | **Same codebase both platforms** | The closer the two binaries, the more 1:1 the function matching. |
| 4 | **High emulator compat** | Xenia *and* RPCS3 "Playable/Ingame" ⇒ clean, well-behaved binaries. |

### ⚠️ Hard exclusions (do not buy for cross-referencing)
- **XNA / C# 360 titles** (e.g. *Bastion* on 360, many XBLA indies). The 360
  binary is **CLR bytecode/JIT**, not native PPC, while the PS3 port is native —
  **useless** for function-level diffing. Native engine on both, or skip.
- **Outsourced/divergent ports** — e.g. *The Orange Box* PS3 (ported by EA UK
  from a different codebase). The whole point is a shared codebase; divergent
  ports defeat it.
- **PS3-exclusive engines with no 360 SKU** (flOw, Flower — already PS3-only).

---

## Tier 0 — Start here (360 side already de-risked)

These are on the **Hedgehog Engine** and the **360 build is already a solved,
open-source static recompilation** (`hedge-dev/XenonRecomp` + *Unleashed
Recompiled*). That gives us a **known-good 360 reference** to diff our PS3 lift
against — the single best place to begin.

| Game | Engine | 360 status | PS3 (RPCS3) | Notes |
|------|--------|-----------|-------------|-------|
| **Sonic Unleashed** | Hedgehog | ✅ Fully recompiled (Unleashed Recompiled) | Ingame | The reference. 360 source-of-truth exists; diff PS3 against it. |
| **Sonic Generations** | Hedgehog | Playable (Xenia) | **Playable** | Same engine/mod infra as Unleashed; cleaner PS3 status. |

> **rexglue** (github.com/rexglue/rexglue-sdk) generates **portable C++ ahead of
> time** (Xenia-rooted, inspired by XenonRecomp/rexdex) — same shape as
> ps3recomp's AOT C. So we can compare *recompiler outputs* directly, not just
> disassembly. (XenonRecomp itself leans on x86 intrinsics for VMX; rexglue is
> the more portable, comparable target.)

---

## Tier 1 — Small native cross-platform (best effort/reward)

Downloadable (XBLA + PSN) or compact retail titles, native C++ engines on both,
generally good compat. Ideal for *completing* a full recompile quickly.

| Game | Dev / Engine | Cross-platform | Compat notes |
|------|--------------|----------------|--------------|
| **Outland** | Housemarque (custom) | XBLA + PSN | Devs' *first* cross-platform title → genuinely shared codebase w/ PS3 GPU tweaks. Small. |
| **Shank / Shank 2** | Klei (custom) | XBLA + PSN | Native 2D engine, compact, well-contained. |
| **Trine 2** | Frozenbyte (custom) | XBLA + PSN | RPCS3: no reported issues. Native engine both sides. |
| **Sonic the Hedgehog 4: Ep. I** | Dimps/Sonic Team | XBLA + PSN | RPCS3 **Playable**; Xenia playable. |
| **Sonic the Hedgehog 4: Ep. II** | Dimps/Sonic Team | XBLA + PSN | RPCS3 **Playable**; Xenia playable (act 1 + saves confirmed). |
| **Sonic CD (2011)** | Retro Engine | XBLA + PSN | Small, very portable engine. |
| **Bionic Commando Rearmed** | GRIN / Diesel | XBLA + PSN | Compact remake; shared engine. (Rearmed 2 also exists.) |
| **Pac-Man CE DX** | Namco | XBLA + PSN | Tiny, arcade-clean. |

---

## Tier 2 — Larger but engine-symmetric (later, high reuse)

Bigger binaries, but the engines are highly symmetric across 360/PS3 and
extremely well documented (UE3 console source has leaked; Criterion is
well-studied). Great for *reusable engine knowledge*, less for a quick finish.

| Game | Engine | Compat highlights | Why |
|------|--------|-------------------|-----|
| **Mirror's Edge** | UE3 (+ Illuminate Labs lighting) | RPCS3 **Playable** | Compact UE3 game; 360+PS3+PC; clean. |
| **Burnout Paradise** | Criterion (custom) | RPCS3 **Playable** | Aligns with the existing `burnout3` work; native both. Online-heavy. |
| **Borderlands** | UE3 | both | Symmetric UE3 port; well-understood. |
| **Batman: Arkham Asylum / City** | UE3 | both | Polished UE3, symmetric. |
| **BioShock / BioShock 2** | modified UE2.5/3 | both | Heavily documented engine. |

> **Source engine caution:** *Portal 2* is a reasonable PS3 port (Valve +
> Steamworks), but *Orange Box* PS3 was an outsourced, divergent codebase —
> prefer Portal 2 if going Source, and verify before relying on 1:1 matching.

---

## Recommended buy/dump order

1. **Sonic Generations** (360 + PS3) — Tier 0, cleanest PS3 compat, engine has a
   known-good 360 recomp reference. **First comparison run.**
2. **Sonic Unleashed** (360 + PS3) — diff our PS3 lift directly against
   *Unleashed Recompiled*'s 360 output.
3. **Outland** (360 + PS3) — smallest fully-shared-codebase title; first
   *complete* end-to-end cross-recompile.
4. One Tier-1 platformer (**Sonic 4 Ep. I** or **Trine 2**) to generalize.
5. A Tier-2 UE3 title (**Mirror's Edge**) once the harness is proven, to start
   building reusable engine-level knowledge.

## How each maps into the harness

For each title, dump both SKUs, then:
- `python tools/compare/xex_parser.py default.xex` (identify 360 build / unpack)
- Export `*.units.json` for each side (Ghidra/IDA or ps3recomp's own tools)
- `python tools/compare/run_compare.py ps3.units.json x360.units.json`
- Read `out/report.md` → **diverging pairs** + **360-only mnemonics** are the
  concrete ps3recomp improvement backlog.

## Trackers (verify before buying)
- Xenia Canary compatibility: <https://github.com/xenia-canary/xenia-canary/wiki/Compatibility-List>
- RPCS3 compatibility: <https://rpcs3.net/compatibility>
- XenonRecomp: <https://github.com/hedge-dev/XenonRecomp> · Unleashed Recompiled: <https://github.com/hedge-dev/UnleashedRecomp>
