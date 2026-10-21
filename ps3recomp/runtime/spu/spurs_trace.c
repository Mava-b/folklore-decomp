/*
 * spurs_trace.c -- see spurs_trace.h.
 *
 * Two modes (SPURS_GOLDEN_TRACE_MODE):
 *
 *   shape (default)  every distinct kind of event is written once, in full,
 *                    as a SHAPE line with a number; after that it is only
 *                    counted, and the counts come out once a second as COUNT
 *                    lines. A job run is one event: its entry state (registers,
 *                    job context, descriptor, stack), the job manager's and
 *                    kernel's DMA and atomics since the previous job (as a set),
 *                    and the job's own transfers (counted, not shown). A whole
 *                    play session stays small, and anything new stands out.
 *   full             every DMA, atomic and call on its own line, with data
 *                    (about 17 MB/s in Folklore -- minutes, not hours).
 *
 * Line format (all numbers hex unless named otherwise):
 *
 *   <seconds> <thread> <actor> <event> key=value ...
 *
 *   thread   "ppu<tid>" or "spu<n>"
 *   actor    PPU: "game"; SPU: "krn" (SPURS kernel, LS < 0xA00), "pm" (the
 *            policy module at LS 0xA00, the job-chain manager), "job<img>"
 *            (a job's placed binary, images 50+)
 *   event    CALL / RET              a libsre call and its result
 *            MEM                     memory behind a pointer argument
 *            GET / PUT               a DMA, after it completed            (full)
 *            GETLLAR / PUTLLC / PUTLLUC   a lock-line atomic, after it ran (full)
 *            JOB_ENTER / JOB_LEAVE   control entering / leaving a job       (full)
 *            REPEAT n=<count>        <count> more events identical to ones among
 *                                    this thread's previous 8 lines        (full)
 *            SEND                    an SPU write to its outbound (interrupt)
 *                                    mailbox -- an event or message to the PPU
 *            SHAPE id=<n> <event>    the first event of a kind            (shape)
 *            COUNT id=<n> n=<count>  more of it in the last second        (shape)
 *
 *   hash=    64-bit content hash of the bytes moved (or shown)
 *   data=    the bytes, only the first time this (ea, size, hash) appears,
 *            and only up to 512 bytes; "seen" when shown before
 */
#include "spurs_trace.h"
#include "spu_context.h"
#include "spu_dma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

extern uint8_t* vm_base;

volatile int g_spurs_trace_on = 0;

static int s_traceState = -1;          /* -1 unread, 0 off, 1 on */
static FILE* s_traceFile = NULL;
static uint64_t s_traceBytes = 0;
static uint64_t s_traceLimitBytes = 0;
static int s_traceStopped = 0;
static int s_shapeMode = 1;            /* SPURS_GOLDEN_TRACE_MODE: shape (1) or full (0) */

#ifdef _WIN32
static CRITICAL_SECTION s_traceLock;
static LARGE_INTEGER s_traceStart;
static LARGE_INTEGER s_traceFrequency;
static void traceLock(void) { EnterCriticalSection(&s_traceLock); }
static void traceUnlock(void) { LeaveCriticalSection(&s_traceLock); }
#else
static pthread_mutex_t s_traceLock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec s_traceStart;
static void traceLock(void) { pthread_mutex_lock(&s_traceLock); }
static void traceUnlock(void) { pthread_mutex_unlock(&s_traceLock); }
#endif

/* ---- content hash and "shown before" set --------------------------------- */

static uint64_t traceHash(const uint8_t* data, uint32_t size)
{
    uint64_t hash = 0x9E3779B97F4A7C15ull ^ size;
    uint32_t offset = 0;
    for (; offset + 8 <= size; offset += 8)
    {
        uint64_t word;
        memcpy(&word, data + offset, 8);
        hash ^= word;
        hash *= 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    for (; offset < size; offset++)
    {
        hash ^= data[offset];
        hash *= 0x100000001B3ull;
    }
    hash ^= hash >> 29;
    hash *= 0xC4CEB9FE1A85EC53ull;
    hash ^= hash >> 32;
    return hash;
}

#define TRACE_SEEN_BITS 22u
static uint64_t* s_seenTable = NULL;   /* open addressing, 0 = empty */

static void traceLock(void);
static void traceUnlock(void);

/* 1 if this key was already in the set (and leaves it there), else adds it. */
static int traceSeenLocked(uint64_t key);
static int traceSeen(uint64_t key)
{
    traceLock();
    const int seen = traceSeenLocked(key);
    traceUnlock();
    return seen;
}

static int traceSeenLocked(uint64_t key)
{
    if (!s_seenTable)
    {
        return 0;
    }
    if (key == 0)
    {
        key = 1;
    }
    const uint64_t mask = (1ull << TRACE_SEEN_BITS) - 1u;
    uint64_t slot = (key * 0x9E3779B97F4A7C15ull) >> (64u - TRACE_SEEN_BITS);
    for (uint32_t probe = 0; probe < 64u; probe++)
    {
        uint64_t* entry = &s_seenTable[(slot + probe) & mask];
        if (*entry == key)
        {
            return 1;
        }
        if (*entry == 0)
        {
            *entry = key;
            return 0;
        }
    }
    return 0;   /* crowded neighbourhood: show the data again */
}

/* ---- output --------------------------------------------------------------- */

static double traceSeconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_traceStart.QuadPart) / (double)s_traceFrequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - s_traceStart.tv_sec) + (double)(now.tv_nsec - s_traceStart.tv_nsec) * 1e-9;
#endif
}

