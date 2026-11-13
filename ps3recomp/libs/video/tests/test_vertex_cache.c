/*
 * ps3recomp - the draw engine's cross-frame vertex cache
 *
 * Driven through rsx_draw_engine_method() against a stub backend that keeps
 * a copy of every persistent vertex buffer it is asked for, so the test can
 * see which draws reuse one, which fetch again, and what the reused bytes
 * are:
 *   - a draw whose guest bytes are the same in two frames gets a buffer, and
 *     the third frame draws from it with nothing staged;
 *   - rewritten guest bytes are fetched again (never drawn stale) and the old
 *     buffer is released;
 *   - an indexed draw is cached over its whole index range, its indices
 *     rebuilt every draw;
 *   - a key whose bytes keep changing stops being hashed for a while;
 *   - PS3RECOMP_ENG_VCACHE=0, or a backend without the hooks, is the old path;
 *   - shutdown releases every buffer;
 *   - rsx_vertex_remap_build_range keeps its contract;
 *   - a draw whose attributes come from separate spans is split into vertex
 *     streams cached on their own (rsx_draw_backend.draw_vbufs): a stream
 *     the guest moves or rewrites every frame is staged alone while the
 *     static one is drawn from its buffer, and the vertices the backend sees
 *     are the ones the whole-draw path would have drawn.
 *
 * clang -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video
 *   -o /tmp/test_vertex_cache libs/video/tests/test_vertex_cache.c
 *   libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c
 *   libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c
 *   libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c
 */
#include "rsx_draw_engine.h"
#include "rsx_primitives.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* ---- guest memory: main at the bottom, local above ----------------------- */

#define GUEST_BYTES     (16u << 20)
#define GUEST_LOCAL_EA  (8u << 20)

u8* vm_base;
u32 ppu_vm_size;

u32 cellGcmResolveLocated(int local, u32 offset)
{
    return local ? GUEST_LOCAL_EA + offset : offset;
}

u32 cellGcmResolveIO(u32 offset)
{
    return offset < GUEST_LOCAL_EA ? offset : 0;
}

void cellGcm_replay_display_buffers_to_engine(void) {}

/* ---- the stub backend ---------------------------------------------------- */

#define STUB_MAX_OBJECTS 4096
#define STUB_MAX_VBUFS   64

static struct {
    u32 next_handle;
    int n_draw, n_draw_vbuf;
    int n_vbuf_create, n_vbuf_release;
    /* the last draw of either kind */
    u32 last_vbuf;              /* 0: it came through draw() */
    u32 last_vertices, last_stride, last_indices;
    u32 indices[24];
    float first_vertex[4];      /* ATTR0 of vertex 0, wherever it came from */
    u8 verts[4 * 256];          /* draw(): the first four staged vertices */
    /* pipelines: the layout each was created with, and the one bound */
    u64 pipe_map[STUB_MAX_OBJECTS];
    u8  pipe_streams[STUB_MAX_OBJECTS];
    u8  pipe_count[STUB_MAX_OBJECTS];
    u32 bound_pipeline;
    /* draw_vbufs(): the last split draw */
    int n_draw_vbufs, bad_split;
    u32 split_streams, split_staged;
    u32 split_vbuf[RSX_BE_MAX_VSTREAMS];
    float split_verts[4][16][4];   /* first four vertices, every slot */
    /* every buffer made, by handle */
    u8* vbuf_data[STUB_MAX_VBUFS];
    u32 vbuf_bytes[STUB_MAX_VBUFS];
    u8  vbuf_alive[STUB_MAX_VBUFS];
} stub;

static u32 stub_handle(void)
{
    return stub.next_handle + 1 < STUB_MAX_OBJECTS ? ++stub.next_handle : 0;
}

