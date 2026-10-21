#!/usr/bin/env python3
"""Find position-independent switch tables in a raw SPU binary.

Folklore's SPURS jobs are position-independent: a switch compiles to a table
of 32-bit offsets, each relative to the start of the table, followed by
`a $rT, $rEntry, $rTableBase; bi $rT`. Nothing in the code names the case
labels as addresses, so branch-following function discovery never sees them,
and a lift without them drops into the interpreter on every such switch.

This prints the case-label addresses as a comma-separated list for
spu_lifter.py --extra-funcs. A table is a run of at least MIN_RUN words that
are all non-zero multiples of 4 and whose targets land inside the binary on a
word that is not itself table data. Each run is accepted only if at least one
of its entries, read as an instruction, is a `stop` (a small positive offset)
or the run contains a negative offset -- true of real tables, rare in code.

  spu_jump_tables.py job.bin --base 0x4C00
"""
import argparse
import struct
import sys

MIN_RUN = 3


def find_tables(data, base):
    words = [struct.unpack(">I", data[offset:offset + 4])[0]
             for offset in range(0, len(data) - 3, 4)]
    size = len(data)
    targets = set()
    index = 0
    while index < len(words):
        runStart = index
        runTargets = []
        hasNegative = False
        while index < len(words):
            value = words[index]
            signed = value - (1 << 32) if value & 0x80000000 else value
            tableOffset = runStart * 4
            target = tableOffset + signed
            if signed == 0 or signed % 4 or target < 0 or target >= size:
                break
            if abs(signed) > 0x10000:
                break
            if signed < 0:
                hasNegative = True
            runTargets.append(target)
            index += 1
        runLength = index - runStart
        if runLength >= MIN_RUN:
            tableEnd = runStart * 4 + runLength * 4
            smallPositive = any(0 < (words[k] if not words[k] & 0x80000000 else 0) < 0x4000
                                for k in range(runStart, index))
            insideTable = all(runStart * 4 <= target < tableEnd for target in runTargets)
            if (hasNegative or smallPositive) and not insideTable:
                for target in runTargets:
                    if not (runStart * 4 <= target < tableEnd):
                        targets.add(base + target)
        if runLength == 0:
            index += 1
    return sorted(targets)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary")
    parser.add_argument("--base", type=lambda text: int(text, 0), default=0)
    arguments = parser.parse_args()
    data = open(arguments.binary, "rb").read()
    targets = find_tables(data, arguments.base)
    sys.stdout.write(",".join("0x%X" % target for target in targets))
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