static unsigned long traceThreadId(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)syscall(SYS_gettid);
#endif
}

/* Write one finished line; the caller holds the lock. */
static void traceWriteLocked(const char* line, size_t length)
{
    if (!s_traceFile || s_traceStopped)
    {
        return;
    }
    if (s_traceBytes + length > s_traceLimitBytes)
    {
        const char* note = "TRACE_LIMIT reached -- stopping (SPURS_GOLDEN_TRACE_MB)\n";
        fwrite(note, 1, strlen(note), s_traceFile);
        fflush(s_traceFile);
        s_traceStopped = 1;
        return;
    }
    fwrite(line, 1, length, s_traceFile);
    s_traceBytes += length;
    static double lastFlush = 0.0;
    const double now = traceSeconds();
    if (now - lastFlush > 1.0)
    {
        fflush(s_traceFile);
        lastFlush = now;
    }
}

/* Per-thread collapse of polling loops: an event identical (everything but
 * the time) to one of this thread's last TRACE_RECENT events is counted, not
 * written, and the count comes out as one REPEAT line before the next new
 * event. That folds a GETLLAR / test / PUTLLC idle loop down to its first lap.
 * Events with signature 0 (calls, job entry and exit) always print and start
 * a fresh window, so a loop is never folded across one of them. */
#define TRACE_RECENT 8u
static SPU_THREAD_LOCAL uint64_t t_recentSignatures[TRACE_RECENT];
static SPU_THREAD_LOCAL uint32_t t_recentNext = 0;
static SPU_THREAD_LOCAL uint32_t t_repeatCount = 0;
static SPU_THREAD_LOCAL char t_lastPrefix[48];

static int traceRecentlySeen(uint64_t signature)
{
    for (uint32_t index = 0; index < TRACE_RECENT; index++)
    {
        if (t_recentSignatures[index] == signature)
        {
            return 1;
        }
    }
    return 0;
}

/* A line growing in a fixed buffer. */
typedef struct
{
    char text[4096];
    size_t length;
} trace_line;

