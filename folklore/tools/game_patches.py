#!/usr/bin/env python3
"""Folklore-specific fixes applied to the lifted PPU code after a relift.

Each patch names the guest function and address it changes, and why, and is
written as a few small edits to that function: insert our code before or after
the line that holds a given call (named by its return address) or label, or
change one operand. The patches carry only the code we add, never a copy of
the lifted game code, so this file can be shared with the project.

tools/relift.sh runs this after ppu_lifter.py. Running it twice is harmless:
a function that already carries its FOLKLORE PATCH marker is skipped.

  game_patches.py src/recomp [--region US|EU]

The same fixes apply to both releases this port supports: BCUS98147 (US,
the default) and BCES00050 (EU). Their executables are the same code shifted
by a few bytes in places, with a handful of functions changed; REGIONS below
holds each one's function names, addresses, call anchors and TOC offsets
(found by aligning the two executables function by function,
folklore/docs/REGIONS.md). The text of every fix is shared.
"""
import pathlib
import re
import sys

# Per release: where each fix goes. "toc" holds the r2-relative offsets of the
# two TOC entries the movie-table fix reads (the work-area allocator's state).
REGIONS = {
    "US": {
        "movieHold": ("func_001A4780", 0x001A48C4),
        "movieTable": ("func_001A289C", [(0x001A3124, "0x001A3128", "0x001A314C"),
                                         (0x001A33F4, "0x001A33F8", "0x001A341C")]),
        "playerAlloc": ("func_001A3B78", 0x001A3BA4),
        "physics": ("func_003C913C", 0x003C913C,
                    {"loop": "003C9184", "back": "003C9170", "exit": "003C91A0"}),
        "toc": {"workTop": 0x20C0, "allocator": 0x263C},
    },
    "EU": {
        "movieHold": ("func_001A4784", 0x001A48C8),
        "movieTable": ("func_001A2664", [(0x001A30E0, "0x001A30E4", "0x001A3108"),
                                         (0x001A33F8, "0x001A33FC", "0x001A3420")]),
        "playerAlloc": ("func_001A3B7C", 0x001A3BA8),
        "physics": ("func_003C90F4", 0x003C90F4,
                    {"loop": "003C913C", "back": "003C9128", "exit": "003C9158"}),
        "toc": {"workTop": 0x20E0, "allocator": 0x265C},
    },
}

MOVIE_TABLE_WHY = (
    "The movie player takes its AU size table (the .dat_ file, 8 bytes per picture) from the "
    "shared work-area allocator: read the top, advance it. The main thread's loader uses the "
    "space above that same top as scratch for archive reads without advancing it. On the PS3 "
    "the logo movie's table is allocated before or after the loader's read; on PC the threads "
    "overlap, the loader's model.bin_ read lands on the table (seen in a Windows log: both at "
    "0x45433600), and the movie gets garbage AU sizes -- corrupt video, a stuck logo, or a "
    "crash in the decoder. The table now goes in a block reserved right after the player "
    "struct at boot."
)


def movieTableBranch(address, region):
    """The table allocation at `address`: take the reserved block when the table fits."""
    toc = region["toc"]
    return (
        "        /* FOLKLORE PATCH 0x%X: the movie's AU size table goes in the block reserved after\n"
        "         * the player (FOLKLORE PATCH 0x%X), not at the shared work-area top (tools/game_patches.py) */\n"
        "        if ((uint32_t)ctx->gpr[0] <= 0x40000u) {\n"
        "            ctx->gpr[0] = (uint32_t)(vm_read32(vm_read32(ctx->gpr[2] + -0x%X)) + 0x240u);\n"
        "            ctx->gpr[4] = (int64_t)(int32_t)(0);\n"
        "            ctx->gpr[5] = (int64_t)(int32_t)(0);\n"
        "            vm_write32(ctx->gpr[31] + 0x0, ctx->gpr[0]);\n"
        "            ctx->gpr[9] = vm_read32(ctx->gpr[2] + -0x%X);\n"
        "            ctx->gpr[3] = vm_read32(ctx->gpr[9]);\n"
        "        } else {\n" % (address, region["playerAlloc"][1], toc["workTop"], toc["allocator"])
    )


