/*
 * spurs_native.cpp -- Folklore's own SPURS: the cellSpurs calls the game
 * makes, answered without Sony's libsre.
 *
 * Folklore uses SPURS in one narrow way (folklore/docs/SPURS.md, from traces
 * of the real library): one SPU, one job chain at a time, whose command list
 * is a GUARD, then JOB / SYNC pairs, then a NEXT back to the start. The game
 * releases one pass of the chain with cellSpursJobGuardNotify. So a chain
 * here is a host thread that walks its command list:
 *
 *   GUARD  wait until the guard's count is 0 (cellSpursJobGuardNotify takes
 *          it there), then put it back to its reset value; or stop, when the
 *          chain has been shut down
 *   JOB    run the job (spurs_native_job.c)
 *   SYNC   nothing: one worker runs the jobs in order anyway
 *   NEXT   jump
 *
 * Anything else stops the chain with a message: it is not something Folklore
 * was seen to do. The jobs' events to the PPU (sys_spu_thread_throw_event on
 * the port cellSpursAttachLv2EventQueue bound) go through the runtime's
 * SPURS port table, which the runtime's own cellSpursAttachLv2EventQueue
 * fills.
 *
 * These handlers are registered for every build, ahead of the runtime's
 * generic cellSpurs, and are what the game runs on. Sony's libsre, lifted
 * from the player's own firmware, is loaded only for comparison
 * (FOLKLORE_SPURS=libsre, run.ps1 -Libsre); it then takes precedence, since
 * ps3_hle_call asks the PRX export table first. The game's cellSyncMutex
 * calls, which libsre also serves, go to the runtime's cellSync otherwise.
 */
#include "ppu_recomp.h"
#include "spurs_native_job.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" int32_t cellSpursAttachLv2EventQueue(void* spurs, uint32_t queue, uint8_t* port, int32_t isDynamic);
extern "C" int32_t cellSpursDetachLv2EventQueue(void* spurs, uint8_t port);
extern "C" int spurs_trace_init(void);
extern "C" void spurs_trace_api_call(uint32_t nid, const uint64_t* gpr, uint32_t lr);
extern "C" void spurs_trace_api_return(uint32_t nid, const uint64_t* argumentGpr, uint32_t result);

namespace
{
    constexpr uint64_t kCellOk = 0;
    constexpr uint32_t kSpursJobErrorNullPointer = 0x80410A11u;   /* CELL_SPURS_JOB_ERROR_NULL_POINTER */
    constexpr uint32_t kSpursJobErrorStat = 0x80410A0Fu;          /* CELL_SPURS_JOB_ERROR_STAT */