static void lineAppend(trace_line* line, const char* format, ...)
{
    if (line->length >= sizeof(line->text) - 2u)
    {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(line->text + line->length, sizeof(line->text) - line->length - 1u,
                                  format, arguments);
    va_end(arguments);
    if (written > 0)
    {
        line->length += (size_t)written;
        if (line->length > sizeof(line->text) - 2u)
        {
            line->length = sizeof(line->text) - 2u;
        }
    }
}

static void lineAppendHex(trace_line* line, const uint8_t* data, uint32_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    for (uint32_t index = 0; index < size && line->length + 3u < sizeof(line->text) - 2u; index++)
    {
        line->text[line->length++] = digits[data[index] >> 4];
        line->text[line->length++] = digits[data[index] & 15u];
    }
    line->text[line->length] = 0;
}

/* Hash `size` bytes and append " hash=... data=..." (data only when new);
 * returns the hash. */
static uint64_t lineAppendContent(trace_line* line, uint32_t ea, const uint8_t* data, uint32_t size)
{
    const uint64_t hash = traceHash(data, size);
    lineAppend(line, " hash=%016llX", (unsigned long long)hash);
    if (size > 512u)
    {
        return hash;
    }
    const uint64_t key = hash ^ ((uint64_t)ea << 20) ^ ((uint64_t)size * 0x9E3779B97F4A7C15ull);
    if (traceSeen(key))
    {
        lineAppend(line, " data=seen");
        return hash;
    }
    lineAppend(line, " data=");
    lineAppendHex(line, data, size);
    return hash;
}

/* Emit a line, collapsing it into a REPEAT count when the same thread just
 * emitted an identical event (signature covers everything but the time). */
static void traceEmit(const char* prefix, trace_line* body, uint64_t signature)
{
    if (signature && traceRecentlySeen(signature))
    {
        t_repeatCount++;
        return;
    }
    traceLock();
    char head[96];
    if (t_repeatCount)
    {
        const int headLength = snprintf(head, sizeof(head), "%.6f %s REPEAT n=%u\n", traceSeconds(),
                                        t_lastPrefix, t_repeatCount);
        traceWriteLocked(head, (size_t)headLength);
        t_repeatCount = 0;
    }
    if (signature)
    {
        t_recentSignatures[t_recentNext] = signature;
        t_recentNext = (t_recentNext + 1u) % TRACE_RECENT;
    }
    else
    {
        memset(t_recentSignatures, 0, sizeof(t_recentSignatures));
        t_recentNext = 0;
    }
    snprintf(t_lastPrefix, sizeof(t_lastPrefix), "%s", prefix);
    const int headLength = snprintf(head, sizeof(head), "%.6f %s ", traceSeconds(), prefix);
    traceWriteLocked(head, (size_t)headLength);
    body->text[body->length++] = '\n';
    traceWriteLocked(body->text, body->length);
    traceUnlock();
}

/* ---- shape mode ------------------------------------------------------------ */

typedef struct
{
    uint64_t key;              /* 0 = empty slot */
    uint32_t id;
    uint64_t pending;          /* events since the last COUNT line */
    uint64_t pendingGets;
    uint64_t pendingPuts;
    uint64_t pendingBytesIn;
    uint64_t pendingBytesOut;
} trace_shape;

#define TRACE_SHAPE_BITS 15u
static trace_shape* s_shapes = NULL;
static uint32_t s_shapeCount = 0;
static uint32_t* s_pendingIds = NULL;      /* shapes with pending counts, in first-seen order */
static uint32_t s_pendingCount = 0;
static trace_shape** s_shapeById = NULL;
static double s_lastCountFlush = 0.0;

static trace_shape* traceShapeFindLocked(uint64_t key, int insert)
{
    if (!s_shapes)
    {
        return NULL;
    }
    if (key == 0)
    {
        key = 1;
    }
    const uint64_t mask = (1ull << TRACE_SHAPE_BITS) - 1u;
    uint64_t slot = (key * 0x9E3779B97F4A7C15ull) >> (64u - TRACE_SHAPE_BITS);
    for (uint32_t probe = 0; probe <= mask; probe++)
    {
        trace_shape* entry = &s_shapes[(slot + probe) & mask];
        if (entry->key == key)
        {
            return entry;
        }
        if (entry->key == 0)
        {
            if (!insert || s_shapeCount >= (mask >> 1))
            {
                return NULL;
            }
            entry->key = key;
            entry->id = ++s_shapeCount;
            s_shapeById[entry->id] = entry;
            return entry;
        }
    }
    return NULL;
}

/* Write the COUNT lines for everything counted since the last ones. */
static void traceFlushCountsLocked(int force)
{
    const double now = traceSeconds();
    if (!force && now - s_lastCountFlush < 1.0)
    {
        return;
    }
    s_lastCountFlush = now;
    for (uint32_t index = 0; index < s_pendingCount; index++)
    {
        trace_shape* shape = s_shapeById[s_pendingIds[index]];
        char text[192];
        int length;
        if (shape->pendingGets || shape->pendingPuts)
        {
            length = snprintf(text, sizeof(text), "%.6f - COUNT id=%u n=%llu gets=%llu puts=%llu in=%llX out=%llX\n",
                              now, shape->id, (unsigned long long)shape->pending,
                              (unsigned long long)shape->pendingGets, (unsigned long long)shape->pendingPuts,
                              (unsigned long long)shape->pendingBytesIn, (unsigned long long)shape->pendingBytesOut);
        }
        else
        {
            length = snprintf(text, sizeof(text), "%.6f - COUNT id=%u n=%llu\n", now, shape->id,
                              (unsigned long long)shape->pending);
        }
        traceWriteLocked(text, (size_t)length);
        shape->pending = 0;
        shape->pendingGets = 0;
        shape->pendingPuts = 0;
        shape->pendingBytesIn = 0;
        shape->pendingBytesOut = 0;
    }
    s_pendingCount = 0;
}

/* 1 if an event of this kind has been written before. */
static int traceShapeKnown(uint64_t key)
{
    traceLock();
    const int known = traceShapeFindLocked(key, 0) != NULL;
    traceUnlock();
    return known;
}

/* One event of kind `key`: written in full (as SHAPE id=<n> <body>) the first
 * time, counted after that. gets/puts/bytes add to the COUNT line. */
static void traceShapeEvent(const char* prefix, uint64_t key, trace_line* body, uint32_t gets, uint32_t puts,
                            uint64_t bytesIn, uint64_t bytesOut)
{
    traceLock();
    traceFlushCountsLocked(0);
    trace_shape* shape = traceShapeFindLocked(key, 0);
    if (!shape)
    {
        shape = traceShapeFindLocked(key, 1);
        if (!shape)
        {
            /* The table is full: say so once and drop the rest, rather than
             * writing every event in full from here on. */
            static int s_fullNoted = 0;
            if (!s_fullNoted)
            {
                s_fullNoted = 1;
                const char* note = "SHAPE_TABLE_FULL -- further new kinds of event are not recorded\n";
                traceWriteLocked(note, strlen(note));
            }
            traceUnlock();
            return;
        }
        char head[128];
        const int headLength = snprintf(head, sizeof(head), "%.6f %s SHAPE id=%u ", traceSeconds(), prefix,
                                        shape->id);
        traceWriteLocked(head, (size_t)headLength);
        body->text[body->length++] = '\n';
        traceWriteLocked(body->text, body->length);
        traceUnlock();
        return;
    }
    if (shape->pending == 0)
    {
        s_pendingIds[s_pendingCount++] = shape->id;
    }
    shape->pending++;
    shape->pendingGets += gets;
    shape->pendingPuts += puts;
    shape->pendingBytesIn += bytesIn;
    shape->pendingBytesOut += bytesOut;
    traceUnlock();
}

static void traceAtExit(void)
{
    if (!g_spurs_trace_on)
    {
        return;
    }
    traceLock();
    traceFlushCountsLocked(1);
    if (s_traceFile)
    {
        fflush(s_traceFile);
    }
    traceUnlock();
}

int spurs_trace_init(void)
{
    if (s_traceState >= 0)
    {
        return s_traceState;
    }
    const char* path = getenv("SPURS_GOLDEN_TRACE");
    if (!path || !path[0])
    {
        s_traceState = 0;
        return 0;
    }
#ifdef _WIN32
    InitializeCriticalSection(&s_traceLock);
    QueryPerformanceFrequency(&s_traceFrequency);
    QueryPerformanceCounter(&s_traceStart);
#else
    clock_gettime(CLOCK_MONOTONIC, &s_traceStart);
#endif
    s_traceFile = fopen(path, "wb");
    if (!s_traceFile)
    {
        fprintf(stderr, "[spurs-trace] cannot open %s -- trace off\n", path);
        s_traceState = 0;
        return 0;
    }
    setvbuf(s_traceFile, NULL, _IOFBF, 8u << 20);
    s_seenTable = (uint64_t*)calloc((size_t)1u << TRACE_SEEN_BITS, sizeof(uint64_t));
    const char* modeText = getenv("SPURS_GOLDEN_TRACE_MODE");
    s_shapeMode = !(modeText && strcmp(modeText, "full") == 0);
    if (s_shapeMode)
    {
        s_shapes = (trace_shape*)calloc((size_t)1u << TRACE_SHAPE_BITS, sizeof(trace_shape));
        s_shapeById = (trace_shape**)calloc((size_t)1u << TRACE_SHAPE_BITS, sizeof(trace_shape*));
        s_pendingIds = (uint32_t*)calloc((size_t)1u << TRACE_SHAPE_BITS, sizeof(uint32_t));
    }
    const char* limitText = getenv("SPURS_GOLDEN_TRACE_MB");
    unsigned long limitMegabytes = limitText && limitText[0] ? strtoul(limitText, NULL, 10) : 0ul;
    if (!limitMegabytes)
    {
        limitMegabytes = s_shapeMode ? 1024ul : 4096ul;
    }
    s_traceLimitBytes = (uint64_t)limitMegabytes << 20;
    fprintf(s_traceFile, "# SPURS golden trace v2, %s mode -- see runtime/spu/spurs_trace.c for the format\n",
            s_shapeMode ? "shape" : "full");
    fflush(s_traceFile);
    fprintf(stderr, "[spurs-trace] recording to %s (%s mode, limit %lu MB)\n", path,
            s_shapeMode ? "shape" : "full", limitMegabytes);
    atexit(traceAtExit);
    s_traceState = 1;
    g_spurs_trace_on = 1;
    return 1;
}

/* ---- PPU: calls into libsre ---------------------------------------------- */

static const char* spursNidName(uint32_t nid)
{
    switch (nid)
    {
    case 0xACFC8DBCu: return "cellSpursInitialize";
    case 0xCA4C4600u: return "cellSpursFinalize";
    case 0xB9BC6207u: return "cellSpursAttachLv2EventQueue";
    case 0x4E66D483u: return "cellSpursDetachLv2EventQueue";
    case 0x60EB2DECu: return "cellSpursCreateJobChain";
    case 0xBFEA60FAu: return "cellSpursKickJobChain";
    case 0x738E40E6u: return "cellSpursShutdownJobChain";
    case 0xA7C066DEu: return "cellSpursJoinJobChain";
    case 0x68AAEBA9u: return "cellSpursJobGuardInitialize";
    case 0xD5D0B256u: return "cellSpursJobGuardNotify";
    case 0xA9072DEEu: return "cellSyncMutexInitialize";
    case 0x1BB675C2u: return "cellSyncMutexLock";
    case 0x91F2B7B0u: return "cellSyncMutexUnlock";
    default:          return "?";
    }
}

/* Dump the memory behind each argument that looks like a guest pointer. */
static void traceArgumentMemory(const char* prefix, int phase, const uint64_t* gpr)
{
    for (int registerIndex = 3; registerIndex <= 10; registerIndex++)
    {
        const uint32_t address = (uint32_t)gpr[registerIndex];
        if (address < 0x10000u || address >= 0xD0000000u || !mfc_ea_range_committed(address, 512u))
        {
            continue;
        }
        trace_line line;
        line.length = 0;
        lineAppend(&line, "MEM %s r%d ea=%08X size=200", phase ? "after" : "before", registerIndex, address);
        lineAppendContent(&line, address, vm_base + address, 512u);
        traceEmit(prefix, &line, 0);
    }
}

#define TRACE_NID_GUARD_NOTIFY 0xD5D0B256u

/* Shape mode writes these calls in full every time: they set the chain up or
 * take it down, and happen a few dozen times a session. Every other call
 * (cellSpursJobGuardNotify ~190/s, the cellSyncMutex calls libsre also
 * serves ~260/s) is folded into counts. */
static int traceCallIsRare(uint32_t nid)
{
    switch (nid)
    {
    case 0xACFC8DBCu:   /* Initialize */
    case 0xCA4C4600u:   /* Finalize */
    case 0xB9BC6207u:   /* AttachLv2EventQueue */
    case 0x4E66D483u:   /* DetachLv2EventQueue */
    case 0x60EB2DECu:   /* CreateJobChain */
    case 0xBFEA60FAu:   /* KickJobChain */
    case 0x738E40E6u:   /* ShutdownJobChain */
    case 0xA7C066DEu:   /* JoinJobChain */
    case 0x68AAEBA9u:   /* JobGuardInitialize */
        return 1;
    default:
        return 0;
    }
}

static const char* spursNidName(uint32_t nid);

/* One folded call: a kind per (call, caller, object) -- and for the guard,
 * per guard contents too, which take only two values. The SHAPE line shows
 * the first 32 bytes of the object (r3). */
static void traceFoldedCall(const char* prefix, int isReturn, uint32_t nid, uint32_t lr, uint32_t object,
                            uint32_t result)
{
    trace_line line;
    line.length = 0;
    if (isReturn)
    {
        lineAppend(&line, "RET %s nid=%08X r3=%08X result=%08X", spursNidName(nid), nid, object, result);
    }
    else
    {
        lineAppend(&line, "CALL %s nid=%08X lr=%08X r3=%08X", spursNidName(nid), nid, lr, object);
    }
    uint64_t key = traceHash((const uint8_t*)line.text, (uint32_t)line.length);
    if (object >= 0x10000u && object < 0xD0000000u && mfc_ea_range_committed(object, 32u))
    {
        lineAppend(&line, " object=");
        lineAppendHex(&line, vm_base + object, 32u);
        if (nid == TRACE_NID_GUARD_NOTIFY)
        {
            key ^= traceHash(vm_base + object, 32u) * 0x9E3779B97F4A7C15ull;
        }
    }
    traceShapeEvent(prefix, key ^ 0x63616C6Cull, &line, 0, 0, 0, 0);
}

void spurs_trace_api_call(uint32_t nid, const uint64_t* gpr, uint32_t lr)
{
    if (!g_spurs_trace_on)
    {
        return;
    }
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "ppu%lu game", traceThreadId());
    if (s_shapeMode && !traceCallIsRare(nid))
    {
        traceFoldedCall(prefix, 0, nid, lr, (uint32_t)gpr[3], 0);
        return;
    }
    trace_line line;
    line.length = 0;
    lineAppend(&line, "CALL %s nid=%08X lr=%08X", spursNidName(nid), nid, lr);
    for (int registerIndex = 3; registerIndex <= 10; registerIndex++)
    {
        lineAppend(&line, " r%d=%08X", registerIndex, (uint32_t)gpr[registerIndex]);
    }
    traceEmit(prefix, &line, 0);
    traceArgumentMemory(prefix, 0, gpr);
}

