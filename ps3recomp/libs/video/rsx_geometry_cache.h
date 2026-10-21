/*
 * rsx_geometry_cache.h -- converted vertex payloads kept across frames.
 *
 * The live renderer turns each draw's guest vertex arrays into its own
 * compact float layout on the CPU: read the indices, de-duplicate them, then
 * decode every referenced vertex. Most of a frame's geometry is the same from
 * one frame to the next (Folklore skins its characters on the GPU, so even
 * those meshes do not change), and at the pub that conversion was the
 * largest part of the 22 ms the present thread spent on each frame.
 *
 * An entry is found by a key over everything that decides the draw's
 * output except the vertex bytes themselves -- the layout, every attribute's
 * format and address, the index buffer's address, format and contents, the
 * draw ranges -- and is only used if the vertex bytes it was built from
 * still hash the same. A changed buffer is a miss, never stale geometry.
 */
#pragma once

#include "rsx_vertex_compact.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A 64-bit hash, ~8 bytes per cycle on x86-64. Not cryptographic. */
u64 rsx_geom_hash(const void* data, size_t bytes, u64 seed);
/* Fold one 64-bit value into a running hash. */
u64 rsx_geom_hash_u64(u64 hash, u64 value);

#define RSX_GEOM_MAX_SPANS 16u

/* A guest byte range the entry's vertices were decoded from. */
typedef struct rsx_geom_span {
    u32 location;
    u32 offset;
    u32 bytes;
    u64 hash;
} rsx_geom_span;

typedef struct rsx_geom_entry {
    u64 key;
    u64 check;                /* second hash of the same inputs */
    u32 last_frame;
    u32 n_spans;
    rsx_geom_span spans[RSX_GEOM_MAX_SPANS];
    u32 n_refs;               /* unique vertices */
    u32 n_source_refs;        /* vertex references before de-duplication */
    u32 n_cuts;               /* primitive-restart cuts */
    u32 stride;               /* bytes per converted vertex */
    int refs_remapped;
    rsx_vertex_ref* refs;     /* n_refs */
    u32* occurrence;          /* n_source_refs, when refs_remapped */
    u32* cuts;                /* n_cuts */
    u8* vertices;             /* n_refs * stride */
    size_t bytes;             /* everything above that was allocated */
} rsx_geom_entry;

/* The entry for (key, check), or NULL. Marks it used in `frame`. */
rsx_geom_entry* rsx_geom_cache_find(u64 key, u64 check, u32 frame);

/* Store a converted draw, replacing any entry with the same key. Copies
 * every array. Returns 0 when it does not fit (too large, or allocation
 * failed); the draw is simply not cached. */
int rsx_geom_cache_store(u64 key, u64 check, u32 frame,
                         const rsx_geom_span* spans, u32 n_spans,
                         const rsx_vertex_ref* refs, u32 n_refs,
                         u32 n_source_refs, int refs_remapped,
                         const u32* occurrence,
                         const u32* cuts, u32 n_cuts,
                         const u8* vertices, u32 stride);

/* Drop an entry found stale. */
void rsx_geom_cache_drop(rsx_geom_entry* entry);

/* Once a frame: free entries not used for a while, and enough of the oldest
 * ones to stay inside the memory budget. */
void rsx_geom_cache_end_frame(u32 frame);

/* Bytes held, and entries held. */
size_t rsx_geom_cache_bytes(void);
u32 rsx_geom_cache_entries(void);

void rsx_geom_cache_clear(void);

#ifdef __cplusplus
}
#endif
