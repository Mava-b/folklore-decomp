#!/usr/bin/env python3
"""Route Folklore's frame limiter through the engine settings.

Each of the game's main loops (func_0017AB20, func_0017D2D8, func_0017DC20,
func_00180E0C) ends its frame with

    while (elapsed() <= TARGET) usleep(10000);

elapsed in 10 us units, TARGET 3000 (30 ms) or 4700 (47 ms). In the lifted
code that is a compare against the immediate followed, within two lines, by
`r3 = 0x2710` for the usleep -- or, where the compiler put the sleep first
(func_0017ADB0, the loop of the story-event scenes), a compare whose branch
goes back to a label just above it that starts with that `r3 = 0x2710`. This rewrites the compare to
folklore_limiter_target(TARGET) and the sleep to
folklore_limiter_sleep_us(0x2710) (src/engine/engine_settings.cpp), so the
engine menu can change the frame-rate cap and the wake interval at run time.

Idempotent; run it again after a relift. Usage: patch_limiter.py <chunk.cpp>...
"""
import re
import sys

DECLARATION = 'extern "C" int folklore_limiter_target(int); extern "C" int folklore_limiter_sleep_us(int);'
COMPARE = re.compile(r'int64_t b = \(int64_t\)(0x125C|0xBB8);')
SLEEP = '(int64_t)(int32_t)(0x2710);'
SLEEP_PATCHED = '(int64_t)(int32_t)folklore_limiter_sleep_us(0x2710);'


LOOP_BRANCH = re.compile(r'goto (loc_[0-9A-Fa-f]+);')


def sleep_before(lines, compare_index):
    """The sleep of a `sleep; elapsed = clock(); if (elapsed <= T) goto sleep;`
    loop: the line after the compare branches back to a label a few lines up,
    and the label's first statement loads the 10 ms usleep argument."""
    if compare_index + 1 >= len(lines):
        return None
    branch = LOOP_BRANCH.search(lines[compare_index + 1])
    if not branch:
        return None
    label = branch.group(1) + ':'
    for position in range(max(0, compare_index - 10), compare_index):
        if lines[position].strip() == label and position + 1 < compare_index:
            if SLEEP in lines[position + 1]:
                return position + 1
            return None
    return None


def patch(path):
    with open(path, newline='') as handle:
        lines = handle.read().split('\n')
    changed = 0
    for index, line in enumerate(lines):
        match = COMPARE.search(line)
        if not match:
            continue
        window = range(index + 1, min(index + 3, len(lines)))
        sleep_line = next((position for position in window if SLEEP in lines[position]), None)
        if sleep_line is None:
            sleep_line = sleep_before(lines, index)
        if sleep_line is None:
            continue
        lines[index] = line.replace(match.group(0), 'int64_t b = (int64_t)folklore_limiter_target(%s);' % match.group(1))
        lines[sleep_line] = lines[sleep_line].replace(SLEEP, SLEEP_PATCHED)
        changed += 1
    if changed and not any(DECLARATION in line for line in lines):
        for index, line in enumerate(lines):
            if line.startswith('#include "ppu_recomp.h"'):
                newline = '\r' if line.endswith('\r') else ''
                lines.insert(index + 1, DECLARATION + newline)
                break
    if changed:
        with open(path, 'w', newline='') as handle:
            handle.write('\n'.join(lines))
    sys.stderr.write('%s: %d limiter site(s) patched\n' % (path, changed))


if __name__ == '__main__':
    for argument in sys.argv[1:]:
        patch(argument)