void spurs_trace_api_return(uint32_t nid, const uint64_t* argumentGpr, uint32_t result)
{
    if (!g_spurs_trace_on)
    {
        return;
    }
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "ppu%lu game", traceThreadId());
    if (s_shapeMode && !traceCallIsRare(nid))
    {
        traceFoldedCall(prefix, 1, nid, 0, (uint32_t)argumentGpr[3], result);
        return;
    }
    trace_line line;
    line.length = 0;
    lineAppend(&line, "RET %s nid=%08X r3=%08X", spursNidName(nid), nid, result);
    traceEmit(prefix, &line, 0);
    traceArgumentMemory(prefix, 1, argumentGpr);
}

/* ---- SPU ------------------------------------------------------------------ */

static SPU_THREAD_LOCAL int t_jobImage = 0;

static void spuPrefix(spu_context* ctx, char* prefix, size_t capacity)
{
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK;
    if (t_jobImage)
    {
        snprintf(prefix, capacity, "spu%u job%d", (unsigned)(ctx->spu_id & 0xFFu), t_jobImage);
    }
    else if (pc < 0xA00u)
    {
        snprintf(prefix, capacity, "spu%u krn", (unsigned)(ctx->spu_id & 0xFFu));
    }
    else
    {
        snprintf(prefix, capacity, "spu%u pm", (unsigned)(ctx->spu_id & 0xFFu));
    }
}

