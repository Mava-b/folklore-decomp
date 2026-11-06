/*
 * engine_capture_finder.cpp -- a diagnostic: find where the game keeps the
 * capture state (F5-F8), the way a memory search tool does.
 *
 * Why: auto-capture has to know, for the boss Ids' captures, when the Id
 * is red (Time: flick now) and how full the capture bar is. The capture
 * code is known (engine_motion_watch.cpp, folklore/docs/STATUS.md 104) but
 * not where it keeps those two things, and finding that from the code alone
 * is slow. The player knows both by looking at the screen, so:
 *
 *   F5  the Id is red now        F6  the Id is not red now
 *   F7  the bar is higher than at the last F7/F8
 *   F8  the bar is lower than at the last F7/F8
 *
 * Each press copies the game's memory (main memory and the game's data,
 * about 300 MB; the game pauses for a moment). Colour: a byte is kept as a
 * candidate while it holds one value at every F5 and another at every F6.
 * Bar: a 4-byte float is kept while it went up at every F7 and down at
 * every F8 (and stays a sane number). After each press the log says how many
 * candidates are left, and lists them once there are few:
 *
 *   [capture-finder] red: 14 bytes left: 0x00A3C1F4 (red 01, other 00) ...
 *   [capture-finder] bar: 3 floats left: 0x00A3C230 = 0.62 ...
 *
 * F9 starts both searches again. Copies are kept in memory (about 1 GB at
 * most) until the game closes.
 */
#include "engine_capture_finder.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C"
{
    extern uint8_t* vm_base;
}

namespace
{
    /* What is searched: main memory (the game's heap) and the game's own
     * data and bss above 0x10000000; only pages that exist. */
    struct Range
    {
        uint32_t guestStart;
        uint32_t bytes;
    };
    constexpr Range kRanges[] = {
        {0x00010000u, 0x0FFF0000u},
        {0x10000000u, 0x01000000u},
    };
    constexpr size_t kListLimit = 40;

    struct Block
    {
        uint32_t guestStart;
        uint32_t bytes;
        size_t offset;          /* into the snapshots */
    };

    std::vector<Block> blocks;
    size_t totalBytes = 0;

    /* Colour search. */
    std::vector<uint8_t> redValues;
    std::vector<uint8_t> otherValues;
    std::vector<uint8_t> colourAlive;    /* one bit a byte */
    bool haveRed = false;
    bool haveOther = false;
    bool colourStarted = false;
    unsigned redPresses = 0;
    unsigned otherPresses = 0;

    /* Bar search. */
    std::vector<uint8_t> previousBar;
    std::vector<uint8_t> barAlive;       /* one bit a 4-byte word */
    bool haveBarStart = false;
    unsigned barPresses = 0;