    uint32_t readGuest32(uint32_t address)
    {
        const uint8_t* bytes = vm_base + address;
        return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
               (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
    }

    uint64_t readGuest64(uint32_t address)
    {
        return (static_cast<uint64_t>(readGuest32(address)) << 32) | readGuest32(address + 4);
    }

    void writeGuest32(uint32_t address, uint32_t value)
    {
        uint8_t* bytes = vm_base + address;
        bytes[0] = static_cast<uint8_t>(value >> 24);
        bytes[1] = static_cast<uint8_t>(value >> 16);
        bytes[2] = static_cast<uint8_t>(value >> 8);
        bytes[3] = static_cast<uint8_t>(value);
    }

    /* The guard, as the real cellSpursJobGuardInitialize lays it out: u32
     * count, u32 reset value, u64 chain, u32 requestSpuCount. */
    constexpr uint32_t kGuardCount = 0x00;
    constexpr uint32_t kGuardReset = 0x04;
    constexpr uint32_t kGuardChain = 0x0C;
    constexpr uint32_t kGuardRequestSpuCount = 0x10;

    struct JobChain
    {
        uint32_t chainEa = 0;
        uint32_t commandListEa = 0;
        uint32_t guardEa = 0;
        bool guardAutoReset = false;

        std::mutex lock;
        std::condition_variable wake;      /* guard notified, or shutdown */
        std::condition_variable finished;  /* the worker has stopped */
        bool running = false;
        bool shutdownRequested = false;
        uint64_t passes = 0;

        folklore_job_runner* runner = nullptr;
    };

    /* Chains live for the whole run (a chain's struct is reused when the game
     * creates it again at the same address), and are never freed: the worker
     * threads are detached, and nothing may be destroyed under them at exit. */
    std::mutex* gChainsLock = new std::mutex();
    std::map<uint32_t, JobChain*>* gChains = new std::map<uint32_t, JobChain*>();

    JobChain* findChain(uint32_t chainEa)
    {
        std::lock_guard<std::mutex> guard(*gChainsLock);
        auto found = gChains->find(chainEa);
        return found == gChains->end() ? nullptr : found->second;
    }

    void reportOnce(const char* what, uint64_t value)
    {
        static int reportCount = 0;
        if (reportCount < 16)
        {
            reportCount++;
            std::fprintf(stderr, "[spurs-native] %s 0x%llX -- chain stopped\n", what,
                         static_cast<unsigned long long>(value));
        }
    }

    /* Wait at a GUARD command. Returns false when the chain is to stop. */
    bool passGuard(JobChain* chain, uint32_t guardEa)
    {
        std::unique_lock<std::mutex> hold(chain->lock);
        while (!chain->shutdownRequested && readGuest32(guardEa + kGuardCount) != 0)
        {
            chain->wake.wait_for(hold, std::chrono::milliseconds(50));
        }
        if (chain->shutdownRequested)
        {
            return false;
        }
        if (chain->guardAutoReset)
        {
            writeGuest32(guardEa + kGuardCount, readGuest32(guardEa + kGuardReset));
        }
        chain->passes++;
        return true;
    }

    void walkChain(JobChain* chain)
    {
        uint32_t commandAddress = chain->commandListEa;
        for (;;)
        {
            {
                std::lock_guard<std::mutex> hold(chain->lock);
                if (chain->shutdownRequested)
                {
                    break;
                }
            }
            const uint64_t command = readGuest64(commandAddress);
            const uint32_t opcode = static_cast<uint32_t>(command & 7u);
            if (opcode == 0)
            {
                /* JOB (its descriptor's address), or NOP when 0. */
                if (command != 0)
                {
                    folklore_job_runner_run(chain->runner, static_cast<uint32_t>(command));
                }
                commandAddress += 8;
            }
            else if (opcode == 2)
            {
                /* SYNC / LWSYNC: the one worker runs the jobs in order. */
                commandAddress += 8;
            }
            else if (opcode == 3)
            {
                /* NEXT */
                commandAddress = static_cast<uint32_t>(command & ~7ull);
            }
            else if (opcode == 7 && ((command >> 3) & 15u) == 1u)
            {
                /* GUARD */
                if (!passGuard(chain, static_cast<uint32_t>(command & ~0x7Full)))
                {
                    break;
                }
                commandAddress += 8;
            }
            else if (opcode == 7 && ((command >> 3) & 15u) == 15u)
            {
                /* END */
                break;
            }
            else
            {
                reportOnce("unsupported job-chain command", command);
                break;
            }
        }
        std::lock_guard<std::mutex> hold(chain->lock);
        chain->running = false;
        chain->finished.notify_all();
    }

    void returnValue(ppu_context* context, uint64_t value)
    {
        context->gpr[3] = value;
    }

    /* cellSpursInitialize(spurs, nSpu, spuPriority, ppuPriority, exitIfNoWork) */
    void hleInitialize(ppu_context* context)
    {
        std::fprintf(stderr, "[spurs-native] cellSpursInitialize spurs=0x%08X nSpu=%u (native SPURS, no libsre)\n",
                     static_cast<uint32_t>(context->gpr[3]), static_cast<uint32_t>(context->gpr[4]));
        returnValue(context, kCellOk);
    }

    /* cellSpursFinalize(spurs) */
    void hleFinalize(ppu_context* context)
    {
        std::lock_guard<std::mutex> guard(*gChainsLock);
        for (auto& entry : *gChains)
        {
            std::lock_guard<std::mutex> hold(entry.second->lock);
            entry.second->shutdownRequested = true;
            entry.second->wake.notify_all();
        }
        returnValue(context, kCellOk);
    }

    /* cellSpursAttachLv2EventQueue(spurs, queue, port*, isDynamic) */
    void hleAttachLv2EventQueue(ppu_context* context)
    {
        const int32_t result = cellSpursAttachLv2EventQueue(
            reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(context->gpr[3]))),
            static_cast<uint32_t>(context->gpr[4]),
            reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(static_cast<uint32_t>(context->gpr[5]))),
            static_cast<int32_t>(context->gpr[6]));
        returnValue(context, static_cast<uint32_t>(result));
    }

    /* cellSpursDetachLv2EventQueue(spurs, port) */
    void hleDetachLv2EventQueue(ppu_context* context)
    {
        const int32_t result = cellSpursDetachLv2EventQueue(
            reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(context->gpr[3]))),
            static_cast<uint8_t>(context->gpr[4]));
        returnValue(context, static_cast<uint32_t>(result));
    }

    /* cellSpursCreateJobChain(spurs, chain, commandList, sizeJobDescriptor,
     *                         maxGrabbedJob, priorities, maxContention,
     *                         autoReadyCount, tag1, tag2) */
    void hleCreateJobChain(ppu_context* context)
    {
        const uint32_t chainEa = static_cast<uint32_t>(context->gpr[4]);
        const uint32_t commandListEa = static_cast<uint32_t>(context->gpr[5]);
        const uint32_t descriptorSize = static_cast<uint32_t>(context->gpr[6]);
        if (!chainEa || !commandListEa)
        {
            returnValue(context, kSpursJobErrorNullPointer);
            return;
        }
        if (descriptorSize != 0x80u)
        {
            std::fprintf(stderr, "[spurs-native] CreateJobChain: descriptor size 0x%X (only 0x80 seen in Folklore)\n",
                         descriptorSize);
        }
        JobChain* chain = nullptr;
        {
            std::lock_guard<std::mutex> guard(*gChainsLock);
            auto found = gChains->find(chainEa);
            if (found == gChains->end())
            {
                chain = new JobChain();
                chain->chainEa = chainEa;
                chain->runner = folklore_job_runner_create();
                (*gChains)[chainEa] = chain;
            }
            else
            {
                chain = found->second;
            }
        }
        std::lock_guard<std::mutex> hold(chain->lock);
        if (chain->running)
        {
            returnValue(context, kSpursJobErrorStat);
            return;
        }
        chain->commandListEa = commandListEa;
        chain->guardEa = 0;
        chain->shutdownRequested = false;
        chain->passes = 0;
        folklore_job_runner_reset(chain->runner);
        returnValue(context, kCellOk);
    }

    /* cellSpursJobGuardInitialize(chain, guard, notifyCount, requestSpuCount, autoReset) */
    void hleJobGuardInitialize(ppu_context* context)
    {
        const uint32_t chainEa = static_cast<uint32_t>(context->gpr[3]);
        const uint32_t guardEa = static_cast<uint32_t>(context->gpr[4]);
        const uint32_t notifyCount = static_cast<uint32_t>(context->gpr[5]);
        JobChain* chain = findChain(chainEa);
        if (!chain || !guardEa)
        {
            returnValue(context, kSpursJobErrorNullPointer);
            return;
        }
        std::lock_guard<std::mutex> hold(chain->lock);
        writeGuest32(guardEa + kGuardCount, notifyCount);
        writeGuest32(guardEa + kGuardReset, notifyCount);
        writeGuest32(guardEa + 0x08, 0);
        writeGuest32(guardEa + kGuardChain, chainEa);
        writeGuest32(guardEa + kGuardRequestSpuCount, static_cast<uint32_t>(context->gpr[6]));
        writeGuest32(guardEa + 0x14, 0);
        chain->guardEa = guardEa;
        chain->guardAutoReset = (context->gpr[7] & 0xFFu) != 0;
        returnValue(context, kCellOk);
    }

    /* cellSpursJobGuardNotify(guard) */
    void hleJobGuardNotify(ppu_context* context)
    {
        const uint32_t guardEa = static_cast<uint32_t>(context->gpr[3]);
        if (!guardEa)
        {
            returnValue(context, kSpursJobErrorNullPointer);
            return;
        }
        JobChain* chain = findChain(readGuest32(guardEa + kGuardChain));
        if (!chain)
        {
            returnValue(context, kSpursJobErrorStat);
            return;
        }
        std::lock_guard<std::mutex> hold(chain->lock);
        const uint32_t count = readGuest32(guardEa + kGuardCount);
        if (count > 0)
        {
            writeGuest32(guardEa + kGuardCount, count - 1);
        }
        chain->wake.notify_all();
        returnValue(context, kCellOk);
    }

    /* cellSpursKickJobChain(chain, numReadyCount) */
    void hleKickJobChain(ppu_context* context)
    {
        JobChain* chain = findChain(static_cast<uint32_t>(context->gpr[3]));
        if (!chain)
        {
            returnValue(context, kSpursJobErrorNullPointer);
            return;
        }
        std::lock_guard<std::mutex> hold(chain->lock);
        if (!chain->running && !chain->shutdownRequested)
        {
            chain->running = true;
            std::thread(walkChain, chain).detach();
        }
        returnValue(context, kCellOk);
    }

    /* cellSpursShutdownJobChain(chain) */
    void hleShutdownJobChain(ppu_context* context)
    {
        JobChain* chain = findChain(static_cast<uint32_t>(context->gpr[3]));
        if (!chain)
        {
            returnValue(context, kSpursJobErrorNullPointer);
            return;
        }
        std::lock_guard<std::mutex> hold(chain->lock);
        chain->shutdownRequested = true;
        chain->wake.notify_all();
        returnValue(context, kCellOk);
    }

    /* cellSpursJoinJobChain(chain) */
    void hleJoinJobChain(ppu_context* context)
    {
        JobChain* chain = findChain(static_cast<uint32_t>(context->gpr[3]));
        if (!chain)
        {
            returnValue(context, kSpursJobErrorNullPointer);
            return;
        }
        std::unique_lock<std::mutex> hold(chain->lock);
        while (chain->running)
        {
            chain->finished.wait(hold);
        }
        returnValue(context, kCellOk);
    }

    /* Each handler, recorded by SPURS_GOLDEN_TRACE (run.ps1 -TraceSpurs) the
     * same way the real library's calls are, so the two traces compare. */
    template <uint32_t Nid, void (*Handler)(ppu_context*)>
    void traced(ppu_context* context)
    {
        if (!spurs_trace_init())
        {
            Handler(context);
            return;
        }
        uint64_t arguments[11];
        std::memcpy(arguments, context->gpr, sizeof(arguments));
        spurs_trace_api_call(Nid, context->gpr, static_cast<uint32_t>(context->lr));
        Handler(context);
        spurs_trace_api_return(Nid, arguments, static_cast<uint32_t>(context->gpr[3]));
    }

    struct HandlerEntry
    {
        uint32_t nid;
        const char* name;
        void (*handler)(ppu_context*);
    };

    const HandlerEntry kHandlers[] = {
        { 0xACFC8DBCu, "cellSpursInitialize",          traced<0xACFC8DBCu, hleInitialize> },
        { 0xCA4C4600u, "cellSpursFinalize",            traced<0xCA4C4600u, hleFinalize> },
        { 0xB9BC6207u, "cellSpursAttachLv2EventQueue", traced<0xB9BC6207u, hleAttachLv2EventQueue> },
        { 0x4E66D483u, "cellSpursDetachLv2EventQueue", traced<0x4E66D483u, hleDetachLv2EventQueue> },
        { 0x60EB2DECu, "cellSpursCreateJobChain",      traced<0x60EB2DECu, hleCreateJobChain> },
        { 0xBFEA60FAu, "cellSpursKickJobChain",        traced<0xBFEA60FAu, hleKickJobChain> },
        { 0x738E40E6u, "cellSpursShutdownJobChain",    traced<0x738E40E6u, hleShutdownJobChain> },
        { 0xA7C066DEu, "cellSpursJoinJobChain",        traced<0xA7C066DEu, hleJoinJobChain> },
        { 0x68AAEBA9u, "cellSpursJobGuardInitialize",  traced<0x68AAEBA9u, hleJobGuardInitialize> },
        { 0xD5D0B256u, "cellSpursJobGuardNotify",      traced<0xD5D0B256u, hleJobGuardNotify> },
    };

    struct Registrar
    {
        Registrar()
        {
            for (const HandlerEntry& entry : kHandlers)
            {
                ps3_hle_register_ctx(entry.nid, entry.name, entry.handler);
            }
        }
    };

    Registrar gRegistrar;
}