/* ---- shape mode: one record per job run ------------------------------------ */

#define TRACE_MANAGER_OPS 48u
#define TRACE_JOB_DMA_KINDS 24u

typedef struct
{
    /* What the kernel and the job manager did since the previous job: the set
     * of distinct (actor, command, address, size), and how many in all. */
    uint64_t managerKey;
    uint32_t managerOps;
    uint32_t managerKindCount;
    uint64_t managerKinds[TRACE_MANAGER_OPS];
    trace_line managerText;

    /* The job running now. */
    int inJob;
    uint64_t entryKey;
    trace_line entryText;
    uint32_t gets;
    uint32_t puts;
    uint64_t bytesIn;
    uint64_t bytesOut;
    uint32_t dmaKindCount;
    uint32_t dmaKinds[TRACE_JOB_DMA_KINDS];     /* (isPut << 31) | size */
    uint32_t dmaKindRuns[TRACE_JOB_DMA_KINDS];
} trace_job_record;

static SPU_THREAD_LOCAL trace_job_record* t_record = NULL;

static trace_job_record* traceRecord(void)
{
    if (!t_record)
    {
        t_record = (trace_job_record*)calloc(1, sizeof(trace_job_record));
    }
    return t_record;
}

static void traceManagerOp(spu_context* ctx, const char* name, uint32_t ea, uint32_t size)
{
    trace_job_record* record = traceRecord();
    if (!record)
    {
        return;
    }
    const int isKernel = ((uint32_t)ctx->pc & SPU_LS_MASK) < 0xA00u;
    char text[64];
    const int length = snprintf(text, sizeof(text), " %s:%s@%08X/%X", isKernel ? "krn" : "pm", name, ea, size);
    const uint64_t kind = traceHash((const uint8_t*)text, (uint32_t)length) | 1u;
    record->managerOps++;
    for (uint32_t index = 0; index < record->managerKindCount; index++)
    {
        if (record->managerKinds[index] == kind)
        {
            return;
        }
    }
    if (record->managerKindCount < TRACE_MANAGER_OPS)
    {
        record->managerKinds[record->managerKindCount++] = kind;
        record->managerKey += kind * 0x9E3779B97F4A7C15ull;
        lineAppend(&record->managerText, "%s", text);
    }
}

