/*
 * rsx_geometry_cache.c -- see rsx_geometry_cache.h.
 *
 * A set-associative table (4 ways) of converted draws. Only the present
 * thread uses it, so there is no locking.
 */
#include "rsx_geometry_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GEOM_SETS 4096u
#define GEOM_WAYS 4u
/* An entry not used for this many frames is freed (~10 s at 60 fps). */
#define GEOM_MAX_AGE_FRAMES 600u
/* A single draw larger than this is not worth keeping. */
#define GEOM_MAX_ENTRY_BYTES (32u << 20)

static const u64 kPrime1 = 0x9E3779B185EBCA87ull;
static const u64 kPrime2 = 0xC2B2AE3D27D4EB4Full;
static const u64 kPrime3 = 0x165667B19E3779F9ull;
static const u64 kPrime4 = 0x85EBCA77C2B2AE63ull;
static const u64 kPrime5 = 0x27D4EB2F165667C5ull;

static rsx_geom_entry s_table[GEOM_SETS][GEOM_WAYS];
static size_t s_bytes = 0;
static u32 s_entries = 0;
static size_t s_budget = 0;

static inline u64 rotateLeft(u64 value, unsigned amount)
{
    return (value << amount) | (value >> (64u - amount));
}

static inline u64 loadU64(const u8* bytes)
{
    u64 value;
    memcpy(&value, bytes, 8);
    return value;
}

static inline u64 hashRound(u64 accumulator, u64 input)
{
    accumulator += input * kPrime2;
    accumulator = rotateLeft(accumulator, 31);
    return accumulator * kPrime1;
}

static inline u64 hashMerge(u64 hash, u64 accumulator)
{
    hash ^= hashRound(0, accumulator);
    return hash * kPrime1 + kPrime4;
}

static inline u64 hashAvalanche(u64 hash)
{
    hash ^= hash >> 33;
    hash *= kPrime2;
    hash ^= hash >> 29;
    hash *= kPrime3;
    hash ^= hash >> 32;
    return hash;
}

u64 rsx_geom_hash(const void* data, size_t bytes, u64 seed)
{
    const u8* cursor = (const u8*)data;
    const size_t length = bytes;
    u64 hash;
    if (bytes >= 32)
    {
        u64 laneA = seed + kPrime1 + kPrime2;
        u64 laneB = seed + kPrime2;
        u64 laneC = seed;
        u64 laneD = seed - kPrime1;
        while (bytes >= 32)
        {
            laneA = hashRound(laneA, loadU64(cursor));
            laneB = hashRound(laneB, loadU64(cursor + 8));
            laneC = hashRound(laneC, loadU64(cursor + 16));
            laneD = hashRound(laneD, loadU64(cursor + 24));
            cursor += 32;
            bytes -= 32;
        }
        hash = rotateLeft(laneA, 1) + rotateLeft(laneB, 7) +
               rotateLeft(laneC, 12) + rotateLeft(laneD, 18);
        hash = hashMerge(hash, laneA);
        hash = hashMerge(hash, laneB);
        hash = hashMerge(hash, laneC);
        hash = hashMerge(hash, laneD);
    }
    else
    {
        hash = seed + kPrime5;
    }
    hash += (u64)length;
    while (bytes >= 8)
    {
        hash ^= hashRound(0, loadU64(cursor));
        hash = rotateLeft(hash, 27) * kPrime1 + kPrime4;
        cursor += 8;
        bytes -= 8;
    }
    while (bytes > 0)
    {
        hash ^= (u64)(*cursor) * kPrime5;
        hash = rotateLeft(hash, 11) * kPrime1;
        cursor++;
        bytes--;
    }
    return hashAvalanche(hash);
}

u64 rsx_geom_hash_u64(u64 hash, u64 value)
{
    return hashAvalanche(hash ^ hashRound(kPrime3, value)) + kPrime4;
}

static size_t geom_budget(void)
{
    if (!s_budget)
    {
        const char* text = getenv("LD_GEOM_CACHE_MB");
        const long megabytes = text ? strtol(text, NULL, 0) : 384;
        s_budget = (size_t)(megabytes > 16 ? megabytes : 16) << 20;
    }
    return s_budget;
}

static void geom_free(rsx_geom_entry* entry)
{
    if (!entry->bytes)
    {
        return;
    }
    free(entry->refs);
    free(entry->occurrence);
    free(entry->cuts);
    free(entry->vertices);
    s_bytes -= entry->bytes;
    s_entries--;
    memset(entry, 0, sizeof(*entry));
}

rsx_geom_entry* rsx_geom_cache_find(u64 key, u64 check, u32 frame)
{
    rsx_geom_entry* set = s_table[key & (GEOM_SETS - 1u)];
    for (u32 way = 0; way < GEOM_WAYS; way++)
    {
        rsx_geom_entry* entry = &set[way];
        if (entry->bytes && entry->key == key && entry->check == check)
        {
            entry->last_frame = frame;
            return entry;
        }
    }
    return NULL;
}

