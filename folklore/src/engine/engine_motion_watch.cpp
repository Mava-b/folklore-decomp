/*
 * engine_motion_watch.cpp -- a diagnostic: which of the game's functions
 * read the motion sensor values, and from where they are called (F3).
 *
 * Why: auto-capture (motion controls off) feeds every capture full-scale
 * motion, which finishes the ordinary "pull" captures but not the boss Ids'
 * Time / Beat / Balance / Stop & Go captures, which need the right motion at
 * the right moment. To do those automatically the port has to know the
 * game's capture state, and the way in is the code that reads the sensors.
 * The game's pad library keeps them in its pad array (three pads; the four
 * sensor words, each minus 512, at +24..+31), but nothing in the game's
 * code reads them there in any way a reading of the code can find.
 *
 * So: F3 starts a 20-second watch. The page holding the pad array is made a
 * guard page; every access to it stops once in the exception handler below,
 * which notes the access (read or write, which field) and walks the host
 * stack -- the lifted game functions call each other directly, so the host
 * stack is the game's call chain -- and turns each frame back into the game
 * function it belongs to (function_table). The page is armed again after
 * each access (single step). At the end the log gets one line per distinct
 * call chain, with counts:
 *
 *   [motion-watch] 812 reads of sensors (pad 0, +24..+31) by
 *       func_001CEA08 <- func_0023F1C0 <- func_00241A88 <- ...
 *
 * Only the sensor fields are reported in full; other accesses to the page
 * are counted. Each access costs two exceptions, so the game runs slower
 * while it watches; it stops by itself after 20 s, or press F3 again.
 */
#include "engine_motion_watch.h"
#include "engine_region.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C"
{
    extern uint8_t* vm_base;
    struct ppu_context;
    typedef struct
    {
        uint64_t addr;
        void (*func)(ppu_context*);
        const char* name;
    } func_entry;
    extern const func_entry function_table[];
    extern const uint64_t function_table_count;
}

namespace
{
    /* The pad library's array of three pads (TOC slot of its pad lookup,
     * folklore/docs/REGIONS.md): the 1.10 update added a field, so a pad is
     * 108 bytes there instead of 104. */
#if FOLKLORE_GAME_VERSION == 110
    constexpr uint32_t kPadArray = 0x10955AFCu;
    constexpr uint32_t kPadStride = 108u;
#else
    constexpr uint32_t kPadArray = 0x109547F0u;
    constexpr uint32_t kPadStride = 104u;
#endif
    constexpr uint32_t kPadCount = 3u;
    constexpr uint32_t kSensorEnd = 32u;
    constexpr unsigned long long kWatchMs = 20000ull;
    constexpr int kMaxFrames = 14;
    constexpr size_t kMaxChains = 256;

    struct HostFunction
    {
        uintptr_t hostAddress;
        uint32_t guestAddress;
    };

    struct Chain
    {
        uint32_t frames[kMaxFrames];
        int frameCount;
        uint32_t pad;
        uint64_t reads;
        uint64_t writes;
        uint32_t wordMask;       /* bit n: the pad's bytes 4n..4n+3 were where an access started */
    };

    std::vector<HostFunction> hostFunctions;
    uintptr_t hostFunctionsEnd = 0;
    PVOID handlerHandle = nullptr;
    std::atomic<bool> watching{false};
    uint8_t* pageStart = nullptr;
    SIZE_T pageBytes = 0;
    DWORD originalProtection = 0;
    unsigned long long watchStartMs = 0;

    /* Taken inside the exception handler, so a plain spin lock (no waits
     * that could need the loader or the heap). */
    std::atomic_flag chainLock = ATOMIC_FLAG_INIT;
    Chain chains[kMaxChains];
    size_t chainCount = 0;
    uint64_t droppedChains = 0;
    uint64_t otherAccesses = 0;
    uint64_t sensorAccesses = 0;

    thread_local bool rearmAfterStep = false;

    void buildFunctionMap()
    {
        if (!hostFunctions.empty())
        {
            return;
        }
        hostFunctions.reserve((size_t)function_table_count);
        for (uint64_t entryIndex = 0; entryIndex < function_table_count; entryIndex++)
        {
            const func_entry& entry = function_table[entryIndex];
            if (entry.func)
            {
                hostFunctions.push_back({(uintptr_t)entry.func, (uint32_t)entry.addr});
            }
        }
        std::sort(hostFunctions.begin(), hostFunctions.end(),
                  [](const HostFunction& left, const HostFunction& right)
                  {
                      return left.hostAddress < right.hostAddress;
                  });
        /* Past the last lifted function: anything further is not game code. */
        hostFunctionsEnd = hostFunctions.empty() ? 0 : hostFunctions.back().hostAddress + 0x40000;
    }