PHYSICS_PLAN = (
    "        {\n"
    "            const uint32_t simulation = vm_read32((uint32_t)ctx->gpr[31] + 0x8);\n"
    "            float currentTime;\n"
    "            float simulatedTime;\n"
    "            float markerTime;\n"
    "            float stepSize;\n"
    "            uint32_t bits = vm_read32(simulation + 0x10);\n"
    "            memcpy(&currentTime, &bits, 4);\n"
    "            bits = vm_read32(simulation + 0x14);\n"
    "            memcpy(&simulatedTime, &bits, 4);\n"
    "            bits = vm_read32(simulation + 0x1C);\n"
    "            memcpy(&markerTime, &bits, 4);\n"
    "            physicsStepLimit = folklore_physics_plan_steps(currentTime, simulatedTime, markerTime,\n"
    "                                                           (float)ctx->fpr[31], &stepSize);\n"
    "            if (physicsStepLimit >= 0)\n"
    "            {\n"
    "                ctx->fpr[31] = (double)stepSize;\n"
    "            }\n"
    "        }\n"
)

# Edits, applied in order inside the named function:
#   ("before_function", text)          text goes in front of the function
#   ("before_line", pattern, text)     text goes in front of the line matching pattern
#   ("after_line", pattern, text)      text goes after the line matching pattern
#   ("replace", pattern, replacement)  re.sub of the one match of pattern
# Every pattern must match exactly once in the function.
def buildPatches(regionName):
    region = REGIONS[regionName]
    holdFunction, holdAddress = region["movieHold"]
    tableFunction, tableSites = region["movieTable"]
    allocFunction, allocAddress = region["playerAlloc"]
    physicsFunction, physicsAddress, labels = region["physics"]
    patches = [
        {
            "address": holdAddress,
            "function": holdFunction,
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
            "edits": [
                # The wait's timeout argument (r4), set right after the cond is loaded from +0x64.
                ("replace", r"(\+ 0x64\);\n\s*ctx->gpr\[4\] = )\(int64_t\)\(int32_t\)\(0\);",
                 r"\g<1>100000; /* FOLKLORE PATCH 0x%X: timed hold wait (tools/game_patches.py) */" % holdAddress),
            ],
        },
    ]
    for siteAddress, firstReturn, lastReturn in tableSites:
        patches.append({
            "address": siteAddress,
            "function": tableFunction,
            "why": MOVIE_TABLE_WHY,
            "edits": [
                # Around the allocator calls returning to firstReturn .. lastReturn.
                ("before_line", r"ctx->lr = %s;" % firstReturn, movieTableBranch(siteAddress, region)),
                ("after_line", r"ctx->lr = %s;" % lastReturn, "        }\n"),
            ],
        })
    patches.append({
        "address": allocAddress,
        "function": allocFunction,
        "why": (
            "Movie init allocates the player struct (0x240 bytes) once at boot; 256 KB more right "
            "after it holds the AU size table of whichever movie is open (see the movie table fix), up to "
            "32768 pictures."
        ),
        "edits": [
            # The size passed to the allocator.
            ("replace", r"(ctx->gpr\[3\] = ctx->gpr\[3\] \+ \(int64_t\)\(0x240)\);",
             r"\g<1> + 0x40000); /* FOLKLORE PATCH 0x%X: 256 KB for the AU size table after the player struct (tools/game_patches.py) */" % allocAddress),
        ],
    })
    patches.append({
        "address": physicsAddress,
        "function": physicsFunction,
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
        "edits": [
            ("before_function",
             'extern "C" int folklore_physics_plan_steps(float, float, float, float, float*); /* FOLKLORE PATCH 0x%X */\n'
             % physicsAddress),
            ("after_line", r"^void %s\(ppu_context\* ctx\) \{" % physicsFunction,
             "        /* FOLKLORE PATCH 0x%X: step Havok by the frame time, not a fixed 1/30 s\n"
             "         * (folklore_physics_plan_steps in src/engine/engine_settings.cpp, tools/game_patches.py) */\n"
             "        int physicsStepLimit = -1;\n"
             "        int physicsStepsTaken = 0;\n" % physicsAddress),
            # After setFrameTimeMarker/advanceTime, before the step loop is entered.
            ("before_line", r"goto loc_%s;" % labels["loop"], PHYSICS_PLAN),
            # Count each step; stop when the frame is covered.
            ("before_line", r"^loc_%s:" % labels["loop"], "        physicsStepsTaken++;\n"),
            ("after_line", r"^loc_%s:" % labels["loop"],
             "        if (physicsStepLimit >= 0 && physicsStepsTaken >= physicsStepLimit)\n"
             "        {\n"
             "            goto loc_%s;\n"
             "        }\n" % labels["exit"]),
            ("after_line", r"goto loc_%s;" % labels["back"], "loc_%s:\n" % labels["exit"]),
        ],
    })
    return patches


