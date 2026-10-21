#!/usr/bin/env python3
"""Folklore-specific fixes applied to the lifted PPU code after a relift.

Each patch names the guest address it changes and why. tools/relift.sh runs
this after ppu_lifter.py; running it twice is harmless (a patch already
applied is skipped).

  game_patches.py src/recomp
"""
import pathlib
import sys

PATCHES = [
    {
        "address": 0x001A48C4,
        "why": (
            "MoviePostThread waits for the movie player's 'hold' to be released "
            "with an untimed sys_cond_wait (cond at player+0x8C). The release "
            "only comes if MovieInputThread also saw the hold, and both threads "
            "check it on separate 10 ms polls: on the PS3 their low priority "
            "orders them, on PC threads they race, and when only MoviePost sees "
            "the hold it waits forever -- the black screen on the logo movie. "
            "A 100 ms timeout lets it re-check; the loop after the wait already "
            "handles a hold that is still set or gone."
        ),
        "old": (
            "        ctx->gpr[3] = vm_read32(ctx->gpr[31] + 0x64);\n"
            "        ctx->gpr[4] = (int64_t)(int32_t)(0);\n"
            "        vm_write8(ctx->gpr[30] + 0x68, ctx->gpr[0]);\n"
            "        ctx->gpr[11] = (int64_t)(int32_t)(0x6B);\n"
        ),
        "new": (
            "        ctx->gpr[3] = vm_read32(ctx->gpr[31] + 0x64);\n"
            "        ctx->gpr[4] = 100000; /* FOLKLORE PATCH 0x1A48C4: timed hold wait (tools/game_patches.py) */\n"
            "        vm_write8(ctx->gpr[30] + 0x68, ctx->gpr[0]);\n"
            "        ctx->gpr[11] = (int64_t)(int32_t)(0x6B);\n"
        ),
    },
    {
        "address": 0x001A3124,
        "why": "The movie player takes its AU size table (the .dat_ file, 8 bytes per picture) from the shared work-area allocator: read the top, advance it. The main thread's loader uses the space above that same top as scratch for archive reads without advancing it. On the PS3 the logo movie's table is allocated before or after the loader's read; on PC the threads overlap, the loader's model.bin_ read lands on the table (seen in a Windows log: both at 0x45433600), and the movie gets garbage AU sizes -- corrupt video, a stuck logo, or a crash in the decoder. The table now goes in a block reserved right after the player struct at boot.",
        "old": '        ctx->gpr[0] = vm_read32(ctx->gpr[1] + 0xA8);\n        vm_write32(ctx->gpr[31] + 0x4, ctx->gpr[0]);\n        ctx->lr = 0x001A3128; func_00187C7C(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[4] = (int64_t)(int32_t)(0);\n        ctx->gpr[0] = ctx->gpr[3] | ctx->gpr[3];\n        ctx->gpr[3] = vm_read32(ctx->gpr[31] + 0x4);\n        ctx->gpr[5] = (int64_t)(int32_t)(0);\n        ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[0];\n        vm_write32(ctx->gpr[31] + 0x0, ctx->gpr[0]);\n        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);\n        ctx->lr = 0x001A314C; func_00187CEC(ctx); DRAIN_TRAMPOLINE(ctx);\n',
        "new": "        ctx->gpr[0] = vm_read32(ctx->gpr[1] + 0xA8);\n        vm_write32(ctx->gpr[31] + 0x4, ctx->gpr[0]);\n        /* FOLKLORE PATCH 0x1A3124: the movie's AU size table goes in the block reserved after\n         * the player (FOLKLORE PATCH 0x1A3BA4), not at the shared work-area top (tools/game_patches.py) */\n        if ((uint32_t)ctx->gpr[0] <= 0x40000u) {\n            ctx->gpr[0] = (uint32_t)(vm_read32(vm_read32(ctx->gpr[2] + -0x20C0)) + 0x240u);\n            ctx->gpr[4] = (int64_t)(int32_t)(0);\n            ctx->gpr[5] = (int64_t)(int32_t)(0);\n            vm_write32(ctx->gpr[31] + 0x0, ctx->gpr[0]);\n            ctx->gpr[9] = vm_read32(ctx->gpr[2] + -0x263C);\n            ctx->gpr[3] = vm_read32(ctx->gpr[9]);\n        } else {\n        ctx->lr = 0x001A3128; func_00187C7C(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[4] = (int64_t)(int32_t)(0);\n        ctx->gpr[0] = ctx->gpr[3] | ctx->gpr[3];\n        ctx->gpr[3] = vm_read32(ctx->gpr[31] + 0x4);\n        ctx->gpr[5] = (int64_t)(int32_t)(0);\n        ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[0];\n        vm_write32(ctx->gpr[31] + 0x0, ctx->gpr[0]);\n        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);\n        ctx->lr = 0x001A314C; func_00187CEC(ctx); DRAIN_TRAMPOLINE(ctx);\n        }\n",
    },
    {
        "address": 0x001A33F4,
        "why": "The movie player takes its AU size table (the .dat_ file, 8 bytes per picture) from the shared work-area allocator: read the top, advance it. The main thread's loader uses the space above that same top as scratch for archive reads without advancing it. On the PS3 the logo movie's table is allocated before or after the loader's read; on PC the threads overlap, the loader's model.bin_ read lands on the table (seen in a Windows log: both at 0x45433600), and the movie gets garbage AU sizes -- corrupt video, a stuck logo, or a crash in the decoder. The table now goes in a block reserved right after the player struct at boot.",
        "old": '        ctx->gpr[0] = vm_read32(ctx->gpr[1] + 0xA8);\n        vm_write32(ctx->gpr[31] + 0x4, ctx->gpr[0]);\n        ctx->lr = 0x001A33F8; func_00187C7C(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[4] = (int64_t)(int32_t)(0);\n        ctx->gpr[0] = ctx->gpr[3] | ctx->gpr[3];\n        ctx->gpr[3] = vm_read32(ctx->gpr[31] + 0x4);\n        ctx->gpr[5] = (int64_t)(int32_t)(0);\n        ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[0];\n        vm_write32(ctx->gpr[31] + 0x0, ctx->gpr[0]);\n        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);\n        ctx->lr = 0x001A341C; func_00187CEC(ctx); DRAIN_TRAMPOLINE(ctx);\n',
        "new": "        ctx->gpr[0] = vm_read32(ctx->gpr[1] + 0xA8);\n        vm_write32(ctx->gpr[31] + 0x4, ctx->gpr[0]);\n        /* FOLKLORE PATCH 0x1A33F4: the movie's AU size table goes in the block reserved after\n         * the player (FOLKLORE PATCH 0x1A3BA4), not at the shared work-area top (tools/game_patches.py) */\n        if ((uint32_t)ctx->gpr[0] <= 0x40000u) {\n            ctx->gpr[0] = (uint32_t)(vm_read32(vm_read32(ctx->gpr[2] + -0x20C0)) + 0x240u);\n            ctx->gpr[4] = (int64_t)(int32_t)(0);\n            ctx->gpr[5] = (int64_t)(int32_t)(0);\n            vm_write32(ctx->gpr[31] + 0x0, ctx->gpr[0]);\n            ctx->gpr[9] = vm_read32(ctx->gpr[2] + -0x263C);\n            ctx->gpr[3] = vm_read32(ctx->gpr[9]);\n        } else {\n        ctx->lr = 0x001A33F8; func_00187C7C(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[4] = (int64_t)(int32_t)(0);\n        ctx->gpr[0] = ctx->gpr[3] | ctx->gpr[3];\n        ctx->gpr[3] = vm_read32(ctx->gpr[31] + 0x4);\n        ctx->gpr[5] = (int64_t)(int32_t)(0);\n        ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[0];\n        vm_write32(ctx->gpr[31] + 0x0, ctx->gpr[0]);\n        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);\n        ctx->lr = 0x001A341C; func_00187CEC(ctx); DRAIN_TRAMPOLINE(ctx);\n        }\n",
    },
    {
        "address": 0x001A3BA4,
        "why": 'Movie init allocates the player struct (0x240 bytes) once at boot; 256 KB more right after it holds the AU size table of whichever movie is open (see 0x1A3124), up to 32768 pictures.',
        "old": '        ctx->gpr[30] = vm_read32(ctx->gpr[2] + -0x20C0);\n        ctx->gpr[9] = ctx->gpr[3] | ctx->gpr[3];\n        ctx->gpr[3] = ctx->gpr[3] + (int64_t)(0x240);\n',
        "new": '        ctx->gpr[30] = vm_read32(ctx->gpr[2] + -0x20C0);\n        ctx->gpr[9] = ctx->gpr[3] | ctx->gpr[3];\n        ctx->gpr[3] = ctx->gpr[3] + (int64_t)(0x240 + 0x40000); /* FOLKLORE PATCH 0x1A3BA4: 256 KB for the AU size table after the player struct (tools/game_patches.py) */\n',
    },
    {
        "address": 0x003C913C,
        "why": (
            "Every Havok world is stepped here: setFrameTimeMarker(frame time), then "
            "stepDeltaTime(1/30) until the simulation reaches the marker. The cloth and "
            "hair objects drive their anchor bodies with hard keyframes worked out for "
            "the frame time, so above 30 fps the 1/30 steps fall on some frames and not "
            "others, the anchors overshoot and snap back, and the cloth whips around "
            "(RPCS3's wiki: 'cloth physics break above 50 FPS'). The step now covers "
            "exactly the frame (folklore_physics_plan_steps, engine setting "
            "physics_frame_step); at 30 fps it is the same single 1/30 step."
        ),
        "old": 'void func_003C913C(ppu_context* ctx) {\n        uint64_t _cs_31 = ctx->gpr[31];\n        vm_write64(ctx->gpr[1] + -0x90, ctx->gpr[1]); ctx->gpr[1] += -0x90;\n        ctx->gpr[0] = ctx->lr;\n        { uint64_t tmp; memcpy(&tmp, &ctx->fpr[31], 8); vm_write64(ctx->gpr[1] + 0x88, tmp); }\n        ctx->fpr[31] = ctx->fpr[2];\n        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[31]);\n        ctx->gpr[31] = ctx->gpr[3] | ctx->gpr[3];\n        vm_write64(ctx->gpr[1] + 0xA0, ctx->gpr[0]);\n        ctx->lr = 0x003C915C; func_002FECD4(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        ctx->lr = 0x003C9168; func_00301450(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        goto loc_003C9184;\nloc_003C9170:\n        ctx->lr = 0x003C9174; func_003051CC(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        ctx->lr = 0x003C9180; func_002FED28(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\nloc_003C9184:\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        ctx->lr = 0x003C918C; func_002FECFC(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->fpr[1] = ctx->fpr[31];\n        { int64_t a = (int64_t)ctx->gpr[3]; int64_t b = (int64_t)0; uint32_t cr_val = ((a < b) ? 8 : (a > b) ? 4 : 2) | ((ctx->xer >> 31) & 1u); ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        if (((ctx->cr >> 1) & 1)) goto loc_003C9170;\n        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xA0);\n        ctx->gpr[31] = _cs_31;\n        { uint64_t tmp = vm_read64(ctx->gpr[1] + 0x88); memcpy(&ctx->fpr[31], &tmp, 8); }\n        ctx->lr = ctx->gpr[0];\n        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x90);\n        return;\n}\n\n',
        "new": 'extern "C" int folklore_physics_plan_steps(float, float, float, float, float*); /* FOLKLORE PATCH 0x3C913C */\nvoid func_003C913C(ppu_context* ctx) {\n        /* FOLKLORE PATCH 0x3C913C: step Havok by the frame time, not a fixed 1/30 s\n         * (folklore_physics_plan_steps in src/engine/engine_settings.cpp, tools/game_patches.py) */\n        int physicsStepLimit = -1;\n        int physicsStepsTaken = 0;\n        uint64_t _cs_31 = ctx->gpr[31];\n        vm_write64(ctx->gpr[1] + -0x90, ctx->gpr[1]); ctx->gpr[1] += -0x90;\n        ctx->gpr[0] = ctx->lr;\n        { uint64_t tmp; memcpy(&tmp, &ctx->fpr[31], 8); vm_write64(ctx->gpr[1] + 0x88, tmp); }\n        ctx->fpr[31] = ctx->fpr[2];\n        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[31]);\n        ctx->gpr[31] = ctx->gpr[3] | ctx->gpr[3];\n        vm_write64(ctx->gpr[1] + 0xA0, ctx->gpr[0]);\n        ctx->lr = 0x003C915C; func_002FECD4(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        ctx->lr = 0x003C9168; func_00301450(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        {\n            const uint32_t simulation = vm_read32((uint32_t)ctx->gpr[31] + 0x8);\n            float currentTime;\n            float simulatedTime;\n            float markerTime;\n            float stepSize;\n            uint32_t bits = vm_read32(simulation + 0x10);\n            memcpy(&currentTime, &bits, 4);\n            bits = vm_read32(simulation + 0x14);\n            memcpy(&simulatedTime, &bits, 4);\n            bits = vm_read32(simulation + 0x1C);\n            memcpy(&markerTime, &bits, 4);\n            physicsStepLimit = folklore_physics_plan_steps(currentTime, simulatedTime, markerTime,\n                                                           (float)ctx->fpr[31], &stepSize);\n            if (physicsStepLimit >= 0)\n            {\n                ctx->fpr[31] = (double)stepSize;\n            }\n        }\n        goto loc_003C9184;\nloc_003C9170:\n        ctx->lr = 0x003C9174; func_003051CC(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        ctx->lr = 0x003C9180; func_002FED28(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        physicsStepsTaken++;\nloc_003C9184:\n        if (physicsStepLimit >= 0 && physicsStepsTaken >= physicsStepLimit)\n        {\n            goto loc_003C91A0;\n        }\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        ctx->lr = 0x003C918C; func_002FECFC(ctx); DRAIN_TRAMPOLINE(ctx);\n        /* nop */;\n        ctx->fpr[1] = ctx->fpr[31];\n        { int64_t a = (int64_t)ctx->gpr[3]; int64_t b = (int64_t)0; uint32_t cr_val = ((a < b) ? 8 : (a > b) ? 4 : 2) | ((ctx->xer >> 31) & 1u); ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }\n        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n        if (((ctx->cr >> 1) & 1)) goto loc_003C9170;\nloc_003C91A0:\n        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xA0);\n        ctx->gpr[31] = _cs_31;\n        { uint64_t tmp = vm_read64(ctx->gpr[1] + 0x88); memcpy(&ctx->fpr[31], &tmp, 8); }\n        ctx->lr = ctx->gpr[0];\n        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x90);\n        return;\n}\n\n',
    },
]


def main():
    recompDir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "src/recomp")
    sources = sorted(recompDir.glob("*.cpp"))
    failures = 0
    for patch in PATCHES:
        applied = False
        for source in sources:
            text = source.read_text()
            if patch["new"] in text:
                print("already applied: 0x%08X (%s)" % (patch["address"], source.name))
                applied = True
                break
            count = text.count(patch["old"])
            if count == 1:
                source.write_text(text.replace(patch["old"], patch["new"]))
                print("applied: 0x%08X (%s)" % (patch["address"], source.name))
                applied = True
                break
            if count > 1:
                print("ambiguous: 0x%08X matches %d times in %s" % (patch["address"], count, source.name))
        if not applied:
            print("NOT FOUND: 0x%08X" % patch["address"])
            failures += 1
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