    /* The game function a host code address is in; 0 if it is not game code. */
    uint32_t guestFunctionFor(uintptr_t hostAddress)
    {
        if (hostFunctions.empty() || hostAddress < hostFunctions.front().hostAddress ||
            hostAddress >= hostFunctionsEnd)
        {
            return 0;
        }
        size_t low = 0;
        size_t high = hostFunctions.size();
        while (high - low > 1)
        {
            const size_t middle = (low + high) / 2;
            if (hostFunctions[middle].hostAddress <= hostAddress)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }
        return hostFunctions[low].guestAddress;
    }

    /* The game functions on the stack at the faulting instruction, innermost
     * first, with runs of the same function folded. */
    int walkGameFrames(const CONTEXT* faultContext, uint32_t* frames)
    {
        CONTEXT context = *faultContext;
        int frameCount = 0;
        for (int depth = 0; depth < 64 && frameCount < kMaxFrames && context.Rip; depth++)
        {
            const uint32_t guestFunction = guestFunctionFor((uintptr_t)context.Rip);
            if (guestFunction && (frameCount == 0 || frames[frameCount - 1] != guestFunction))
            {
                frames[frameCount] = guestFunction;
                frameCount++;
            }
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION functionEntry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!functionEntry)
            {
                /* A leaf function (no unwind data) can only be the faulting
                 * one: its return address is on top of the stack. Anywhere
                 * else the walk has gone wrong; stop. */
                if (depth > 0 || !context.Rsp)
                {
                    break;
                }
                context.Rip = *(const DWORD64*)context.Rsp;
                context.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, functionEntry, &context, &handlerData,
                             &establisherFrame, nullptr);
        }
        return frameCount;
    }

    void noteSensorAccess(const CONTEXT* faultContext, bool isWrite, uint32_t pad, uint32_t offset)
    {
        uint32_t frames[kMaxFrames];
        const int frameCount = walkGameFrames(faultContext, frames);
        while (chainLock.test_and_set(std::memory_order_acquire))
        {
            YieldProcessor();
        }
        sensorAccesses++;
        Chain* found = nullptr;
        for (size_t chainIndex = 0; chainIndex < chainCount; chainIndex++)
        {
            Chain& chain = chains[chainIndex];
            if (chain.pad == pad && chain.frameCount == frameCount &&
                std::memcmp(chain.frames, frames, sizeof(uint32_t) * (size_t)frameCount) == 0)
            {
                found = &chain;
                break;
            }
        }
        if (!found && chainCount < kMaxChains)
        {
            found = &chains[chainCount];
            chainCount++;
            std::memset(found, 0, sizeof(*found));
            std::memcpy(found->frames, frames, sizeof(uint32_t) * (size_t)frameCount);
            found->frameCount = frameCount;
            found->pad = pad;
        }
        if (found)
        {
            if (isWrite)
            {
                found->writes++;
            }
            else
            {
                found->reads++;
            }
            found->wordMask |= 1u << (offset / 4u);
        }
        else
        {
            droppedChains++;
        }
        chainLock.clear(std::memory_order_release);
    }

    void armPage()
    {
        DWORD previous = 0;
        VirtualProtect(pageStart, pageBytes, originalProtection | PAGE_GUARD, &previous);
    }

    LONG CALLBACK watchHandler(PEXCEPTION_POINTERS pointers)
    {
        const DWORD code = pointers->ExceptionRecord->ExceptionCode;
        if (code == STATUS_GUARD_PAGE_VIOLATION)
        {
            const uintptr_t address = (uintptr_t)pointers->ExceptionRecord->ExceptionInformation[1];
            if (!pageStart || address < (uintptr_t)pageStart || address >= (uintptr_t)pageStart + pageBytes)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (watching.load(std::memory_order_relaxed))
            {
                const bool isWrite = pointers->ExceptionRecord->ExceptionInformation[0] == 1;
                const uint32_t guestAddress = (uint32_t)(address - (uintptr_t)vm_base);
                const uint32_t arrayEnd = kPadArray + kPadStride * kPadCount;
                bool sensor = false;
                if (guestAddress >= kPadArray && guestAddress < arrayEnd)
                {
                    const uint32_t pad = (guestAddress - kPadArray) / kPadStride;
                    const uint32_t offset = (guestAddress - kPadArray) % kPadStride;
                    /* Every access to the pads is recorded (a copy of a
                     * whole pad would read the sensors too); the report
                     * puts the ones that touch the sensor words first. */
                    sensor = true;
                    noteSensorAccess(pointers->ContextRecord, isWrite, pad, offset);
                }
                if (!sensor)
                {
                    otherAccesses++;
                }
                /* Step over the access, then guard the page again. */
                rearmAfterStep = true;
                pointers->ContextRecord->EFlags |= 0x100;
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code == STATUS_SINGLE_STEP && rearmAfterStep)
        {
            rearmAfterStep = false;
            if (watching.load(std::memory_order_relaxed))
            {
                armPage();
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool touchesSensors(const Chain& chain)
    {
        /* Words 6 and 7 are the sensors (+24..+31); an access starting at
         * +16 or +20 can be a wide copy that runs over them. */
        return (chain.wordMask & 0xF0u) != 0;
    }

    void printChain(const Chain& chain, const char* label)
    {
        char offsets[160];
        int written = 0;
        for (uint32_t word = 0; word < 32 && written < (int)sizeof(offsets) - 8; word++)
        {
            if (chain.wordMask & (1u << word))
            {
                written += std::snprintf(offsets + written, sizeof(offsets) - (size_t)written, "%s+%u",
                                         written ? "," : "", word * 4u);
            }
        }
        char frameText[kMaxFrames * 20];
        int frameWritten = 0;
        for (int frameIndex = 0; frameIndex < chain.frameCount; frameIndex++)
        {
            frameWritten += std::snprintf(frameText + frameWritten, sizeof(frameText) - (size_t)frameWritten,
                                          "%sfunc_%08X", frameIndex ? " <- " : "", chain.frames[frameIndex]);
        }
        std::fprintf(stderr, "[motion-watch] %s pad %u: %llu reads, %llu writes at %s by %s\n", label, chain.pad,
                     (unsigned long long)chain.reads, (unsigned long long)chain.writes, written ? offsets : "?",
                     chain.frameCount ? frameText : "(no game code on the stack)");
    }

    void printReport()
    {
        while (chainLock.test_and_set(std::memory_order_acquire))
        {
            YieldProcessor();
        }
        std::fprintf(stderr, "[motion-watch] finished: %llu accesses to the pads in %zu call chains, %llu other "
                             "accesses to the page%s\n",
                     (unsigned long long)sensorAccesses, chainCount, (unsigned long long)otherAccesses,
                     droppedChains ? " (some chains not recorded: too many)" : "");
        for (size_t chainIndex = 0; chainIndex < chainCount; chainIndex++)
        {
            if (touchesSensors(chains[chainIndex]))
            {
                printChain(chains[chainIndex], "SENSORS");
            }
        }
        for (size_t chainIndex = 0; chainIndex < chainCount; chainIndex++)
        {
            if (!touchesSensors(chains[chainIndex]))
            {
                printChain(chains[chainIndex], "other");
            }
        }
        std::fflush(stderr);
        chainLock.clear(std::memory_order_release);
    }

    void stopWatch()
    {
        watching.store(false);
        DWORD previous = 0;
        VirtualProtect(pageStart, pageBytes, originalProtection, &previous);
        printReport();
    }
}

void engineMotionWatchToggle()
{
    if (watching.load())
    {
        stopWatch();
        return;
    }
    if (!vm_base)
    {
        return;
    }
    buildFunctionMap();
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    const uintptr_t pageSize = systemInfo.dwPageSize ? systemInfo.dwPageSize : 4096u;
    const uintptr_t first = (uintptr_t)vm_base + kPadArray;
    const uintptr_t last = first + kPadStride * kPadCount;
    pageStart = (uint8_t*)(first & ~(pageSize - 1u));
    pageBytes = (SIZE_T)(((last + pageSize - 1u) & ~(pageSize - 1u)) - (uintptr_t)pageStart);
    MEMORY_BASIC_INFORMATION memoryInfo;
    if (!VirtualQuery(pageStart, &memoryInfo, sizeof(memoryInfo)) || memoryInfo.State != MEM_COMMIT)
    {
        std::fprintf(stderr, "[motion-watch] the pad array's memory is not there yet; try again in game\n");
        std::fflush(stderr);
        return;
    }
    originalProtection = memoryInfo.Protect & ~(DWORD)(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    if (!handlerHandle)
    {
        handlerHandle = AddVectoredExceptionHandler(1, watchHandler);
    }
    while (chainLock.test_and_set(std::memory_order_acquire))
    {
        YieldProcessor();
    }
    chainCount = 0;
    droppedChains = 0;
    otherAccesses = 0;
    sensorAccesses = 0;
    chainLock.clear(std::memory_order_release);
    watchStartMs = GetTickCount64();
    watching.store(true);
    DWORD previous = 0;
    if (!VirtualProtect(pageStart, pageBytes, originalProtection | PAGE_GUARD, &previous))
    {
        watching.store(false);
        std::fprintf(stderr, "[motion-watch] could not watch the pad array (error %lu)\n", GetLastError());
        std::fflush(stderr);
        return;
    }
    std::fprintf(stderr, "[motion-watch] started (20 s; pad array 0x%08X, %u-byte pads, %zu game functions "
                         "mapped)\n",
                 kPadArray, kPadStride, hostFunctions.size());
    std::fflush(stderr);
}

void engineMotionWatchUpdate()
{
    if (watching.load() && GetTickCount64() - watchStartMs >= kWatchMs)
    {
        stopWatch();
    }
}

bool engineMotionWatchActive(unsigned* secondsLeft)
{
    if (!watching.load())
    {
        return false;
    }
    if (secondsLeft)
    {
        const unsigned long long elapsed = GetTickCount64() - watchStartMs;
        *secondsLeft = elapsed >= kWatchMs ? 0u : (unsigned)((kWatchMs - elapsed + 999ull) / 1000ull);
    }
    return true;
}