static void traceJobDma(int isPut, uint32_t size)
{
    trace_job_record* record = traceRecord();
    if (!record)
    {
        return;
    }
    if (isPut)
    {
        record->puts++;
        record->bytesOut += size;
    }
    else
    {
        record->gets++;
        record->bytesIn += size;
    }
    const uint32_t kind = ((uint32_t)isPut << 31) | size;
    for (uint32_t index = 0; index < record->dmaKindCount; index++)
    {
        if (record->dmaKinds[index] == kind)
        {
            record->dmaKindRuns[index]++;
            return;
        }
    }
    if (record->dmaKindCount < TRACE_JOB_DMA_KINDS)
    {
        record->dmaKinds[record->dmaKindCount] = kind;
        record->dmaKindRuns[record->dmaKindCount] = 1;
        record->dmaKindCount++;
    }
}

/* A 1 KB-granular map of local store at job entry: '.' all zero, 'x' not. */
static void lineAppendZeroMap(trace_line* line, const spu_context* ctx)
{
    lineAppend(line, " lsmap=");
    for (uint32_t block = 0; block < SPU_LS_SIZE / 1024u && line->length + 2u < sizeof(line->text) - 2u; block++)
    {
        const uint64_t* words = (const uint64_t*)(ctx->ls + block * 1024u);
        int nonZero = 0;
        for (uint32_t index = 0; index < 128u; index++)
        {
            if (words[index])
            {
                nonZero = 1;
                break;
            }
        }
        line->text[line->length++] = nonZero ? 'x' : '.';
    }
    line->text[line->length] = 0;
}