static int stub_init(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return 0; }
static void stub_shutdown(void* u) { (void)u; }
static void stub_submit(void* u, u32 r) { (void)u; (void)r; }
static u32 stub_texture_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips,
                               u32 faces, u32 remap, u32 rsx_fmt)
{ (void)u; (void)f; (void)w; (void)h; (void)mips; (void)faces; (void)remap; (void)rsx_fmt; return stub_handle(); }
static void stub_texture_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h,
                                const void* src, u32 row, u32 rows)
{ (void)u; (void)t; (void)face; (void)mip; (void)w; (void)h; (void)src; (void)row; (void)rows; }
static void stub_release(void* u, u32 h) { (void)u; (void)h; }
static u32 stub_color_create(void* u, rsx_be_format f, u32 w, u32 h,
                             const void* seed, u32 seed_row)
{ (void)u; (void)f; (void)w; (void)h; (void)seed; (void)seed_row; return stub_handle(); }
static u32 stub_surface_view(void* u, u32 s, u32 remap, u32 fmt)
{ (void)u; (void)remap; (void)fmt; return s; }
static u32 stub_depth_create(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return stub_handle(); }
static u32 stub_depth_snapshot(void* u, u32 d, u32 w, u32 h)
{ (void)u; (void)d; (void)w; (void)h; return stub_handle(); }
static u32 stub_pipeline_create(void* u, const char* vs, const char* ps,
                                const rsx_be_render_state* rs,
                                const rsx_vertex_layout_plan* layout,
                                u32 stride, rsx_be_format rt, u32 rt_count)
{
    (void)u; (void)rs; (void)stride; (void)rt; (void)rt_count;
    if (!(vs && ps && *vs && *ps)) return 0;
    const u32 h = stub_handle();
    if (h && layout) {
        stub.pipe_map[h] = layout->stream_map;
        stub.pipe_streams[h] = layout->n_streams;
        stub.pipe_count[h] = layout->count;
    }
    return h;
}
static void stub_pipeline_release(void* u, u32 p) { (void)u; (void)p; }
static void stub_bind_targets(void* u, const u32* s, u32 n, u32 d) { (void)u; (void)s; (void)n; (void)d; }
static void stub_bind_pipeline(void* u, u32 p) { (void)u; stub.bound_pipeline = p; }
static void stub_bind_cb(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void stub_bind_textures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 m)
{ (void)u; (void)t; (void)s; (void)m; }
static void stub_viewport(void* u, float x, float y, float w, float h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void stub_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void stub_stencil_ref(void* u, u32 r) { (void)u; (void)r; }
static void stub_clear_color(void* u, u32 s, const float rgba[4]) { (void)u; (void)s; (void)rgba; }
static void stub_clear_ds(void* u, u32 d, u32 f, float z, u8 s)
{ (void)u; (void)d; (void)f; (void)z; (void)s; }
static void stub_present(void* u, u32 s) { (void)u; (void)s; }
static void stub_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{ (void)u; (void)s; (void)x; (void)y; (void)w; (void)h; (void)out; (void)pitch; }

static void note_draw(u32 vbuf, const void* verts, u32 count, u32 stride,
                      const u32* indices, u32 index_count)
{
    stub.last_vbuf = vbuf;
    stub.last_vertices = count;
    stub.last_stride = stride;
    stub.last_indices = indices ? index_count : 0;
    memset(stub.indices, 0, sizeof stub.indices);
    if (indices)
        memcpy(stub.indices, indices, (index_count < 24 ? index_count : 24) * sizeof(u32));
    memset(stub.first_vertex, 0, sizeof stub.first_vertex);
    if (verts && stride >= sizeof stub.first_vertex)
        memcpy(stub.first_vertex, verts, sizeof stub.first_vertex);
    memset(stub.verts, 0, sizeof stub.verts);
    if (verts && stride <= 256)
        memcpy(stub.verts, verts, (u64)(count < 4 ? count : 4) * stride);
}

static void stub_draw(void* u, rsx_topology t, const void* verts, u32 count,
                      u32 stride, const u32* indices, u32 index_count)
{
    (void)u; (void)t;
    stub.n_draw++;
    note_draw(0, verts, count, stride, indices, index_count);
}

static u32 stub_vbuf_create(void* u, const void* data, u32 bytes)
{
    (void)u;
    for (u32 h = 1; h < STUB_MAX_VBUFS; h++) {
        if (stub.vbuf_alive[h]) continue;
        free(stub.vbuf_data[h]);
        stub.vbuf_data[h] = (u8*)malloc(bytes);
        if (!stub.vbuf_data[h]) return 0;
        memcpy(stub.vbuf_data[h], data, bytes);
        stub.vbuf_bytes[h] = bytes;
        stub.vbuf_alive[h] = 1;
        stub.n_vbuf_create++;
        return h;
    }
    return 0;
}

static void stub_vbuf_release(void* u, u32 h)
{
    (void)u;
    if (h && h < STUB_MAX_VBUFS && stub.vbuf_alive[h]) {
        stub.vbuf_alive[h] = 0;
        stub.n_vbuf_release++;
    }
}

static int g_bad_vbuf_draws;
static void stub_draw_vbuf(void* u, rsx_topology t, u32 vbuf, u32 count,
                           u32 stride, const u32* indices, u32 index_count)
{
    (void)u; (void)t;
    stub.n_draw_vbuf++;
    if (!vbuf || vbuf >= STUB_MAX_VBUFS || !stub.vbuf_alive[vbuf] ||
        (u64)count * stride > stub.vbuf_bytes[vbuf]) {
        g_bad_vbuf_draws++;
        note_draw(vbuf, NULL, count, stride, indices, index_count);
        return;
    }
    note_draw(vbuf, stub.vbuf_data[vbuf], count, stride, indices, index_count);
}

/* A split draw: every stream checked against the bound pipeline's stream map,
 * and the first vertices put back together in slot order. */
static void stub_draw_vbufs(void* u, rsx_topology t, const rsx_be_vstream* streams,
                            u32 n_streams, u32 count, const u32* indices,
                            u32 index_count)
{
    (void)u; (void)t;
    stub.n_draw_vbufs++;
    const u32 p = stub.bound_pipeline;
    const u64 map = p < STUB_MAX_OBJECTS ? stub.pipe_map[p] : 0;
    const u32 nslots = p < STUB_MAX_OBJECTS ? stub.pipe_count[p] : 0;
    if (p >= STUB_MAX_OBJECTS || stub.pipe_streams[p] != n_streams || n_streams < 2 ||
        n_streams > RSX_BE_MAX_VSTREAMS) {
        stub.bad_split++;
        return;
    }
    const u8* data[RSX_BE_MAX_VSTREAMS];
    u32 rank_count[RSX_BE_MAX_VSTREAMS] = { 0 };
    for (u32 s = 0; s < nslots; s++) rank_count[(map >> (4u * s)) & 0xFu]++;
    stub.split_streams = n_streams;
    stub.split_staged = 0;
    for (u32 k = 0; k < n_streams; k++) {
        stub.split_vbuf[k] = streams[k].vbuf;
        if (streams[k].stride != rank_count[k] * 16u) { stub.bad_split++; return; }
        if (streams[k].vbuf) {
            const u32 h = streams[k].vbuf;
            if (h >= STUB_MAX_VBUFS || !stub.vbuf_alive[h] ||
                (u64)count * streams[k].stride > stub.vbuf_bytes[h]) { stub.bad_split++; return; }
            data[k] = stub.vbuf_data[h];
        } else {
            if (!streams[k].data) { stub.bad_split++; return; }
            data[k] = (const u8*)streams[k].data;
            stub.split_staged++;
        }
    }
    memset(stub.split_verts, 0, sizeof stub.split_verts);
    for (u32 v = 0; v < count && v < 4; v++) {
        u32 rank[RSX_BE_MAX_VSTREAMS] = { 0 };
        for (u32 s = 0; s < nslots && s < 16; s++) {
            const u32 k = (u32)(map >> (4u * s)) & 0xFu;
            memcpy(stub.split_verts[v][s], data[k] + (u64)v * streams[k].stride + rank[k] * 16u, 16);
            rank[k]++;
        }
    }
    note_draw(0, NULL, count, streams[0].stride, indices, index_count);
}

static rsx_draw_backend g_backend = {
    .init = stub_init,
    .shutdown = stub_shutdown,
    .submit_and_wait = stub_submit,
    .texture_create = stub_texture_create,
    .texture_upload = stub_texture_upload,
    .texture_release = stub_release,
    .color_target_create = stub_color_create,
    .color_target_release = stub_release,
    .surface_view = stub_surface_view,
    .depth_target_create = stub_depth_create,
    .depth_target_release = stub_release,
    .depth_snapshot = stub_depth_snapshot,
    .pipeline_create = stub_pipeline_create,
    .pipeline_release = stub_pipeline_release,
    .bind_targets = stub_bind_targets,
    .bind_pipeline = stub_bind_pipeline,
    .bind_vs_constants = stub_bind_cb,
    .bind_ps_constants = stub_bind_cb,
    .bind_textures = stub_bind_textures,
    .bind_vertex_textures = stub_bind_textures,
    .set_viewport = stub_viewport,
    .set_scissor = stub_scissor,
    .set_stencil_ref = stub_stencil_ref,
    .draw = stub_draw,
    .clear_color = stub_clear_color,
    .clear_depth_stencil = stub_clear_ds,
    .present = stub_present,
    .readback = stub_readback,
    .vbuf_create = stub_vbuf_create,
    .vbuf_release = stub_vbuf_release,
    .draw_vbuf = stub_draw_vbuf,
};

/* ---- driving the engine -------------------------------------------------- */

#define M_CONTEXT_DMA_COLOR_A 0x0194
#define M_SURFACE_CLIP_H      0x0200
#define M_SURFACE_CLIP_V      0x0204
#define M_COLOR_A_OFFSET      0x0210
#define M_VTXBUF_OFFSET       0x1680
#define M_VTXFMT              0x1740
#define M_BEGIN_END           0x1808
#define M_DRAW_ARRAYS         0x1814
#define M_IDXBUF_OFFSET       0x181C
#define M_IDXBUF_FORMAT       0x1820
#define M_DRAW_INDEX          0x1824
#define DMA_LOCAL             0xFEED0000u

#define VTX_OFFSET  0x00010000u        /* main memory */
#define IDX_OFFSET  0x00030000u

static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }

