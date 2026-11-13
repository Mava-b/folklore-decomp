/*
 * engine_motion_watch.cpp -- a diagnostic: which of the game's functions
 * read the motion input, and from where they are called (F3).
 *
 * Why: auto-capture (motion controls off) feeds every capture full-scale
 * motion, which finishes the ordinary "pull" captures but not the boss Ids'
 * Time / Beat / Balance / Stop & Go captures, which need the right motion at
 * the right moment. To do those automatically the port has to know the
 * game's capture state, and the way in is the code that reads the motion.
 *
 * Round 1 watched the pad library's pad array: only the library and one
 * reader touch the sensor words; the reader (func_000F8FC8, called four times
 * a frame by func_000F90A0) copies them into the game's motion object, which
 * keeps the last ten frames of the four sensor values (+64..+207) and what
 * it makes of them (+244..+259). Round 2 (this) watches the motion object:
 * the capture code reads it.
 *
 * F3 starts a 20-second watch. The page holding the object is made a guard
 * page; every access to it stops once in the exception handler below, which
 * notes the access (read or write, which field) and walks the host stack --
 * the lifted game functions call each other directly, so the host stack is
 * the game's call chain -- and turns each frame back into the game function
 * it belongs to (function_table). The page is armed again after each access
 * (single step). At the end the log gets one line per distinct call chain,
 * with counts and the fields it touched:
 *
 *   [motion-watch] read motion: 812 reads, 0 writes at +64,+68 by
 *       func_000A7ED0 <- func_000A5230 <- ...
 *
 * Accesses to the rest of the page are only counted. Each access costs two
 * exceptions, so the game runs slower while it watches; it stops by itself
 * after 20 s, or press F3 again.
 */
#include "engine_motion_watch.h"
#include "engine_region.h"

#ifndef _WIN32
/* The watch uses Windows guard pages and a vectored exception handler; on
 * other platforms F3 does nothing (it is a diagnostic, not a game feature). */
void engineMotionWatchToggle() {}
void engineMotionWatchUpdate() {}
bool engineMotionWatchActive(unsigned* secondsLeft)
{
    if (secondsLeft)
    {
        *secondsLeft = 0;
    }
    return false;
}
#else
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
    /* The game's motion object (the TOC slot func_000F4AC4 passes to the
     * motion reader; the same in both releases of a version). The pad
     * library's pad array, watched in round 1: 1.00 0x109547F0 (104-byte
     * pads), 1.10 0x10955AFC (108-byte pads). */
#if FOLKLORE_GAME_VERSION == 110
    constexpr uint32_t kWatchStart = 0x1014ED5Cu;
#else
    constexpr uint32_t kWatchStart = 0x1014ED4Cu;
#endif
    constexpr uint32_t kWatchBytes = 0x180u;
    constexpr uint32_t kPad = 0u;
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
        uint64_t wordMask[2];    /* bit n: bytes 4n..4n+3 of the object were where an access started */
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
            const uint32_t word = offset / 4u;
            if (word < 128u)
            {
                found->wordMask[word / 64u] |= 1ull << (word % 64u);
            }
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
                bool sensor = false;
                if (guestAddress >= kWatchStart && guestAddress < kWatchStart + kWatchBytes)
                {
                    sensor = true;
                    noteSensorAccess(pointers->ContextRecord, isWrite, kPad, guestAddress - kWatchStart);
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

    void printChain(const Chain& chain, const char* label)
    {
        char offsets[400];
        int written = 0;
        for (uint32_t word = 0; word < 128 && written < (int)sizeof(offsets) - 8; word++)
        {
            if (chain.wordMask[word / 64u] & (1ull << (word % 64u)))
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
        std::fprintf(stderr, "[motion-watch] %s motion: %llu reads, %llu writes at %s by %s\n", label,
                     (unsigned long long)chain.reads, (unsigned long long)chain.writes, written ? offsets : "?",
                     chain.frameCount ? frameText : "(no game code on the stack)");
    }

    void printReport()
    {
        while (chainLock.test_and_set(std::memory_order_acquire))
        {
            YieldProcessor();
        }
        std::fprintf(stderr, "[motion-watch] finished: %llu accesses to the motion object in %zu call chains, "
                             "%llu other accesses to the page%s\n",
                     (unsigned long long)sensorAccesses, chainCount, (unsigned long long)otherAccesses,
                     droppedChains ? " (some chains not recorded: too many)" : "");
        for (size_t chainIndex = 0; chainIndex < chainCount; chainIndex++)
        {
            if (chains[chainIndex].reads)
            {
                printChain(chains[chainIndex], "read");
            }
        }
        for (size_t chainIndex = 0; chainIndex < chainCount; chainIndex++)
        {
            if (!chains[chainIndex].reads)
            {
                printChain(chains[chainIndex], "write");
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
    const uintptr_t first = (uintptr_t)vm_base + kWatchStart;
    const uintptr_t last = first + kWatchBytes;
    pageStart = (uint8_t*)(first & ~(pageSize - 1u));
    pageBytes = (SIZE_T)(((last + pageSize - 1u) & ~(pageSize - 1u)) - (uintptr_t)pageStart);
    MEMORY_BASIC_INFORMATION memoryInfo;
    if (!VirtualQuery(pageStart, &memoryInfo, sizeof(memoryInfo)) || memoryInfo.State != MEM_COMMIT)
    {
        std::fprintf(stderr, "[motion-watch] the motion object's memory is not there yet; try again in game\n");
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
        std::fprintf(stderr, "[motion-watch] could not watch the motion object (error %lu)\n", GetLastError());
        std::fflush(stderr);
        return;
    }
    std::fprintf(stderr, "[motion-watch] started (20 s; motion object 0x%08X, %zu game functions mapped)\n",
                 kWatchStart, hostFunctions.size());
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
#endif /* _WIN32 */