static void traceShapeJobEnter(spu_context* ctx, int codeImage)
{
    trace_job_record* record = traceRecord();
    if (!record)
    {
        return;
    }
    const uint32_t contextAddress = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    const uint32_t jobAddress = ctx->gpr[4]._u32[0] & SPU_LS_MASK;
    const uint32_t stackAddress = ctx->gpr[1]._u32[0] & SPU_LS_MASK;
    trace_line* line = &record->entryText;
    line->length = 0;
    lineAppend(line, "JOB img=%d pc=%05X r0=%05X r1=%05X r3=%05X r4=%05X", codeImage,
               (unsigned)(ctx->pc & SPU_LS_MASK), ctx->gpr[0]._u32[0] & SPU_LS_MASK, stackAddress,
               contextAddress, jobAddress);
    /* The job context: only its non-zero words (offset=value). */
    lineAppend(line, " context=");
    if (contextAddress + 0x80u <= SPU_LS_SIZE)
    {
        for (uint32_t offset = 0; offset < 0x80u; offset += 4u)
        {
            uint32_t word;
            memcpy(&word, ctx->ls + contextAddress + offset, 4);
            if (word)
            {
                const uint32_t value = ((word & 0xFFu) << 24) | ((word & 0xFF00u) << 8) | ((word >> 8) & 0xFF00u) |
                                       (word >> 24);
                lineAppend(line, "%X:%X,", offset, value);
            }
        }
    }
    uint64_t descriptorHash = 0;
    if (jobAddress + 0x80u <= SPU_LS_SIZE)
    {
        lineAppend(line, " descriptor=");
        lineAppendHex(line, ctx->ls + jobAddress, 0x80u);
        descriptorHash = traceHash(ctx->ls + jobAddress, 0x80u);
    }
    if (stackAddress + 0x20u <= SPU_LS_SIZE)
    {
        lineAppend(line, " stack=");
        lineAppendHex(line, ctx->ls + stackAddress, 0x20u);
    }
    /* The key leaves out r5/r6 (left-overs) and the stack contents. */
    uint64_t key = descriptorHash * 31u + record->managerKey;
    key = key * 31u + (uint64_t)codeImage;
    key = key * 31u + ((uint64_t)ctx->pc & SPU_LS_MASK);
    key = key * 31u + stackAddress;
    key = key * 31u + contextAddress;
    key = key * 31u + jobAddress;
    if (contextAddress + 0x80u <= SPU_LS_SIZE)
    {
        key = key * 31u + traceHash(ctx->ls + contextAddress, 0x80u);
    }
    key ^= 0x6A6F62ull;
    lineAppend(line, " r5=%08X r6=%08X events=%X", ctx->gpr[5]._u32[0], ctx->gpr[6]._u32[0], ctx->event_mask);
    lineAppend(line, " manager(%u ops):%s", record->managerOps, record->managerText.length ? record->managerText.text : " -");
    if (!traceShapeKnown(key))
    {
        lineAppendZeroMap(line, ctx);
    }
    record->entryKey = key;
    record->inJob = 1;
    record->gets = 0;
    record->puts = 0;
    record->bytesIn = 0;
    record->bytesOut = 0;
    record->dmaKindCount = 0;
    record->managerKey = 0;
    record->managerOps = 0;
    record->managerKindCount = 0;
    record->managerText.length = 0;
    record->managerText.text[0] = 0;
}

static void traceShapeJobLeave(spu_context* ctx)
{
    trace_job_record* record = traceRecord();
    if (!record || !record->inJob)
    {
        return;
    }
    record->inJob = 0;
    trace_line* line = &record->entryText;
    lineAppend(line, " return=%05X r3=%08X dma:", (unsigned)(ctx->pc & SPU_LS_MASK), ctx->gpr[3]._u32[0]);
    for (uint32_t index = 0; index < record->dmaKindCount; index++)
    {
        lineAppend(line, " %s/%Xx%u", (record->dmaKinds[index] >> 31) ? "PUT" : "GET",
                   record->dmaKinds[index] & 0x7FFFFFFFu, record->dmaKindRuns[index]);
    }
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "spu%u job", (unsigned)(ctx->spu_id & 0xFFu));
    traceShapeEvent(prefix, record->entryKey, line, record->gets, record->puts, record->bytesIn, record->bytesOut);
}

/* ---- SPU hooks ---------------------------------------------------------------- */

void spurs_trace_dma(struct spu_context* ctx, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size)
{
    if (!g_spurs_trace_on || !ctx || !size)
    {
        return;
    }
    const int isGet = (cmd & 0xF0u) == 0x40u;
    const int isPut = (cmd & 0xF0u) == 0x20u;
    if (!isGet && !isPut)
    {
        return;
    }
    if (s_shapeMode)
    {
        if (t_jobImage)
        {
            traceJobDma(isPut, size);
        }
        else
        {
            traceManagerOp(ctx, isGet ? "GET" : "PUT", ea, size);
        }
        return;
    }
    char prefix[48];
    spuPrefix(ctx, prefix, sizeof(prefix));
    const uint8_t* data = ctx->ls + (lsa & SPU_LS_MASK);
    trace_line line;
    line.length = 0;
    lineAppend(&line, "%s ea=%08X lsa=%05X size=%X pc=%05X", isGet ? "GET" : "PUT", ea, lsa & SPU_LS_MASK, size,
               (unsigned)(ctx->pc & SPU_LS_MASK));
    uint64_t signature = traceHash((const uint8_t*)line.text, (uint32_t)line.length);
    if ((lsa & SPU_LS_MASK) + size <= SPU_LS_SIZE)
    {
        signature ^= lineAppendContent(&line, ea, data, size) * 0x9E3779B97F4A7C15ull;
    }
    signature |= 1u;
    traceEmit(prefix, &line, signature);
}