class PatchError(Exception):
    pass


def findLine(body, pattern):
    """(start, end) of the one line in body matching pattern; end includes the newline."""
    matches = list(re.finditer(pattern, body, re.MULTILINE))
    if len(matches) != 1:
        raise PatchError("pattern %r matches %d times" % (pattern, len(matches)))
    lineStart = body.rfind("\n", 0, matches[0].start()) + 1
    lineEnd = body.find("\n", matches[0].end())
    lineEnd = len(body) if lineEnd < 0 else lineEnd + 1
    return lineStart, lineEnd


def applyEdits(prefix, body, edits):
    for edit in edits:
        kind = edit[0]
        if kind == "before_function":
            prefix = prefix + edit[1]
        elif kind == "before_line":
            lineStart, _ = findLine(body, edit[1])
            body = body[:lineStart] + edit[2] + body[lineStart:]
        elif kind == "after_line":
            _, lineEnd = findLine(body, edit[1])
            body = body[:lineEnd] + edit[2] + body[lineEnd:]
        elif kind == "replace":
            matchCount = len(re.findall(edit[1], body, re.MULTILINE))
            if matchCount != 1:
                raise PatchError("pattern %r matches %d times" % (edit[1], matchCount))
            body = re.sub(edit[1], edit[2], body, count=1, flags=re.MULTILINE)
        else:
            raise PatchError("unknown edit %r" % kind)
    return prefix, body


def functionSpan(text, name):
    """(start, end) of `void name(ppu_context* ctx) { ... }` in text, or None."""
    header = "void %s(ppu_context* ctx) {" % name
    start = text.find(header)
    if start < 0:
        return None
    if start > 0 and text[start - 1] != "\n":
        return None
    end = text.find("\n}\n", start)
    if end < 0:
        return None
    return start, end + 3


def applyPatch(sources, patch):
    marker = "FOLKLORE PATCH 0x%X" % patch["address"]
    for source in sources:
        text = source.read_text()
        span = functionSpan(text, patch["function"])
        if span is None:
            continue
        start, end = span
        body = text[start:end]
        if marker in body:
            return "already applied", source.name
        prefix, newBody = applyEdits("", body, patch["edits"])
        source.write_text(text[:start] + prefix + newBody + text[end:])
        return "applied", source.name
    raise PatchError("function %s not found" % patch["function"])


def main():
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith("--region")]
    regionName = "US"
    for index, argument in enumerate(sys.argv[1:]):
        if argument.startswith("--region="):
            regionName = argument.split("=", 1)[1]
        elif argument == "--region" and index + 2 < len(sys.argv):
            regionName = sys.argv[index + 2]
            if regionName in arguments:
                arguments.remove(regionName)
    if regionName not in REGIONS:
        print("unknown region %r (US or EU)" % regionName)
        sys.exit(2)
    recompDirectory = pathlib.Path(arguments[0] if arguments else "src/recomp")
    sources = sorted(recompDirectory.glob("*.cpp"))
    failures = 0
    for patch in buildPatches(regionName):
        try:
            status, sourceName = applyPatch(sources, patch)
            print("%s: 0x%08X (%s in %s)" % (status, patch["address"], patch["function"], sourceName))
        except PatchError as error:
            print("FAILED: 0x%08X (%s): %s" % (patch["address"], patch["function"], error))
            failures += 1
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