static void put_be32(u32 ea, u32 v)
{
    vm_base[ea] = (u8)(v >> 24); vm_base[ea + 1] = (u8)(v >> 16);
    vm_base[ea + 2] = (u8)(v >> 8); vm_base[ea + 3] = (u8)v;
}

static void put_be_float(u32 ea, float f)
{
    u32 v; memcpy(&v, &f, sizeof v); put_be32(ea, v);
}

/* Vertex i: (x0 + i, i, 0, 1), float32 x4, stride 16, at VTX_OFFSET. */
static void write_vertices(u32 n, float x0)
{
    for (u32 i = 0; i < n; i++) {
        const u32 ea = VTX_OFFSET + i * 16u;
        put_be_float(ea, x0 + (float)i);
        put_be_float(ea + 4, (float)i);
        put_be_float(ea + 8, 0.0f);
        put_be_float(ea + 12, 1.0f);
    }
}

static void engine_up(const rsx_draw_backend* be)
{
    for (u32 h = 1; h < STUB_MAX_VBUFS; h++) { free(stub.vbuf_data[h]); }
    memset(&stub, 0, sizeof stub);
    g_bad_vbuf_draws = 0;
    rsx_draw_engine_set_backend(be);
    if (rsx_draw_engine_init(1280, 720) != 0) {
        printf("[FAIL] engine init\n");
        exit(1);
    }
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_COLOR_A_OFFSET, 0x40000u);
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));       /* float32 x4, stride 16 */
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);   /* bit 31: main */
}