void spurs_trace_atomic(struct spu_context* ctx, uint32_t cmd, uint32_t ea, uint32_t lsa, uint32_t atomicStat)
{
    if (!g_spurs_trace_on || !ctx)
    {
        return;
    }
    const char* name = cmd == 0xD0u ? "GETLLAR" : cmd == 0xB4u ? "PUTLLC" : "PUTLLUC";
    if (s_shapeMode)
    {
        if (t_jobImage)
        {
            traceJobDma(cmd != 0xD0u, 128u);
        }
        else
        {
            traceManagerOp(ctx, name, ea, 128u);
        }
        return;
    }
    char prefix[48];
    spuPrefix(ctx, prefix, sizeof(prefix));
    trace_line line;
    line.length = 0;
    lineAppend(&line, "%s ea=%08X lsa=%05X stat=%X pc=%05X", name, ea, lsa & SPU_LS_MASK, atomicStat,
               (unsigned)(ctx->pc & SPU_LS_MASK));
    uint64_t signature = traceHash((const uint8_t*)line.text, (uint32_t)line.length);
    if ((lsa & SPU_LS_MASK) + 128u <= SPU_LS_SIZE)
    {
        signature ^= lineAppendContent(&line, ea, ctx->ls + (lsa & SPU_LS_MASK), 128u) * 0x9E3779B97F4A7C15ull;
    }
    signature |= 1u;
    traceEmit(prefix, &line, signature);
}

void spurs_trace_channel(struct spu_context* ctx, uint32_t channel, uint32_t value)
{
    if (!g_spurs_trace_on || !ctx)
    {
        return;
    }
    char prefix[48];
    spuPrefix(ctx, prefix, sizeof(prefix));
    trace_line line;
    line.length = 0;
    lineAppend(&line, "SEND ch=%u value=%08X pc=%05X", channel, value, (unsigned)(ctx->pc & SPU_LS_MASK));
    if (s_shapeMode)
    {
        uint64_t key = traceHash((const uint8_t*)line.text, (uint32_t)line.length);
        key = key * 31u + (uint64_t)t_jobImage;
        traceShapeEvent(prefix, key ^ 0x73656E64ull, &line, 0, 0, 0, 0);
        return;
    }
    traceEmit(prefix, &line, 0);
}

static void lineAppendLs(trace_line* line, spu_context* ctx, const char* label, uint32_t address, uint32_t size)
{
    address &= SPU_LS_MASK;
    if (address + size > SPU_LS_SIZE)
    {
        return;
    }
    lineAppend(line, " %s@%05X=", label, address);
    lineAppendHex(line, ctx->ls + address, size);
}

void spurs_trace_branch(struct spu_context* ctx, int codeImage)
{
    if (!g_spurs_trace_on || !ctx || codeImage == t_jobImage)
    {
        return;
    }
    if (s_shapeMode)
    {
        if (codeImage)
        {
            if (t_jobImage)
            {
                traceShapeJobLeave(ctx);   /* straight from one job's code into another's */
            }
            t_jobImage = codeImage;
            traceShapeJobEnter(ctx, codeImage);
        }
        else
        {
            traceShapeJobLeave(ctx);
            t_jobImage = 0;
        }
        return;
    }
    trace_line line;
    line.length = 0;
    char prefix[48];
    if (codeImage)
    {
        t_jobImage = codeImage;
        spuPrefix(ctx, prefix, sizeof(prefix));
        lineAppend(&line, "JOB_ENTER img=%d pc=%05X r0=%05X r1=%05X r3=%05X r4=%05X r5=%08X r6=%08X", codeImage,
                   (unsigned)(ctx->pc & SPU_LS_MASK), ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                   ctx->gpr[1]._u32[0] & SPU_LS_MASK, ctx->gpr[3]._u32[0] & SPU_LS_MASK,
                   ctx->gpr[4]._u32[0] & SPU_LS_MASK, ctx->gpr[5]._u32[0], ctx->gpr[6]._u32[0]);
        /* cellSpursJobMain2(CellSpursJobContext2* context, CellSpursJob256* job):
         * the context and the descriptor the job manager built in LS. */
        lineAppendLs(&line, ctx, "context", ctx->gpr[3]._u32[0], 0x80u);
        lineAppendLs(&line, ctx, "job", ctx->gpr[4]._u32[0], 0x100u);
    }
    else
    {
        spuPrefix(ctx, prefix, sizeof(prefix));
        lineAppend(&line, "JOB_LEAVE img=%d to=%05X r3=%08X", t_jobImage, (unsigned)(ctx->pc & SPU_LS_MASK),
                   ctx->gpr[3]._u32[0]);
        t_jobImage = 0;
    }
    traceEmit(prefix, &line, 0);
}

void spurs_trace_note(const char* text)
{
    if (!g_spurs_trace_on || !text)
    {
        return;
    }
    trace_line line;
    line.length = 0;
    lineAppend(&line, "NOTE %s", text);
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "thr%lu -", traceThreadId());
    traceEmit(prefix, &line, 0);
}