void rsx_geom_cache_drop(rsx_geom_entry* entry)
{
    if (entry)
    {
        geom_free(entry);
    }
}

static void* geom_copy(const void* source, size_t bytes)
{
    if (!bytes)
    {
        return NULL;
    }
    void* copy = malloc(bytes);
    if (copy)
    {
        memcpy(copy, source, bytes);
    }
    return copy;
}

int rsx_geom_cache_store(u64 key, u64 check, u32 frame,
                         const rsx_geom_span* spans, u32 n_spans,
                         const rsx_vertex_ref* refs, u32 n_refs,
                         u32 n_source_refs, int refs_remapped,
                         const u32* occurrence,
                         const u32* cuts, u32 n_cuts,
                         const u8* vertices, u32 stride)
{
    if (n_spans > RSX_GEOM_MAX_SPANS || !n_refs || !stride)
    {
        return 0;
    }
    const size_t refBytes = (size_t)n_refs * sizeof(rsx_vertex_ref);
    const size_t occurrenceBytes = refs_remapped ? (size_t)n_source_refs * sizeof(u32) : 0;
    const size_t cutBytes = (size_t)n_cuts * sizeof(u32);
    const size_t vertexBytes = (size_t)n_refs * stride;
    const size_t total = refBytes + occurrenceBytes + cutBytes + vertexBytes;
    if (total > GEOM_MAX_ENTRY_BYTES || (refs_remapped && !occurrence))
    {
        return 0;
    }

    /* The way: this key's own entry, else an empty one, else the least
     * recently used. */
    rsx_geom_entry* set = s_table[key & (GEOM_SETS - 1u)];
    rsx_geom_entry* target = NULL;
    for (u32 way = 0; way < GEOM_WAYS && !target; way++)
    {
        if (set[way].bytes && set[way].key == key)
        {
            target = &set[way];
        }
    }
    for (u32 way = 0; way < GEOM_WAYS && !target; way++)
    {
        if (!set[way].bytes)
        {
            target = &set[way];
        }
    }
    if (!target)
    {
        target = &set[0];
        for (u32 way = 1; way < GEOM_WAYS; way++)
        {
            if ((s32)(set[way].last_frame - target->last_frame) < 0)
            {
                target = &set[way];
            }
        }
    }
    geom_free(target);

    rsx_geom_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.refs = (rsx_vertex_ref*)geom_copy(refs, refBytes);
    entry.occurrence = occurrenceBytes ? (u32*)geom_copy(occurrence, occurrenceBytes) : NULL;
    entry.cuts = cutBytes ? (u32*)geom_copy(cuts, cutBytes) : NULL;
    entry.vertices = (u8*)geom_copy(vertices, vertexBytes);
    if (!entry.refs || !entry.vertices ||
        (occurrenceBytes && !entry.occurrence) || (cutBytes && !entry.cuts))
    {
        free(entry.refs);
        free(entry.occurrence);
        free(entry.cuts);
        free(entry.vertices);
        return 0;
    }
    entry.key = key;
    entry.check = check;
    entry.last_frame = frame;
    entry.n_spans = n_spans;
    memcpy(entry.spans, spans, (size_t)n_spans * sizeof(*spans));
    entry.n_refs = n_refs;
    entry.n_source_refs = n_source_refs;
    entry.n_cuts = n_cuts;
    entry.stride = stride;
    entry.refs_remapped = refs_remapped;
    entry.bytes = total;
    *target = entry;
    s_bytes += total;
    s_entries++;
    return 1;
}

void rsx_geom_cache_end_frame(u32 frame)
{
    const size_t budget = geom_budget();
    /* Sweep every 64 frames, or at once when over budget. */
    if ((frame & 63u) != 0 && s_bytes <= budget)
    {
        return;
    }
    u32 maxAge = GEOM_MAX_AGE_FRAMES;
    for (;;)
    {
        for (u32 setIndex = 0; setIndex < GEOM_SETS; setIndex++)
        {
            for (u32 way = 0; way < GEOM_WAYS; way++)
            {
                rsx_geom_entry* entry = &s_table[setIndex][way];
                if (entry->bytes && frame - entry->last_frame > maxAge)
                {
                    geom_free(entry);
                }
            }
        }
        if (s_bytes <= budget || maxAge == 0)
        {
            break;
        }
        maxAge /= 2u;
    }
}

size_t rsx_geom_cache_bytes(void)
{
    return s_bytes;
}

u32 rsx_geom_cache_entries(void)
{
    return s_entries;
}

void rsx_geom_cache_clear(void)
{
    for (u32 setIndex = 0; setIndex < GEOM_SETS; setIndex++)
    {
        for (u32 way = 0; way < GEOM_WAYS; way++)
        {
            geom_free(&s_table[setIndex][way]);
        }
    }
}