static void engine_down(void)
{
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
}

static void draw_arrays(u32 first, u32 count)
{
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, first | ((count - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

/* u16 indices, main memory. */
static void draw_indexed(const u16* idx, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        vm_base[IDX_OFFSET + i * 2u] = (u8)(idx[i] >> 8);
        vm_base[IDX_OFFSET + i * 2u + 1u] = (u8)idx[i];
    }
    m(M_IDXBUF_OFFSET, IDX_OFFSET);
    m(M_IDXBUF_FORMAT, 1u | (1u << 4));   /* main, u16 */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_INDEX, 0u | ((count - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

static rsx_draw_engine_vcache_stats stats(void)
{
    rsx_draw_engine_vcache_stats s;
    rsx_draw_engine_get_vcache_stats(&s);
    return s;
}

/* ---- 1. hit, then invalidation ------------------------------------------- */

static void test_hit_and_invalidate(void)
{
    printf("-- hit and invalidation (DRAW_ARRAYS)\n");
    unsetenv("PS3RECOMP_ENG_VCACHE");
    engine_up(&g_backend);
    CHECK(stats().enabled, "the cache is on by default with a backend that has the hooks");
    write_vertices(3, 10.0f);

    draw_arrays(0, 3);
    CHECK(stub.n_draw == 1 && stub.n_vbuf_create == 0,
          "frame 0: fetched and staged, no buffer yet (%d draws, %d buffers)",
          stub.n_draw, stub.n_vbuf_create);
    rsx_draw_engine_present();

    draw_arrays(0, 3);
    CHECK(stub.n_draw == 2 && stub.n_vbuf_create == 1,
          "frame 1: the same bytes again make a persistent buffer (%d buffers)",
          stub.n_vbuf_create);
    rsx_draw_engine_present();

    draw_arrays(0, 3);
    CHECK(stub.n_draw == 2 && stub.n_draw_vbuf == 1 && stub.last_vbuf != 0 &&
          g_bad_vbuf_draws == 0,
          "frame 2: drawn from the buffer, nothing staged (%d draws, %d from a buffer)",
          stub.n_draw, stub.n_draw_vbuf);
    CHECK(stub.last_vertices == 3 && stub.last_indices == 0,
          "a DRAW_ARRAYS run stays non-indexed from the buffer (%u vertices, %u indices)",
          stub.last_vertices, stub.last_indices);
    CHECK(stub.first_vertex[0] == 10.0f && stub.first_vertex[3] == 1.0f,
          "the buffer holds the decoded vertices (x=%g w=%g)",
          (double)stub.first_vertex[0], (double)stub.first_vertex[3]);
    rsx_draw_engine_vcache_stats s = stats();
    CHECK(s.hits == 1 && s.created == 1 && s.misses == 2,
          "counters: %llu hits, %llu created, %llu misses",
          (unsigned long long)s.hits, (unsigned long long)s.created,
          (unsigned long long)s.misses);

    /* A second draw of the same geometry in the same frame hits too. */
    draw_arrays(0, 3);
    CHECK(stub.n_draw_vbuf == 2, "a repeat within the frame hits as well");
    rsx_draw_engine_present();

    /* The guest rewrites one float: fetched again, the old buffer released. */
    put_be_float(VTX_OFFSET, 99.0f);
    draw_arrays(0, 3);
    CHECK(stub.n_draw == 3 && stub.last_vbuf == 0 && stub.first_vertex[0] == 99.0f,
          "rewritten bytes are fetched again, not drawn stale (x=%g)",
          (double)stub.first_vertex[0]);
    CHECK(stub.n_vbuf_release == 1, "and the stale buffer is released (%d)",
          stub.n_vbuf_release);
    rsx_draw_engine_present();

    draw_arrays(0, 3);
    CHECK(stub.n_vbuf_create == 2 && stub.last_vbuf == 0,
          "stable again for a frame: a new buffer (%d)", stub.n_vbuf_create);
    rsx_draw_engine_present();
    draw_arrays(0, 3);
    CHECK(stub.last_vbuf != 0 && stub.first_vertex[0] == 99.0f,
          "and then drawn from it, with the new bytes (x=%g)",
          (double)stub.first_vertex[0]);

    /* A different range of the same array is a different key. */
    write_vertices(6, 10.0f);
    rsx_draw_engine_present();
    draw_arrays(3, 3);
    CHECK(stub.last_vbuf == 0 && stub.first_vertex[0] == 13.0f,
          "another range is another key (x=%g)", (double)stub.first_vertex[0]);

    const int created = stub.n_vbuf_create;
    engine_down();
    CHECK(stub.n_vbuf_release == created,
          "shutdown releases every buffer (%d of %d)", stub.n_vbuf_release, created);
}

/* ---- 2. an indexed draw over its whole index range ----------------------- */

static void test_indexed(void)
{
    printf("-- indexed draws\n");
    engine_up(&g_backend);
    write_vertices(8, 0.0f);
    /* Vertices 2..5, used out of order and shared. */
    static const u16 idx[6] = { 4, 3, 2, 3, 4, 5 };
    for (int f = 0; f < 3; f++) {
        draw_indexed(idx, 6);
        if (f < 2) rsx_draw_engine_present();
    }
    CHECK(stub.n_vbuf_create == 1 && stub.last_vbuf != 0 && g_bad_vbuf_draws == 0,
          "the third frame draws from a buffer (%d buffers, last vbuf %u)",
          stub.n_vbuf_create, stub.last_vbuf);
    CHECK(stub.last_vertices == 4 && stub.last_indices == 6,
          "the buffer is the index range 2..5 (%u vertices), %u indices",
          stub.last_vertices, stub.last_indices);
    CHECK(stub.indices[0] == 2 && stub.indices[1] == 1 && stub.indices[2] == 0 &&
          stub.indices[3] == 1 && stub.indices[4] == 2 && stub.indices[5] == 3,
          "indices are relative to the range (%u %u %u %u %u %u)",
          stub.indices[0], stub.indices[1], stub.indices[2],
          stub.indices[3], stub.indices[4], stub.indices[5]);
    CHECK(stub.first_vertex[0] == 2.0f, "buffer vertex 0 is guest vertex 2 (x=%g)",
          (double)stub.first_vertex[0]);

    /* Same vertex range, different indices: the buffer is reused, the
     * indices are this draw's own. */
    static const u16 idx2[6] = { 2, 3, 4, 5, 4, 3 };
    rsx_draw_engine_present();
    const int created = stub.n_vbuf_create;
    draw_indexed(idx2, 6);
    CHECK(stub.n_vbuf_create == created && stub.last_vbuf != 0 &&
          stub.indices[0] == 0 && stub.indices[1] == 1 && stub.indices[2] == 2 &&
          stub.indices[3] == 3 && stub.indices[4] == 2 && stub.indices[5] == 1,
          "another index list over the same range reuses the buffer with its own indices "
          "(%u %u %u %u %u %u)", stub.indices[0], stub.indices[1], stub.indices[2],
          stub.indices[3], stub.indices[4], stub.indices[5]);

    /* A sparse index list (range longer than the references) is deduplicated
     * as before and not cached. */
    static const u16 idx3[3] = { 7, 0, 4 };
    draw_indexed(idx3, 3);
    CHECK(stub.last_vbuf == 0 && stub.last_vertices == 3,
          "a sparse index list takes the dedup path (%u vertices)", stub.last_vertices);
    engine_down();
}

/* ---- 3. a key whose bytes keep changing rests ---------------------------- */

static void test_resting(void)
{
    printf("-- streamed geometry rests\n");
    engine_up(&g_backend);
    for (int f = 0; f < 6; f++) {
        write_vertices(3, 100.0f * (float)(f + 1));
        draw_arrays(0, 3);
        CHECK(stub.last_vbuf == 0 && stub.first_vertex[0] == 100.0f * (float)(f + 1),
              "frame %d: changed bytes are always fetched (x=%g)", f,
              (double)stub.first_vertex[0]);
        rsx_draw_engine_present();
    }
    rsx_draw_engine_vcache_stats s = stats();
    CHECK(s.created == 0 && s.resting >= 1,
          "no buffer is made, and the key rests after three changes (%llu resting)",
          (unsigned long long)s.resting);
    engine_down();
}

/* ---- 4. off ---------------------------------------------------------------- */

static void test_off(void)
{
    printf("-- off\n");
    setenv("PS3RECOMP_ENG_VCACHE", "0", 1);
    engine_up(&g_backend);
    write_vertices(3, 1.0f);
    for (int f = 0; f < 4; f++) { draw_arrays(0, 3); rsx_draw_engine_present(); }
    CHECK(!stats().enabled && stub.n_vbuf_create == 0 && stub.n_draw == 4 &&
          stub.n_draw_vbuf == 0,
          "PS3RECOMP_ENG_VCACHE=0: every draw is fetched (%d draws, %d buffers)",
          stub.n_draw, stub.n_vbuf_create);
    engine_down();
    unsetenv("PS3RECOMP_ENG_VCACHE");

    rsx_draw_backend plain = g_backend;
    plain.vbuf_create = NULL;
    plain.vbuf_release = NULL;
    plain.draw_vbuf = NULL;
    engine_up(&plain);
    for (int f = 0; f < 4; f++) { draw_arrays(0, 3); rsx_draw_engine_present(); }
    CHECK(!stats().enabled && stub.n_draw == 4,
          "a backend without the hooks keeps the old path (%d draws)", stub.n_draw);
    engine_down();
}

/* ---- 5. rsx_vertex_remap_build_range ------------------------------------- */

static void test_range_remap(void)
{
    printf("-- rsx_vertex_remap_build_range\n");
    rsx_vertex_remap remap;
    memset(&remap, 0, sizeof remap);

    rsx_vertex_ref refs[6] = { {7, 1}, {5, 1}, {6, 1}, {7, 1}, {5, 1}, {6, 1} };
    u32 n = 0;
    int ok = rsx_vertex_remap_build_range(&remap, refs, 6, &n);
    CHECK(ok && n == 3 && refs[0].vertex_id == 5 && refs[1].vertex_id == 6 &&
          refs[2].vertex_id == 7 && refs[0].base_index == 1,
          "refs become the range 5..7 (%d, %u)", ok, n);
    const rsx_vertex_ref orig[6] = { {7, 1}, {5, 1}, {6, 1}, {7, 1}, {5, 1}, {6, 1} };
    int maps = 1;
    for (u32 i = 0; i < 6; i++)
        maps &= refs[rsx_vertex_remap_index(&remap, i)].vertex_id == orig[i].vertex_id;
    CHECK(maps, "every occurrence maps back to its own vertex");

    rsx_vertex_ref sparse[3] = { {0, 0}, {10, 0}, {20, 0} };
    CHECK(!rsx_vertex_remap_build_range(&remap, sparse, 3, &n) && sparse[1].vertex_id == 10,
          "a range longer than the reference count is refused, refs untouched");
    rsx_vertex_ref bases[3] = { {0, 0}, {1, 4}, {2, 0} };
    CHECK(!rsx_vertex_remap_build_range(&remap, bases, 3, &n),
          "references with different base indices are refused");
    rsx_vertex_remap_destroy(&remap);
}

/* ---- 6. split streams ---------------------------------------------------- */

#define M_VTXBUF_OFFSET_N(a) (M_VTXBUF_OFFSET + 4u * (a))
#define M_VTXFMT_N(a)        (M_VTXFMT + 4u * (a))
#define RING_OFFSET          0x00100000u       /* local memory */

/* ATTR4: four bytes a vertex (UB256), stride 4, in a ring in local memory
 * that moves every frame -- Resistance's per-frame lighting stream. */
static void write_ring(u32 frame, u32 n, u32 at)
{
    for (u32 i = 0; i < n; i++) {
        u8* p = vm_base + GUEST_LOCAL_EA + at + i * 4u;
        p[0] = (u8)frame; p[1] = (u8)i; p[2] = (u8)(2u * i); p[3] = 255;
    }
}

static void bind_ring(u32 at)
{
    m(M_VTXFMT_N(4), 7u | (4u << 4) | (4u << 8));   /* UB256 x4, stride 4 */
    m(M_VTXBUF_OFFSET_N(4), at);                      /* bit 31 clear: local */
}

/* The whole-draw path's vertices for the same bytes, every slot. */
static float g_ref[3][16][4];

static int split_matches(u32 frame, float x0, const char* what)
{
    int ok = 1;
    for (u32 v = 0; v < 3; v++)
        for (u32 s = 0; s < 16; s++) {
            float want[4];
            memcpy(want, g_ref[v][s], sizeof want);
            if (s == 0) { want[0] = x0 + (float)v; want[1] = (float)v; want[2] = 0; want[3] = 1; }
            if (s == 4) { want[0] = (float)frame; want[1] = (float)v; want[2] = 2.0f * v; want[3] = 255; }
            if (memcmp(want, stub.split_verts[v][s], sizeof want)) {
                if (ok) printf("   %s: vertex %u slot %u is (%g %g %g %g), want (%g %g %g %g)\n",
                               what, v, s,
                               (double)stub.split_verts[v][s][0], (double)stub.split_verts[v][s][1],
                               (double)stub.split_verts[v][s][2], (double)stub.split_verts[v][s][3],
                               (double)want[0], (double)want[1], (double)want[2], (double)want[3]);
                ok = 0;
            }
        }
    return ok;
}

static void test_split(void)
{
    printf("-- split streams\n");
    rsx_draw_backend split = g_backend;
    split.draw_vbufs = stub_draw_vbufs;

    /* The reference: the same bytes through the path with no cache. */
    setenv("PS3RECOMP_ENG_VCACHE", "0", 1);
    engine_up(&split);
    write_vertices(3, 10.0f);
    write_ring(0, 3, RING_OFFSET);
    bind_ring(RING_OFFSET);
    draw_arrays(0, 3);
    CHECK(stub.n_draw == 1 && stub.n_draw_vbufs == 0 && stub.last_stride == 256,
          "with the cache off a two-span draw is one interleaved stream (stride %u)",
          stub.last_stride);
    memcpy(g_ref, stub.verts, sizeof g_ref);
    CHECK(g_ref[1][4][0] == 0.0f && g_ref[1][4][1] == 1.0f && g_ref[1][4][2] == 2.0f &&
          g_ref[1][4][3] == 255.0f && g_ref[2][0][0] == 12.0f,
          "reference: ATTR4 of vertex 1 is (0 1 2 255), ATTR0 of vertex 2 x=12");
    engine_down();
    unsetenv("PS3RECOMP_ENG_VCACHE");

    engine_up(&split);
    CHECK(stats().split_enabled, "split streams are on with a backend that has draw_vbufs");
    write_vertices(3, 10.0f);
    for (u32 f = 0; f < 4; f++) {
        const u32 at = RING_OFFSET + f * 0x100u;     /* the ring moves */
        write_ring(f, 3, at);
        bind_ring(at);
        draw_arrays(0, 3);
        char what[32]; snprintf(what, sizeof what, "frame %u", f);
        CHECK(stub.n_draw == 0 && stub.n_draw_vbufs == (int)f + 1 && stub.bad_split == 0 &&
              stub.split_streams == 2,
              "frame %u: a split draw of two streams (%d split draws, %d bad)", f,
              stub.n_draw_vbufs, stub.bad_split);
        CHECK(split_matches(f, 10.0f, what),
              "frame %u: every slot of every vertex as the whole-draw path draws it", f);
        if (f >= 2)
            CHECK(stub.split_vbuf[0] != 0 && stub.split_vbuf[1] == 0 && stub.split_staged == 1,
                  "frame %u: the mesh stream from its buffer, only the moved ring staged", f);
        rsx_draw_engine_present();
    }
    {
        const u32 p = stub.bound_pipeline;
        CHECK(stub.pipe_streams[p] == 2 && stub.pipe_map[p] == (1ull << 16),
              "the pipeline was made for the split: ATTR4's slot in stream 1, the rest in 0 "
              "(map %llx)", (unsigned long long)stub.pipe_map[p]);
    }
    rsx_draw_engine_vcache_stats s = stats();
    CHECK(s.split_draws == 4 && s.stream_hits == 2 && s.created == 1 && s.hits == 0,
          "counters: %llu split draws, %llu stream hits, %llu stream misses, %llu created",
          (unsigned long long)s.split_draws, (unsigned long long)s.stream_hits,
          (unsigned long long)s.stream_misses, (unsigned long long)s.created);

    /* The mesh rewritten: its stream is fetched again, never drawn stale. */
    put_be_float(VTX_OFFSET, 77.0f);
    write_ring(4, 3, RING_OFFSET + 0x400u);
    bind_ring(RING_OFFSET + 0x400u);
    draw_arrays(0, 3);
    CHECK(stub.split_vbuf[0] == 0 && stub.split_staged == 2 && stub.split_verts[0][0][0] == 77.0f &&
          stub.split_verts[1][0][0] == 11.0f && stub.split_verts[0][4][0] == 4.0f,
          "a rewritten mesh stream is staged again with its new bytes (x=%g)",
          (double)stub.split_verts[0][0][0]);
    rsx_draw_engine_present();

    /* Both streams still: both drawn from buffers, nothing staged. */
    const int created = stub.n_vbuf_create;
    for (u32 f = 0; f < 3; f++) { draw_arrays(0, 3); rsx_draw_engine_present(); }
    CHECK(stub.split_staged == 0 && stub.split_vbuf[0] && stub.split_vbuf[1] &&
          stub.n_vbuf_create == created + 2 && stub.bad_split == 0,
          "two still streams: both from buffers, nothing staged (%d buffers made)",
          stub.n_vbuf_create - created);
    CHECK(stub.split_verts[0][0][0] == 77.0f && stub.split_verts[2][4][0] == 4.0f &&
          stub.split_verts[2][4][2] == 4.0f,
          "and the buffers hold the current bytes");

    /* The ring right behind the mesh (one span): no split, the whole-draw key. */
    const int splits = stub.n_draw_vbufs;
    m(M_VTXBUF_OFFSET_N(4), 0x80000000u | (VTX_OFFSET + 3u * 16u));
    draw_arrays(0, 3);
    CHECK(stub.n_draw_vbufs == splits && stub.n_draw == 1,
          "attributes in one span are one stream (no split draw)");
    engine_down();

    /* Off: the whole-draw path. */
    setenv("PS3RECOMP_ENG_VCACHE_SPLIT", "0", 1);
    engine_up(&split);
    write_vertices(3, 10.0f);
    for (u32 f = 0; f < 3; f++) {
        write_ring(f, 3, RING_OFFSET + f * 0x100u);
        bind_ring(RING_OFFSET + f * 0x100u);
        draw_arrays(0, 3);
        rsx_draw_engine_present();
    }
    CHECK(!stats().split_enabled && stub.n_draw_vbufs == 0 && stub.n_draw == 3 &&
          stub.n_vbuf_create == 0,
          "PS3RECOMP_ENG_VCACHE_SPLIT=0: whole-draw keys, the moving ring misses every frame "
          "(%d draws, %d buffers)", stub.n_draw, stub.n_vbuf_create);
    engine_down();
    unsetenv("PS3RECOMP_ENG_VCACHE_SPLIT");
}

/* ---- 7. rsx_vertex_fetch_slots == rsx_vertex_fetch_one ------------------- */

static u8* g_fetch_mem;
static const u8* fetch_guest(void* user, u32 location, u32 offset, u32 bytes)
{
    (void)user; (void)location;
    return (u64)offset + bytes <= 4096u ? g_fetch_mem + offset : NULL;
}

static void test_fetch_slots(void)
{
    printf("-- rsx_vertex_fetch_slots\n");
    static u8 mem[4096];
    g_fetch_mem = mem;
    unsigned seed = 12345u;
    int ok = 1, runs = 0;
    for (int run = 0; run < 2000; run++) {
        for (u32 i = 0; i < sizeof mem; i++) { seed = seed * 1103515245u + 12345u; mem[i] = (u8)(seed >> 16); }
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        seed = seed * 1103515245u + 12345u;
        rsx_vertex_layout_plan_init(&plan.layout, (seed >> 8) & 0xFFFFu);
        if (!plan.layout.count) continue;
        plan.guest_ptr = fetch_guest;
        static const u32 types[] = { 1, 2, 3, 4, 5, 6, 7 };
        for (u32 a = 0; a < 16; a++) {
            rsx_vertex_fetch_attr* f = &plan.attr[a];
            seed = seed * 1103515245u + 12345u;
            f->default_value[0] = (float)a; f->default_value[3] = 1.0f;
            if ((seed >> 4) & 3u) {
                f->desc.type = types[(seed >> 8) % 7u];
                f->desc.size = 1u + ((seed >> 12) & 3u);
                f->desc.offset = (seed >> 16) & 0x3FFu;
                f->stride = 16u + ((seed >> 20) & 15u);
                f->elem_size = 16u;
            }
        }
        const rsx_vertex_ref ref = { (seed >> 3) & 31u, 0 };
        u8 one[16 * 16], sub[16 * 16];
        const int r1 = rsx_vertex_fetch_one(&plan, &ref, one);
        u8 slots[16]; u32 n = 0;
        seed = seed * 1103515245u + 12345u;
        for (u32 s = 0; s < plan.layout.count; s++) if ((seed >> s) & 1u) slots[n++] = (u8)s;
        const int r2 = rsx_vertex_fetch_slots(&plan, &ref, slots, n, sub);
        int has0 = 0;
        for (u32 k = 0; k < n; k++) has0 |= plan.layout.attrs[slots[k]] == 0;
        if (r1) {
            if (!r2) ok = 0;
            for (u32 k = 0; k < n && ok; k++)
                if (memcmp(sub + k * 16u, one + slots[k] * 16u, 16)) ok = 0;
        } else if (r2 && has0) ok = 0;
        runs++;
    }
    CHECK(ok && runs > 1000, "a subset of slots fetches the same bytes as the whole vertex (%d runs)", runs);
}

int main(void)
{
    vm_base = (u8*)calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;

    test_hit_and_invalidate();
    test_indexed();
    test_resting();
    test_off();
    test_range_remap();
    test_split();
    test_fetch_slots();

    for (u32 h = 1; h < STUB_MAX_VBUFS; h++) free(stub.vbuf_data[h]);
    free(vm_base);
    if (g_failures) { printf("\n%d check(s) FAILED\n", g_failures); return 1; }
    printf("\nall vertex cache checks passed\n");
    return 0;
}