    void findBlocks()
    {
        if (!blocks.empty())
        {
            return;
        }
        for (const Range& range : kRanges)
        {
            uint32_t guestAddress = range.guestStart;
            const uint64_t rangeEnd = (uint64_t)range.guestStart + range.bytes;
            while (guestAddress < rangeEnd)
            {
                MEMORY_BASIC_INFORMATION memoryInfo;
                if (!VirtualQuery(vm_base + guestAddress, &memoryInfo, sizeof(memoryInfo)))
                {
                    break;
                }
                const uint64_t regionEnd = (uint64_t)((uint8_t*)memoryInfo.BaseAddress - vm_base) +
                                           memoryInfo.RegionSize;
                const uint64_t blockEnd = regionEnd < rangeEnd ? regionEnd : rangeEnd;
                const bool readable = memoryInfo.State == MEM_COMMIT &&
                                      !(memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                                      (memoryInfo.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                                             PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
                if (readable && blockEnd > guestAddress)
                {
                    blocks.push_back({guestAddress, (uint32_t)(blockEnd - guestAddress), totalBytes});
                    totalBytes += (size_t)(blockEnd - guestAddress);
                }
                if (blockEnd <= guestAddress)
                {
                    break;
                }
                guestAddress = (uint32_t)blockEnd;
            }
        }
        std::fprintf(stderr, "[capture-finder] searching %zu blocks, %.0f MB\n", blocks.size(),
                     (double)totalBytes / (1024.0 * 1024.0));
    }

    bool takeSnapshot(std::vector<uint8_t>& target)
    {
        findBlocks();
        try
        {
            target.resize(totalBytes);
        }
        catch (...)
        {
            std::fprintf(stderr, "[capture-finder] not enough memory for a copy of the game's memory\n");
            return false;
        }
        for (const Block& block : blocks)
        {
            std::memcpy(target.data() + block.offset, vm_base + block.guestStart, block.bytes);
        }
        return true;
    }

    uint32_t guestAddressOf(size_t offset)
    {
        for (const Block& block : blocks)
        {
            if (offset >= block.offset && offset < block.offset + block.bytes)
            {
                return block.guestStart + (uint32_t)(offset - block.offset);
            }
        }
        return 0;
    }

    bool bitSet(const std::vector<uint8_t>& bits, size_t index)
    {
        return (bits[index >> 3] >> (index & 7u)) & 1u;
    }

    void bitClear(std::vector<uint8_t>& bits, size_t index)
    {
        bits[index >> 3] &= (uint8_t)~(1u << (index & 7u));
    }

    float readFloat(const uint8_t* bytes)
    {
        const uint32_t bigEndian = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
                                   ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
        float value;
        std::memcpy(&value, &bigEndian, sizeof(value));
        return value;
    }

    bool saneBarValue(float value)
    {
        return std::isfinite(value) && std::fabs(value) < 1.0e6f;
    }

    void reportColour()
    {
        size_t alive = 0;
        for (size_t byteIndex = 0; byteIndex < totalBytes; byteIndex++)
        {
            if (bitSet(colourAlive, byteIndex))
            {
                alive++;
            }
        }
        std::fprintf(stderr, "[capture-finder] red: %zu bytes left after %u red and %u other presses\n", alive,
                     redPresses, otherPresses);
        if (alive > 0 && alive <= kListLimit)
        {
            for (size_t byteIndex = 0; byteIndex < totalBytes; byteIndex++)
            {
                if (bitSet(colourAlive, byteIndex))
                {
                    std::fprintf(stderr, "[capture-finder]   0x%08X: red %02X, other %02X\n",
                                 guestAddressOf(byteIndex), redValues[byteIndex], otherValues[byteIndex]);
                }
            }
        }
        std::fflush(stderr);
    }

    void reportBar(const std::vector<uint8_t>& current)
    {
        size_t alive = 0;
        const size_t wordCount = totalBytes / 4u;
        for (size_t wordIndex = 0; wordIndex < wordCount; wordIndex++)
        {
            if (bitSet(barAlive, wordIndex))
            {
                alive++;
            }
        }
        std::fprintf(stderr, "[capture-finder] bar: %zu floats left after %u presses\n", alive, barPresses);
        if (alive > 0 && alive <= kListLimit)
        {
            for (size_t wordIndex = 0; wordIndex < wordCount; wordIndex++)
            {
                if (bitSet(barAlive, wordIndex))
                {
                    std::fprintf(stderr, "[capture-finder]   0x%08X = %g\n", guestAddressOf(wordIndex * 4u),
                                 (double)readFloat(current.data() + wordIndex * 4u));
                }
            }
        }
        std::fflush(stderr);
    }

    void colourPress(bool isRed)
    {
        std::vector<uint8_t> current;
        if (!takeSnapshot(current))
        {
            return;
        }
        if (isRed)
        {
            redPresses++;
        }
        else
        {
            otherPresses++;
        }
        if (!colourStarted)
        {
            std::vector<uint8_t>& reference = isRed ? redValues : otherValues;
            bool& haveReference = isRed ? haveRed : haveOther;
            if (!haveReference)
            {
                reference.swap(current);
                haveReference = true;
            }
            else
            {
                /* A second press of the same kind before the other one:
                 * nothing to compare yet; keep the newest. */
                reference.swap(current);
            }
            if (haveRed && haveOther)
            {
                colourStarted = true;
                colourAlive.assign((totalBytes + 7u) / 8u, 0);
                for (size_t byteIndex = 0; byteIndex < totalBytes; byteIndex++)
                {
                    if (redValues[byteIndex] != otherValues[byteIndex])
                    {
                        colourAlive[byteIndex >> 3] |= (uint8_t)(1u << (byteIndex & 7u));
                    }
                }
                reportColour();
            }
            else
            {
                std::fprintf(stderr, "[capture-finder] %s noted; now press %s\n", isRed ? "red" : "not red",
                             isRed ? "F6 when the Id is not red" : "F5 when the Id is red");
                std::fflush(stderr);
            }
            return;
        }
        const std::vector<uint8_t>& reference = isRed ? redValues : otherValues;
        for (size_t byteIndex = 0; byteIndex < totalBytes; byteIndex++)
        {
            if (bitSet(colourAlive, byteIndex) && current[byteIndex] != reference[byteIndex])
            {
                bitClear(colourAlive, byteIndex);
            }
        }
        reportColour();
    }

    void barPress(bool wentUp)
    {
        std::vector<uint8_t> current;
        if (!takeSnapshot(current))
        {
            return;
        }
        barPresses++;
        const size_t wordCount = totalBytes / 4u;
        if (!haveBarStart)
        {
            barAlive.assign((wordCount + 7u) / 8u, 0xFF);
            haveBarStart = true;
            previousBar.swap(current);
            std::fprintf(stderr, "[capture-finder] bar starting point noted; now F7 each time it has gone up "
                                 "since the last press, F8 each time it has gone down\n");
            std::fflush(stderr);
            return;
        }
        for (size_t wordIndex = 0; wordIndex < wordCount; wordIndex++)
        {
            if (!bitSet(barAlive, wordIndex))
            {
                continue;
            }
            const float before = readFloat(previousBar.data() + wordIndex * 4u);
            const float now = readFloat(current.data() + wordIndex * 4u);
            const bool keep = saneBarValue(before) && saneBarValue(now) && (wentUp ? now > before : now < before);
            if (!keep)
            {
                bitClear(barAlive, wordIndex);
            }
        }
        reportBar(current);
        previousBar.swap(current);
    }
}

void engineCaptureFinderKey(int key)
{
    if (!vm_base)
    {
        return;
    }
    switch (key)
    {
    case 5:
        colourPress(true);
        break;
    case 6:
        colourPress(false);
        break;
    case 7:
        barPress(true);
        break;
    case 8:
        barPress(false);
        break;
    case 9:
        std::vector<uint8_t>().swap(redValues);
        std::vector<uint8_t>().swap(otherValues);
        std::vector<uint8_t>().swap(colourAlive);
        std::vector<uint8_t>().swap(previousBar);
        std::vector<uint8_t>().swap(barAlive);
        haveRed = false;
        haveOther = false;
        colourStarted = false;
        haveBarStart = false;
        redPresses = 0;
        otherPresses = 0;
        barPresses = 0;
        std::fprintf(stderr, "[capture-finder] both searches start again\n");
        std::fflush(stderr);
        break;
    default:
        break;
    }
}
