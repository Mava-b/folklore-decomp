/*
 * ps3recomp - platform-neutral RSX draw engine (see rsx_draw_engine.h)
 *
 * The orchestration carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has shipped on. Line numbers in the comments
 * below are that file's, so the two can be read side by side; the file itself
 * is vendored and stays exactly as it is.
 *
 * What is deliberately left out, because it is not renderer behaviour a title
 * needs: the shader disk cache, movie mode and its compositor, the a010
 * probe, and every YZ_PERF_PROFILE block.
 */
#include "rsx_draw_engine.h"

#include "rsx_fp_decompiler.h"
#include "rsx_primitives.h"
#include "rsx_restart_cuts.h"
#include "rsx_tiling.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>   /* PS3_MODS: the packs' folders, PS3_MODS_DUMP's */
#if !defined(_WIN32)
#include <dirent.h>
#endif
#include "ps3emu/log_first.h"

/* Guest memory and the RSX offset resolvers, declared the way every backend
 * that reads guest data declares them (rsx_vertex_fetch.h). */
extern u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 offset);
/* cellGcmSys.c: the display buffers registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void);
/* Non-zero only when the offset's page is in the IO table, so a caller can
 * tell IO-mapped main memory from "assume VRAM". */
u32 cellGcmResolveIO(u32 offset);
/* The guest VM's OOB guard: the size a host backed, or 0 for "the whole 32-bit
 * space is backed and no check is needed" (runtime/memory/vm.h). */
extern u32 ppu_vm_size;

/* ------------------------------------------------------------------------- */

#define ENG_MAX_SURFACES   64
#define ENG_MAX_ZDEPTHS    64
#define ENG_MAX_TEXTURES   RSX_DRAW_ENGINE_TEXTURE_CACHE
#define ENG_MAX_VTEXTURES  64
#define ENG_MAX_PIPELINES  32768       /* 8192 filled up in long sessions */
#define ENG_PIPELINE_INDEX 65536u        /* open-addressed key index, 2x the table (u16 slots) */
/* One BEGIN/END can carry far more than 256 packets: each DRAW_ARRAYS /
 * DRAW_INDEX_ARRAY word covers at most 256 vertices, so a big mesh arrives as
 * hundreds of them. At 256 the rest was dropped without a word: Folklore's
 * cliff and rock meshes (one 22,770-vertex list) lost about a quarter of their
 * triangles, sky showing through. Contiguous packets are merged on arrival
 * (eng_queue_batch: the primitive stream is their concatenation, so this is
 * exact), and the queue is sized for meshes that are not contiguous
 * (Folklore's D3D12 patch 0019, same fix). */
#define ENG_MAX_BATCHES    4096
#define ENG_VERT_STRIDE    (RSX_DSP_NUM_VERTEX_ATTR * 16u)
#define ENG_INVALID        0xFFFFFFFFu
/* Guest textures larger than this are a misdecoded register, not an image;
 * rsx_live_draw.c rejects the same 4096 bound (2033). */
#define ENG_MAX_TEX_DIM    4096u
/* A surface declaration beyond this is a guest pointer read as clip
 * dimensions. rsx_live_draw.c's surface-guard (2625) preserves the existing
 * target rather than destroying a live one. */
#define ENG_MAX_SURFACE_DIM 8192u

#define ENG_VP_CB_BYTES    ((RSX_DSP_NUM_CONSTANTS + 2) * 16u)
#define ENG_FP_MAX_BYTES   0x10000u

#define M_ALPHA_TEST_ENABLE   0x0304
#define M_ALPHA_FUNC          0x0308
#define M_ALPHA_REF           0x030C
#define M_BLEND_ENABLE        0x0310
#define M_BLEND_SFACTOR       0x0314
#define M_BLEND_DFACTOR       0x0318
#define M_BLEND_EQUATION      0x0320
#define M_COLOR_MASK          0x0324
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_DEPTH_BOUNDS_ENABLE 0x0380
#define M_DEPTH_BOUNDS_MIN    0x0384
#define M_DEPTH_BOUNDS_MAX    0x0388
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_DEPTH_FUNC          0x0A6C
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST_ENABLE   0x0A74
#define M_ZSTENCIL_CLEAR      0x1D8C
#define M_POLY_OFFSET_POINT   0x0A60
#define M_POLY_OFFSET_LINE    0x0A64
#define M_POLY_OFFSET_FILL    0x0A68
#define M_POLY_OFFSET_FACTOR  0x0A78
#define M_POLY_OFFSET_BIAS    0x0A7C
#define M_ZMIN_MAX_CONTROL    0x1D78
#define M_CLEAR_REPORT_VALUE  0x17C8
#define M_ZPASS_PIXEL_COUNT   0x17CC

/* ---- module state -------------------------------------------------------- */

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 pitch;              /* guest bytes per row, from SET_SURFACE_PITCH */
    u32 aa_grid;            /* samples per pixel, aa_sx | aa_sy << 8          */
    rsx_be_format fmt;
    u32 handle;
    u32 feedback;          /* last immutable own-target copy, engine-owned */
    u64 color_serial;       /* g.write_serial of the last colour write, or 0 */
    int guest_dirty;        /* rendered since guest memory last got a copy   */
    u32 last_use;           /* g.frames when last bound, sampled or presented */
    u64 last_stamp;         /* g.use_serial then: the order within a frame   */
} eng_surface;

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 handle;
    u32 snapshot;          /* sampleable copy, or 0                        */
    u32 snap_w, snap_h;    /* the region that copy holds                    */
    int cleared;
    int had_write;
    int snapshot_valid;
    u64 depth_serial;      /* g.write_serial of the last depth write or clear */
    int guest_dirty;       /* written since guest memory last got a copy     */
    u32 zfmt, pitch;       /* SURFACE_FORMAT depth field and zeta pitch then */
    int swizzled;          /* SURFACE_FORMAT type SWIZZLE: Morton order in memory */
    u32 used_w, used_h;    /* the clip that write went through              */
    u32 conv;              /* depth_as_color of the depth at conv_serial, or 0 */
    u32 conv_w, conv_h, conv_fmt;
    u64 conv_serial;
    u32 last_use;          /* g.frames when last bound or sampled             */
    u64 last_stamp;        /* g.use_serial then: the order within a frame     */
    u32 ext_w, ext_h;      /* the largest clip it was bound with: past it,
                            * nothing was drawn or cleared (eng_clear_rect) */
    u32 dbz_copy;          /* float copy for the emulated depth-bounds test */
    u32 dbz_w, dbz_h;
    u64 dbz_serial;        /* depth_serial that copy holds                 */
    u32 snapshot_rgba;     /* depth_snapshot_rgba8 copy (backend-owned), or 0 */
    u32 rgba_w, rgba_h;    /* the region that copy holds                    */
    int snapshot_rgba_valid;
} eng_zdepth;

typedef struct {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
    u32 handle;
    u64 content_hash;
    u64 sparse_hash;            /* eng_texture_sparse_hash, the per-frame check */
    u32 last_full_frame;        /* g.frames of the last full content hash */
    u32 last_hash_frame;
    u64 last_use_serial;
    u32 depth;                  /* slices of a 3D texture, else 1 */
    u32 levels;                 /* SET_TEXTURE_FORMAT mipmap count it was built with */
    int rewritten;              /* guest bytes rewritten by the engine itself */

} eng_texture;

typedef struct {
    u64 key;
    u32 handle;
    u8 fixed;
    u16 tex_used;           /* units the pixel program samples */
    u64 fp_hash;            /* the guest fragment program's debug name (eng_fp_memo.structural) */
} eng_pipeline;

typedef struct { u32 first, count; } eng_batch;

typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} eng_display_buffer;

/* GET_REPORTs whose counts may be outstanding at once (a title reads them a
 * frame or two later; Resistance issues 16 a frame). */
#define ENG_ZPASS_TICKETS 1024u
/* A report summing more queries than this (a title that never clears the
 * count) is answered conservatively instead of walked. */
#define ENG_ZPASS_MAX_RANGE 256u

static struct {
    const rsx_draw_backend* be;
    int ready;
    int default_on;
    u32 width, height;

    rsx_dispatch rsx;

    eng_surface surfaces[ENG_MAX_SURFACES];
    u32 n_surfaces;
    eng_zdepth zdepths[ENG_MAX_ZDEPTHS];
    u32 n_zdepths;
    eng_texture textures[ENG_MAX_TEXTURES];
    u32 n_textures;
    eng_pipeline pipelines[ENG_MAX_PIPELINES];
    u32 n_pipelines;
    u16 pipeline_index[ENG_PIPELINE_INDEX];   /* pipelines[] slot + 1, 0 = empty */
    eng_display_buffer display_buffers[8];

    u32 frames;
    u64 texture_use_serial;
    u32 guest_draws;
    u32 last_guest_draws;
    u32 last_present_surface;
    u32 last_flip_buffer;
    u64 write_serial;       /* orders colour and depth writes to one address */
    u64 use_serial;         /* stamps surface and zeta slots for eviction    */

    /* staging: decoded texture levels, the constant blocks, the index list */
    u8* tex_staging;
    u32 tex_staging_cap;
    u8* vp_cb;
    u8* fp_cb;
    u32 fp_cb_cap;
    u32* indices;
    u32 index_cap;

    /* The draw's fragment-program constants: the program memo's block (see
     * eng_fp_current), valid until the next draw's pipeline lookup. */
    const rsx_fp_constant_block* fp_constants;

    /* ZPASS counting (rsx_draw_engine_zpass_*). Draws are counted into
     * queries, numbered from 1: zp_query is the one draws go into now (0:
     * none open), zp_last the last one handed out, zp_first the first since
     * the last CLEAR_REPORT_VALUE (0 reads as 1). A ticket remembers the
     * range of queries its report sums. */
    u32 zp_query, zp_last, zp_first;
    u32 zp_ticket;
    struct { u32 ticket, first, last; } zp_tickets[ENG_ZPASS_TICKETS];
} g;

/* The decompilers' HLSL. The VP decompiler builds bodies up to 192 KB. */
static char s_vs_hlsl[256 * 1024];
static char s_ps_hlsl[256 * 1024];

/* The draw accumulator: one BEGIN_END group's batches, references and cuts. */
static struct {
    eng_batch arr[ENG_MAX_BATCHES];
    u32 n_arr;
    eng_batch idx[ENG_MAX_BATCHES];
    u32 n_idx;
    u32 n_packets;

    rsx_vertex_ref* refs;
    u32 n_refs, cap_refs, n_source_refs;
    int refs_remapped;
    rsx_vertex_remap ref_remap;

    u32* cuts;
    u32 n_cuts, cap_cuts;

    u8* verts;
    u64 verts_cap;
    u32 n_verts;
    u32 vbuf;          /* a cached persistent vertex buffer this draw uses, else 0;
                        * dc.verts was NOT filled then */
    /* A split draw (eng_vc_split_fetch): n_streams > 1 streams, slot s in
     * stream (stream_map >> 4*s) & 0xF; a stream's vertices are a persistent
     * buffer or a region of dc.verts. 0: one interleaved stream as above. */
    u32 n_streams;
    u64 stream_map;
    rsx_be_vstream streams[RSX_BE_MAX_VSTREAMS];

    /* Canonical ATTR0..15 float4 snapshots for register-based vertices.
     * Bounded separately from compact GPU staging; never fetch stale arrays. */
    float (*immediate)[RSX_DSP_NUM_VERTEX_ATTR][4];
    u32 n_immediate, cap_immediate;

    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int fetch_ok;

    /* INLINE_ARRAY stream for this group, when the vertices came down the
     * FIFO rather than from a vertex array. */
    const u8* inl;
    u32 inl_bytes;
    u32 inline_dropped_at_begin;
} dc;

/* ---- guest memory -------------------------------------------------------- */

/* rsx_dsp_* locations are RSX_LOCATION_LOCAL = 0 / _MAIN = 1;
 * cellGcmResolveLocated's argument is the opposite sense (1 = local), and
 * getting that backwards resolves every VRAM object through the IO table. */
static rsx_vertex_guest_ptr_fn s_guest_reader;
static void* s_guest_user;

void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user)
{
    s_guest_reader = reader;
    s_guest_user = user;
}

static const u8* eng_guest_ptr(void* user, u32 location, u32 offset,
                               u32 min_bytes)
{
    (void)user;
    if (!min_bytes || min_bytes > (256u << 20)) return NULL;
    if (s_guest_reader) return s_guest_reader(s_guest_user, location, offset, min_bytes);
    if (!vm_base) return NULL;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    if (!ea) return NULL;
    if (ppu_vm_size && (u64)ea + min_bytes > ppu_vm_size) return NULL;
    return vm_base + ea;
}

/* Guest memory as the engine reaches it, for the tiled range copies (rsx_tiling.h). */
static u8* eng_tiled_ptr(void* user, u32 location, u32 offset, u32 len)
{
    return (u8*)eng_guest_ptr(user, location, offset, len);
}

static const rsx_tiled_mem s_eng_mem = { eng_tiled_ptr, NULL, NULL };

/* Whether [offset, offset + len) touches a tile bound over main memory. */
static int eng_tiled(u32 location, u32 offset, u32 len)
{
    rsx_tile t;
    return rsx_tiling_find(location, offset, len, &t);
}

static u8* s_detiled;
static u32 s_detiled_cap;

/* Guest bytes as the RSX reads them: through the tiling where a tile bound
 * over main memory covers them, as they lie elsewhere. The result lasts
 * until the next call. */
static const u8* eng_guest_linear(u32 location, u32 offset, u32 len)
{
    const u8* raw = eng_guest_ptr(NULL, location, offset, len);
    if (!raw || !eng_tiled(location, offset, len)) return raw;
    if (s_detiled_cap < len) {
        u8* n = (u8*)realloc(s_detiled, len);
        if (!n) return NULL;
        s_detiled = n;
        s_detiled_cap = len;
    }
    rsx_tiled_read(&s_eng_mem, location, offset, s_detiled, len);
    return s_detiled;
}

/* Detiled copies the vertex, index and program fetches of one draw read
 * (eng_fetch_ptr). The fetch plan keeps pointers into them for the whole
 * draw, so a chunk never moves; all are reused from the next draw on. */
#define ENG_ARENA_CHUNK (1u << 20)
typedef struct eng_arena_chunk {
    struct eng_arena_chunk* next;
    u32 cap, used;
    u8 data[];
} eng_arena_chunk;
static eng_arena_chunk* s_arena;

static u8* eng_arena_alloc(u32 bytes)
{
    bytes = (bytes + 15u) & ~15u;
    for (eng_arena_chunk* c = s_arena; c; c = c->next)
        if (c->cap - c->used >= bytes) {
            u8* p = c->data + c->used;
            c->used += bytes;
            return p;
        }
    const u32 cap = bytes > ENG_ARENA_CHUNK ? bytes : ENG_ARENA_CHUNK;
    eng_arena_chunk* c = (eng_arena_chunk*)malloc(sizeof(*c) + cap);
    if (!c) return NULL;
    c->next = s_arena;
    c->cap = cap;
    c->used = bytes;
    s_arena = c;
    return c->data;
}

static void eng_arena_reset(void)
{
    for (eng_arena_chunk* c = s_arena; c; c = c->next) c->used = 0;
}

/* [offset, offset + len) read through the tiling into the arena; what
 * guest memory does not back reads as zero. */
static const u8* eng_fetch_detiled(u32 location, u32 offset, u32 len)
{
    if ((u64)offset + len > 0x100000000ull) len = (u32)(0x100000000ull - offset);
    u8* p = len ? eng_arena_alloc(len) : NULL;
    if (p) rsx_tiled_read(&s_eng_mem, location, offset, p, len);
    return p;
}

/* eng_guest_ptr for the vertex, index and program fetches: a range that
 * touches a tiled main-memory region comes back detiled, valid until the
 * next draw. */
static const u8* eng_fetch_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    const u8* raw = eng_guest_ptr(user, location, offset, min_bytes);
    if (!raw || !eng_tiled(location, offset, min_bytes)) return raw;
    return eng_fetch_detiled(location, offset, min_bytes);
}

static u64 eng_fnv1a(const void* data, u32 n, u64 hash)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}

static u32 eng_pipeline_slot(u64 key)
{
    return (u32)((key ^ (key >> 29)) * 0x9E3779B97F4A7C15ull >> 40) & (ENG_PIPELINE_INDEX - 1u);
}

/* ---- render state -------------------------------------------------------- */

/* PS3RECOMP_ENG_DEPTH_CLAMP=0: always clip at the near and far planes, as
 * before SET_ZMIN_MAX_CONTROL was honoured. */
static int eng_depth_clamp_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_DEPTH_CLAMP");
        on = !(e && *e && atoi(e) == 0);
    }
    return on;
}

void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func   = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    rs->alpha_ref_format = sf.color_format;
    /* Alpha to coverage (ANTI_ALIASING_CONTROL 0x1D7C: bit 0 enable, bit 4
     * alpha-to-coverage). Foliage and wire fences are drawn with it and no
     * alpha test, so without it their cut-out texels came out solid: grass as
     * opaque cards, fences as black panels. An antialiased surface is held at
     * its sample resolution here, not as Metal MSAA, so coverage cannot come
     * from alpha; a fragment whose alpha is under one half covers no sample in
     * the common case, so it is emulated as an alpha test GEQUAL 0.5 when the
     * title's own test is off (Folklore's D3D12 patch 0020, same rule).
     * PS3_RSX_NO_A2C=1 turns the emulation off. */
    {
        static int a2c_off = -1;
        if (a2c_off < 0) a2c_off = getenv("PS3_RSX_NO_A2C") ? 1 : 0;
        const u32 aa_control = rsx_dsp_reg(rsx, 0x1D7C);
        if (!a2c_off && !rs->alpha_test_enable && (aa_control & 0x11u) == 0x11u && sf.antialias != 0) {
            rs->alpha_test_enable = 1;
            rs->alpha_func = 0x0206u;                                  /* GEQUAL */
            rs->alpha_ref_raw = sf.color_format == 14u ? 0x3800u       /* half 0.5 */
                              : (sf.color_format == 15u || sf.color_format == 16u) ? 0x3F000000u
                              : 0x80u;                                 /* 128/255 */
        }
    }
    rs->rt_fp16 = sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->blend_enable = rsx_dsp_reg(rsx, M_BLEND_ENABLE) & 1;
    const u32 sfac = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 dfac = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq   = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sfac & 0xFFFF; rs->sf_a = sfac >> 16;
    rs->df_rgb = dfac & 0xFFFF; rs->df_a = dfac >> 16;
    rs->eq_rgb = eq & 0xFFFF;   rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, 0x183C) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, 0x1830);
    rs->front_face  = rsx_dsp_reg(rsx, 0x1834);
    /* The RAW register: a game-written 0 is a real "write no colour channel"
     * (a depth-prime pass), and rsx_dispatch_init seeds the nv40 reset value
     * so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->color_mask_mrt = rsx_dsp_reg(rsx, 0x0370) & 0xFFF0u;
    rs->blend_mrt      = rsx_dsp_reg(rsx, 0x036C) & 0xEu;
    rs->stencil_enable    = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
    rs->depth_bounds_test = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_ENABLE) & 1;
    /* SET_ZMIN_MAX_CONTROL (cellGcmSetZMinMaxControl): bits 0-3 clip at the
     * near and far planes, 4-7 clamp the depth, 8-11 clip ignoring W; reset
     * value 1 (rsx_dispatch_init). As RPCS3's Vulkan renderer: clamp when the
     * clamp is on or near/far clipping is off. Shadow-map passes draw their
     * casters this way ("pancaking"): a caster between the light and the
     * cascade's near plane lands on depth 0 instead of being cut away, so
     * clipped, the shadows of tall objects vanish from the near cascade. */
    {
        const u32 z = rsx_dsp_reg(rsx, M_ZMIN_MAX_CONTROL);
        rs->depth_clamp = eng_depth_clamp_on() && ((z & 0xF0u) != 0 || (z & 0x0Fu) == 0);
    }
}

u64 rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash)
{
#define ENG_HASH_FIELD(name) hash = eng_fnv1a(&rs->name, sizeof(rs->name), hash)
    /* The alpha REFERENCE is deliberately absent: it lives in the fragment
     * constant block, so a title animating a fade must not compile a new
     * pipeline per frame. Enable and compare mode still select a variant.
     * Every field is listed by name so struct padding can never be identity
     * (rsx_live_draw.c:3151-3185). */
    ENG_HASH_FIELD(alpha_test_enable);
    ENG_HASH_FIELD(alpha_func);
    ENG_HASH_FIELD(blend_enable);
    ENG_HASH_FIELD(sf_rgb);
    ENG_HASH_FIELD(df_rgb);
    ENG_HASH_FIELD(sf_a);
    ENG_HASH_FIELD(df_a);
    ENG_HASH_FIELD(eq_rgb);
    ENG_HASH_FIELD(eq_a);
    ENG_HASH_FIELD(depth_test);
    ENG_HASH_FIELD(depth_write);
    ENG_HASH_FIELD(depth_func);
    ENG_HASH_FIELD(cull_enable);
    ENG_HASH_FIELD(cull_face);
    ENG_HASH_FIELD(front_face);
    ENG_HASH_FIELD(color_mask);
    ENG_HASH_FIELD(color_mask_mrt);
    ENG_HASH_FIELD(blend_mrt);
    ENG_HASH_FIELD(rt_fp16);
    ENG_HASH_FIELD(stencil_enable);
    ENG_HASH_FIELD(stencil_two_sided);
    ENG_HASH_FIELD(s_func);
    ENG_HASH_FIELD(s_func_mask);
    ENG_HASH_FIELD(s_write_mask);
    ENG_HASH_FIELD(s_fail);
    ENG_HASH_FIELD(s_zfail);
    ENG_HASH_FIELD(s_zpass);
    ENG_HASH_FIELD(bs_func);
    ENG_HASH_FIELD(bs_fail);
    ENG_HASH_FIELD(bs_zfail);
    ENG_HASH_FIELD(bs_zpass);
    ENG_HASH_FIELD(depth_bounds_test);
    ENG_HASH_FIELD(depth_clamp);
#undef ENG_HASH_FIELD
    return hash;
}

/* ---- topology expansion -------------------------------------------------- */

/* Every expansion is bounded by the restart cuts, so a strip that the guest
 * broke with the sentinel index gets no connecting triangle across the break.
 * Missing that decode is what upstream's aa7fb63 records as "the exploded
 * spiky mesh that occluded the scene". */
u32 rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                         const u32* cuts, u32 cut_count)
{
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        return source_refs - source_refs % 3u;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUAD_STRIP: {
        /* Two vertices per quad after the first pair, so a segment of n
         * carries (n - 2) / 2 quads and an odd trailing vertex is dropped. */
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 4) total += ((count - 2u) / 2u) * 6u;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUADS:
        return (source_refs / 4u) * 6u;
    default:
        return 0;
    }
}

static u32 eng_topology_vertex(const u32* occurrence_to_unique, u32 occurrence)
{
    return occurrence_to_unique ? occurrence_to_unique[occurrence] : occurrence;
}

void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices)
{
    u32 write = 0;
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES: {
        const u32 count = source_refs - source_refs % 3u;
        for (u32 o = 0; o < count; o++)
            indices[write++] = eng_topology_vertex(occurrence_to_unique, o);
        break;
    }
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            /* Odd triangles swap their first two vertices, so the whole strip
             * keeps one winding. */
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 1u - (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 2u);
            }
        }
        break;
    /* A polygon is a convex fan around its own first vertex, which is what
     * both of this tree's other renderers make of one. */
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 4) continue;
            /* Every quad is split along the diagonal between the two pairs,
             * so each one keeps the strip's winding without alternating the
             * way a triangle strip has to; a cut simply starts the pairing
             * again from the segment's own first vertex. This is the
             * expansion the vtable path's emit_vertices performs. */
            for (u32 q = 0; q + 3 < count; q += 2) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 3u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 q = 0; q < source_refs / 4u; q++) {
            const u32 base = q * 4u;
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 1u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 3u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
        }
        break;
    default:
        break;
    }
}

/* Does this primitive become an indexed triangle list, and is the index buffer
 * needed at all? rsx_vertex_topology_plan answers that for the reference
 * engine, whose own expansion stops at quads. Quad strips and polygons are
 * this engine's, so their two cases are decided here rather than in the
 * shared helper, which the vendored rsx_live_draw.c also calls. */
static int eng_topology_rebuild(u32 primitive, int refs_remapped, int* indexed)
{
    if (primitive == RSX_PRIMITIVE_QUAD_STRIP ||
        primitive == RSX_PRIMITIVE_POLYGON) {
        (void)refs_remapped;
        *indexed = 1;
        return 1;
    }
    return rsx_vertex_topology_plan(primitive, refs_remapped, indexed);
}

/* ---- surfaces ------------------------------------------------------------ */

static rsx_be_format eng_surface_format(u32 color_format)
{
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default:   return RSX_BE_FMT_R8G8B8A8;
    }
}

/* Decode the guest's own bytes behind a colour surface into host R,G,B,A rows
 * so a freshly created target starts with what the title CPU-initialised it
 * to. Returns the staging pointer, or NULL when the bytes are unreadable or
 * the format is not one the decoder handles. */
static const void* eng_surface_seed(u32 location, u32 offset, u32 w, u32 h,
                                    rsx_be_format fmt, u32* out_row_bytes)
{
    *out_row_bytes = 0;
    if (fmt != RSX_BE_FMT_R8G8B8A8) return NULL;
    /* Only a surface in IO-mapped MAIN memory is seeded. RSX local memory is
     * reserved and not backed by this tree's guest VM -- runtime/memory/vm.h
     * maps the whole space PROT_NONE and commits main memory and the stacks,
     * leaving a runner to commit what its own title needs -- so reading a VRAM
     * surface's backing would fault long before it could help. Both halves are
     * checked: the DMA context says which space the offset is in, and the IO
     * table says the page is really mapped. */
    if (location != RSX_LOCATION_MAIN ||
        (!s_guest_reader && !cellGcmResolveIO(offset))) return NULL;
    rsx_tex_layout tl;
    /* A8R8G8B8 with the LN bit: a surface is linear, never swizzled. */
    rsx_texture_layout(0x85u | 0x20u, w, h, &tl);
    if (!tl.face_bytes || !tl.dst_row_bytes) return NULL;
    const u8* src = eng_guest_linear(location, offset, tl.face_bytes);
    if (!src) return NULL;
    const u32 bytes = tl.dst_row_bytes * tl.rows;
    if (g.tex_staging_cap < bytes) {
        u8* n = (u8*)realloc(g.tex_staging, bytes);
        if (!n) return NULL;
        g.tex_staging = n;
        g.tex_staging_cap = bytes;
    }
    rsx_texture_decode(g.tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    *out_row_bytes = tl.dst_row_bytes;
    return g.tex_staging;
}

/* The surface and zeta tables keep every target a title has rendered to, and
 * nothing used to take an entry out. A title that runs through enough scenes
 * fills both 64-entry tables, and a target created after that got no slot: a
 * shadow map was rendered nowhere and read back as zeros from guest memory, so
 * every shadow it casts was lost. A full table gives up the entry used
 * longest ago, preferably one not used since before the previous frame.
 * What an evicted target held and guest memory never got is gone, as it is
 * for any address the engine never saw.
 *
 * When every entry was used this frame or the last, the one bound, sampled or
 * presented longest ago (last_stamp) goes all the same, never `keep`. A title
 * that packs its shadow maps into one pool puts each at an offset that
 * depends on the sizes of the maps before it, so the set of addresses keeps
 * moving with the lights in view; Resistance fills 64 slots within minutes.
 * With no slot to give, every later pass into a new address was dropped
 * whole -- its clear and draws -- and the receivers sampled whatever guest
 * memory held there: a hard-edged dark quad on the ground for each such
 * light. The slot found is never one the current draw resolved: those carry
 * the newest stamps. Returns the slot, or ENG_INVALID. */
#define ENG_EVICT_MIN_AGE 2u
#define ENG_TOUCH(e) ((e)->last_use = g.frames, (e)->last_stamp = ++g.use_serial)
static u32 eng_oldest_slot(const char* what, const u32* last_use, const u64* last_stamp,
                           size_t stride, u32 n, u32 keep)
{
    u32 best = ENG_INVALID, best_use = 0;
    for (u32 i = 0; i < n; i++) {
        const u32 use = *(const u32*)((const u8*)last_use + i * stride);
        if (i == keep || g.frames - use < ENG_EVICT_MIN_AGE) continue;
        if (best == ENG_INVALID || use < best_use) { best = i; best_use = use; }
    }
    if (best == ENG_INVALID) {
        u64 oldest = ~(u64)0;
        for (u32 i = 0; i < n; i++) {
            const u64 t = *(const u64*)((const u8*)last_stamp + (size_t)i * stride);
            if (i != keep && t < oldest) { oldest = t; best = i; }
        }
        static u32 full_logs = 0;
        if (PS3_LOG_FIRST(full_logs, 8))
            fprintf(stderr, "[rsx engine] %s table full: every entry was used this frame or the"
                            " last, evicting the least recently used all the same\n", what);
    }
    return best;
}

/* The colour target for this (location, offset), created or reallocated when
 * the size or format moved. Returns a slot index, or ENG_INVALID.
 * rsx_live_draw.c's surface_get (2605-2690).
 *
 * A smaller clip on the same rows is the same memory, not a new surface: with
 * the same format and pitch, RSX draws into the top-left of what is already
 * there. God of War: Ascension renders its scene into a 1280x720 target, then
 * runs a 640x360 pass at the same address and pitch and copies the full
 * 1280x720 out for its SPU anti-aliasing; recreating the target at 640x360
 * lost the scene and left that quarter of the frame black.
 *
 * Only on the same sample grid, though: an antialiased target is stored at
 * sample resolution, so the same memory declared without antialiasing is a
 * different layout, and drawing it into the larger target would squeeze the
 * frame into its left part. */
static u32 eng_surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                          rsx_be_format want_fmt, u32 pitch, u32 aa_grid)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;

    u32 slot = ENG_MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset) {
            eng_surface* s = &g.surfaces[i];
            if (s->fmt == want_fmt &&
                ((s->w == want_w && s->h == want_h) ||
                 (pitch && s->pitch == pitch && s->aa_grid == aa_grid &&
                  s->w >= want_w && s->h >= want_h))) {
                ENG_TOUCH(s);
                return i;
            }
            slot = i;
            break;
        }

    /* Never destroy a usable render target because one malformed command
     * decoded a guest pointer as clip dimensions. */
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM) {
        static u32 logs = 0;
        if (PS3_LOG_FIRST(logs, 8))
            fprintf(stderr, "[rsx engine] rejected implausible surface 0x%X %ux%u;"
                            " keeping the %s target\n", offset, want_w, want_h,
                    slot < ENG_MAX_SURFACES ? "existing" : "absent");
        return slot < ENG_MAX_SURFACES ? slot : ENG_INVALID;
    }
    int evicted = 0;
    if (slot == ENG_MAX_SURFACES) {
        if (g.n_surfaces < ENG_MAX_SURFACES) {
            slot = g.n_surfaces;
        } else {
            slot = eng_oldest_slot("surface", &g.surfaces[0].last_use, &g.surfaces[0].last_stamp,
                                   sizeof(eng_surface), g.n_surfaces, g.last_present_surface);
            if (slot == ENG_INVALID) return ENG_INVALID;
            evicted = 1;
        }
    }

    u32 seed_row = 0;
    const void* seed = eng_surface_seed(location, offset, want_w, want_h,
                                        want_fmt, &seed_row);
    const u32 handle = g.be->color_target_create(g.be->user, want_fmt,
                                                 want_w, want_h, seed, seed_row);
    if (!handle)
        return (!evicted && slot < g.n_surfaces && g.surfaces[slot].handle) ? slot : ENG_INVALID;

    eng_surface* s = &g.surfaces[slot];
    if (evicted) {
        static u32 logs = 0;
        if (PS3_LOG_FIRST(logs, 4))
            fprintf(stderr, "[rsx engine] surface table full: %u:0x%08X %ux%u takes the slot"
                            " of 0x%08X %ux%u, the least recently used\n", location, offset,
                    want_w, want_h, s->offset, s->w, s->h);
    }
    if (s->feedback) { g.be->texture_release(g.be->user, s->feedback); s->feedback = 0; }
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    s->location = location; s->offset = offset;
    s->w = want_w; s->h = want_h; s->fmt = want_fmt; s->aa_grid = aa_grid;
    s->handle = handle;
    s->color_serial = 0;
    s->guest_dirty = 0;
    ENG_TOUCH(s);
    if (slot == g.n_surfaces) g.n_surfaces++;
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 16))
        fprintf(stderr, "[rsx engine] surface %u:0x%08X %ux%u fmt %d%s\n",
                location, offset, want_w, want_h, (int)want_fmt,
                seed ? " (seeded from guest memory)" : ""); }
    return slot;
}

/* The colour targets SET_SURFACE_COLOR_TARGET names, as slots into
 * g.surfaces, with A first. One for an ordinary draw; an MRT set adds B, C
 * and D, each created at target A's size and format, which is the rule the
 * vtable path's target_for binds by. Resolution stops at the first member
 * that cannot be created, as that path stops at the first gap. Returns how
 * many were resolved, or 0. */
static u32 eng_current_target_set(u32 slots[RSX_BE_MAX_COLOR_TARGETS])
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    /* SET_SURFACE_COLOR_TARGET 2 selects B alone; every other value starts at
     * A, and the MRT selectors add B, C and D on top of it. */
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    const rsx_be_format fmt = eng_surface_format(sf.color_format);
    const u32 aa_grid = sf.aa_sx | (sf.aa_sy << 8);
    const u32 first = eng_surface_get(sf.color_location[sel], sf.color_offset[sel],
                                      sf.clip_w, sf.clip_h, fmt, sf.color_pitch[sel], aa_grid);
    if (first == ENG_INVALID) return 0;
    g.surfaces[first].pitch = sf.color_pitch[sel];
    slots[0] = first;
    u32 n = 1;

    static const u32 mrt_from[3] = {
        CELL_GCM_SURFACE_TARGET_MRT1, CELL_GCM_SURFACE_TARGET_MRT2,
        CELL_GCM_SURFACE_TARGET_MRT3
    };
    for (u32 i = 0; i < 3; i++) {
        if (sf.color_target < mrt_from[i]) break;
        /* At A's size, which may be larger than the clip (see above): the
         * framebuffer takes A's extent, and every attachment must cover it. */
        const u32 slot = eng_surface_get(sf.color_location[i + 1],
                                         sf.color_offset[i + 1],
                                         g.surfaces[first].w, g.surfaces[first].h, fmt,
                                         sf.color_pitch[i + 1], aa_grid);
        if (slot == ENG_INVALID) break;
        g.surfaces[slot].pitch = sf.color_pitch[i + 1];
        /* A set naming one buffer twice would attach the same target twice,
         * which no host API allows. */
        int seen = 0;
        for (u32 k = 0; k < n; k++) if (slots[k] == slot) seen = 1;
        if (seen) break;
        slots[n++] = slot;
    }
    return n;
}

/* ---- coherence with guest memory around 2D transfers ------------------- */

/* Bytes per pixel of a surface a 2D transfer can touch: A8R8G8B8, and the
 * FP16 RGBA target an HDR pass renders into (Twisted Metal copies its bloom
 * out of one for the SPU post-FX). 0 for any other format. */
static u32 eng_transfer_bpp(rsx_be_format fmt)
{
    switch (fmt) {
    case RSX_BE_FMT_R8G8B8A8:      return 4u;
    case RSX_BE_FMT_R16G16B16A16F: return 8u;
    default:                       return 0u;
    }
}

/* The surface holding guest bytes [offset, offset + pitch * h), with the byte
 * position of `offset` inside its row (*xb) and its row (*y). Only a surface
 * laid out with the transfer's own pitch qualifies: another pitch puts the
 * rows elsewhere. The CLOSEST base at or below the address wins: surfaces'
 * guest ranges overlap, and an FP16 target right after an RGBA8 one must not
 * resolve into the RGBA8 one's rows (rsx_live_draw.c's resolve takes the same
 * rule, and its comment records copying out of a stale surface at 0x0 before
 * it did). */
static u32 eng_surface_at(u32 location, u32 offset, u32 pitch, u32* xb, u32* y)
{
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != location || offset < s->offset) continue;
        const u32 bpp = eng_transfer_bpp(s->fmt);
        if (!bpp || s->pitch != pitch) continue;
        const u32 d = offset - s->offset;
        if (d / pitch >= s->h || d % pitch >= s->w * bpp) continue;
        if (best != ENG_INVALID && g.surfaces[best].offset > s->offset) continue;
        best = i;
        *xb = d % pitch;
        *y = d / pitch;
    }
    return best;
}

static u8* s_coherence_rows;
static u32 s_coherence_cap;

/* RSX_ENGINE_COHERENCE_LOG=<n>: log the first n transfers the engine syncs,
 * with how many non-zero pixels moved -- the question when a composite comes
 * out black is whether the GPU surface had anything in it. */
static int eng_coherence_log(void)
{
    static int left = -1;
    if (left < 0) { const char* e = getenv("RSX_ENGINE_COHERENCE_LOG"); left = e ? atoi(e) : 0; }
    return left > 0 ? left-- : 0;
}

/* Pixels of `bpp` bytes whose colour (the first three quarters: R,G,B) is
 * not all zero. */
static u32 eng_nonzero_pixels(const u8* rows, u32 n, u32 bpp)
{
    u32 k = 0;
    for (u32 i = 0; i < n; i++, rows += bpp) {
        u8 any = 0;
        for (u32 b = 0; b < bpp * 3u / 4u; b++) any |= rows[b];
        k += any != 0;
    }
    return k;
}

static u8* eng_coherence_buffer(u32 bytes)
{
    if (s_coherence_cap < bytes) {
        u8* n = (u8*)realloc(s_coherence_rows, bytes);
        if (!n) return NULL;
        s_coherence_rows = n;
        s_coherence_cap = bytes;
    }
    return s_coherence_rows;
}

/* One row of 32-bit pixels between the backend's R,G,B,A and the guest's
 * A8R8G8B8 bytes (or straight through with TEX_RGBA). Copied a byte at a
 * time, with the TEX_RGBA test inside the loop, these two conversions were
 * about a quarter of the FIFO thread's time while Twisted Metal booted; at
 * the -O2 the runtime builds with, GCC vectorises neither form. Each pixel is
 * one 32-bit rotate instead: bytes s0..s3 load as s0 | s1<<8 | s2<<16 | s3<<24
 * on a little-endian host (ps3emu/endian.h), so rotating left by 8 gives
 * s3,s0,s1,s2 (to the guest) and right by 8 gives s1,s2,s3,s0 (from it). */
static void eng_row_convert(u8* dst, const u8* src, u32 w, int rgba, int to_guest)
{
    if (rgba) { memcpy(dst, src, (size_t)w * 4u); return; }
    for (u32 c = 0; c < w; c++) {
        u32 v;
        memcpy(&v, src + (size_t)c * 4u, 4);
        v = to_guest ? (v << 8) | (v >> 24) : (v >> 8) | (v << 24);
        memcpy(dst + (size_t)c * 4u, &v, 4);
    }
}

void rsx_draw_engine_guest_read(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    if (!g.ready || !g.be->readback || !pitch || !w || !h) return;
    u32 xb = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &xb, &y);
    if (slot == ENG_INVALID) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u: no surface\n",
                    location, offset, w, h);
        return;
    }
    const eng_surface* s = &g.surfaces[slot];
    const u32 bpp = eng_transfer_bpp(s->fmt);
    /* The transfer counts 32-bit pixels, so w * 4 bytes per row; an FP16
     * surface pixel is two of them. Read back every surface pixel those bytes
     * touch, then store exactly the bytes. */
    u32 bytes = w * 4u;
    if (xb + bytes > s->w * bpp) bytes = s->w * bpp - xb;
    if (y + h > s->h) h = s->h - y;
    const u32 x = xb / bpp, skip = xb % bpp;
    const u32 n = (skip + bytes + bpp - 1u) / bpp;
    u8* rows = eng_coherence_buffer(n * bpp * h);
    if (!rows) return;
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    g.be->readback(g.be->user, s->handle, x, y, n, h, rows, n * bpp);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u <- surface 0x%08X (%u bpp) at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, bpp, x, y,
                eng_nonzero_pixels(rows, n * h, bpp));
    const int rgba = rsx_texture_argb_is_rgba();
    for (u32 r = 0; r < h; r++) {
        u8* dst = (u8*)eng_guest_ptr(NULL, location, offset + r * pitch, bytes);
        if (!dst) return;
        u8* src = rows + (size_t)r * n * bpp;
        if (bpp == 8u) {
            /* R,G,B,A halves, each stored big-endian: the layout
             * rsx_texture_decode reads W16_Z16_Y16_X16_FLOAT from. */
            for (u32 c = 0; c < n * 4u; c++) {
                const u8 t = src[2 * c]; src[2 * c] = src[2 * c + 1]; src[2 * c + 1] = t;
            }
        } else if (!rgba) {
            eng_row_convert(src, src, n, 0, 1);   /* in place: one pixel per load/store */
        }
        if (rsx_tiling_active()) rsx_tiled_write(&s_eng_mem, location, offset + r * pitch, src + skip, bytes);
        else                     memcpy(dst, src + skip, bytes);
    }
}

void rsx_draw_engine_guest_wrote(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    if (!g.ready || !g.be->color_target_write || !pitch || !w || !h) return;
    u32 x = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &x, &y);
    /* color_target_write takes R,G,B,A bytes: an A8R8G8B8 surface only. */
    if (slot == ENG_INVALID || g.surfaces[slot].fmt != RSX_BE_FMT_R8G8B8A8) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u: no A8R8G8B8 surface\n",
                    location, offset, w, h);
        return;
    }
    x /= 4u;
    const eng_surface* s = &g.surfaces[slot];
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    u8* rows = eng_coherence_buffer(w * 4u * h);
    if (!rows) return;
    const int rgba = rsx_texture_argb_is_rgba();
    for (u32 r = 0; r < h; r++) {
        const u8* src = eng_guest_linear(location, offset + r * pitch, w * 4u);
        if (!src) return;
        eng_row_convert(rows + (size_t)r * w * 4u, src, w, rgba, 0);
    }
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u -> surface 0x%08X at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, x, y,
                eng_nonzero_pixels(rows, w * h, 4u));
    g.be->color_target_write(g.be->user, s->handle, x, y, w, h, rows, w * 4u);
}

static u32 eng_current_surface(void)
{
    u32 slots[RSX_BE_MAX_COLOR_TARGETS];
    return eng_current_target_set(slots) ? slots[0] : ENG_INVALID;
}

/* ---- per-zeta depth ------------------------------------------------------ */

/* One depth target per guest zeta address. Sharing a single resource across a
 * title's shadow, scene and post passes cross-contaminates later depth tests;
 * upstream's ef3271b records that as "the black player-character mass". */
static void eng_zdepth_extend(eng_zdepth* z, u32 rt_w, u32 rt_h)
{
    if (!rt_w) rt_w = g.width;
    if (!rt_h) rt_h = g.height;
    if (rt_w > z->ext_w) z->ext_w = rt_w < z->w ? rt_w : z->w;
    if (rt_h > z->ext_h) z->ext_h = rt_h < z->h ? rt_h : z->h;
}

static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* The attachment must cover the whole canvas: an early pass declaring a
     * smaller clip than the live viewport otherwise gets a target the host
     * API rejects when a later, larger pass binds it. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM)
        return ENG_INVALID;

    u32 slot = ENG_MAX_ZDEPTHS;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) {
                ENG_TOUCH(z);
                eng_zdepth_extend(z, rt_w, rt_h);
                return i;
            }
            slot = i;
            if (want_w < z->w) want_w = z->w;
            if (want_h < z->h) want_h = z->h;
            break;
        }
    }
    int evicted = 0;
    if (slot == ENG_MAX_ZDEPTHS) {
        if (g.n_zdepths < ENG_MAX_ZDEPTHS) {
            slot = g.n_zdepths;
        } else {
            slot = eng_oldest_slot("zeta", &g.zdepths[0].last_use, &g.zdepths[0].last_stamp,
                                   sizeof(eng_zdepth), g.n_zdepths, ENG_INVALID);
            if (slot == ENG_INVALID) return ENG_INVALID;
            evicted = 1;
        }
    }

    const u32 handle = g.be->depth_target_create(g.be->user, want_w, want_h);
    if (!handle)
        return (!evicted && slot < g.n_zdepths && g.zdepths[slot].handle) ? slot : ENG_INVALID;

    eng_zdepth* z = &g.zdepths[slot];
    if (evicted) {
        static u32 logs = 0;
        if (PS3_LOG_FIRST(logs, 4))
            fprintf(stderr, "[rsx engine] zeta table full: %u:0x%08X takes the slot of"
                            " 0x%08X, the least recently used\n", location, offset, z->offset);
    }
    /* The old size's snapshot goes with it: it can never be valid again. */
    if (z->handle) g.be->depth_target_release(g.be->user, z->handle);
    if (z->conv) g.be->texture_release(g.be->user, z->conv);
    if (z->snapshot) g.be->texture_release(g.be->user, z->snapshot);
    if (z->dbz_copy && g.be->texture_release) g.be->texture_release(g.be->user, z->dbz_copy);
    z->dbz_copy = 0; z->dbz_w = z->dbz_h = 0; z->dbz_serial = 0;
    z->conv = 0;
    z->location = location; z->offset = offset;
    z->w = want_w; z->h = want_h;
    z->handle = handle;
    z->snapshot = 0;
    z->cleared = 0;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->snapshot_rgba = 0;          /* released with the old depth target */
    z->snapshot_rgba_valid = 0;
    z->depth_serial = 0;
    z->guest_dirty = 0;
    ENG_TOUCH(z);
    z->ext_w = z->ext_h = 0;
    eng_zdepth_extend(z, rt_w, rt_h);
    if (slot == g.n_zdepths) g.n_zdepths++;
    return slot;
}

/* A depth target read back as a texture, but only once the pass has executed
 * a depth-WRITING draw: a write-enable bit alone does not prove the pass
 * produced a usable depth map, and a clear-only zeta falls through to guest
 * memory instead (rsx_live_draw.c:6016-6018, 6265-6269). */
static u32 eng_zdepth_snapshot(u32 slot, u32 tex_w, u32 tex_h)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    /* The texture covers the top-left tex_w x tex_h of the zeta. The host
     * depth target is at least canvas-sized (eng_zdepth_get), so a 128x160
     * shadow map lives in the corner of a 1280x720 image; resolving all of it
     * stretched the texture's normalized coordinates over the whole canvas
     * and the lookups landed outside the map. The snapshot is the region the
     * texture names, copied 1:1 from the top-left (as the Vulkan backend's
     * copy already does). */
    const u32 w = (tex_w && tex_w < z->w) ? tex_w : z->w;
    const u32 h = (tex_h && tex_h < z->h) ? tex_h : z->h;
    if (z->snapshot_valid && z->snapshot && z->snap_w == w && z->snap_h == h)
        return z->snapshot;
    if (!g.be->depth_snapshot) return 0;
    const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, w, h);
    if (!tex) return 0;
    /* The stale copy goes. A depth clear invalidates the snapshot and the
     * next read takes a new one, so a shadow map cleared and read every frame
     * used to leak one backend texture a frame per depth target, until the
     * backend's object table filled and new textures came back as 0. The
     * Metal backend keeps a released object until no recorded draw can still
     * name it; the Vulkan one hands back the same handle, refilled. */
    if (z->snapshot && z->snapshot != tex && g.be->texture_release)
        g.be->texture_release(g.be->user, z->snapshot);
    z->snapshot = tex;
    z->snap_w = w;
    z->snap_h = h;
    z->snapshot_valid = 1;
    return tex;
}

/* PS3RECOMP_ENG_DEPTH_VIA_GUEST=1 (read at engine init): a depth target read
 * as A8R8G8B8 or DEPTH16 goes through guest memory even when the backend can
 * convert it itself (eng_zdepth_as_color). */
static int s_depth_via_guest;

/* A Z24S8 depth target read as a linear A8R8G8B8 texture of its own pitch:
 * a deferred renderer rebuilding depth from the bytes. Storing it into guest
 * memory for that (eng_zdepth_to_guest) waits for the GPU, converts on the CPU
 * and decodes and uploads the texture again, every time the depth changed:
 * at several reads a frame that is most of the FIFO thread. The backend
 * converts it on the GPU instead, in command order, into the texels the
 * decode would give, and the unit samples a view of that with its own
 * crossbar. Guest memory is left as it was (guest_dirty stays set for any
 * other reader). That holds for the zeta still bound too: a title may read
 * its depth with depth writes off and the zeta left in place, and the
 * conversion, in command order, takes the depth as the draws so far left it,
 * which is what the store reads back.
 * A Z16 depth target read as a linear DEPTH16 texture of its own pitch, a
 * shadow map, takes the same way: without it every read of a freshly
 * rendered shadow map is a wait for the GPU plus a decode and an upload of
 * up to 2 MB. Returns the view, or 0 for the store through guest memory. */
static u32 eng_zdepth_as_color(u32 slot, const rsx_dsp_texture* t)
{
    eng_zdepth* z = &g.zdepths[slot];
    const u32 base_fmt = t->format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
    const int z16 = z->zfmt == 1u;      /* SURFACE_FORMAT Z16 */
    const int bytes = base_fmt == RSX_TEX_FMT_A8R8G8B8 && !z16 && !rsx_texture_argb_is_rgba();
    const int d16 = base_fmt == RSX_TEX_FMT_DEPTH16 && z16;
    if (s_depth_via_guest || !g.be->depth_as_color || !g.be->surface_view ||
        !z->handle || !(bytes || d16) || !(t->format & RSX_TEX_FMT_LINEAR) ||
        t->cubemap || t->dimension == 3 || t->mipmaps > 1)
        return 0;
    const u32 used_w = z->used_w < z->w ? z->used_w : z->w;
    const u32 used_h = z->used_h < z->h ? z->used_h : z->h;
    if (!t->width || !t->height || t->width > used_w || t->height > used_h ||
        t->pitch != z->pitch)
        return 0;
    if (!z->conv || z->conv_serial != z->depth_serial || z->conv_fmt != base_fmt ||
        z->conv_w != t->width || z->conv_h != t->height) {
        const u32 tex = g.be->depth_as_color(g.be->user, z->handle, t->width, t->height,
                                             base_fmt);
        if (!tex) return 0;
        if (z->conv) g.be->texture_release(g.be->user, z->conv);
        z->conv = tex;
        z->conv_w = t->width; z->conv_h = t->height; z->conv_fmt = base_fmt;
        z->conv_serial = z->depth_serial;
    }
    return g.be->surface_view(g.be->user, z->conv, t->remap & 0xFFFFu, t->format);
}

/* ---- textures ------------------------------------------------------------ */

static rsx_be_format eng_texfmt(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:              return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8:            return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:        return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_BC1:             return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:             return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3:             return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:             return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16:          return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:         return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F:   return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:            return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F:   return RSX_BE_FMT_R32G32B32A32F;
    default:                         return RSX_BE_FMT_R8;
    }
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the content hash has to cover, or a title
 * animating one face or one lower level keeps its stale upload. */
static u32 eng_texture_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* A 3D texture: each level is `depth` slices (halving with the level), one
 * face_bytes each -- consecutive for a linear image, one Morton block for a
 * swizzled one. */
static u32 eng_texture_span_3d(u32 fmt, u32 w, u32 h, u32 depth, u32 levels, u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    u32 total = 0, d = depth ? depth : 1u;
    for (u32 m = 0; m < n; m++) { total += lv[m].tl.face_bytes * d; if (d > 1) d >>= 1; }
    return total;
}

/* Slice z of a swizzled 3D level, de-interleaved into row-major rows of
 * w * bpp bytes, so the 2D decode can convert it as a linear image. */
static void eng_deswizzle_3d_slice(u8* dst, const u8* src, u32 w, u32 h, u32 d, u32 z, u32 bpp)
{
    u32 lw = 0, lh = 0, ld = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    while ((1u << ld) < d) ld++;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            memcpy(dst + ((size_t)y * w + x) * bpp,
                   src + (size_t)rsx_swizzle_offset_3d(x, y, z, lw, lh, ld) * bpp, bpp);
}

/* Slice z of a block-compressed 3D level in NVIDIA's VTC layout
 * (https://registry.khronos.org/OpenGL/extensions/NV/NV_texture_compression_vtc.txt),
 * into dst in plain S3TC order. Slices go in groups of four, and each 4x4x4
 * block is four consecutive 4x4 blocks, one per layer; the d % 4 slices left
 * over form a last group of 4x4x1, 4x4x2 or 4x4x3 blocks. So block i of slice
 * z is at (4 * floor(z / 4) * blocks + i * n + z % 4) * block_bytes, n the
 * layer count of z's group. For full groups that is the spec's blockIndex; for
 * the last group the spec's formula stores the slices one after another, which
 * contradicts its prose for d % 4 >= 2, and the prose is followed. `blocks` is
 * the blocks in one slice. Only a volume whose width and height are powers of
 * two is stored this way; any other is slice after slice. */
static void eng_unvtc_3d_slice(u8* dst, const u8* src, u32 blocks, u32 d, u32 z, u32 block_bytes)
{
    const u32 group = z & ~3u;
    const u32 n = group + 4 <= d ? 4 : d - group;
    const u8* base = src + (size_t)group * blocks * block_bytes;
    for (u32 i = 0; i < blocks; i++)
        memcpy(dst + (size_t)i * block_bytes,
               base + ((size_t)i * n + (z & 3)) * block_bytes, block_bytes);
}

/* One hash per cached texture per presented frame. Word-at-a-time FNV over
 * the whole span is deliberately cheap: this is a mutation detector, not a
 * content id (rsx_live_draw.c:2002-2026). */
static u64 eng_texture_content_hash(u32 location, u32 offset, u32 span,
                                    int* readable)
{
    /* in tiled main memory, every byte the texture's texels are stored in */
    if (rsx_tiling_active()) rsx_tiling_footprint(location, offset, span, &offset, &span);
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= span; i += 8) {
        u64 word;
        memcpy(&word, src + i, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ull;
    }
    for (; i < span; i++) { hash ^= src[i]; hash *= 1099511628211ull; }
    *readable = 1;
    return hash;
}

/* The cheap per-frame change check: 64 bytes out of every 2 KB, plus the
 * last 64. A rewrite of the whole texture (a video frame, a UI redraw, a
 * streamed replacement) changes it at once; a small write between the
 * sampled blocks is left to the full hash, which every texture still gets
 * every ENG_TEXHASH_FULL_EVERY frames. */
#define ENG_TEXHASH_STRIDE      2048u
#define ENG_TEXHASH_FULL_EVERY  8u          /* a power of two */
#define ENG_TEXHASH_SMALL       16384u      /* at or below: full every frame */
static u64 eng_texture_sparse_hash(u32 location, u32 offset, u32 span,
                                   int* readable)
{
    if (rsx_tiling_active()) rsx_tiling_footprint(location, offset, span, &offset, &span);
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i + 64 <= span; i += ENG_TEXHASH_STRIDE)
        for (u32 k = 0; k < 64; k += 8) {
            u64 word;
            memcpy(&word, src + i + k, sizeof(word));
            hash = (hash ^ word) * 1099511628211ull;
        }
    for (u32 i = span > 64 ? span - 64 : 0; i < span; i++)
        hash = (hash ^ src[i]) * 1099511628211ull;
    *readable = 1;
    return hash;
}

/* Whether a cached texture gets the full hash at this use: always when it
 * is small or under PS3RECOMP_ENG_TEXHASH_FULL=1 (the old behaviour, read
 * at engine init), otherwise once ENG_TEXHASH_FULL_EVERY frames have passed
 * since its last one. New entries start staggered by slot, so the full
 * hashes of a scene's textures spread over the frames instead of landing on
 * the same one. */
static int s_texhash_full_always;
static int eng_texture_full_rehash(u32 span, u32 last_full_frame)
{
    return s_texhash_full_always || span <= ENG_TEXHASH_SMALL ||
           (u32)(g.frames - last_full_frame) >= ENG_TEXHASH_FULL_EVERY;
}

static int eng_staging_reserve(u32 bytes)
{
    if (g.tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(g.tex_staging, bytes);
    if (!n) return 0;
    g.tex_staging = n;
    g.tex_staging_cap = bytes;
    return 1;
}

/* ---- texture packs (PS3_MODS) ---------------------------------------------
 * A pack is a folder of PNGs, each named by the key of the texture it
 * replaces: 16 hex digits, then optionally anything that is not a hex digit
 * (`3f09a1c2b4d5e6f7.png`, `3f09a1c2b4d5e6f7_logo.png`), anywhere below the
 * folder.
 *
 *   PS3_MODS=<pack>,<pack>,..  the packs in use, first wins when two replace
 *                              the same texture; each a folder name inside
 *                              PS3_MODS_DIR, or an absolute path. Unset or
 *                              empty: no packs (nothing below runs).
 *   PS3_MODS_DIR=<dir>         where packs live (a launcher: <data>/mods).
 *   PS3_MODS_DUMP=<dir>        writes every colour texture the game uploads,
 *                              once each, as <dir>/<key>.png, the name a pack
 *                              uses, and a line in <dir>/textures.txt (key,
 *                              size, format, guest address). Dump, edit,
 *                              drop the file into a pack.
 *
 * The key is the texture's content, not where it sits: the engine's content
 * hash (eng_texture_content_hash, over every level of the texture as the
 * guest stored it) mixed with its SET_TEXTURE_FORMAT base and size, so a
 * texture streamed to another address keeps its key, and a texture the game
 * rewrites gets a new one. Changing that hash renames every pack file.
 *
 * Colour textures only: those decoded to RGBA8 (A8R8G8B8, D8R8G8B8, the
 * packed 16-bit formats) or block-compressed (DXT1/3/5). A replacement is
 * uploaded as RGBA8 at its own size (larger is fine: a higher-resolution
 * pack), with a box-filtered mip chain when the original has mips, and keeps
 * the original's sampler state and channel crossbar: the PNG holds the
 * channels as the texture stores them, so one the game reads through a
 * remap looks remapped in the dump too, and the replacement is read the same
 * way. A texture the game samples with unnormalised coordinates (format bit
 * 0x40) must keep its size. Cube maps and volumes are not replaced. */
typedef struct { u64 key; u32 path; u16 pack; u8 logged, bad; } eng_mod_entry;
static struct {
    int init, on;
    eng_mod_entry* e; u32 n, cap;
    u32* idx; u32 idx_size;              /* open addressing, entry index + 1 */
    char* pool; u32 pool_used, pool_cap; /* file paths */
    char pack[16][160]; u32 npacks;
    char dump_dir[512];
    u64* dumped; u32 dumped_n, dumped_size;
    u32 applied;
} s_mods;

static u64 eng_texmod_key(u64 content, u32 fmt, u32 w, u32 h)
{
    u64 k = content ^ (((((u64)(fmt & 0x9Fu)) << 48) | ((u64)w << 24) | (u64)h) * 0x9E3779B97F4A7C15ull);
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull; k ^= k >> 33;
    return k ? k : 1u;
}

static int eng_hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The key a pack file's name gives, or 0. */
static u64 eng_mod_name_key(const char* name)
{
    u64 k = 0;
    for (int i = 0; i < 16; i++) {
        const int v = eng_hexval((unsigned char)name[i]);
        if (v < 0) return 0;
        k = (k << 4) | (u64)v;
    }
    if (eng_hexval((unsigned char)name[16]) >= 0) return 0;
    const size_t n = strlen(name);
    if (n < 20 || name[n - 4] != '.' || (name[n - 3] | 32) != 'p' || (name[n - 2] | 32) != 'n' ||
        (name[n - 1] | 32) != 'g') return 0;
    return k ? k : 1u;
}

static const eng_mod_entry* eng_mod_find(u64 key)
{
    if (!s_mods.idx_size) return NULL;
    for (u32 p = (u32)(key ^ (key >> 32)) & (s_mods.idx_size - 1u); s_mods.idx[p];
         p = (p + 1) & (s_mods.idx_size - 1u))
        if (s_mods.e[s_mods.idx[p] - 1].key == key) return &s_mods.e[s_mods.idx[p] - 1];
    return NULL;
}

static void eng_mod_index_rebuild(void)
{
    u32 size = 64;
    while (size < s_mods.n * 2u + 2u) size <<= 1;
    u32* idx = (u32*)calloc(size, sizeof *idx);
    if (!idx) return;
    free(s_mods.idx);
    s_mods.idx = idx; s_mods.idx_size = size;
    for (u32 i = 0; i < s_mods.n; i++) {
        u32 p = (u32)(s_mods.e[i].key ^ (s_mods.e[i].key >> 32)) & (size - 1u);
        while (idx[p]) p = (p + 1) & (size - 1u);
        idx[p] = i + 1;
    }
}

/* One file of pack `pack`: kept unless an earlier pack (or an earlier file of
 * this one) already replaces that texture. */
static int eng_mod_add(u64 key, const char* path, u32 pack)
{
    if (eng_mod_find(key)) return 0;
    const u32 len = (u32)strlen(path) + 1u;
    if (s_mods.pool_used + len > s_mods.pool_cap) {
        u32 cap = s_mods.pool_cap ? s_mods.pool_cap * 2u : 65536u;
        while (cap < s_mods.pool_used + len) cap *= 2u;
        char* np = (char*)realloc(s_mods.pool, cap);
        if (!np) return 0;
        s_mods.pool = np; s_mods.pool_cap = cap;
    }
    if (s_mods.n == s_mods.cap) {
        const u32 cap = s_mods.cap ? s_mods.cap * 2u : 256u;
        eng_mod_entry* ne = (eng_mod_entry*)realloc(s_mods.e, cap * sizeof *ne);
        if (!ne) return 0;
        s_mods.e = ne; s_mods.cap = cap;
    }
    memcpy(s_mods.pool + s_mods.pool_used, path, len);
    eng_mod_entry* m = &s_mods.e[s_mods.n++];
    memset(m, 0, sizeof *m);
    m->key = key; m->path = s_mods.pool_used; m->pack = (u16)pack;
    s_mods.pool_used += len;
    if (s_mods.n * 2u + 2u > s_mods.idx_size) eng_mod_index_rebuild();
    else {
        u32 p = (u32)(key ^ (key >> 32)) & (s_mods.idx_size - 1u);
        while (s_mods.idx[p]) p = (p + 1) & (s_mods.idx_size - 1u);
        s_mods.idx[p] = s_mods.n;
    }
    return 1;
}

static void eng_mod_scan(const char* dir, u32 pack, int depth, u32* files, u32* shadowed)
{
#if !defined(_WIN32)
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6) eng_mod_scan(path, pack, depth + 1, files, shadowed);
            continue;
        }
        const u64 key = eng_mod_name_key(de->d_name);
        if (!key) continue;
        if (eng_mod_add(key, path, pack)) (*files)++; else (*shadowed)++;
    }
    closedir(d);
#else
    (void)dir; (void)pack; (void)depth; (void)files; (void)shadowed;
#endif
}

#if defined(_WIN32)
#include <direct.h>
#define ENG_MKDIR(d) _mkdir(d)
#else
#define ENG_MKDIR(d) mkdir(d, 0755)
#endif
static void eng_mkdirs(const char* dir)
{
    char b[512];
    snprintf(b, sizeof b, "%s", dir);
    for (char* q = b + 1; *q; q++) if (*q == '/') { *q = 0; ENG_MKDIR(b); *q = '/'; }
    ENG_MKDIR(b);
}

/* Read the knobs and index the packs, once (on the first upload). */
static void eng_mods_init(void)
{
    if (s_mods.init) return;
    s_mods.init = 1;
    const char* dump = getenv("PS3_MODS_DUMP");
    if (dump && *dump) {
        if (!g.be->image_save) fprintf(stderr, "[mods] PS3_MODS_DUMP: this backend cannot write images; no dump\n");
        else { snprintf(s_mods.dump_dir, sizeof s_mods.dump_dir, "%s", dump); eng_mkdirs(dump);
               fprintf(stderr, "[mods] dumping colour textures to %s (<key>.png, textures.txt)\n", dump); }
    }
    const char* list = getenv("PS3_MODS");
    if (!list || !*list) return;
    if (!g.be->image_load) { fprintf(stderr, "[mods] PS3_MODS: this backend cannot read images; no texture packs\n"); return; }
#if defined(_WIN32)
    fprintf(stderr, "[mods] PS3_MODS: texture packs are not supported on this platform\n");
#else
    const char* dir = getenv("PS3_MODS_DIR");
    const char* q = list;
    while (*q && s_mods.npacks < 16) {
        while (*q == ',' || *q == ' ') q++;
        const char* e = q;
        while (*e && *e != ',') e++;
        const char* t = e;
        while (t > q && t[-1] == ' ') t--;
        if (t > q) {
            char name[160];
            snprintf(name, sizeof name, "%.*s", (int)(t - q), q);
            char path[600];
            if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
            else if (dir && *dir) snprintf(path, sizeof path, "%s/%s", dir, name);
            else { fprintf(stderr, "[mods] pack \"%s\": PS3_MODS_DIR is not set; skipped\n", name); q = e; continue; }
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "[mods] pack \"%s\": no folder %s; skipped\n", name, path);
            } else {
                const u32 id = s_mods.npacks++;
                snprintf(s_mods.pack[id], sizeof s_mods.pack[id], "%s", name);
                u32 files = 0, shadowed = 0;
                eng_mod_scan(path, id, 0, &files, &shadowed);
                fprintf(stderr, "[mods] pack %u \"%s\": %u texture(s)%s", id + 1, name, files,
                        shadowed ? "" : "\n");
                if (shadowed) fprintf(stderr, ", %u left to an earlier pack\n", shadowed);
            }
        }
        q = e;
    }
    s_mods.on = s_mods.n > 0;
#endif
}

static int eng_mods_wanted(void)
{
    if (__builtin_expect(!s_mods.init, 0)) eng_mods_init();
    return s_mods.on || s_mods.dump_dir[0];
}

/* Halve an RGBA8 image (box filter; an odd edge repeats its last texel). */
static void eng_rgba_half(const u8* src, u32 w, u32 h, u8* dst, u32 nw, u32 nh)
{
    for (u32 y = 0; y < nh; y++)
        for (u32 x = 0; x < nw; x++) {
            const u32 x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : x0;
            const u32 y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : y0;
            const u8* a = src + ((size_t)y0 * w + x0) * 4; const u8* b = src + ((size_t)y0 * w + x1) * 4;
            const u8* c = src + ((size_t)y1 * w + x0) * 4; const u8* d = src + ((size_t)y1 * w + x1) * 4;
            u8* o = dst + ((size_t)y * nw + x) * 4;
            for (int k = 0; k < 4; k++) o[k] = (u8)((a[k] + b[k] + c[k] + d[k] + 2) >> 2);
        }
}

/* The pack's replacement for `key` as a backend texture, or 0 (none, or it
 * cannot be used: then the original is uploaded as always). */
static u32 eng_texmod_apply(u64 key, u32 fmt, u32 w, u32 h, u32 nlv, u32 remap)
{
    eng_mod_entry* m = (eng_mod_entry*)eng_mod_find(key);
    if (!m || m->bad) return 0;
    const char* path = s_mods.pool + m->path;
    u8* px = NULL; u32 pw = 0, ph = 0;
    if (!g.be->image_load(g.be->user, path, &px, &pw, &ph) || !px || !pw || !ph) {
        fprintf(stderr, "[mods] %s: cannot read the image; the original is used\n", path);
        free(px); m->bad = 1; return 0;
    }
    if (pw > 8192u || ph > 8192u) {
        fprintf(stderr, "[mods] %s is %ux%u: at most 8192x8192; the original is used\n", path, pw, ph);
        free(px); m->bad = 1; return 0;
    }
    if ((fmt & 0x40u) && (pw != w || ph != h)) {
        fprintf(stderr, "[mods] %s is %ux%u: the game reads this texture in texels, so it must stay %ux%u;"
                        " the original is used\n", path, pw, ph, w, h);
        free(px); m->bad = 1; return 0;
    }
    u32 mips = 1;
    if (nlv > 1) { u32 big = pw > ph ? pw : ph; while (big > 1) { big >>= 1; mips++; } }
    const u32 handle = g.be->texture_create(g.be->user, RSX_BE_FMT_R8G8B8A8, pw, ph, mips, 1, remap, fmt);
    if (!handle) { free(px); return 0; }
    g.be->texture_upload(g.be->user, handle, 0, 0, pw, ph, px, pw * 4u, ph);
    u32 cw = pw, chh = ph;
    for (u32 lvl = 1; lvl < mips; lvl++) {
        const u32 nw = cw > 1 ? cw / 2 : 1, nh = chh > 1 ? chh / 2 : 1;
        u8* half = (u8*)malloc((size_t)nw * nh * 4);
        if (!half) break;
        eng_rgba_half(px, cw, chh, half, nw, nh);
        free(px); px = half; cw = nw; chh = nh;
        g.be->texture_upload(g.be->user, handle, 0, lvl, cw, chh, px, cw * 4u, chh);
    }
    free(px);
    s_mods.applied++;
    if (!m->logged) {
        m->logged = 1;
        fprintf(stderr, "[mods] %016llx %ux%u fmt 0x%02X <- \"%s\" %s (%ux%u)\n", (unsigned long long)key,
                w, h, fmt, s_mods.pack[m->pack], path, pw, ph);
    }
    return handle;
}

/* DXT blocks (BC1/2/3, host byte order as rsx_texture_decode leaves them) to
 * RGBA8, for the dump. */
static void eng_bc_color(const u8* b, u8 out[16][4], int punch)
{
    u8 pal[4][4];
    const u32 c[2] = { (u32)b[0] | ((u32)b[1] << 8), (u32)b[2] | ((u32)b[3] << 8) };
    for (int k = 0; k < 2; k++) {
        pal[k][0] = (u8)(((c[k] >> 11) & 31u) * 255u / 31u);
        pal[k][1] = (u8)(((c[k] >> 5) & 63u) * 255u / 63u);
        pal[k][2] = (u8)((c[k] & 31u) * 255u / 31u);
        pal[k][3] = 255;
    }
    for (int ch = 0; ch < 3; ch++) {
        if (!punch || c[0] > c[1]) {
            pal[2][ch] = (u8)((2 * pal[0][ch] + pal[1][ch] + 1) / 3);
            pal[3][ch] = (u8)((pal[0][ch] + 2 * pal[1][ch] + 1) / 3);
        } else {
            pal[2][ch] = (u8)((pal[0][ch] + pal[1][ch] + 1) / 2);
            pal[3][ch] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (!punch || c[0] > c[1]) ? 255 : 0;
    const u32 idx = (u32)b[4] | ((u32)b[5] << 8) | ((u32)b[6] << 16) | ((u32)b[7] << 24);
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(idx >> (2 * i)) & 3u], 4);
}
static void eng_bc_decode(const u8* blocks, u32 row_bytes, u32 w, u32 h, rsx_texfmt f, u8* rgba)
{
    const u32 bb = f == RSX_TEXFMT_BC1 ? 8u : 16u;
    for (u32 by = 0; by * 4 < h; by++)
        for (u32 bx = 0; bx * 4 < w; bx++) {
            const u8* b = blocks + (size_t)by * row_bytes + (size_t)bx * bb;
            u8 t[16][4];
            eng_bc_color(f == RSX_TEXFMT_BC1 ? b : b + 8, t, f == RSX_TEXFMT_BC1);
            if (f == RSX_TEXFMT_BC2) {
                for (int i = 0; i < 16; i++) t[i][3] = (u8)(((b[i / 2] >> ((i & 1) * 4)) & 15u) * 17u);
            } else if (f == RSX_TEXFMT_BC3) {
                u8 a[8]; a[0] = b[0]; a[1] = b[1];
                if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = (u8)(((7 - k) * a[0] + k * a[1] + 3) / 7);
                else { for (int k = 1; k < 5; k++) a[k + 1] = (u8)(((5 - k) * a[0] + k * a[1] + 2) / 5);
                       a[6] = 0; a[7] = 255; }
                u64 bits = 0;
                for (int k = 0; k < 6; k++) bits |= (u64)b[2 + k] << (8 * k);
                for (int i = 0; i < 16; i++) t[i][3] = a[(bits >> (3 * i)) & 7u];
            }
            for (u32 i = 0; i < 16; i++) {
                const u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(rgba + ((size_t)y * w + x) * 4, t[i], 4);
            }
        }
}

/* PS3_MODS_DUMP: level 0 of the texture with this key, decoded (in
 * g.tex_staging, laid out as `tl` says), as <dir>/<key>.png -- once a key. */
static void eng_texmod_dump(u64 key, u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            const rsx_tex_layout* tl)
{
    if (!s_mods.dump_dir[0] || !key) return;
    if (s_mods.dumped_n * 2u + 2u > s_mods.dumped_size) {
        if (s_mods.dumped_size >= (1u << 20)) return;          /* a million: enough */
        const u32 size = s_mods.dumped_size ? s_mods.dumped_size * 2u : 4096u;
        u64* nd = (u64*)calloc(size, sizeof *nd);
        if (!nd) return;
        for (u32 i = 0; i < s_mods.dumped_size; i++)
            if (s_mods.dumped[i]) {
                u32 p = (u32)(s_mods.dumped[i] ^ (s_mods.dumped[i] >> 32)) & (size - 1u);
                while (nd[p]) p = (p + 1) & (size - 1u);
                nd[p] = s_mods.dumped[i];
            }
        free(s_mods.dumped); s_mods.dumped = nd; s_mods.dumped_size = size;
    }
    u32 p = (u32)(key ^ (key >> 32)) & (s_mods.dumped_size - 1u);
    for (; s_mods.dumped[p]; p = (p + 1) & (s_mods.dumped_size - 1u))
        if (s_mods.dumped[p] == key) return;
    s_mods.dumped[p] = key;
    s_mods.dumped_n++;
    char path[640];
    snprintf(path, sizeof path, "%s/%016llx.png", s_mods.dump_dir, (unsigned long long)key);
    int ok = 0;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        ok = g.be->image_save(g.be->user, path, g.tex_staging, w, h, tl->dst_row_bytes);
    } else {
        u8* rgba = (u8*)calloc((size_t)w * h, 4);
        if (!rgba) return;
        eng_bc_decode(g.tex_staging, tl->dst_row_bytes, w, h, tl->fmt, rgba);
        ok = g.be->image_save(g.be->user, path, rgba, w, h, w * 4u);
        free(rgba);
    }
    if (!ok) return;
    static const char* const fname[] = { "R8", "R8G8", "RGBA8", "DXT1", "DXT3", "DXT5" };
    snprintf(path, sizeof path, "%s/textures.txt", s_mods.dump_dir);
    FILE* ix = fopen(path, "a");
    if (ix) {
        fprintf(ix, "%016llx %4ux%-4u %-5s fmt 0x%02X loc %u off 0x%08X frame %u\n", (unsigned long long)key,
                w, h, (unsigned)tl->fmt < 6u ? fname[tl->fmt] : "?", fmt, location, offset, g.frames);
        fclose(ix);
    }
}

/* Decode a guest texture out of guest memory and hand every face and level to
 * the backend. Returns the backend handle, or 0. `into`, when nonzero, is an
 * existing 2D texture of exactly this key to rewrite instead of creating one
 * (rsx_draw_backend.texture_upload_ordered). */
static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                              u32 levels, u32 pitch, int cube, u32 remap, u32 depth,
                              u32 into)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || !lv[0].tl.face_bytes) return 0;
    /* A volume: every slice of every level decoded and uploaded on its own.
     * inFAMOUS 2's occlusion volumes read an 8x8x8 cone LUT; uploaded as its
     * first slice and sampled in 2D it darkened whole boxes round every prop.
     * A DXT volume too: a retail title's explosion smoke is a 256x256x16 DXT5
     * animation, and its first 64 KB read as one 2D image (four slices'
     * blocks interleaved) drew dark squares where the smoke was. */
    if (!into && depth > 1 && !cube && g.be->texture_create_3d && g.be->texture_upload_3d &&
        (lv[0].tl.compressed || lv[0].tl.bytes_per_texel)) {
        const u32 span = eng_texture_span_3d(fmt, w, h, depth, levels, pitch);
        const u8* src = eng_guest_linear(location, offset, span);
        if (!src) return 0;
        const u32 handle = g.be->texture_create_3d(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                                   w, h, depth, nlv, remap, fmt);
        if (!handle) return 0;
        const u8* level_src = src;
        u32 d = depth;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            const int vtc = tl->compressed && !(lv[m].w & (lv[m].w - 1u)) &&
                            !(lv[m].h & (lv[m].h - 1u));
            const u32 lin_bytes = tl->swizzled ? tl->row_bytes * tl->rows : 0u;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows + lin_bytes)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            for (u32 z = 0; z < d; z++) {
                if (vtc) {
                    /* Block rows go up as they are (rsx_texture_decode only copies them). */
                    eng_unvtc_3d_slice(g.tex_staging, level_src, tl->face_bytes / tl->block_bytes, d, z,
                                       tl->block_bytes);
                } else if (tl->swizzled) {
                    u8* lin = g.tex_staging + tl->dst_row_bytes * tl->rows;
                    eng_deswizzle_3d_slice(lin, level_src, lv[m].w, lv[m].h, d, z, tl->bytes_per_texel);
                    rsx_tex_layout ltl = *tl; ltl.swizzled = 0;
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes, lin, lv[m].w, lv[m].h,
                                       &ltl, rsx_texture_argb_is_rgba());
                } else {
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                                       level_src + (size_t)z * tl->face_bytes, lv[m].w, lv[m].h,
                                       tl, rsx_texture_argb_is_rgba());
                }
                g.be->texture_upload_3d(g.be->user, handle, z, m, lv[m].w, lv[m].h,
                                        g.tex_staging, tl->dst_row_bytes);
            }
            level_src += (size_t)tl->face_bytes * d;
            if (d > 1) d >>= 1;
        }
        return handle;
    }
    const u32 faces = cube ? 6u : 1u;
    const u32 face_stride = cube
        ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;
    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    const u8* src = eng_guest_linear(location, offset, span);
    if (!src) return 0;

    /* Texture packs and their dump (PS3_MODS, PS3_MODS_DUMP), by content. An
     * in-place refresh (`into`) is never replaced: the cache does not refresh
     * in place while packs are on (eng_texture_slot). */
    u64 mod_key = 0;
    if (!cube && eng_mods_wanted()) {
        const rsx_texfmt tf = lv[0].tl.fmt;
        if (tf == RSX_TEXFMT_R8G8B8A8 || tf == RSX_TEXFMT_BC1 || tf == RSX_TEXFMT_BC2 || tf == RSX_TEXFMT_BC3) {
            int readable = 0;
            const u64 content = eng_texture_content_hash(location, offset, span, &readable);
            if (readable) mod_key = eng_texmod_key(content, fmt, w, h);
        }
        if (mod_key && s_mods.on && !into) {
            const u32 hnd = eng_texmod_apply(mod_key, fmt, w, h, nlv, remap);
            if (hnd) {
                if (s_mods.dump_dir[0] && eng_staging_reserve(lv[0].tl.dst_row_bytes * lv[0].tl.rows)) {
                    rsx_texture_decode(g.tex_staging, lv[0].tl.dst_row_bytes, src + lv[0].offset,
                                       lv[0].w, lv[0].h, &lv[0].tl, rsx_texture_argb_is_rgba());
                    eng_texmod_dump(mod_key, location, offset, fmt, w, h, &lv[0].tl);
                }
                return hnd;
            }
        }
    }

    const u32 handle = into ? into
        : g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt), w, h, nlv, faces, remap, fmt);
    if (!handle) return 0;

    for (u32 f = 0; f < faces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows)) {
                if (!into) g.be->texture_release(g.be->user, handle);
                return 0;
            }
            rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            if (mod_key && f == 0 && m == 0) eng_texmod_dump(mod_key, location, offset, fmt, w, h, tl);
            g.be->texture_upload(g.be->user, handle, f, m, lv[m].w, lv[m].h,
                                 g.tex_staging, tl->dst_row_bytes, tl->rows);
        }
    }
    return handle;
}

/* The cache slot for a texture unit's bytes, keyed on where they are and what
 * the registers say they are. An already-cached entry is re-hashed at most
 * once per presented frame and re-decoded when the guest changed it, which is
 * what makes an animated UI or a video texture update at all. A miss with the
 * cache full evicts the least recently used entry rather than returning
 * nothing: returning white "made the recovered orphanage render as flat
 * green/black geometry" (rsx_live_draw.c:2326-2334). */
/* Texture cache traffic, printed with the vertex cache stats
 * (PS3RECOMP_ENG_VCACHE_STATS): entries created, of which evicted another,
 * and re-uploads because the guest bytes changed. */
static u64 s_tex_new, s_tex_evicted, s_tex_changed, s_tex_new_bytes, s_tex_changed_bytes;
static int s_vc_stats_on;   /* defined with the vertex cache below */

static u32 eng_texture_slot(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    if (!w || !h || w > ENG_MAX_TEX_DIM || h > ENG_MAX_TEX_DIM) return 0;
    if (!depth) depth = 1;
    const u32 span = depth > 1 ? eng_texture_span_3d(fmt, w, h, depth, levels, pitch)
                               : eng_texture_span(fmt, w, h, levels, pitch, cube);
    if (!span) return 0;

    /* A scene binds the same few hundred textures every frame, and a linear
     * scan of the whole cache per bind averaged several hundred entries.
     * Remember where each key was last found; a remembered slot is used only
     * if its entry still matches, otherwise the scan runs as before. Keys are
     * unique in the cache, so the result is the same either way. */
    static u16 s_slot_memo[4096];
    const u32 memo = (offset >> 4 ^ offset >> 16 ^ fmt * 0x9E37u ^ w * 31u ^ h ^
                      location << 11) & 4095u;
#define ENG_TEXTURE_KEY_MATCHES(e) ((e)->location == location && (e)->offset == offset && \
        (e)->format == fmt && (e)->width == w && (e)->height == h && \
        (e)->pitch == pitch && (e)->remap == remap && \
        (e)->cubemap == (u32)(cube != 0) && (e)->depth == depth)
    eng_texture* hit = NULL;
    if (s_slot_memo[memo] < g.n_textures && ENG_TEXTURE_KEY_MATCHES(&g.textures[s_slot_memo[memo]])) {
        hit = &g.textures[s_slot_memo[memo]];
    } else {
        for (u32 i = 0; i < g.n_textures; i++)
            if (ENG_TEXTURE_KEY_MATCHES(&g.textures[i])) {
                hit = &g.textures[i];
                s_slot_memo[memo] = (u16)i;
                break;
            }
    }
#undef ENG_TEXTURE_KEY_MATCHES
    if (hit) {
        eng_texture* e = hit;
        if (e->handle && e->last_hash_frame != g.frames) {
            /* Every cached texture used to be hashed in full every frame;
             * in a big scene that is hundreds of MB a second for the few
             * that change. Now the sparse check every frame, and the full
             * hash when it differs, when this entry's turn has come, for a
             * small texture, or after a write the engine saw. */
            int readable = 0;
            u64 hash = e->content_hash;
            const u64 sparse = eng_texture_sparse_hash(location, offset, span,
                                                       &readable);
            /* The engine just stored a surface or depth target over these
             * bytes: they are new, so upload without hashing them all. The
             * stored hash becomes unknown and the next ordinary check
             * re-uploads once rather than miss a change. */
            if (e->rewritten && readable) {
                hash = ~e->content_hash;
                e->rewritten = 0;
            } else if (readable && (sparse != e->sparse_hash ||
                             eng_texture_full_rehash(span, e->last_full_frame))) {
                hash = eng_texture_content_hash(location, offset, span, &readable);
                e->last_full_frame = g.frames;
            }
            if (readable) e->sparse_hash = sparse;
            e->last_hash_frame = g.frames;
            if (readable && hash != e->content_hash) {
                /* In place when the backend orders the upload after the draws
                 * that sampled the old bytes; a volume keeps create-and-swap,
                 * and so does a different mip count (the key does not hold it,
                 * and the levels to upload would not all exist). */
                const u32 into = (g.be->texture_upload_ordered && depth <= 1 &&
                                  e->levels == levels && !s_mods.on) ? e->handle : 0u;
                const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                     levels, pitch, cube, remap, depth, into);
                if (fresh && !into) {
                    g.be->texture_release(g.be->user, e->handle);
                    e->handle = fresh;
                    e->levels = levels;
                }
                if (fresh) {
                    s_tex_changed++; s_tex_changed_bytes += span;
                    if (s_vc_stats_on > 0) {
                        static int n_logged;
                        if (s_tex_changed % 150u == 1u && n_logged++ < 60)
                            fprintf(stderr, "[rsx engine] texture changed: %u:0x%08X fmt %02X %ux%u"
                                            " (%u bytes) frame %u\n", location, offset,
                                    fmt & 0xFFu, w, h, span, g.frames);
                    }
                }
                if (fresh) e->content_hash = hash;
            }
        }
        e->last_use_serial = ++g.texture_use_serial;
        return e->handle;
    }

    u32 index;
    u32 evicted = 0;
    if (g.n_textures < ENG_MAX_TEXTURES) {
        index = g.n_textures++;
    } else {
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial < g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].handle;
        s_tex_evicted++;
    }
    s_tex_new++; s_tex_new_bytes += span;

    eng_texture e;
    memset(&e, 0, sizeof(e));
    e.location = location; e.offset = offset; e.format = fmt;
    e.width = w; e.height = h; e.pitch = pitch;
    e.remap = remap; e.cubemap = (u32)(cube != 0); e.depth = depth; e.levels = levels;
    e.last_hash_frame = g.frames;
    e.last_full_frame = g.frames - (index & (ENG_TEXHASH_FULL_EVERY - 1u));
    e.last_use_serial = ++g.texture_use_serial;
    { int readable = 0;
      e.content_hash = eng_texture_content_hash(location, offset, span, &readable);
      e.sparse_hash = eng_texture_sparse_hash(location, offset, span, &readable); }
    e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                  cube, remap, depth, 0u);
    if (e.handle && evicted) g.be->texture_release(g.be->user, evicted);
    if (e.handle || !evicted) g.textures[index] = e;
    return e.handle;
}

/* ---- surfaces read through guest memory --------------------------------- */

/* A texture may name a surface's bytes in a format the surface was not
 * rendered in: a deferred renderer reads its D24S8 depth buffer as A8R8G8B8
 * to rebuild positions, samples its Z16 shadow map as DEPTH16, and reads back
 * as X32_FLOAT a float it packed into an RGBA8 target. On RSX that is only
 * memory. Here the surface is stored into guest memory in its own layout --
 * the bytes RSX would have left there -- and the texture is then decoded from
 * guest memory like any other, so every format the decoder knows reads them
 * the way RSX would. A surface is stored at most once per write to it. */

/* Re-hash, on their next use rather than next frame, the cached textures that
 * start inside [offset, offset + bytes): their bytes changed within this one. */
static void eng_textures_touch(u32 location, u32 offset, u32 bytes)
{
    for (u32 i = 0; i < g.n_textures; i++) {
        eng_texture* e = &g.textures[i];
        if (e->location == location && e->offset >= offset && e->offset - offset < bytes) {
            e->last_hash_frame = g.frames - 1u;
            e->last_full_frame = g.frames - ENG_TEXHASH_FULL_EVERY;   /* hash it all */
            e->rewritten = 1;
        }
    }
}

/* Formats that read an RGBA8 surface's 32-bit pixels as something else: the
 * host image cannot be sampled as them, the guest bytes can. */
static int eng_format_reinterprets_rgba8(u32 base_fmt)
{
    switch (base_fmt) {
    case RSX_TEX_FMT_DEPTH24_D8:
    case 0x91u:     /* DEPTH24_D8_FLOAT */
    case 0x92u:     /* DEPTH16          */
    case 0x93u:     /* DEPTH16_FLOAT    */
    case 0x95u:     /* Y16_X16          */
    case 0x9Cu:     /* X32_FLOAT        */
    case 0x9Fu:     /* Y16_X16_FLOAT    */
        return 1;
    default:
        return 0;
    }
}

/* Store a colour surface into guest memory, A,R,G,B per pixel at its own
 * pitch. Returns 0 when it cannot, and the caller samples as before. */
static int eng_surface_to_guest(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    if (!s->guest_dirty) return 1;
    if (s->fmt != RSX_BE_FMT_R8G8B8A8 || s->pitch < s->w * 4u || !g.be->readback)
        return 0;
    rsx_draw_engine_guest_read(s->location, s->offset, s->pitch, s->w, s->h);
    s->guest_dirty = 0;
    eng_textures_touch(s->location, s->offset, s->pitch * s->h);
    return 1;
}

static float* s_depth_rows;
static u32 s_depth_cap;

/* One row of depth values, clamped to [0, 1] (NaN to 0), quantized and stored
 * big-endian: Z16 as 16 bits, Z24S8 as 24 bits of depth and a zero stencil
 * byte. The format test is hoisted out of the pixel loops and the clamps are
 * selects, so both loops vectorize; a large depth target can be read back
 * several times a frame. */
static void eng_depth_row_to_guest(u8* dst, const float* src, u32 w, u32 bpp)
{
    if (bpp == 2u) {
        for (u32 c = 0; c < w; c++) {
            float d = src[c];
            d = d > 0.0f ? d : 0.0f;
            d = d < 1.0f ? d : 1.0f;
            /* Exactly round(d * 0xFFFF), as depth_as_color takes it: in
             * float the product rounds first and a value within 0.002 of a
             * half could go the other way. */
            const u32 v = (u32)((double)d * 65535.0 + 0.5);
            dst[2u * c] = (u8)(v >> 8); dst[2u * c + 1u] = (u8)v;
        }
        return;
    }
    for (u32 c = 0; c < w; c++) {
        float d = src[c];
        d = d > 0.0f ? d : 0.0f;
        d = d < 1.0f ? d : 1.0f;
        /* Float32 rounding at this magnitude can turn depth 1 into
         * 0x1000000. Quantize in double before storing the 24-bit value. */
        const u32 v = (u32)((double)d * 16777215.0 + 0.5);
        dst[4u * c] = (u8)(v >> 16); dst[4u * c + 1u] = (u8)(v >> 8);
        dst[4u * c + 2u] = (u8)v; dst[4u * c + 3u] = 0;
    }
}

/* Store a depth target into guest memory as its zeta format lays it out:
 * Z16 as one big-endian 16-bit value, Z24S8 as a big-endian word with the
 * depth in the top 24 bits. The stencil byte is written as 0: the host depth
 * target keeps no stencil to copy. Returns 0 when it cannot. */
static int eng_zdepth_to_guest(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->guest_dirty) return 1;
    if (!g.be->depth_readback || !z->handle) return 0;
    const u32 bpp = (z->zfmt == 1u) ? 2u : 4u;      /* SURFACE_FORMAT Z16 = 1 */
    const u32 w = z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h < z->h ? z->used_h : z->h;
    /* A swizzled zeta has no pitch: its texels are in Morton order over a
     * power-of-two extent, the order a swizzled DEPTH16 or DEPTH24_D8
     * texture reads them back in (rsx_texture_layout.c). Resistance renders
     * its 64x64 distant-character shadow map that way and samples it as a
     * swizzled DEPTH16: stored linearly -- or, before this, refused for its
     * pitch of 64 -- the lookup read a colour target that was never written
     * and every pixel of the projected quad came out in shadow. */
    const int swz = z->swizzled;
    if (!w || !h) return 0;
    if (swz ? ((w & (w - 1u)) || (h & (h - 1u))) : z->pitch < w * bpp) return 0;
    u32 lw = 0, lh = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    u8* const swz_base = swz
        ? (u8*)eng_guest_ptr(NULL, z->location, z->offset, w * h * bpp) : NULL;
    if (swz && !swz_base) return 0;
    if (s_depth_cap < w * h) {
        float* n = (float*)realloc(s_depth_rows, (size_t)w * h * sizeof(float));
        if (!n) return 0;
        s_depth_rows = n;
        s_depth_cap = w * h;
    }
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, s_depth_rows,
                             w * (u32)sizeof(float)) != 0)
        return 0;
    /* Rows of a zeta in tiled main memory are built here and stored through the tiling. */
    const int tiled = !swz && eng_tiled(z->location, z->offset, z->pitch * h);
    static u8* tile_row;
    static u32 tile_row_cap;
    if (tiled && tile_row_cap < w * bpp) {
        u8* n = (u8*)realloc(tile_row, w * bpp);
        if (!n) return 0;
        tile_row = n;
        tile_row_cap = w * bpp;
    }
    for (u32 r = 0; r < h; r++) {
        if (swz) {      /* Morton order: each texel where the swizzled read takes it */
            for (u32 c = 0; c < w; c++)
                eng_depth_row_to_guest(swz_base + (size_t)rsx_swizzle_offset(c, r, lw, lh) * bpp,
                                       s_depth_rows + (size_t)r * w + c, 1, bpp);
            continue;
        }
        u8* const row = tiled ? tile_row
                              : (u8*)eng_guest_ptr(NULL, z->location, z->offset + r * z->pitch, w * bpp);
        if (!row) return 0;
        eng_depth_row_to_guest(row, s_depth_rows + (size_t)r * w, w, bpp);
        if (tiled) rsx_tiled_write(&s_eng_mem, z->location, z->offset + r * z->pitch, row, w * bpp);
    }
    z->guest_dirty = 0;
    eng_textures_touch(z->location, z->offset, swz ? w * h * bpp : z->pitch * h);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] depth 0x%08X (%s, %s %u) %ux%u stored to guest memory\n",
                z->offset, bpp == 2u ? "Z16" : "Z24S8", swz ? "swizzled, pitch" : "pitch",
                z->pitch, w, h);
    return 1;
}

/* A texture unit reading a Z24S8 depth buffer through A8R8G8B8 (fog,
 * depth of field and the like read the depth bytes as colour): the backend
 * builds the bytes eng_zdepth_to_guest would store, on the GPU, and the unit
 * samples them where they are. The guest-memory path costs a full wait for
 * the GPU, a readback, and a re-decode and re-upload of the whole texture
 * each time the depth changes -- in Resistance, a 2560x720 zeta sampled
 * that way every frame, 7.4 MB per frame and most of the RSX thread.
 *
 * Taken only where the result is the same image: A8R8G8B8 linear, one 2D
 * level, a Z24S8 zeta, rows at the zeta's pitch and the texture inside the
 * region the guest-memory copy stores. Anything else, and a backend without
 * depth_snapshot_rgba8, goes through guest memory as before. This path does
 * not refresh the zeta's bytes in guest memory; a CPU or SPU reader of them
 * sees what the last guest-memory store left there.
 * PS3RECOMP_ENG_ZRGBA_GPU=0: always the guest-memory path. */
static int eng_zdepth_rgba8_usable(const eng_zdepth* z, const rsx_dsp_texture* t,
                                   u32 base_fmt)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_ZRGBA_GPU"); on = e ? atoi(e) : 1; }
    if (!on || !g.be->depth_snapshot_rgba8 || base_fmt != 0x85u || !z->handle) return 0;
    const u32 w = z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h < z->h ? z->used_h : z->h;
    const u32 row = t->width * 4u;
    /* rsx_texture_layout_pitched: a linear image's rows are its pitch apart
     * when that is wider than the packed row. */
    const u32 tpitch = t->pitch > row ? t->pitch : row;
    const char* why =
        z->zfmt == 1u                              ? "Z16 zeta" :
        !(t->format & RSX_TEX_FMT_LINEAR)          ? "swizzled" :
        t->dimension != 2 || t->cubemap || t->mipmaps > 1 || t->depth > 1 ? "not one 2D level" :
        !t->width || !t->height || t->width > w || t->height > h ? "outside the stored region" :
        tpitch != z->pitch                         ? "pitch differs from the zeta's" :
        rsx_texture_argb_is_rgba()                 ? "TEX_RGBA" : NULL;
    if (!why) return 1;
    static u32 logs;
    if (PS3_LOG_FIRST(logs, 4))
        fprintf(stderr, "[rsx engine] depth %u:0x%08X read as fmt %02X %ux%u pitch %u through"
                        " guest memory: %s (zeta %ux%u pitch %u)\n", z->location, z->offset,
                t->format, t->width, t->height, t->pitch, why, w, h, z->pitch);
    return 0;
}

/* The colour-format copy of a depth target, kept until the next depth write
 * or clear (eng_zdepth_wrote) like the plain snapshot. */
static u32 eng_zdepth_snapshot_rgba8(u32 slot, u32 w, u32 h)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (z->snapshot_rgba_valid && z->snapshot_rgba && z->rgba_w == w && z->rgba_h == h)
        return z->snapshot_rgba;
    const u32 tex = g.be->depth_snapshot_rgba8(g.be->user, z->handle, w, h);
    if (!tex) return 0;
    z->snapshot_rgba = tex;
    z->rgba_w = w;
    z->rgba_h = h;
    z->snapshot_rgba_valid = 1;
    static u32 logs;
    if (PS3_LOG_FIRST(logs, 2))
        fprintf(stderr, "[rsx engine] depth %u:0x%08X %ux%u read as A8R8G8B8 through a GPU copy\n",
                z->location, z->offset, w, h);
    return tex;
}

/* A depth write or clear through the current zeta, recorded with what the
 * guest-memory copy needs to lay it out. */
static void eng_zdepth_wrote(u32 slot, const rsx_dsp_surface* sf)
{
    eng_zdepth* z = &g.zdepths[slot];
    z->depth_serial = ++g.write_serial;
    z->guest_dirty = 1;
    z->zfmt = sf->depth_format;
    z->pitch = sf->zeta_pitch;
    z->swizzled = sf->raster_type == 2u;
    z->used_w = sf->clip_w ? sf->clip_w : g.width;
    z->used_h = sf->clip_h ? sf->clip_h : g.height;
    z->snapshot_rgba_valid = 0;    /* the colour-format copy follows every write */
}

static void eng_surfaces_wrote(const u32* targets, u32 n_targets)
{
    const u64 serial = ++g.write_serial;
    for (u32 i = 0; i < n_targets; i++) {
        g.surfaces[targets[i]].color_serial = serial;
        g.surfaces[targets[i]].guest_dirty = 1;
    }
}

/* SET_TEXTURE_FILTER's min field is [18:16] (1 NEAREST, 2 LINEAR, then 3..6,
 * the four combinations of a nearest/linear minification with a
 * nearest/linear mip filter) and mag [26:24]; SET_TEXTURE_CONTROL0 carries
 * max LOD at [18:7] and min LOD at [30:19], both 4.8 fixed point. RSX's LOD
 * bias, SET_TEXTURE_FILTER [12:0], is not applied: the reference engine
 * leaves it at zero too. */
/* SET_TEXTURE_BORDER_COLOR as rsx_be_sampler_desc.border. The colour is
 * ARGB; a depth texture compares against its low 24 bits as depth. inFAMOUS
 * 2's sun shadow maps (DEPTH24_D8, wrap BORDER, a depth compare) set
 * 0x00FFFFFF, depth 1.0, so a lookup outside the map compares against the far
 * plane; read as transparent black it compared against 0 instead and flipped
 * the shadow term for everything the map does not cover.
 * PS3RECOMP_ENG_BORDER=0: always transparent black, the old behaviour. */
static u8 eng_border_class(u32 format, u32 border)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_BORDER"); on = e ? atoi(e) : 1; }
    if (!on) return 0u;
    const u32 base = format & 0x9Fu;               /* without the LN/UN bits */
    if (base >= 0x90u && base <= 0x93u)            /* DEPTH24_D8(_FLOAT), DEPTH16(_FLOAT) */
        return (border & 0x00FFFFFFu) == 0x00FFFFFFu ? 2u : 0u;   /* 1.0 -> white, 0 -> black */
    if (border == 0x00000000u) return 0u;
    if (border == 0xFF000000u) return 1u;
    if (border == 0xFFFFFFFFu) return 2u;
    return 3u;                                     /* not expressible: edge clamp */
}

static void eng_decode_sampler(u32 filter, u32 wrap, u32 control0,
                               rsx_be_sampler_desc* out)
{
    const u32 minf = (filter >> 16) & 7u;
    const u32 magf = (filter >> 24) & 7u;
    /* 7 CONVOLUTION_MIN / 4 CONVOLUTION_MAG filter through a kernel: bilinear
     * here, and the fragment program applies the kernel
     * (rsx_fp_apply_convolution). Read as nearest they showed Folklore's
     * low-resolution bloom buffer as a grid of squares. */
    out->min_linear  = (u8)(minf == 2 || minf == 4 || minf == 6 || minf == 7);
    out->mag_linear  = (u8)(magf == 2 || magf == 4);
    out->mip_present = (u8)(minf >= 3 && minf <= 6);
    out->mip_linear  = (u8)(minf == 5 || minf == 6);
    out->wrap_s = (u8)(wrap & 0xFu);
    out->wrap_t = (u8)((wrap >> 8) & 0xFu);
    out->wrap_r = (u8)((wrap >> 16) & 0xFu);
    out->min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    out->max_lod = out->mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f
                                    : 0.0f;
    if (out->max_lod < out->min_lod) out->max_lod = out->min_lod;
}

/* ---- pipelines ----------------------------------------------------------- */

static u32 eng_vtex_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (vt.enabled && vt.width && vt.height) mask |= 1u << u;
    }
    return mask;
}

/* Units addressed in texels (RSX_TEX_FMT_UNNORM), with each one's size in
 * dim. No host sampling mode is unnormalised for these, so the fragment
 * program divides the coordinates instead, and the divisor makes the sizes
 * pipeline identity (rsx_live_draw.c keys them the same way). */
static u32 eng_unnorm_units(u32 dim[RSX_DSP_NUM_TEXTURES][3])
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        dim[u][0] = dim[u][1] = dim[u][2] = 0;
        if (!t.enabled || !(t.format & RSX_TEX_FMT_UNNORM) || !t.width || !t.height)
            continue;
        mask |= 1u << u;
        dim[u][0] = t.width;
        dim[u][1] = t.height;
        dim[u][2] = t.depth;
    }
    return mask;
}

/* Units bound to a 3D texture (SET_TEXTURE_FORMAT dimension 3 with a
 * depth), declared Texture3D and sampled with three coordinates. Only when
 * the backend can create one: otherwise the first slice stays a 2D texture
 * and the program samples it in 2D, as before. */
static u32 eng_volume_mask(void)
{
    if (!g.be->texture_create_3d || !g.be->texture_upload_3d) return 0;
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled && !t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height)
            mask |= 1u << u;
    }
    return mask;
}

static u32 eng_cube_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* A cube face is square by construction, so a non-square image
         * cannot be one; the fragment program is compiled against this mask
         * and a texturecube slot filled with a 2D texture is a validation
         * failure, not a wrong pixel. */
        if (t.enabled && t.cubemap && t.width && t.width == t.height)
            mask |= 1u << u;
    }
    return mask;
}

/* ---- program memo ---------------------------------------------------------
 * Everything a draw derives from its programs' bytes -- the program sizes,
 * the VP input analysis, the pipeline-key hashes, the FP's sampled units and
 * its inline constants -- is a function of those bytes alone, and a draw
 * nearly always runs a program an earlier draw ran. Every draw used to redo
 * all of it: scan both programs for their end, hash the VP byte by byte, hash
 * the FP structurally, run three FP analyses and zero and refill a 32 KB
 * constant block. Each memo entry keeps an exact copy of the bytes it was
 * derived from; equal bytes reuse the derivation, any difference at all (a
 * patched constant, another program at the same address) recomputes it. An
 * identical prefix up to the stored size also means an identical size, since
 * the size is found by scanning those bytes for the end flag. Ported from
 * the inFAMOUS 2 toolkit fork (there: pipeline lookup 16-19 ms -> 5 ms per
 * stats window, memo hits VP 99.8%, FP 99.3%). */
#define ENG_PROG_MEMO 8

typedef struct {
    u32 start, instrs;          /* instrs == 0: unused entry */
    u32 input_mask;             /* rsx_vp_analyze_inputs, 0xFFFF when unsure */
    u64 hash;                   /* of the program bytes */
    u32 used;                   /* recency */
    u8 branch_bits_used;        /* rsx_vp_uses_branch_bits: BRB/CLB in the program */
    u8 has_flow;                /* rsx_vp_has_flow: its targets depend on the start slot */
    u8 bytes[RSX_DSP_VP_INSTR * 16u];
} eng_vp_memo;

typedef struct {
    u32 loc, off, size;         /* size == 0: unused entry */
    u64 structural;             /* rsx_fp_structural_hash from the FNV basis; 0: will not hash */
    int constants_ok;
    u32 sampled_units;          /* rsx_fp_texture_units */
    int reads_wpos;             /* rsx_fp_reads_wpos (the window goes in fp_alpha.y/.z) */
    /* The last texture-state masks the gamma / expand analyses ran with. */
    int gamma_valid, expand_valid;
    u8 gamma_in[RSX_DSP_NUM_TEXTURES], expand_in[RSX_DSP_NUM_TEXTURES];
    u32 gamma_units, expand_units;
    u32 used;
    u8* bytes;
    rsx_fp_constant_block constants;
} eng_fp_memo;

static eng_vp_memo s_vp_memo[ENG_PROG_MEMO];
static eng_fp_memo s_fp_memo[ENG_PROG_MEMO];
static u32 s_memo_clock;

static void eng_memo_reset(void)
{
    for (u32 i = 0; i < ENG_PROG_MEMO; i++) {
        s_vp_memo[i].instrs = 0;
        s_fp_memo[i].size = 0;
    }
}

/* The resident vertex program's memo entry, or NULL when there is none. */
static eng_vp_memo* eng_vp_current(void)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) return NULL;
    const u8* uc = (const u8*)(g.rsx.vp + start * 4);
    for (u32 i = 0; i < ENG_PROG_MEMO; i++) {
        eng_vp_memo* m = &s_vp_memo[i];
        if (m->instrs && m->start == start && !memcmp(m->bytes, uc, m->instrs * 16u)) {
            m->used = ++s_memo_clock;
            return m;
        }
    }
    const u32 instrs = rsx_vp_program_size_instrs(uc, (RSX_DSP_VP_INSTR - start) * 16u);
    if (!instrs) return NULL;
    u32 v = 0;
    for (u32 i = 1; i < ENG_PROG_MEMO; i++)
        if (s_vp_memo[i].used < s_vp_memo[v].used) v = i;
    eng_vp_memo* m = &s_vp_memo[v];
    m->start = start;
    m->instrs = instrs;
    memcpy(m->bytes, uc, instrs * 16u);
    m->used = ++s_memo_clock;
    m->hash = eng_fnv1a(uc, instrs * 16u, 1469598103934665603ull);
    m->branch_bits_used = (u8)rsx_vp_uses_branch_bits(uc, instrs * 16u);
    m->has_flow = (u8)rsx_vp_has_flow(uc, instrs * 16u);
    m->input_mask = 0xFFFFu;
    rsx_vp_input_analysis analysis = { 0xFFFFu, 0 };
    if (rsx_vp_analyze_inputs(uc, instrs * 16u, &analysis) == (int)instrs &&
        analysis.exact && analysis.input_mask)
        m->input_mask = analysis.input_mask;
    return m;
}

/* The bound fragment program's memo entry, or NULL when there is none (no
 * program, unmapped, or no end flag within ENG_FP_MAX_BYTES). */
static eng_fp_memo* eng_fp_current(void)
{
    /* A zero SET_SHADER_PROGRAM is "none", not "the program at offset 0": the
     * register carries the location in its low two bits, so a real program can
     * never read back as zero. */
    if (!rsx_dsp_reg(&g.rsx, 0x08E4)) return NULL;
    u32 loc = 0;
    const u32 off = rsx_dsp_fragment_program(&g.rsx, &loc);
    for (u32 i = 0; i < ENG_PROG_MEMO; i++) {
        eng_fp_memo* m = &s_fp_memo[i];
        if (!m->size || m->loc != loc || m->off != off) continue;
        /* detiled when it lies in tiled main memory (eng_fetch_ptr) */
        const u8* uc = eng_fetch_ptr(NULL, loc, off, m->size);
        if (uc && !memcmp(m->bytes, uc, m->size)) {
            m->used = ++s_memo_clock;
            return m;
        }
    }
    const u8* uc = eng_guest_ptr(NULL, loc, off, 16);
    /* in tiled main memory: as far as the size scan may look, detiled */
    const int tiled = uc && eng_tiled(loc, off, ENG_FP_MAX_BYTES);
    if (tiled) uc = eng_fetch_detiled(loc, off, ENG_FP_MAX_BYTES);
    const u32 size = uc ? rsx_fp_program_size(uc, ENG_FP_MAX_BYTES) : 0;
    if (size && !tiled) uc = eng_guest_ptr(NULL, loc, off, size);
    if (!uc || !size) return NULL;
    u32 v = 0;
    for (u32 i = 1; i < ENG_PROG_MEMO; i++)
        if (s_fp_memo[i].used < s_fp_memo[v].used) v = i;
    eng_fp_memo* m = &s_fp_memo[v];
    u8* copy = (u8*)realloc(m->bytes, size);
    if (!copy) { m->size = 0; return NULL; }
    m->bytes = copy;
    memcpy(m->bytes, uc, size);
    m->loc = loc;
    m->off = off;
    m->size = size;
    m->used = ++s_memo_clock;
    m->structural = rsx_fp_structural_hash(uc, size, 1469598103934665603ull);
    memset(&m->constants, 0, sizeof m->constants);
    m->constants_ok = rsx_fp_collect_constants(uc, size, &m->constants) >= 0;
    m->sampled_units = rsx_fp_texture_units(uc, size);
    m->reads_wpos = rsx_fp_reads_wpos(uc, size);
    m->gamma_valid = m->expand_valid = 0;
    return m;
}

/* Are both of the guest's own programs resident? One answer, used by the
 * layout and by the pipeline, because they must not disagree: the built-in
 * program declares all sixteen inputs, so narrowing the layout for it would
 * leave the pipeline's vertex descriptor short of what the shader reads.
 * The memo entries hold copies equal to the guest's bytes (the VP in the
 * register file, the FP in guest memory). */
static int eng_guest_programs_memo(eng_vp_memo** out_vp, eng_fp_memo** out_fp)
{
    eng_vp_memo* vp = eng_vp_current();
    eng_fp_memo* fp = eng_fp_current();
    if (out_vp) *out_vp = vp;
    if (out_fp) *out_fp = fp;
    return vp && fp;
}

/* The layout the active vertex program actually reads. An uncertain analysis
 * falls back to all sixteen registers (rsx_live_draw.c:3625-3643). */
static void eng_vertex_layout(rsx_vertex_layout_plan* layout)
{
    u32 mask = 0xFFFFu;
    eng_vp_memo* vp = NULL;
    if (eng_guest_programs_memo(&vp, NULL)) mask = vp->input_mask;
    rsx_vertex_layout_plan_init(layout, mask);
}

/* What a draw runs before a title has loaded programs of its own.
 *
 * The reference engine has no fallback and drops such a draw, because a title
 * always has both programs resident by the time it draws anything. A general
 * toolkit does not get to assume that: the host harness's fixed-function modes
 * are exactly this case, and so is the first frame of a title that clears and
 * flips before its first SET_TRANSFORM_PROGRAM. It is written in the same HLSL
 * shape the decompilers emit -- ATTRn inputs, the COLOR0/TEXCOORD varyings, the
 * VPConst block and the viewport epilogue -- so it goes through the one
 * pipeline_create the backend already implements rather than needing a second
 * entry point for a built-in shader.
 *
 * Vertex constant rows 0..3 are the transform when the guest programmed them
 * and identity when it did not: an all-zero matrix would collapse every vertex
 * onto the origin. */
static const char* const kEngFixedVS =
"struct VSInput {\n"
"    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
"    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
"    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
"    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
"};\n"
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"};\n"
"VSOutput main(VSInput input) {\n"
"    float4 _p = float4(dot(vp_c[0], input.a0), dot(vp_c[1], input.a0),\n"
"                       dot(vp_c[2], input.a0), dot(vp_c[3], input.a0));\n"
"    float _m = dot(abs(vp_c[0]), 1.0) + dot(abs(vp_c[1]), 1.0)\n"
"             + dot(abs(vp_c[2]), 1.0) + dot(abs(vp_c[3]), 1.0);\n"
"    if (_m == 0.0) _p = input.a0;\n"
"    VSOutput Out;\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    Out.col0 = input.a3; Out.col1 = float4(0,0,0,1); Out.fog = (float4)0;\n"
"    Out.t0 = input.a8; Out.t1 = (float4)0; Out.t2 = (float4)0; Out.t3 = (float4)0;\n"
"    Out.t4 = (float4)0; Out.t5 = (float4)0; Out.t6 = (float4)0; Out.t7 = (float4)0;\n"
"    Out.t8 = (float4)0; Out.t9 = (float4)0;\n"
"    return Out;\n"
"}\n";

static const char* const kEngFixedPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

/* The texture units a translated pixel program samples. Every sampling call
 * the decompiler emits names its unit's sampler, rsx_samp[N], whatever the
 * texture's own spelling (rsx_tex[N] or, with a cube unit, rsx_texN); the
 * declaration's rsx_samp[16] is out of range and so not counted. */
static u32 eng_sampled_units(const char* ps)
{
    u32 mask = 0;
    for (const char* q = ps; (q = strstr(q, "rsx_samp[")) != NULL; q += 9) {
        const unsigned long u = strtoul(q + 9, NULL, 10);
        if (u < RSX_BE_MAX_TEXTURES) mask |= 1u << u;
    }
    return mask;
}

/* The pipeline for this draw, built once per distinct key. A negative result
 * is cached too (handle 0), so a program pair that will not translate is not
 * retried on every draw of every frame. `out_tex_used` gets the units the
 * pixel program samples: all of them for the built-in pair, which keeps
 * every enabled unit bound as before. */
/* Up to 16 comma-separated hex hashes from an environment variable. */
static u32 eng_parse_hash_list(const char* name, u64 out[16])
{
    const char* e = getenv(name);
    u32 n = 0;
    while (e && *e && n < 16) {
        char* end;
        const u64 v = strtoull(e, &end, 16);
        if (end == e) break;
        if (v) out[n++] = v;
        if (*end != ',') break;
        e = end + 1;
    }
    return n;
}

/* The per-draw fragment-program debug knobs; nonzero drops the draw.
 *
 * RSX_ENGINE_SKIP_FP=<hex>[,<hex>...] drops every draw through those
 * fragment programs: which pass paints a wrong smear, halo or black band is
 * found by removing passes one at a time.
 *
 * RSX_ENGINE_REGDUMP_FP=<hex>[,<hex>...] prints the non-zero words of the
 * whole RSX register file at the first draw through each of those programs,
 * so the state of a broken pass can be diffed register by register against a
 * working one -- which state it sets that the engine does not honour. */
static int eng_debug_fp(u64 fp_hash, u32 pipeline)
{
    static int init;
    static u64 skip[16], dump[16];
    static u32 n_skip, n_dump, dumped;
    if (!init) {
        init = 1;
        n_skip = eng_parse_hash_list("RSX_ENGINE_SKIP_FP", skip);
        n_dump = eng_parse_hash_list("RSX_ENGINE_REGDUMP_FP", dump);
    }
    for (u32 i = 0; i < n_dump; i++) {
        if (dump[i] != fp_hash || (dumped & (1u << i))) continue;
        dumped |= 1u << i;
        fprintf(stderr, "[rsx regdump] fp %016llx pipe %u frame %u begin\n",
                (unsigned long long)fp_hash, pipeline, g.frames);
        for (u32 r = 0; r < RSX_DSP_NUM_REGS; r++)
            if (g.rsx.regs[r])
                fprintf(stderr, "[rsx regdump] 0x%04X = 0x%08X\n", r * 4u, g.rsx.regs[r]);
        fprintf(stderr, "[rsx regdump] fp %016llx end\n", (unsigned long long)fp_hash);
    }
    for (u32 i = 0; i < n_skip; i++)
        if (skip[i] == fp_hash) return 1;
    return 0;
}

/* User clip planes. NV4097_SET_USER_CLIP_PLANE_CONTROL (0x1478) holds one
 * 4-bit op per plane (bits 0..23, planes 0..5): 0 disabled, 1 ENABLE_LT, 2
 * ENABLE_GE. The vertex program writes the six distances into o[5].yzw and
 * o[6].yzw (o[5].x is fog, o[6].x the point size). As RPCS3 does
 * (fill_user_clip_data + the VP reg_table): GE keeps d >= 0 (factor +1), LT
 * keeps d <= 0 (factor -1), a disabled plane never clips. A planar
 * reflection clips its mirrored scene at the mirror plane this way; ignored,
 * everything behind the plane was drawn into the reflection.
 * Returns the op per plane, two bits each (0 = no plane enabled), the form
 * rsx_vp_apply_user_clip takes. PS3RECOMP_ENG_USER_CLIP=0 ignores the
 * register (the old behaviour). */
static u32 eng_user_clip_key(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_USER_CLIP"); on = e ? atoi(e) : 1; }
    if (!on) return 0;
    const u32 ctl = rsx_dsp_reg(&g.rsx, 0x1478u);
    u32 key = 0;
    for (u32 p = 0; p < 6; p++) {
        const u32 op = (ctl >> (p * 4u)) & 0xFu;
        if (op == 1u || op == 2u) key |= op << (p * 2u);
    }
    return key;
}

/* A guest fragment program's name for the debug knobs: its structural hash
 * (inline constants excluded) seeded with the FNV offset basis, the value
 * RSX_FP_DEBUG matches in the decompiler (eng_fp_memo.structural). Unlike a
 * backend pipeline handle it does not depend on the order programs are first
 * drawn in, so a program found in one run is named the same in the next. 0
 * for the built-in one. */

/* The depth-bounds test (SET_DEPTH_BOUNDS_TEST_ENABLE 0x0380, MIN 0x0384,
 * MAX 0x0388) on a backend without one (no set_depth_bounds: Metal). A
 * fragment survives only where the depth ALREADY STORED at its pixel lies in
 * [min, max]; the fragment's own depth plays no part. Deferred renderers
 * bound their light volumes and per-cascade shadow passes with it; ignored,
 * those passes reach every pixel their geometry covers. The fragment program
 * does the test instead: a float copy of the depth buffer, taken before the
 * draw (so a bounded draw that also writes depth is tested against the
 * depth before it, as one draw on the hardware is), goes on a texture unit
 * the program does not sample, and rsx_fp_apply_depth_bounds reads it at the
 * pixel and discards. The bounds go in the float4 after fp_alpha. A bound of
 * [0, 1] is no bound. s_dbz says what eng_pipeline_get, the texture binding
 * and the constants need for the current draw.
 * PS3RECOMP_ENG_DEPTH_BOUNDS=0: the test is ignored there, as before. */
static struct { u32 unit; u32 copy; float mn, mx; } s_dbz;   /* unit + 1, or 0 */

static int eng_depth_bounds_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_DEPTH_BOUNDS");
        on = !(e && *e && atoi(e) == 0);
    }
    return on;
}

static void eng_depth_bounds_prepare(u32 zslot, const rsx_be_render_state* rs)
{
    s_dbz.unit = 0; s_dbz.copy = 0;
    if (g.be->set_depth_bounds || !g.be->depth_snapshot || !rs->depth_bounds_test ||
        zslot == ENG_INVALID || !eng_depth_bounds_on())
        return;
    float mn, mx;
    { const u32 a = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MIN), b = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MAX);
      memcpy(&mn, &a, 4); memcpy(&mx, &b, 4); }
    /* Saturated into the depth range, as the hardware does (RPCS3
     * clamp_depth_bounds_value); NaN reads as 0. */
    mn = mn > 0.0f ? (mn < 1.0f ? mn : 1.0f) : 0.0f;
    mx = mx > 0.0f ? (mx < 1.0f ? mx : 1.0f) : 0.0f;
    if (mn <= 0.0f && mx >= 1.0f) return;
    eng_fp_memo* fpm = NULL;
    if (!eng_guest_programs_memo(NULL, &fpm)) return;
    /* A unit the program neither samples nor has declared as a cube or a
     * volume (rsx_fp_apply_depth_bounds reads it as a Texture2D). */
    const u32 busy = fpm->sampled_units | eng_cube_mask() | eng_volume_mask();
    int unit = -1;
    for (int u = RSX_BE_MAX_TEXTURES - 1; u >= 0 && unit < 0; u--)
        if (!((busy >> u) & 1u)) unit = u;
    eng_zdepth* z = &g.zdepths[zslot];
    if (unit < 0 || !z->handle) {
        static u32 said;
        if (PS3_LOG_FIRST(said, 4))
            fprintf(stderr, "[rsx engine] depth bounds %.4f..%.4f not applied: %s\n",
                    mn, mx, unit < 0 ? "no free texture unit" : "no depth target");
        return;
    }
    if (!z->dbz_copy || z->dbz_serial != z->depth_serial || z->dbz_w != z->w || z->dbz_h != z->h) {
        const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, z->w, z->h);
        if (!tex) return;
        if (z->dbz_copy && z->dbz_copy != tex && g.be->texture_release)
            g.be->texture_release(g.be->user, z->dbz_copy);
        z->dbz_copy = tex; z->dbz_w = z->w; z->dbz_h = z->h;
        z->dbz_serial = z->depth_serial;
    }
    s_dbz.unit = (u32)unit + 1u; s_dbz.copy = z->dbz_copy;
    s_dbz.mn = mn; s_dbz.mx = mx;
}

static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout,
                            const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count, int* out_fixed,
                            u32* out_tex_used, u64* out_fp_hash)
{
    *out_fixed = 1;
    *out_tex_used = 0;
    *out_fp_hash = 0;
    eng_vp_memo* vpm = NULL;
    eng_fp_memo* fpm = NULL;
    /* No resident program pair means the built-in one; see kEngFixedVS. */
    const int fixed = !eng_guest_programs_memo(&vpm, &fpm);
    const u8* vp_uc = fixed ? NULL : (const u8*)(g.rsx.vp + vpm->start * 4);
    const u8* fp_uc = fixed ? NULL : fpm->bytes;   /* equal to the guest's bytes, detiled */
    const u32 vp_instrs = fixed ? 0 : vpm->instrs, fp_size = fixed ? 0 : fpm->size;
    if (!fixed && !fp_uc) return 0;

    const u32 fp_ctrl  = rsx_dsp_shader_control(&g.rsx);
    const u32 srgb_output = !fixed && rsx_fp_srgb_output_enabled(
        rsx_dsp_reg(&g.rsx, 0x1FECu), fp_ctrl,
        rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu);
    const u32 cube_mask = eng_cube_mask();
    const u32 vol_mask = eng_volume_mask();
    const u32 vtex_mask = eng_vtex_mask();
    /* BRB/CLB outcomes are baked into the program: the branch bits are
     * identity only for a program that tests them. */
    const u32 vp_branch_bits = (!fixed && vpm->branch_bits_used) ? rsx_dsp_reg(&g.rsx, 0x1FF8u) : 0;
    const u32 ucp = fixed ? 0 : eng_user_clip_key();
    const u32 dbz = fixed ? 0 : s_dbz.unit;   /* eng_depth_bounds_prepare: unit + 1 */
    rsx_fp_shadow_desc shadow[RSX_DSP_NUM_TEXTURES] = {{0}};
    u32 shadow_mask = 0;
    const u32 sampled_units = fixed ? 0 : fpm->sampled_units;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        if (!(sampled_units & (1u << u))) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        rsx_fp_shadow_desc_from_texture(&shadow[u], t.format, t.dimension,
            t.cubemap, t.mipmaps, t.filter, t.wrap, t.control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    u8 gamma_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) gamma_masks[u] = rsx_texture_gamma_mask(t.format, t.remap, t.filter, t.wrap);
    }
    u32 gamma_units = 0;
    if (!fixed) {
        if (!fpm->gamma_valid || memcmp(fpm->gamma_in, gamma_masks, sizeof gamma_masks)) {
            memcpy(fpm->gamma_in, gamma_masks, sizeof gamma_masks);
            fpm->gamma_units = rsx_fp_texture_gamma_units(fp_uc, fp_size, gamma_masks);
            fpm->gamma_valid = 1;
        }
        gamma_units = fpm->gamma_units;
    }
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(gamma_units & (1u << u))) gamma_masks[u] = 0;
    /* Texture sign expansion: SET_TEXTURE_ADDRESS's UNSIGNED_REMAP_BIASED
     * asks that every fetch come back re-centred to -1..1. Same rule as
     * gamma: only units the program samples count. */
    u8 expand_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) expand_masks[u] = rsx_texture_expand_mask(t.format, t.remap, t.filter, t.wrap);
    }
    u32 expand_units = 0;
    if (!fixed) {
        if (!fpm->expand_valid || memcmp(fpm->expand_in, expand_masks, sizeof expand_masks)) {
            memcpy(fpm->expand_in, expand_masks, sizeof expand_masks);
            fpm->expand_units = rsx_fp_texture_expand_units(fp_uc, fp_size, expand_masks);
            fpm->expand_valid = 1;
        }
        expand_units = fpm->expand_units;
    }
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(expand_units & (1u << u))) expand_masks[u] = 0;

    /* Units filtered through the RSX convolution kernel: the kernel goes into
     * the shader (rsx_fp_apply_convolution), so the set and the kernels are
     * pipeline identity. PS3_RSX_NO_CONVOLUTION=1 compares without. */
    u32 conv_mask = 0;
    u8 conv_kernels[16] = {0};
    {
        static int conv_off = -1;
        if (conv_off < 0) conv_off = getenv("PS3_RSX_NO_CONVOLUTION") ? 1 : 0;
        if (!fixed && !conv_off) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES && u < 16; ++u) {
            if (!(sampled_units & (1u << u))) continue;
            rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
            const u32 minf = (t.filter >> 16) & 7u, magf = (t.filter >> 24) & 7u;
            if (!t.enabled || (minf != 7u && magf != 4u)) continue;
            conv_mask |= 1u << u;
            conv_kernels[u] = (u8)((t.filter >> 13) & 7u);
        }
        conv_mask &= ~cube_mask;
    }

    u32 unnorm_dim[RSX_DSP_NUM_TEXTURES][3];
    /* Shadow helpers normalize against the bound resource themselves. Only
     * sampled ordinary UN units need baked dimensions or cache identity. */
    const u32 unnorm_mask = eng_unnorm_units(unnorm_dim) &
        (fixed ? 0xFFFFu : sampled_units) & ~shadow_mask;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++)
        if (!(unnorm_mask & (1u << u)))
            unnorm_dim[u][0] = unnorm_dim[u][1] = unnorm_dim[u][2] = 0;

    static const rsx_fp_constant_block no_constants;
    g.fp_constants = fixed ? &no_constants : &fpm->constants;
    if (!fixed && !fpm->constants_ok) return 0;

    /* An antialiased target is held at its sample grid; WPOS must still count
     * pixels (rsx_fp_apply_wpos_grid). */
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    const u32 wpos_grid[2] = { sf.aa_sx ? sf.aa_sx : 1u, sf.aa_sy ? sf.aa_sy : 1u };
    const int wpos_scaled = !fixed && (wpos_grid[0] > 1u || wpos_grid[1] > 1u);

    /* Identity: the vertex program's own bytes, the fragment program's
     * STRUCTURE (its inline constants are hoisted into the buffered block, so
     * a constant change must not be a new pipeline), the export-width bit of
     * SHADER_CONTROL, the cube and vertex-texture masks, the input layout,
     * and the structural render state. */
    u64 key = 1469598103934665603ull;
    if (fixed) {
        static const u32 fixed_tag = 0x4E464958u;   /* "XIFN" */
        key = eng_fnv1a(&fixed_tag, sizeof fixed_tag, key);
    } else {
        if (!fpm->structural) return 0;
        key = eng_fnv1a(&vpm->hash, sizeof vpm->hash, key);
        if (vpm->branch_bits_used) key = eng_fnv1a(&vp_branch_bits, sizeof vp_branch_bits, key);
        /* Branch targets are absolute VP slots, made relative to the slot the
         * program starts at: the same bytes loaded elsewhere branch elsewhere. */
        if (vpm->has_flow) key = eng_fnv1a(&vpm->start, sizeof vpm->start, key);
        key = eng_fnv1a(&fpm->structural, sizeof fpm->structural, key);
        const u32 fp_ctrl_key = fp_ctrl & 0x4Eu; /* colour width and fragment depth export */
        key = eng_fnv1a(&fp_ctrl_key, sizeof fp_ctrl_key, key);
        if (srgb_output) key = eng_fnv1a(&srgb_output, sizeof srgb_output, key);
        if (gamma_units) key = eng_fnv1a(gamma_masks, sizeof gamma_masks, key);
        if (expand_units) key = eng_fnv1a(expand_masks, sizeof expand_masks, key);
        key = eng_fnv1a(&cube_mask, sizeof cube_mask, key);
        if (vol_mask) key = eng_fnv1a(&vol_mask, sizeof vol_mask, key);
        if (shadow_mask) {
            key = eng_fnv1a(&shadow_mask, sizeof shadow_mask, key);
            key = eng_fnv1a(shadow, sizeof shadow, key);
        }
        key = eng_fnv1a(&vtex_mask, sizeof vtex_mask, key);
        if (ucp) key = eng_fnv1a(&ucp, sizeof ucp, key);
        if (dbz) { const u32 tag = 0x5A424400u | dbz; key = eng_fnv1a(&tag, sizeof tag, key); }
        key = eng_fnv1a(&unnorm_mask, sizeof unnorm_mask, key);
        if (unnorm_mask) key = eng_fnv1a(unnorm_dim, sizeof unnorm_dim, key);
        if (conv_mask) {
            key = eng_fnv1a(&conv_mask, sizeof conv_mask, key);
            key = eng_fnv1a(conv_kernels, sizeof conv_kernels, key);
        }
        if (wpos_scaled) key = eng_fnv1a(wpos_grid, sizeof wpos_grid, key);
    }
    key = eng_fnv1a(&layout->mask, sizeof layout->mask, key);
    key = eng_fnv1a(&layout->stride, sizeof layout->stride, key);
    if (layout->n_streams > 1) {       /* a split draw's vertex buffers */
        key = eng_fnv1a(&layout->stream_map, sizeof layout->stream_map, key);
        key = eng_fnv1a(&layout->n_streams, sizeof layout->n_streams, key);
    }
    key = eng_fnv1a(&rt_fmt, sizeof rt_fmt, key);
    /* How many colour attachments the pass will have is pipeline identity:
     * a host API matches the two against each other. */
    key = eng_fnv1a(&rt_count, sizeof rt_count, key);
    key = rsx_draw_engine_hash_render_state(rs, key);

    u32 slot = eng_pipeline_slot(key);
    for (; g.pipeline_index[slot]; slot = (slot + 1u) & (ENG_PIPELINE_INDEX - 1u)) {
        const eng_pipeline* p = &g.pipelines[g.pipeline_index[slot] - 1u];
        if (p->key == key) {
            *out_fixed = p->fixed;
            *out_tex_used = p->tex_used;
            *out_fp_hash = p->fp_hash;
            return p->handle;
        }
    }
    if (g.n_pipelines >= ENG_MAX_PIPELINES) {
        static int said;
        if (!said) { said = 1; fprintf(stderr, "[rsx engine] pipeline table full (%u): new draws are dropped\n", (unsigned)ENG_MAX_PIPELINES); }
        return 0;
    }

    u32 handle = 0;
    int vi = 1, fi = 1;
    u32 nconst = 0;
    u32 dbz_bit = 0;
    if (fixed) {
        snprintf(s_vs_hlsl, sizeof s_vs_hlsl, "%s", kEngFixedVS);
        snprintf(s_ps_hlsl, sizeof s_ps_hlsl, "%s", kEngFixedPS);
    } else {
        rsx_vp_set_branch_base(vpm->start);
        rsx_vp_set_branch_bits(vp_branch_bits);
        vi = rsx_vp_decompile_compact_ex(vp_uc, vp_instrs * 16u, vtex_mask,
                                         layout->mask, s_vs_hlsl,
                                         sizeof s_vs_hlsl);
        if (vi > 0 && ucp && rsx_vp_apply_user_clip(s_vs_hlsl, sizeof s_vs_hlsl, ucp) < 0) {
            vi = -1;
            static u32 said;
            if (PS3_LOG_FIRST(said, 8))
                fprintf(stderr, "[rsx engine] user clip planes 0x%06X: vertex program text not in the"
                                " decompiler's form, draw skipped\n", rsx_dsp_reg(&g.rsx, 0x1478u));
        }
        fi = rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp_uc, fp_size, fp_ctrl, cube_mask, vol_mask,
                shadow_mask ? shadow : NULL, gamma_masks, expand_units ? expand_masks : NULL,
                s_ps_hlsl, sizeof s_ps_hlsl, &nconst);
        if (fi > 0 && nconst != g.fp_constants->count) fi = -1;
        if (fi > 0 && wpos_scaled &&
            rsx_fp_apply_wpos_grid(s_ps_hlsl, sizeof s_ps_hlsl, wpos_grid[0], wpos_grid[1]) < 0)
            fi = -1;
        if (fi > 0 && rsx_fp_apply_unnorm_scale(s_ps_hlsl, sizeof s_ps_hlsl, unnorm_mask,
                                                (const u32 (*)[3])unnorm_dim, cube_mask,
                                                vol_mask) < 0)
            fi = -1;
        if (fi > 0 && conv_mask &&
            rsx_fp_apply_convolution(s_ps_hlsl, sizeof s_ps_hlsl, conv_mask,
                                     conv_kernels, cube_mask) < 0)
            fi = -1;
        if (fi > 0 && srgb_output &&
            rsx_fp_apply_srgb_output(s_ps_hlsl, sizeof s_ps_hlsl) < 0)
            fi = -1;
        if (fi > 0 && rs->alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(s_ps_hlsl, sizeof s_ps_hlsl,
                                             rs->alpha_func) < 0)
            fi = -1;
        if (fi > 0 && dbz) {
            if (rsx_fp_apply_depth_bounds(s_ps_hlsl, sizeof s_ps_hlsl, dbz - 1u) > 0) {
                dbz_bit = 1u << (dbz - 1u);   /* read with Load: no rsx_samp[] names it */
            } else {
                static u32 said;   /* drawn unbounded rather than dropped */
                if (PS3_LOG_FIRST(said, 8))
                    fprintf(stderr, "[rsx engine] depth bounds not applied to a fragment program"
                                    " (unexpected text)\n");
            }
        }
    }
    if (vi > 0 && fi > 0)
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
    /* The debug name (see above), which the memo already holds. */
    const u64 fp_hash = fixed ? 0 : fpm->structural;
    /* RSX_ENGINE_SHADER_DUMP=<existing dir>: each new pipeline's two programs
     * as <dir>/pipe<handle>.vs.hlsl and .ps.hlsl, the handle being the one
     * RSX_ENGINE_PASS_DUMP's index names, and a guest fragment program's own
     * ucode as .fp.bin with its address and debug hash (the value
     * RSX_FP_DEBUG, RSX_ENGINE_SKIP_FP and RSX_ENGINE_REGDUMP_FP take) in
     * .fp.txt. A failed pipeline is pipe0. */
    {
        const char* dir = getenv("RSX_ENGINE_SHADER_DUMP");
        if (dir && *dir) {
            char path[1024];
            FILE* f;
            snprintf(path, sizeof path, "%s/pipe%u.vs.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_vs_hlsl, f); fclose(f); }
            snprintf(path, sizeof path, "%s/pipe%u.ps.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_ps_hlsl, f); fclose(f); }
            if (!fixed && fp_uc && fp_size) {
                u32 fp_loc = 0;
                const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
                snprintf(path, sizeof path, "%s/pipe%u.fp.bin", dir, handle);
                if ((f = fopen(path, "wb"))) { fwrite(fp_uc, 1, fp_size, f); fclose(f); }
                snprintf(path, sizeof path, "%s/pipe%u.fp.txt", dir, handle);
                if ((f = fopen(path, "w"))) {
                    fprintf(f, "location %u offset 0x%08X size %u ctrl 0x%08X\n"
                               "fp %016llx\n",
                            fp_loc, fp_off, fp_size, fp_ctrl,
                            (unsigned long long)fp_hash);
                    fclose(f);
                }
            }
        }
    }
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 32))
        fprintf(stderr, "[rsx engine] pipeline %016llx: %s vp %d, fp %d,"
                        " %u constants, fp %016llx -> %s\n",
                (unsigned long long)key, fixed ? "built-in" : "guest",
                vi, fi, nconst, (unsigned long long)fp_hash,
                handle ? "ok" : "FAILED (draw dropped)"); }

    g.pipelines[g.n_pipelines].key = key;
    g.pipelines[g.n_pipelines].fp_hash = fp_hash;
    g.pipelines[g.n_pipelines].handle = handle;
    g.pipelines[g.n_pipelines].fixed = (u8)fixed;
    g.pipelines[g.n_pipelines].tex_used =
        (u16)(fixed ? 0xFFFFu : (eng_sampled_units(s_ps_hlsl) | dbz_bit));
    *out_tex_used = g.pipelines[g.n_pipelines].tex_used;
    *out_fp_hash = fp_hash;
    g.pipeline_index[slot] = (u16)(g.n_pipelines + 1u);
    g.n_pipelines++;
    *out_fixed = fixed;
    return handle;
}

/* ---- draw accumulation --------------------------------------------------- */

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_packets = 0;
    dc.n_refs = dc.n_source_refs = dc.n_verts = dc.n_cuts = 0;
    dc.refs_remapped = 0;
    dc.vbuf = 0;
    dc.n_streams = 0;
    dc.stream_map = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL;
    dc.inl_bytes = 0;
    dc.n_immediate = 0;
}

static int dc_push_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs,
                                                     (size_t)next * sizeof(*r));
        if (!r) return 0;
        dc.refs = r;
        dc.cap_refs = next;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
    return 1;
}

/* Room for `more` references past dc.n_refs, grown as dc_push_ref grows. */
static int dc_reserve_refs(u32 more)
{
    const u64 want = (u64)dc.n_refs + more;
    if (want <= dc.cap_refs) return 1;
    if (want > 0xFFFFFFFFull) return 0;
    u64 next = dc.cap_refs ? dc.cap_refs : 4096u;
    while (next < want) next *= 2u;
    if (next > 0xFFFFFFFFull) next = want;
    rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs, (size_t)next * sizeof(*r));
    if (!r) return 0;
    dc.refs = r;
    dc.cap_refs = (u32)next;
    return 1;
}

/* A run of big-endian indices appended as references in one pass: the
 * capacity is reserved up front and the loop works on locals, which the
 * per-index dc_push_ref (reloading dc after every store) could not. A
 * restart sentinel still cuts the strip where it stands. */
static int dc_push_index_run(const u8* run, u32 count, int is_u32, u32 base_index,
                             int restart_en, u32 restart_val)
{
    if (!dc_reserve_refs(count)) return 0;
    rsx_vertex_ref* out = dc.refs;
    u32 n = dc.n_refs;
    if (is_u32) {
        for (u32 i = 0; i < count; i++) {
            const u8* p = run + (size_t)i * 4u;
            const u32 index = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
            if (restart_en && index == restart_val) {
                dc.n_refs = n;
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n)) return 0;
                continue;
            }
            out[n].vertex_id = index; out[n].base_index = base_index; n++;
        }
    } else if (!restart_en) {
        for (u32 i = 0; i < count; i++) {
            out[n + i].vertex_id = (u32)((run[2u * i] << 8) | run[2u * i + 1u]);
            out[n + i].base_index = base_index;
        }
        n += count;
    } else {
        for (u32 i = 0; i < count; i++) {
            const u32 index = (u32)((run[2u * i] << 8) | run[2u * i + 1u]);
            if (index == restart_val) {
                dc.n_refs = n;
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n)) return 0;
                continue;
            }
            out[n].vertex_id = index; out[n].base_index = base_index; n++;
        }
    }
    dc.n_refs = n;
    return 1;
}

static int dc_reserve_verts(u64 bytes)
{
    if (bytes <= dc.verts_cap) return 1;
    u64 next = dc.verts_cap ? dc.verts_cap : (1u << 20);
    while (next < bytes) next *= 2u;
    u8* v = (u8*)realloc(dc.verts, (size_t)next);
    if (!v) return 0;
    dc.verts = v;
    dc.verts_cap = next;
    return 1;
}

static int dc_reserve_indices(u32 count)
{
    if (count <= g.index_cap) return 1;
    u32 next = g.index_cap ? g.index_cap : 4096u;
    while (next < count) next *= 2u;
    u32* p = (u32*)realloc(g.indices, (size_t)next * sizeof(u32));
    if (!p) return 0;
    g.indices = p;
    g.index_cap = next;
    return 1;
}

/* ---- vertex cache ---------------------------------------------------------
 * Most of a frame's fetched vertices are static geometry whose guest bytes do
 * not change from one frame to the next, yet every draw fetched, decoded and
 * staged them again: dc_fetch was ~41% of Resistance's RSX thread and the
 * dedup remap another ~13%. A draw over one consecutive run of vertices
 * (rsx_vertex_remap_build_range, or a plain DRAW_ARRAYS) whose every enabled
 * attribute maps one stable guest span is keyed on its layout, attribute
 * formats and spans, and checked by a hash of those source bytes. When the
 * same bytes are seen in two different frames the decoded vertices go into a
 * persistent backend buffer (rsx_draw_backend.vbuf_create), and later draws
 * with the same key and the same bytes draw from it. The bytes are hashed at
 * every draw, so geometry the guest rewrites is fetched again, never drawn
 * stale. A key whose bytes change three times running (skinned or streamed
 * geometry) is not hashed again for 240 frames.
 *   PS3RECOMP_ENG_VCACHE=0        off (every draw fetched, as before)
 *   PS3RECOMP_ENG_VCACHE_PRUNE=0  a full key table starts over instead of
 *                                 keeping what was drawn recently
 *   PS3RECOMP_ENG_VCACHE_STATS=1  a counter line every 120 frames
 * Ported from the inFAMOUS 2 port's toolkit fork. */
typedef struct {
    u64 key, content;
    u32 vbuf, bytes, n, stride;
    u32 last_frame, seen_frame, skip_until;
    u8 seen, changes;
} eng_vc_entry;
#define ENG_VC_SIZE   65536u
#define ENG_VC_BUDGET (512ull << 20)
#define ENG_VC_KEEP   60u     /* frames a key survives a prune undrawn */
static eng_vc_entry s_vc[ENG_VC_SIZE];
static eng_vc_entry s_vc_tmp[ENG_VC_SIZE];
static u32 s_vc_cursor, s_vc_used;
static u64 s_vc_bytes;
static int s_vc_enabled = -1;    /* -1: not read yet */
static int s_vc_prune_on = -1;
static int s_vc_stats_on = -1;
static int s_vc_split_enabled = -1;
static u64 s_vc_hit, s_vc_miss, s_vc_skip, s_vc_new, s_vc_hashed;
static u64 s_vc_split, s_vc_stream_hit, s_vc_stream_miss;
/* Bumped whenever the table is rebuilt or emptied: an entry pointer taken
 * before that no longer names its key, and a handle from it may be gone. */
static u32 s_vc_epoch;

static int eng_vc_on(void)
{
    if (s_vc_enabled < 0) {
        const char* e = getenv("PS3RECOMP_ENG_VCACHE");
        s_vc_enabled = !(e && *e && atoi(e) == 0);
    }
    return s_vc_enabled && g.be->vbuf_create && g.be->draw_vbuf && g.be->vbuf_release;
}

/* Eight independent FNV-style lanes, as the texture content hash: one serial
 * multiply chain caps a hash at ~2 bytes a cycle. */
static u64 eng_hash_span(const u8* src, u64 n, u64 seed)
{
    u64 h[8];
    for (int k = 0; k < 8; k++) h[k] = seed ^ (0x9E3779B97F4A7C15ull * (u64)(k + 1));
    u64 i = 0;
    for (; i + 64 <= n; i += 64) {
        u64 w[8]; memcpy(w, src + i, sizeof w);
        for (int k = 0; k < 8; k++) h[k] = (h[k] ^ w[k]) * 1099511628211ull;
    }
    u64 r = n;
    for (int k = 0; k < 8; k++) r ^= h[k] * (u64)(2 * k + 1);
    for (; i < n; i++) { r ^= src[i]; r *= 1099511628211ull; }
    return r;
}

/* Word-at-a-time mix for the cache key. */
static inline u64 eng_vc_mix(u64 h, u64 v)
{
    h ^= v * 0x9E3779B97F4A7C15ull;
    h = (h << 27) | (h >> 37);
    return h * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
}

static void eng_vc_drop(eng_vc_entry* e)
{
    if (e->vbuf) { g.be->vbuf_release(g.be->user, e->vbuf); s_vc_bytes -= e->bytes; }
    e->vbuf = 0; e->bytes = 0;
}

static void eng_vc_clear(void)
{
    if (g.be && g.be->vbuf_release)
        for (u32 i = 0; i < ENG_VC_SIZE; i++) eng_vc_drop(&s_vc[i]);
    memset(s_vc, 0, sizeof s_vc);
    s_vc_used = 0; s_vc_bytes = 0; s_vc_cursor = 0;
    s_vc_epoch++;
}

/* The table full: keep what was drawn recently, forget the rest. Skinned and
 * streamed geometry lands at a new address every frame and adds keys that are
 * never seen again; dropping the whole table would take the static geometry's
 * buffers with it, which the next two frames then fetch, decode and stage
 * again. The entries not drawn in the last ENG_VC_KEEP frames are dropped and
 * the rest re-inserted; only when that frees too little does the cache start
 * over. PS3RECOMP_ENG_VCACHE_PRUNE=0 = start over every time. Returns
 * non-zero when the prune left enough room. */
static int eng_vc_prune(void)
{
    if (s_vc_prune_on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_VCACHE_PRUNE");
        s_vc_prune_on = !(e && *e && atoi(e) == 0);
    }
    if (!s_vc_prune_on) return 0;
    s_vc_epoch++;
    u32 kept = 0;
    memcpy(s_vc_tmp, s_vc, sizeof s_vc);
    memset(s_vc, 0, sizeof s_vc);
    for (u32 i = 0; i < ENG_VC_SIZE; i++) {
        eng_vc_entry* o = &s_vc_tmp[i];
        if (!o->key) continue;
        if (o->last_frame + ENG_VC_KEEP < g.frames && o->skip_until <= g.frames) {
            eng_vc_drop(o);
            continue;
        }
        u32 p = (u32)(o->key ^ (o->key >> 32)) & (ENG_VC_SIZE - 1u);
        while (s_vc[p].key) p = (p + 1) & (ENG_VC_SIZE - 1u);
        s_vc[p] = *o;
        kept++;
    }
    s_vc_used = kept;
    { static int n = 0; if (PS3_LOG_FIRST(n, 16))
        fprintf(stderr, "[rsx engine] vertex cache full: kept %u recent keys (%.0f MB resident), frame %u\n",
                kept, s_vc_bytes / 1e6, g.frames); }
    return kept <= ENG_VC_SIZE / 2u;
}

/* The entry for `key`, made if new. NULL when the probe run is full or the
 * table had to be pruned or emptied first (s_vc_epoch moves then). */
static eng_vc_entry* eng_vc_find(u64 key)
{
    if (!key) key = 1;
    u32 p = (u32)(key ^ (key >> 32)) & (ENG_VC_SIZE - 1u);
    for (u32 probe = 0; probe < 32; probe++, p = (p + 1) & (ENG_VC_SIZE - 1u)) {
        if (s_vc[p].key == key) return &s_vc[p];
        if (!s_vc[p].key) {
            if (s_vc_used >= ENG_VC_SIZE / 4u * 3u) {   /* full: prune, else start over */
                if (eng_vc_prune()) return NULL;
                { static int n = 0; if (PS3_LOG_FIRST(n, 64))
                    fprintf(stderr, "[rsx engine] vertex cache: %u keys, %.0f MB resident -> start over (frame %u)\n",
                            s_vc_used, s_vc_bytes / 1e6, g.frames); }
                eng_vc_clear();
                return NULL;
            }
            eng_vc_entry* e = &s_vc[p];
            memset(e, 0, sizeof *e); e->key = key; s_vc_used++;
            return e;
        }
    }
    return NULL;
}

/* Key, then (unless the key is resting) the content hash of the spans. NULL:
 * this draw is not cached. Call after the fetch plan is prepared. */
static eng_vc_entry* eng_vc_probe(const rsx_vertex_layout_plan* layout, u64* content)
{
    const rsx_vertex_fetch_plan* pl = &dc.fetch_plan;
    if (pl->inline_data || !dc.n_refs) return NULL;
    u64 key = eng_vc_mix(0x243F6A8885A308D3ull, ((u64)layout->count << 32) | layout->stride);
    key = eng_vc_mix(key, dc.n_refs);
    key = eng_vc_mix(key, ((u64)dc.refs[0].vertex_id << 32) | dc.refs[0].base_index);
    key = eng_vc_mix(key, ((u64)pl->base_offset << 32) | pl->divider_mask);
    const u8* lo[RSX_DSP_NUM_VERTEX_ATTR]; const u8* hi[RSX_DSP_NUM_VERTEX_ATTR];
    u32 nr = 0;
    for (u32 slot = 0; slot < layout->count; slot++) {
        const u32 a = layout->attrs[slot];
        const rsx_vertex_fetch_attr* f = &pl->attr[a];
        key = eng_vc_mix(key, ((u64)a << 56) ^ ((u64)f->desc.type << 48) ^ ((u64)f->desc.size << 40) ^
                              ((u64)((pl->divider_mask >> a) & 1u) << 39) ^ ((u64)f->desc.frequency << 16) ^
                              ((u64)f->stride << 8) ^ f->elem_size);
        /* The current value too, enabled or not: a disabled attribute reads
         * it, and so does an enabled one whose format does not decode. */
        { u64 dv[2]; memcpy(dv, f->default_value, sizeof dv);
          key = eng_vc_mix(eng_vc_mix(key, dv[0]), dv[1]); }
        if (f->desc.type && f->desc.size) {
            /* An enabled attribute is read from a plain span or it is not
             * cached: the per-element resolver path has no span to hash. */
            if (!f->elem_size || !f->stable_base) return NULL;
            key = eng_vc_mix(key, (u64)(uintptr_t)f->stable_base);
            key = eng_vc_mix(key, ((u64)f->stable_first << 32) | f->stable_last);
            lo[nr] = f->stable_base;
            hi[nr] = f->stable_base + (u64)(f->stable_last - f->stable_first) * f->stride + f->elem_size;
            nr++;
        }
    }
    eng_vc_entry* e = eng_vc_find(key);
    if (!e) return NULL;
    if (e->skip_until > g.frames) { s_vc_skip++; return NULL; }
    /* Hash the union of the attribute spans: interleaved attributes share one. */
    for (u32 i = 1; i < nr; i++)            /* insertion sort by start */
        for (u32 j = i; j > 0 && lo[j] < lo[j - 1]; j--) {
            const u8* t = lo[j]; lo[j] = lo[j - 1]; lo[j - 1] = t;
            t = hi[j]; hi[j] = hi[j - 1]; hi[j - 1] = t;
        }
    u64 c = 0x51ED270B27A3F5E1ull;
    for (u32 i = 0; i < nr; ) {
        const u8* a0 = lo[i]; const u8* a1 = hi[i];
        for (i++; i < nr && lo[i] <= a1 + 64; i++) if (hi[i] > a1) a1 = hi[i];
        c = eng_hash_span(a0, (u64)(a1 - a0), c);
        s_vc_hashed += (u64)(a1 - a0);
    }
    *content = c;
    return e;
}

/* After a fetch: a second sighting of the same bytes in a later frame makes
 * the decoded vertices persistent; changed bytes drop them. */
static void eng_vc_after_fetch(eng_vc_entry* e, u64 content, const u8* data,
                               u32 n, u32 stride)
{
    const u64 bytes64 = (u64)n * stride;
    if (e->seen && e->content == content && e->seen_frame != g.frames) {
        if (!e->vbuf && bytes64 <= 0xFFFFFFFFull) {
            const u32 bytes = (u32)bytes64;
            while (s_vc_bytes + bytes > ENG_VC_BUDGET) {      /* evict the stale */
                u32 tries = 0;
                for (; tries < ENG_VC_SIZE; tries++, s_vc_cursor = (s_vc_cursor + 1) & (ENG_VC_SIZE - 1u))
                    if (s_vc[s_vc_cursor].vbuf && s_vc[s_vc_cursor].last_frame + 2 < g.frames) break;
                if (tries == ENG_VC_SIZE) break;
                eng_vc_drop(&s_vc[s_vc_cursor]);
            }
            if (s_vc_bytes + bytes <= ENG_VC_BUDGET) {
                e->vbuf = g.be->vbuf_create(g.be->user, data, bytes);
                if (e->vbuf) {
                    e->bytes = bytes; e->n = n; e->stride = stride;
                    s_vc_bytes += bytes; s_vc_new++;
                }
            }
        }
        e->changes = 0;
    } else if (e->seen && e->content != content) {
        eng_vc_drop(e);
        if (++e->changes >= 3) { e->skip_until = g.frames + 240u; e->changes = 0; }
    }
    e->content = content; e->seen = 1; e->seen_frame = g.frames; e->last_frame = g.frames;
}

/* RSX_VPC_LOG=<constant>[,<draw>]: a vertex-program constant as it stood at
 * the given guest draw of each frame (default draw 40, inside the main pass),
 * printed every 30 frames with the frame's draw count. Resistance keeps the
 * camera position in c16, which tracks where the player is. */
static int s_vpc_idx = -2, s_vpc_draw = 40;
static u32 s_vpc_val[4];
static int s_vpc_have;

static void eng_vpc_log_draw(void)
{
    if (s_vpc_idx == -2) {
        const char* e = getenv("RSX_VPC_LOG");
        s_vpc_idx = e && *e ? atoi(e) : -1;
        const char* c = e ? strchr(e, ',') : NULL;
        if (c) s_vpc_draw = atoi(c + 1);
        if (s_vpc_idx >= RSX_DSP_NUM_CONSTANTS) s_vpc_idx = -1;
    }
    if (s_vpc_idx < 0 || g.guest_draws != (u32)s_vpc_draw) return;
    memcpy(s_vpc_val, g.rsx.constants[s_vpc_idx], sizeof s_vpc_val);
    s_vpc_have = 1;
}

static void eng_vpc_log_frame(void)
{
    if (s_vpc_idx < 0 || g.frames % 30u) return;
    float f[4];
    memcpy(f, s_vpc_val, sizeof f);
    if (s_vpc_have)
        fprintf(stderr, "[vpc] frame %u draws %u c%d = %.2f %.2f %.2f %.2f\n", g.frames,
                g.guest_draws, s_vpc_idx, f[0], f[1], f[2], f[3]);
    else
        fprintf(stderr, "[vpc] frame %u draws %u (fewer than %d draws)\n", g.frames,
                g.guest_draws, s_vpc_draw);
    s_vpc_have = 0;
}

static void eng_vc_frame_end(void)
{
    if (s_vc_stats_on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_VCACHE_STATS");
        s_vc_stats_on = e && *e && atoi(e) != 0;
    }
    if (!s_vc_stats_on || g.frames % 120u) return;
    static u64 h0, m0, k0, n0, b0, d0, sh0, sm0;
    fprintf(stderr, "[rsx engine] vertex cache, last 120 frames: %llu hits, %llu misses, %llu resting, "
                    "%llu new; split draws %llu (streams: %llu hits, %llu misses); "
                    "hashed %.1f MB; %u keys, %.0f MB resident\n",
            (unsigned long long)(s_vc_hit - h0), (unsigned long long)(s_vc_miss - m0),
            (unsigned long long)(s_vc_skip - k0), (unsigned long long)(s_vc_new - n0),
            (unsigned long long)(s_vc_split - d0), (unsigned long long)(s_vc_stream_hit - sh0),
            (unsigned long long)(s_vc_stream_miss - sm0),
            (s_vc_hashed - b0) / 1e6, s_vc_used, s_vc_bytes / 1e6);
    h0 = s_vc_hit; m0 = s_vc_miss; k0 = s_vc_skip; n0 = s_vc_new; b0 = s_vc_hashed;
    static u64 tn0, te0, tc0, tnb0, tcb0;
    fprintf(stderr, "[rsx engine] texture cache, last 120 frames: %llu new (%.1f MB, %llu evicting), "
                    "%llu changed (%.1f MB); %u entries\n",
            (unsigned long long)(s_tex_new - tn0), (s_tex_new_bytes - tnb0) / 1e6,
            (unsigned long long)(s_tex_evicted - te0), (unsigned long long)(s_tex_changed - tc0),
            (s_tex_changed_bytes - tcb0) / 1e6, g.n_textures);
    tn0 = s_tex_new; te0 = s_tex_evicted; tc0 = s_tex_changed; tnb0 = s_tex_new_bytes; tcb0 = s_tex_changed_bytes;
    d0 = s_vc_split; sh0 = s_vc_stream_hit; sm0 = s_vc_stream_miss;
}

/* ---- split draws ----------------------------------------------------------
 * The whole-draw key above misses every frame when one of a draw's streams is
 * rewritten every frame while the others hold still. Resistance's world draws
 * read their meshes from main memory and a 4-byte lighting stream (ATTR4,
 * UB256, stride 4) from a ring in local memory that moves every frame, so
 * every world draw was fetched, decoded and staged whole again, every frame.
 * A draw whose enabled attributes come from separate spans (eng_vc_probe's
 * union rule: spans closer than 64 bytes are one) is split instead, one
 * stream per span. Each stream is keyed, hashed and kept resident on its own,
 * the way a whole draw is above, and only the streams that missed are
 * fetched and staged. Disabled attributes (their current value, the same for
 * every vertex) ride in stream 0. The pipeline is created for the stream map
 * (rsx_vertex_layout_plan.stream_map) and the backend binds each stream as
 * its own vertex buffer (rsx_draw_backend.draw_vbufs). Streams are numbered
 * by their lowest slot, so a stream that moves keeps its number and the draw
 * keeps its pipeline.
 *   PS3RECOMP_ENG_VCACHE_SPLIT=0  whole-draw keys only, as before */
static int eng_vc_split_on(void)
{
    if (s_vc_split_enabled < 0) {
        const char* e = getenv("PS3RECOMP_ENG_VCACHE_SPLIT");
        s_vc_split_enabled = !(e && *e && atoi(e) == 0);
    }
    return s_vc_split_enabled && g.be->draw_vbufs && eng_vc_on();
}

typedef struct {
    u8 slots[RSX_DSP_NUM_VERTEX_ATTR];   /* ascending */
    u32 n_slots;
    const u8* lo;                        /* the stream's span; NULL: none */
    const u8* hi;
} eng_vc_stream;

static u64 eng_vc_stream_key(const eng_vc_stream* st)
{
    const rsx_vertex_fetch_plan* pl = &dc.fetch_plan;
    u64 key = eng_vc_mix(0x13198A2E03707344ull, st->n_slots);
    key = eng_vc_mix(key, dc.n_refs);
    key = eng_vc_mix(key, ((u64)dc.refs[0].vertex_id << 32) | dc.refs[0].base_index);
    key = eng_vc_mix(key, ((u64)pl->base_offset << 32) | pl->divider_mask);
    for (u32 k = 0; k < st->n_slots; k++) {
        const u32 a = pl->layout.attrs[st->slots[k]];
        const rsx_vertex_fetch_attr* f = &pl->attr[a];
        key = eng_vc_mix(key, ((u64)a << 56) ^ ((u64)f->desc.type << 48) ^ ((u64)f->desc.size << 40) ^
                              ((u64)((pl->divider_mask >> a) & 1u) << 39) ^ ((u64)f->desc.frequency << 16) ^
                              ((u64)f->stride << 8) ^ f->elem_size);
        { u64 dv[2]; memcpy(dv, f->default_value, sizeof dv);
          key = eng_vc_mix(eng_vc_mix(key, dv[0]), dv[1]); }
        if (f->desc.type && f->desc.size) {
            key = eng_vc_mix(key, (u64)(uintptr_t)f->stable_base);
            key = eng_vc_mix(key, ((u64)f->stable_first << 32) | f->stable_last);
        }
    }
    return key;
}

/* 1: a split draw, dc.streams filled (or dc.fetch_ok cleared); 0: not one,
 * the whole-draw path takes it. Call after the fetch plan is prepared, for a
 * consecutive run with the cache on. */
static int eng_vc_split_fetch(const rsx_vertex_layout_plan* layout)
{
    const rsx_vertex_fetch_plan* pl = &dc.fetch_plan;
    if (pl->inline_data || !dc.n_refs || layout->count < 2) return 0;

    const u8* lo[RSX_DSP_NUM_VERTEX_ATTR]; const u8* hi[RSX_DSP_NUM_VERTEX_ATTR];
    u8 order[RSX_DSP_NUM_VERTEX_ATTR];
    u32 ne = 0;
    for (u32 slot = 0; slot < layout->count; slot++) {
        const rsx_vertex_fetch_attr* f = &pl->attr[layout->attrs[slot]];
        if (!f->desc.type || !f->desc.size) continue;
        if (!f->elem_size || !f->stable_base) return 0;   /* not cacheable at all */
        lo[slot] = f->stable_base;
        hi[slot] = f->stable_base + (u64)(f->stable_last - f->stable_first) * f->stride + f->elem_size;
        order[ne++] = (u8)slot;
    }
    if (ne < 2) return 0;
    for (u32 i = 1; i < ne; i++)                 /* by span start */
        for (u32 j = i; j > 0 && lo[order[j]] < lo[order[j - 1]]; j--) {
            const u8 t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    eng_vc_stream st[RSX_BE_MAX_VSTREAMS + 1];
    u32 ns = 0;
    for (u32 i = 0; i < ne; i++) {
        const u32 slot = order[i];
        if (ns && lo[slot] <= st[ns - 1].hi + 64) {
            eng_vc_stream* s = &st[ns - 1];
            s->slots[s->n_slots++] = (u8)slot;
            if (hi[slot] > s->hi) s->hi = hi[slot];
            continue;
        }
        if (ns == RSX_BE_MAX_VSTREAMS) return 0;  /* too many: whole-draw */
        eng_vc_stream* s = &st[ns++];
        s->n_slots = 0;
        s->slots[s->n_slots++] = (u8)slot;
        s->lo = lo[slot]; s->hi = hi[slot];
    }
    if (ns < 2) return 0;
    /* Each stream's slots ascending, the streams by their lowest slot; then
     * the disabled slots join stream 0 (the one holding the lowest enabled
     * slot), in order. */
    for (u32 k = 0; k < ns; k++)
        for (u32 i = 1; i < st[k].n_slots; i++)
            for (u32 j = i; j > 0 && st[k].slots[j] < st[k].slots[j - 1]; j--) {
                const u8 t = st[k].slots[j]; st[k].slots[j] = st[k].slots[j - 1]; st[k].slots[j - 1] = t;
            }
    for (u32 i = 1; i < ns; i++)
        for (u32 j = i; j > 0 && st[j].slots[0] < st[j - 1].slots[0]; j--) {
            const eng_vc_stream t = st[j]; st[j] = st[j - 1]; st[j - 1] = t;
        }
    for (u32 slot = 0; slot < layout->count; slot++) {
        const rsx_vertex_fetch_attr* f = &pl->attr[layout->attrs[slot]];
        if (f->desc.type && f->desc.size) continue;
        u32 j = st[0].n_slots++;
        for (; j > 0 && st[0].slots[j - 1] > slot; j--) st[0].slots[j] = st[0].slots[j - 1];
        st[0].slots[j] = (u8)slot;
    }
    u64 map = 0;
    for (u32 k = 0; k < ns; k++)
        for (u32 j = 0; j < st[k].n_slots; j++)
            map |= (u64)k << (4u * st[k].slots[j]);

    /* Look every stream up first: a prune or a start-over while doing so
     * moves the entries already found, so look them all up again once. */
    eng_vc_entry* e[RSX_BE_MAX_VSTREAMS];
    u64 key[RSX_BE_MAX_VSTREAMS];
    for (u32 k = 0; k < ns; k++) key[k] = eng_vc_stream_key(&st[k]);
    for (int attempt = 0; ; attempt++) {
        const u32 epoch = s_vc_epoch;
        u32 k = 0;
        for (; k < ns; k++) {
            e[k] = eng_vc_find(key[k]);
            if (s_vc_epoch != epoch) break;
        }
        if (k == ns) break;
        if (attempt) return 0;
    }

    u64 content[RSX_BE_MAX_VSTREAMS];
    int hit[RSX_BE_MAX_VSTREAMS];
    u64 miss_bytes = 0;
    for (u32 k = 0; k < ns; k++) {
        const u32 stride = st[k].n_slots * 16u;
        hit[k] = 0;
        content[k] = 0;
        if (e[k] && e[k]->skip_until > g.frames) { s_vc_skip++; e[k] = NULL; }
        if (e[k]) {
            u64 c = 0x51ED270B27A3F5E1ull;
            c = eng_hash_span(st[k].lo, (u64)(st[k].hi - st[k].lo), c);
            s_vc_hashed += (u64)(st[k].hi - st[k].lo);
            content[k] = c;
            hit[k] = e[k]->vbuf && e[k]->content == c && e[k]->n == dc.n_refs &&
                     e[k]->stride == stride;
            /* Drawn this frame: the budget's eviction leaves it alone. */
            if (hit[k]) e[k]->last_frame = g.frames;
        }
        if (hit[k]) s_vc_stream_hit++; else { s_vc_stream_miss++; miss_bytes += (u64)dc.n_refs * stride; }
    }

    if (miss_bytes && !dc_reserve_verts(miss_bytes)) { dc.fetch_ok = 0; return 1; }
    u64 off = 0;
    for (u32 k = 0; k < ns; k++) {
        const u32 stride = st[k].n_slots * 16u;
        dc.streams[k].stride = stride;
        if (hit[k]) { dc.streams[k].vbuf = e[k]->vbuf; dc.streams[k].data = NULL; continue; }
        u8* base = dc.verts + off;
        for (u32 i = 0; i < dc.n_refs; i++)
            if (!rsx_vertex_fetch_slots(pl, &dc.refs[i], st[k].slots, st[k].n_slots,
                                        base + (u64)i * stride)) {
                dc.fetch_ok = 0;
                return 1;
            }
        dc.streams[k].vbuf = 0;
        dc.streams[k].data = base;
        off += (u64)dc.n_refs * stride;
    }
    for (u32 k = 0; k < ns; k++)
        if (!hit[k] && e[k])
            eng_vc_after_fetch(e[k], content[k], (const u8*)dc.streams[k].data,
                               dc.n_refs, dc.streams[k].stride);
    dc.n_streams = ns;
    dc.stream_map = map;
    dc.n_verts = dc.n_refs;
    s_vc_split++;
    return 1;
}

void rsx_draw_engine_get_vcache_stats(rsx_draw_engine_vcache_stats* out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->enabled = g.be ? eng_vc_on() : 0;
    out->hits = s_vc_hit; out->misses = s_vc_miss; out->resting = s_vc_skip;
    out->created = s_vc_new; out->hashed_bytes = s_vc_hashed;
    out->keys = s_vc_used; out->resident_bytes = s_vc_bytes;
    out->split_enabled = g.be ? eng_vc_split_on() : 0;
    out->split_draws = s_vc_split;
    out->stream_hits = s_vc_stream_hit; out->stream_misses = s_vc_stream_miss;
}

/* Turn this group's batches into a reference list, recording a CUT rather
 * than fetching a phantom vertex wherever the restart sentinel appears, then
 * collapse repeated references and fetch each unique vertex once.
 * rsx_live_draw.c's fetch_batches_hoisted (4165-4292). */
static void dc_fetch(const rsx_vertex_layout_plan* layout, int allow_remap)
{
    dc.vbuf = 0;
    if (dc.n_immediate) {
        dc.n_source_refs = dc.n_immediate;
        if (!dc_reserve_verts((u64)dc.n_immediate * layout->stride)) {
            dc.fetch_ok = 0;
            return;
        }
        for (u32 v = 0; v < dc.n_immediate; v++)
            for (u32 slot = 0; slot < layout->count; slot++)
                memcpy(dc.verts + (u64)v * layout->stride + slot * 16u,
                       dc.immediate[v][layout->attrs[slot]], 16u);
        dc.n_verts = dc.n_immediate;
        return;
    }
    for (u32 b = 0; b < dc.n_arr && dc.fetch_ok; b++) {
        if (!dc_reserve_refs(dc.arr[b].count)) { dc.fetch_ok = 0; break; }
        rsx_vertex_ref* out = dc.refs + dc.n_refs;
        for (u32 i = 0; i < dc.arr[b].count; i++) {
            out[i].vertex_id = dc.arr[b].first + i;
            out[i].base_index = 0;
        }
        dc.n_refs += dc.arr[b].count;
    }

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        /* The index array's location field is a DMA selector, not the
         * rsx_dsp location enum: 0 selects local memory. */
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;

        for (u32 b = 0; b < dc.n_idx && dc.fetch_ok; b++) {
            const u32 first = dc.idx[b].first, count = dc.idx[b].count;
            const u64 start = (u64)ia.offset + (u64)first * esz;
            const u64 bytes = (u64)count * esz;
            const u8* run = NULL;
            if (count && start <= 0xFFFFFFFFull && bytes <= 0xFFFFFFFFull &&
                start + bytes <= 0x100000000ull)
                run = eng_fetch_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
            if (run) {
                if (!dc_push_index_run(run, count, ia.is_u32, base_index, restart_en, restart_val))
                    dc.fetch_ok = 0;
                continue;
            }
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u8* p = run ? run + (size_t)i * esz
                    : eng_fetch_ptr(NULL, ia_loc, ia.offset + (first + i) * esz, esz);
                if (!p) { dc.fetch_ok = 0; break; }
                const u32 index = ia.is_u32
                    ? (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                    : (u32)((p[0] << 8) | p[1]);
                if (restart_en && index == restart_val) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                              dc.n_refs))
                        dc.fetch_ok = 0;
                    continue;
                }
                if (!dc_push_ref(index, base_index)) dc.fetch_ok = 0;
            }
        }
    }
    if (!dc.fetch_ok) return;

    dc.n_source_refs = dc.n_refs;
    /* Only a topology this engine rebuilds through an index buffer may share
     * repeated references. A line strip or a point list is drawn in the order
     * it arrived, so collapsing duplicates would reorder it. */
    int consecutive = 0;   /* refs = one run of vertex ids, one base_index */
    if (allow_remap && dc.n_refs > 1) {
        /* The draw's whole index range first: a mesh's indices cover it
         * (see rsx_vertex_remap_build_range), and fetching it in address
         * order skips the deduplication hash (~13% of Resistance's RSX
         * thread) and leaves one consecutive run, which the vertex cache
         * needs. PS3RECOMP_ENG_DEDUP=1 deduplicates always, for an A/B. */
        static int dedup = -1;
        if (dedup < 0) { const char* e = getenv("PS3RECOMP_ENG_DEDUP"); dedup = e && *e && atoi(e) != 0; }
        u32 unique = dc.n_refs, ranged = 0;
        if (!dedup && rsx_vertex_remap_build_range(&dc.ref_remap, dc.refs, dc.n_refs, &ranged)) {
            /* Already in order (a DRAW_ARRAYS run): no index buffer needed,
             * as the dedup path decided for it before. */
            int identity = ranged == dc.n_source_refs;
            for (u32 i = 0; identity && i < dc.n_source_refs; i++)
                identity = dc.ref_remap.occurrence_to_unique[i] == i;
            dc.refs_remapped = !identity;
            dc.n_refs = ranged;
            consecutive = 1;
        } else if (rsx_vertex_remap_build(&dc.ref_remap, dc.refs, dc.n_refs, &unique)) {
            dc.refs_remapped = unique < dc.n_refs;
            dc.n_refs = unique;
        }
    }
    if (!consecutive && dc.n_refs) {
        consecutive = 1;
        for (u32 i = 1; consecutive && i < dc.n_refs; i++)
            consecutive = dc.refs[i].vertex_id == dc.refs[0].vertex_id + i &&
                          dc.refs[i].base_index == dc.refs[0].base_index;
    }

    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout, eng_fetch_ptr, NULL);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_prepare_ex(&dc.fetch_plan, dc.refs, dc.n_refs, consecutive);
    eng_vc_entry* ve = NULL;
    u64 vc_content = 0;
    if (consecutive && layout->stride && eng_vc_on()) {
        if (eng_vc_split_on() && eng_vc_split_fetch(layout)) return;
        ve = eng_vc_probe(layout, &vc_content);
        if (ve && ve->vbuf && ve->content == vc_content &&
            ve->n == dc.n_refs && ve->stride == layout->stride) {
            ve->last_frame = g.frames;
            dc.vbuf = ve->vbuf;
            dc.n_verts = dc.n_refs;
            s_vc_hit++;
            return;
        }
        if (ve) s_vc_miss++;
    }
    if (!dc_reserve_verts((u64)dc.n_refs * layout->stride)) {
        dc.fetch_ok = 0;
        return;
    }
    if (!rsx_vertex_fetch_many(&dc.fetch_plan, dc.refs, dc.n_refs,
                               layout->stride ? dc.verts : NULL)) {
        dc.fetch_ok = 0;
        return;
    }
    dc.n_verts = dc.n_refs;
    if (ve) eng_vc_after_fetch(ve, vc_content, dc.verts, dc.n_refs, layout->stride);
}

/* ---- sink ---------------------------------------------------------------- */

static void sink_begin(void* user, const rsx_dispatch* r, u32 prim)
{
    (void)user; (void)prim;
    dc_reset();
    dc.inline_dropped_at_begin = r->inline_dropped;
}

/* A malformed/unbounded group must not render a truncated primitive. */
#define ENG_MAX_IMMEDIATE_VERTICES 4096u
static void sink_immediate_vertex(void* user, const rsx_dispatch* r)
{
    (void)user;
    dc.n_packets++;
    if (!dc.fetch_ok) return;
    if (dc.n_immediate == ENG_MAX_IMMEDIATE_VERTICES) {
        dc.fetch_ok = 0;
        return;
    }
    if (dc.n_immediate == dc.cap_immediate) {
        const u32 next = dc.cap_immediate ? dc.cap_immediate * 2u : 64u;
        void* p = realloc(dc.immediate, (size_t)next * sizeof(*dc.immediate));
        if (!p) { dc.fetch_ok = 0; return; }
        dc.immediate = p;
        dc.cap_immediate = next;
    }
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        float* value = dc.immediate[dc.n_immediate][attr];
        rsx_dsp_vertex_default(r, attr, value);
        /* The legacy default getter treats all-zero words as unwritten.
         * Direct and folded constants may explicitly set W/alpha to zero. */
        const u32 base = (0x1C00u + attr * 16u) >> 2;
        if (r->seen[base] || r->seen[base + 1] ||
            r->seen[base + 2] || r->seen[base + 3])
            memcpy(value, &r->regs[base], 16u);
    }
    dc.n_immediate++;
}

/* Queue a (first, count) range, extending the last one when this one starts
 * where it ended. Returns 0 when the queue is full (logged once). */
static int eng_queue_batch(eng_batch* q, u32* n, u32 first, u32 count)
{
    if (*n && q[*n - 1].first + q[*n - 1].count == first) {
        q[*n - 1].count += count;
        return 1;
    }
    if (*n >= ENG_MAX_BATCHES) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "[rsx engine] more than %u separate draw ranges in one "
                    "BEGIN/END: the rest are dropped\n", (unsigned)ENG_MAX_BATCHES); }
        return 0;
    }
    q[*n].first = first;
    q[*n].count = count;
    (*n)++;
    return 1;
}

static void sink_draw_arrays(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    eng_queue_batch(dc.arr, &dc.n_arr, first, count);
}

/* INLINE_ARRAY delivers one finished stream rather than a (first, count)
 * range; queue it as an ordinary consecutive batch and let the fetch plan
 * read the vertices out of it (rsx_vertex_fetch_plan_set_inline). */
static void sink_inline_array(void* user, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)user;
    /* Record source presence even when its declaration or length is invalid,
     * so mixed immediate/inline groups cannot draw a partial prefix. */
    dc.inl = data;
    dc.inl_bytes = bytes;
    const u32 stride = rsx_vertex_inline_layout(r, NULL);
    if (!stride || bytes < stride) return;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
}

static void sink_draw_index(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    eng_queue_batch(dc.idx, &dc.n_idx, first, count);
}

/* Which texture units this draw binds, and from where: a unit naming a
 * registered colour surface samples that live target rather than guest
 * memory (every post-process, shadow and reflection in a PS3 title is a
 * render-to-texture read back through a texture unit), a unit naming a
 * tracked zeta in DEPTH24_D8 samples its snapshot, and everything else is a
 * guest upload (rsx_live_draw.c:5986-6062). */
/* A direct copy is valid only for the entire matching physical sample
 * layout. Reinterpreted, swizzled, mipmapped and subrectangle aliases need a
 * different conversion and must not silently sample stale guest bytes. */
static int eng_color_feedback_compatible(const eng_surface* s,
                                          const rsx_dsp_texture* t,
                                          const rsx_dsp_surface* sf)
{
    const u32 base = t->format & 0x9fu;
    u32 bpp = 0, color = 0;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:        if (base == 0x85u) { bpp = 4; color = 8; } break;
    case RSX_BE_FMT_R16G16B16A16F:   if (base == 0x9au) { bpp = 8; color = 11; } break;
    case RSX_BE_FMT_R32G32B32A32F:   if (base == 0x9bu) { bpp = 16; color = 12; } break;
    case RSX_BE_FMT_R32F:            if (base == 0x9cu) { bpp = 4; color = 13; } break;
    default: break;
    }
    if (!bpp || sf->color_format != color || sf->raster_type != 1 ||
        !(t->format & 0x20u) || t->dimension != 2 ||
        t->cubemap || t->mipmaps > 1 || t->depth > 1 ||
        t->width != s->w || t->height != s->h) return 0;
    const u64 row = (u64)s->w * bpp;
    const u64 sp = s->pitch ? s->pitch : row;
    const u64 tp = t->pitch ? t->pitch : row;
    return row && sp >= row && tp == sp;
}

static u32 eng_color_feedback_failure(u32 unit, const eng_surface* s,
                                       const rsx_dsp_texture* t)
{
    static u32 logs;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    if (PS3_LOG_FIRST(logs, 8))
        fprintf(stderr, "[rsx engine] rejected colour feedback unit %u: "
                "target %ux%u fmt%u pitch%u, texture %ux%u fmt%02X pitch%u "
                "dim%u levels%u cube%u color%u raster%u\n", unit, s->w, s->h, (u32)s->fmt,
                s->pitch, t->width, t->height, t->format, t->pitch,
                t->dimension, t->mipmaps, t->cubemap, sf.color_format, sf.raster_type);
    return ENG_INVALID;
}

/* A depth snapshot is a one-channel float image. RSX presents a depth
 * texture's value on every lane its crossbar selects (RPCS3
 * VKFormats.cpp get_component_mapping: DEPTH24_D8/DEPTH16 map R,R,R,R before
 * the remap), so the snapshot is bound through a remapped view rather than
 * as is, which would read 0 from G/B and 1 from A. */
static u32 eng_zdepth_view(u32 snap, const rsx_dsp_texture* t)
{
    if (!g.be->surface_view) return snap;
    const u32 view = g.be->surface_view(g.be->user, snap, t->remap & 0xFFFFu, t->format);
    return view ? view : snap;
}

static u32 sink_bind_textures(const u32* target_slots, u32 n_targets,
                              u32 current_zslot, u32 tex_used,
                              u32 textures[RSX_BE_MAX_TEXTURES],
                              rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES])
{
    u32 mask = 0;
    u32 feedback[RSX_BE_MAX_COLOR_TARGETS] = {0};
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* A unit left enabled by an earlier draw but not sampled by this
         * program is skipped: resolving it would cost a cache check, and an
         * aliased surface a store to guest memory, for nothing. */
        if (!t.enabled || !((tex_used >> u) & 1u)) continue;
        mask |= 1u << u;
        eng_decode_sampler(t.filter, t.wrap, t.control0, &samplers[u]);
        samplers[u].border = eng_border_class(t.format, t.border_color);

        int sampled = -1, own = 0, surface_at = -1;
        /* DXT data is never a render target's output: no surface colour
         * format is block-compressed, so whatever a compressed texture names
         * came from memory. Matching it against a target at the same offset
         * found the current colour target and dropped the draw as an
         * incompatible feedback read (a 16x16 DXT1 texture over a 1280x720
         * target in Resistance: Fall of Man). Neither colour nor depth
         * surfaces are looked up for it. */
        const u32 base_fmt = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
        const int compressed = base_fmt == RSX_TEX_FMT_DXT1 ||
                               base_fmt == RSX_TEX_FMT_DXT23 ||
                               base_fmt == RSX_TEX_FMT_DXT45;
        for (u32 i = 0; i < g.n_surfaces && !compressed; i++) {
            if (!g.surfaces[i].handle || g.surfaces[i].location != t.location ||
                g.surfaces[i].offset != t.offset)
                continue;
            surface_at = (int)i;
            ENG_TOUCH(&g.surfaces[i]);
            /* A current attachment requires an ordered immutable copy; it
             * cannot be sampled directly while the host pass writes it. */
            for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) own = 1;
            if (own) break;
            sampled = (int)i;
            break;
        }
        /* The depth at this address, when it was written after any colour
         * there: a depth-only pass names its zeta as a colour target too. */
        int zsrc = -1;
        for (u32 i = 0; i < g.n_zdepths && !compressed; i++) {
            const eng_zdepth* z = &g.zdepths[i];
            if (z->location != t.location || z->offset != t.offset || !z->depth_serial)
                continue;
            if (surface_at < 0 || z->depth_serial > g.surfaces[surface_at].color_serial) {
                zsrc = (int)i;
                ENG_TOUCH(&g.zdepths[i]);
            }
            break;
        }
        if (zsrc >= 0) {
            /* DEPTH24_D8 keeps the backend's own snapshot when it has one;
             * anything else goes through guest memory. */
            if (base_fmt == RSX_TEX_FMT_DEPTH24_D8 && current_zslot != (u32)zsrc) {
                const u32 snap = eng_zdepth_snapshot((u32)zsrc, t.width, t.height);
                if (snap) { textures[u] = eng_zdepth_view(snap, &t); continue; }
            }
            const u32 conv = eng_zdepth_as_color((u32)zsrc, &t);
            if (conv) { textures[u] = conv; continue; }
            /* A backend without depth_as_color may still build the A8R8G8B8
             * bytes itself (depth_snapshot_rgba8). */
            if (!g.be->depth_as_color && !s_depth_via_guest &&
                eng_zdepth_rgba8_usable(&g.zdepths[zsrc], &t, base_fmt)) {
                const u32 snap = eng_zdepth_snapshot_rgba8((u32)zsrc, t.width, t.height);
                if (snap) {
                    const u32 view = g.be->surface_view
                        ? g.be->surface_view(g.be->user, snap, t.remap & 0xFFFFu, t.format)
                        : 0;
                    if (view || (t.remap & 0xFFFFu) == 0xAAE4u) {
                        textures[u] = view ? view : snap;
                        continue;
                    }
                }
            }
            if (eng_zdepth_to_guest((u32)zsrc)) {
                textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                               t.width, t.height, t.mipmaps, t.pitch,
                                               (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
                continue;
            }
        }
        if (own && zsrc < 0 && g.be->color_snapshot) {
            eng_surface* s = &g.surfaces[surface_at];
            rsx_dsp_surface sf;
            rsx_dsp_get_surface(&g.rsx, &sf);
            if (!eng_color_feedback_compatible(s, &t, &sf))
                return eng_color_feedback_failure(u, s, &t);
            u32 k = 0;
            while (k < n_targets && target_slots[k] != (u32)surface_at) k++;
            if (k == n_targets) return eng_color_feedback_failure(u, s, &t);
            if (!feedback[k]) {
                const u32 copy = g.be->color_snapshot(g.be->user, s->handle);
                if (!copy) return eng_color_feedback_failure(u, s, &t);
                if (s->feedback) g.be->texture_release(g.be->user, s->feedback);
                s->feedback = feedback[k] = copy;
            }
            if (g.be->surface_view) {
                textures[u] = g.be->surface_view(g.be->user, feedback[k],
                                               t.remap & 0xffffu, t.format);
                if (!textures[u]) return eng_color_feedback_failure(u, s, &t);
            } else if ((t.remap & 0xffffu) == 0xaae4u) {
                textures[u] = feedback[k];
            } else {
                return eng_color_feedback_failure(u, s, &t);
            }
            continue;
        }
        if (sampled >= 0 && zsrc < 0 &&
            g.surfaces[sampled].fmt == RSX_BE_FMT_R8G8B8A8 &&
            eng_format_reinterprets_rgba8(base_fmt) &&
            eng_surface_to_guest((u32)sampled)) {
            textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                           t.width, t.height, t.mipmaps, t.pitch,
                                           (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
            continue;
        }
        if (sampled >= 0) {
            const u32 view = g.be->surface_view
                ? g.be->surface_view(g.be->user, g.surfaces[sampled].handle,
                                     t.remap & 0xFFFFu, t.format)
                : 0;
            textures[u] = view ? view : g.surfaces[sampled].handle;
            continue;
        }
        if (base_fmt == RSX_TEX_FMT_DEPTH24_D8) {
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset && current_zslot != i) {
                    ENG_TOUCH(&g.zdepths[i]);
                    const u32 snap = eng_zdepth_snapshot(i, t.width, t.height);
                    if (snap) textures[u] = eng_zdepth_view(snap, &t);
                    break;
                }
            if (textures[u]) continue;
        }
        textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                       t.width, t.height, t.mipmaps, t.pitch,
                                       (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
    }
    return mask;
}

static u32 sink_bind_vertex_textures(
    u32 vtex_mask, u32 textures[RSX_BE_MAX_VERTEX_TEXTURES],
    rsx_be_sampler_desc samplers[RSX_BE_MAX_VERTEX_TEXTURES])
{
    u32 bound = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        eng_decode_sampler(vt.filter, vt.wrap, vt.control0, &samplers[u]);
        samplers[u].border = eng_border_class(vt.format, vt.border_color);
        /* A vertex unit has no crossbar, so its remap is the identity. */
        textures[u] = eng_texture_slot(vt.location, vt.offset, vt.format,
                                       vt.width, vt.height, vt.mipmaps,
                                       vt.pitch, 0, 0xAAE4u, 1u);
        if (textures[u]) bound |= 1u << u;
    }
    return bound;
}

/* ---- per-pass capture (diagnostic) --------------------------------------- */

/* RSX_ENGINE_PASS_DUMP=<existing dir>: capture the draws pass by pass, from
 * the first draw RSX_ENGINE_PASS_DUMP_AFTER seconds after the engine's first
 * one or, without that, from the frame after the first presented frame of at
 * least RSX_ENGINE_PASS_DUMP_MINDRAWS guest draws (default 1).
 *
 * A pass is a run of draws and clears into one target set and zeta. When the
 * set changes, each colour target of the pass that ended is read back to
 * <dir>/passNNN_tK.ppm, its zeta to <dir>/passNNN_z.pgm (stretched from its
 * smallest to its largest value), and <dir>/passes.txt gets one line: the
 * targets, the zeta and its depth range, the draw and clear counts, each
 * clear's scissor, and the first draw's state and textures. Each texture
 * names the colour surface (S) and the depth target (Z) at its address with
 * the serial of their last write -- the newer one is what was sampled -- or G
 * for guest memory. Each presented surface goes to <dir>/presentNNN.ppm,
 * named after the pass count, and the capture runs on across presents until
 * RSX_ENGINE_PASS_DUMP_MAX passes (default 64). RSX_ENGINE_PASS_DUMP_DRAWS=1
 * also writes one line per draw to <dir>/draws.txt: its pipeline, colour
 * mask, blend, depth state and the textures it samples.
 *
 * The question it answers is which pass of a multi-pass frame first comes out
 * wrong. */
static struct {
    int state;              /* 0 unread, 1 waiting, 2 capturing, 3 done/off */
    const char* dir;
    u32 min_draws;
    u32 after_s;
    u32 max_passes;
    time_t start;
    FILE* index;
    FILE* draws_log;        /* RSX_ENGINE_PASS_DUMP_DRAWS: one line a draw */
    u32 pass;
    int open;
    u32 n_targets, targets[RSX_BE_MAX_COLOR_TARGETS], zslot;
    u32 draws, clears;
    char first[2048];
} s_pd;

static int pd_open_index(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/passes.txt", s_pd.dir);
    s_pd.index = fopen(path, "w");
    if (!s_pd.index) { s_pd.state = 3; return 0; }
    if (getenv("RSX_ENGINE_PASS_DUMP_DRAWS")) {
        snprintf(path, sizeof path, "%s/draws.txt", s_pd.dir);
        s_pd.draws_log = fopen(path, "w");
    }
    s_pd.state = 2;
    s_pd.open = 0;
    return 1;
}

static int pd_capturing(void)
{
    if (s_pd.state == 0) {
        s_pd.dir = getenv("RSX_ENGINE_PASS_DUMP");
        const char* m = getenv("RSX_ENGINE_PASS_DUMP_MINDRAWS");
        s_pd.min_draws = m ? (u32)strtoul(m, NULL, 0) : 1u;
        const char* a = getenv("RSX_ENGINE_PASS_DUMP_AFTER");
        s_pd.after_s = a ? (u32)strtoul(a, NULL, 0) : 0u;
        const char* x = getenv("RSX_ENGINE_PASS_DUMP_MAX");
        s_pd.max_passes = x ? (u32)strtoul(x, NULL, 0) : 64u;
        s_pd.start = time(NULL);
        s_pd.state = (s_pd.dir && *s_pd.dir) ? 1 : 3;
    }
    if (s_pd.state == 1 && s_pd.after_s &&
        difftime(time(NULL), s_pd.start) >= (double)s_pd.after_s && pd_open_index())
        fprintf(s_pd.index, "from a draw of frame %u, %u s in\n", g.frames, s_pd.after_s);
    /* RSX_ENGINE_PASS_DUMP_TRIGGER=<file>: start when that file appears, so a
     * driver script can capture the frame it has steered the game to rather
     * than guess a time (how long a title takes to reach a scene varies from
     * run to run). Checked once a frame. */
    if (s_pd.state == 1) {
        static const char* trig = (const char*)1;
        static u32 last_frame = ~0u;
        if (trig == (const char*)1) trig = getenv("RSX_ENGINE_PASS_DUMP_TRIGGER");
        if (trig && *trig && g.frames != last_frame) {
            last_frame = g.frames;
            FILE* t = fopen(trig, "rb");
            if (t) {
                fclose(t);
                if (pd_open_index())
                    fprintf(s_pd.index, "from a draw of frame %u, on %s\n", g.frames, trig);
            }
        }
    }
    return s_pd.state == 2;
}

static float pd_half(u16 h)
{
    const u32 e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)       v = (float)m / 16777216.0f;              /* m * 2^-24 */
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else {
        v = (1.0f + (float)m / 1024.0f);
        for (u32 i = e; i > 15; i--) v *= 2.0f;
        for (u32 i = e; i < 15; i++) v *= 0.5f;
    }
    return (h & 0x8000u) ? -v : v;
}

static u8 pd_unit(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (u8)(v * 255.0f + 0.5f);
}

/* One colour surface to a binary PPM, alpha dropped, floats clamped to [0,1]. */
static void pd_write_surface(u32 slot, const char* path)
{
    const eng_surface* s = &g.surfaces[slot];
    u32 bpp;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:      bpp = 4;  break;
    case RSX_BE_FMT_R16G16B16A16F: bpp = 8;  break;
    case RSX_BE_FMT_R32G32B32A32F: bpp = 16; break;
    case RSX_BE_FMT_R32F:          bpp = 4;  break;
    default: return;
    }
    if (!s->handle || !s->w || !s->h || !g.be->readback) return;
    u8* raw = (u8*)malloc((size_t)s->w * s->h * bpp);
    u8* rgb = (u8*)malloc((size_t)s->w * s->h * 3u);
    FILE* f = (raw && rgb) ? fopen(path, "wb") : NULL;
    if (f) {
        memset(raw, 0, (size_t)s->w * s->h * bpp);
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, raw, s->w * bpp);
        for (size_t i = 0; i < (size_t)s->w * s->h; i++) {
            const u8* p = raw + i * bpp;
            u8* o = rgb + i * 3u;
            if (s->fmt == RSX_BE_FMT_R8G8B8A8) {
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            } else if (s->fmt == RSX_BE_FMT_R16G16B16A16F) {
                for (u32 c = 0; c < 3; c++) {
                    u16 h; memcpy(&h, p + c * 2u, 2);
                    o[c] = pd_unit(pd_half(h));
                }
            } else if (s->fmt == RSX_BE_FMT_R32G32B32A32F) {
                for (u32 c = 0; c < 3; c++) {
                    float v; memcpy(&v, p + c * 4u, 4);
                    o[c] = pd_unit(v);
                }
            } else {
                float v; memcpy(&v, p, 4);
                o[0] = o[1] = o[2] = pd_unit(v);
            }
        }
        fprintf(f, "P6\n%u %u\n255\n", s->w, s->h);
        fwrite(rgb, 1, (size_t)s->w * s->h * 3u, f);
        fclose(f);
    }
    free(raw);
    free(rgb);
}

/* A depth target to a greyscale PPM, stretched from its smallest to its
 * largest value so a shallow range still shows; the range goes to the index. */
static void pd_write_depth(u32 zslot, const char* path, float* lo_out, float* hi_out)
{
    const eng_zdepth* z = &g.zdepths[zslot];
    *lo_out = *hi_out = -1.0f;
    if (!g.be->depth_readback || !z->handle) return;
    const u32 w = z->used_w && z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h && z->used_h < z->h ? z->used_h : z->h;
    float* d = (float*)malloc((size_t)w * h * sizeof(float));
    u8* px = (u8*)malloc((size_t)w * h);
    FILE* f = (d && px) ? fopen(path, "wb") : NULL;
    if (f) {
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, d, w * 4u) == 0) {
            float lo = 1.0f, hi = 0.0f;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (d[i] < lo) lo = d[i];
                if (d[i] > hi) hi = d[i];
            }
            const float span = hi > lo ? hi - lo : 1.0f;
            for (size_t i = 0; i < (size_t)w * h; i++)
                px[i] = (u8)((d[i] - lo) / span * 255.0f);
            fprintf(f, "P5\n%u %u\n255\n", w, h);
            fwrite(px, 1, (size_t)w * h, f);
            *lo_out = lo; *hi_out = hi;
        }
        fclose(f);
    }
    free(d);
    free(px);
}

static void pd_close_pass(void)
{
    if (!s_pd.open) return;
    char path[1024];
    fprintf(s_pd.index, "pass %03u: draws %u clears %u |", s_pd.pass, s_pd.draws,
            s_pd.clears);
    for (u32 k = 0; k < s_pd.n_targets; k++) {
        const eng_surface* s = &g.surfaces[s_pd.targets[k]];
        fprintf(s_pd.index, " %c 0x%08X fmt %d %ux%u p%u", 'A' + (int)k, s->offset,
                (int)s->fmt, s->w, s->h, s->pitch);
        snprintf(path, sizeof path, "%s/pass%03u_t%u.ppm", s_pd.dir, s_pd.pass, k);
        pd_write_surface(s_pd.targets[k], path);
    }
    if (s_pd.zslot != ENG_INVALID) {
        float lo, hi;
        snprintf(path, sizeof path, "%s/pass%03u_z.pgm", s_pd.dir, s_pd.pass);
        pd_write_depth(s_pd.zslot, path, &lo, &hi);
        const eng_zdepth* z = &g.zdepths[s_pd.zslot];
        fprintf(s_pd.index, " | zeta 0x%08X p%u depth %.9g..%.9g (%ux%u, written %ux%u)",
                z->offset, z->pitch, lo, hi, z->w, z->h, z->used_w, z->used_h);
    }
    fprintf(s_pd.index, " |%s\n", s_pd.first);
    fflush(s_pd.index);
    s_pd.pass++;
    s_pd.open = 0;
    if (s_pd.pass >= s_pd.max_passes) {
        fprintf(s_pd.index, "stopped after %u passes\n", s_pd.pass);
        fclose(s_pd.index);
        s_pd.index = NULL;
        if (s_pd.draws_log) { fclose(s_pd.draws_log); s_pd.draws_log = NULL; }
        s_pd.state = 3;
        fprintf(stderr, "[rsx engine] pass dump: %u passes written to %s\n",
                s_pd.pass, s_pd.dir);
    }
}

/* PS3RECOMP_ENG_POLY_OFFSET=0: draw without the guest's polygon offset (the
 * behaviour before it was honoured). */
static int eng_poly_offset_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_POLY_OFFSET");
        on = !(e && *e && atoi(e) == 0);
    }
    return on;
}

/* A draw (or, with rs NULL, a clear) into this target set: close the pass
 * when the set changed, and describe the first draw of a new one. */
static void pd_note(const u32* targets, u32 n_targets, u32 zslot,
                    const rsx_be_render_state* rs, u32 pipeline, u32 tex_mask)
{
    if (!pd_capturing()) return;
    int same = s_pd.open && s_pd.n_targets == n_targets && s_pd.zslot == zslot;
    for (u32 k = 0; same && k < n_targets; k++) same = s_pd.targets[k] == targets[k];
    if (!same) {
        pd_close_pass();
        s_pd.open = 1;
        s_pd.n_targets = n_targets;
        memcpy(s_pd.targets, targets, n_targets * sizeof(u32));
        s_pd.zslot = zslot;
        s_pd.draws = s_pd.clears = 0;
        s_pd.first[0] = 0;
    }
    if (!rs) {
        s_pd.clears++;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const size_t n = strlen(s_pd.first);
        snprintf(s_pd.first + n, sizeof s_pd.first - n,
                 " [clear after %u draws, scissor %u,%u %ux%u]",
                 s_pd.draws, h & 0xFFFFu, v & 0xFFFFu, h >> 16, v >> 16);
        return;
    }
    if (s_pd.draws_log) {
        fprintf(s_pd.draws_log, "pass %03u draw %u: pipe %u cmask %08X blend %u %X/%X"
                " dt %u dw %u at %u |", s_pd.pass, s_pd.draws, pipeline, rs->color_mask,
                rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->depth_test, rs->depth_write,
                rs->alpha_test_enable);
        if (rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FILL) & 1u) {
            const u32 f = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FACTOR);
            const u32 b = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
            float ff, fb;
            memcpy(&ff, &f, sizeof ff);
            memcpy(&fb, &b, sizeof fb);
            fprintf(s_pd.draws_log, " po %g/%g", ff, fb);
        }
        if (rsx_dsp_reg(&g.rsx, M_ZPASS_PIXEL_COUNT) & 1u)
            fputs(" zp", s_pd.draws_log);
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
            if (!(tex_mask & (1u << u))) continue;
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            fprintf(s_pd.draws_log, " t%u %u:0x%08X f%02X %ux%u flt%08X r%04X", u, t.location, t.offset,
                    t.format & 0xFFu, t.width, t.height, t.filter, t.remap & 0xFFFFu);
        }
        /* vertex inputs: aN type x size / stride @ offset, or the constant
         * value when the attribute has no stream */
        rsx_vertex_layout_plan layout;
        eng_vertex_layout(&layout);
        for (u32 a = 0; a < 16; a++) {
            if (!(layout.mask & (1u << a))) continue;
            rsx_dsp_vertex_attr va;
            rsx_dsp_get_vertex_attr(&g.rsx, a, &va);
            if (va.type && va.size) {
                fprintf(s_pd.draws_log, " a%u %ux%u/%u@%u:%08X", a, va.type, va.size,
                        va.stride, va.location, va.offset);
                /* the first two elements' bytes, from the array's start */
                const u32 at = rsx_dsp_resolve_vertex_offset(
                    rsx_dsp_vertex_data_base_offset(&g.rsx), va.offset);
                const u32 n = va.stride ? va.stride + 4u : 8u;
                const u8* p = eng_guest_ptr(NULL, va.location, at, n);
                if (p) {
                    fputc('[', s_pd.draws_log);
                    for (u32 b = 0; b < n && b < 16u; b++)
                        fprintf(s_pd.draws_log, "%02X", p[b]);
                    fputc(']', s_pd.draws_log);
                }
            } else {
                float d[4];
                rsx_dsp_vertex_default(&g.rsx, a, d);
                fprintf(s_pd.draws_log, " a%u =%g,%g,%g,%g", a, d[0], d[1], d[2], d[3]);
            }
        }
        fprintf(s_pd.draws_log, "\n");
    }
    if (s_pd.draws++) return;

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    size_t at = strlen(s_pd.first);
    at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
        " rt_sel %u cfmt %u zfmt %u clip %ux%u | pipe %u dt %u dw %u df %u cmask %08X"
        " blend %u %X/%X stencil %u vpz %g+%g |",
        sf.color_target, sf.color_format, sf.depth_format, sf.clip_w, sf.clip_h,
        pipeline, rs->depth_test, rs->depth_write, rs->depth_func, rs->color_mask,
        rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->stencil_enable,
        vp.scale[2], vp.translate[2]);
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES && at < sizeof s_pd.first; u++) {
        if (!(tex_mask & (1u << u))) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* S<n> a colour surface, Z<n> a depth target, each with the serial
         * of its last write: the newer one is what the binding sampled. */
        char src[48] = "G";
        size_t sl = 0;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == t.location &&
                g.surfaces[i].offset == t.offset) {
                sl = (size_t)snprintf(src, sizeof src, "S%u@%llu", i,
                                      (unsigned long long)g.surfaces[i].color_serial);
                break;
            }
        for (u32 i = 0; i < g.n_zdepths; i++)
            if (g.zdepths[i].location == t.location && g.zdepths[i].offset == t.offset) {
                snprintf(src + sl, sizeof src - sl, "%sZ%u@%llu", sl ? "," : "", i,
                         (unsigned long long)g.zdepths[i].depth_serial);
                break;
            }
        at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
            " t%u %u:0x%08X f%02X %ux%u p%u%s r%04X zf%u %s", u, t.location, t.offset,
            t.format & 0xFFu, t.width, t.height, t.pitch, t.cubemap ? " cube" : "",
            t.remap & 0xFFFFu, t.wrap >> 28, src);
    }
}

/* Called on every present: arm on the first frame heavy enough when no delay
 * was asked for, and while capturing, close the pass in progress and write
 * the presented surface. */
static void pd_present(u32 presented)
{
    if (s_pd.state == 0) pd_capturing();
    if (s_pd.state == 1 && !s_pd.after_s && g.guest_draws >= s_pd.min_draws) {
        if (pd_open_index())
            fprintf(s_pd.index, "frame %u (after a frame of %u guest draws)\n",
                    g.frames + 1, g.guest_draws);
        return;
    }
    if (s_pd.state == 2) {
        char path[1024];
        pd_close_pass();
        if (s_pd.state != 2) return;               /* that pass was the last */
        snprintf(path, sizeof path, "%s/present%03u.ppm", s_pd.dir, s_pd.pass);
        pd_write_surface(presented, path);
        fprintf(s_pd.index, "present%03u: surface %u 0x%08X, %u guest draws\n",
                s_pd.pass, presented, g.surfaces[presented].offset, g.guest_draws);
        fflush(s_pd.index);
    }
}

/* The guest scissor intersected with a w x h target, as out[x, y, w, h] in
 * the target's sample grid. The nv40 reset is a full 4096x4096 window and a
 * never-written register reads 0 here, which the w == 0 test treats as no
 * scissor, so only a real game scissor narrows anything
 * (rsx_live_draw.c:6180-6197). */
static void eng_scissor_rect(const rsx_dsp_surface* sf, u32 w, u32 h, u32 out[4])
{
    u32 sx = 0, sy = 0, sw = w, sh = h;
    const u32 hr = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
    const u32 vr = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
    /* The scissor is in pixels: scale it to the sample grid. */
    const u32 gx = (hr & 0xFFFFu) * sf->aa_sx, gw = (hr >> 16) * sf->aa_sx;
    const u32 gy = (vr & 0xFFFFu) * sf->aa_sy, gh = (vr >> 16) * sf->aa_sy;
    if (gw > 0 && gh > 0) {
        u32 right = sx + sw, bottom = sy + sh;
        if (gx > sx) sx = gx;
        if (gy > sy) sy = gy;
        if (gx + gw < right)  right = gx + gw;
        if (gy + gh < bottom) bottom = gy + gh;
        sw = right > sx ? right - sx : 0;
        sh = bottom > sy ? bottom - sy : 0;
    }
    out[0] = sx; out[1] = sy; out[2] = sw; out[3] = sh;
}

/* What a CLEAR_SURFACE may write in a w x h target: the surface clip from
 * the origin, then the scissor, as for a draw. Returns 0 when nothing is
 * left, 1 for the whole target, 2 for part of it (out[] the rectangle).
 *
 * Resistance: Fall of Man's co-op split screen renders both views into one
 * 2x1 antialiased 1280x720 scene target: the scissor on the left half (0,0
 * 640x720), a colour and depth clear and player 1's view, then the scissor
 * on the right half (640,0 640x720), a clear and player 2's view. Cleared
 * whole, the second clear wiped player 1's view, and only the HUD, drawn
 * later into the display buffer, was left on that side. */
static int eng_clear_rect(const rsx_dsp_surface* sf, u32 w, u32 h, u32 out[4])
{
    const u32 cw = sf->clip_w && sf->clip_w < w ? sf->clip_w : w;
    const u32 ch = sf->clip_h && sf->clip_h < h ? sf->clip_h : h;
    eng_scissor_rect(sf, cw, ch, out);
    if (!out[2] || !out[3]) return 0;
    /* Only a scissor narrower than the clip makes a clear partial. A clip
     * smaller than the target alone (a 1280x720 composite pass clearing the
     * 2560x720 zeta of a 2x1 antialiased scene, every frame) stays a whole
     * clear as before: as a partial one it cost every frame a pass of its
     * own, and Resistance's campaign fell from ~30 fps to ~1. */
    const int whole = out[0] == 0 && out[1] == 0 && out[2] >= cw && out[3] >= ch;
    if (whole) { out[0] = out[1] = 0; out[2] = w; out[3] = h; return 1; }
    /* PS3RECOMP_ENG_RECT_CLEAR=0: every clear whole, as before. */
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_RECT_CLEAR"); on = !(e && *e && atoi(e) == 0); }
    if (!on) { out[0] = out[1] = 0; out[2] = w; out[3] = h; return 1; }
    return 2;
}

static void eng_log_rect_clear(const char* what, const u32 rc[4], u32 w, u32 h)
{
    static u32 logs = 0;
    if (PS3_LOG_FIRST(logs, 8))
        fprintf(stderr, "[rsx engine] %s clear limited to %u,%u %ux%u of a %ux%u target\n",
                what, rc[0], rc[1], rc[2], rc[3], w, h);
}

/* PS3RECOMP_ENG_ZPASS=1: count ZPASS samples on the host. Off by default, the
 * FIFO walker answering ZPASS reports with its old constant ("visible"):
 * deferring the labels behind each report until the GPU finishes cost
 * Resistance at 60 fps (84 -> 50 fps in an A/B, and one run crawled at
 * ~2 fps), with no visible gain found in that title. */
int rsx_draw_engine_zpass_supported(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_ZPASS");
        on = e && *e && atoi(e) != 0;
    }
    return on && g.ready && g.be && g.be->set_occlusion && g.be->occlusion_result;
}

/* The query the next draw counts into, 0 when ZPASS counting is off. */
static u32 eng_zpass_query(void)
{
    if (!(rsx_dsp_reg(&g.rsx, M_ZPASS_PIXEL_COUNT) & 1u) ||
        !rsx_draw_engine_zpass_supported())
        return 0;
    if (!g.zp_query) {
        g.zp_query = ++g.zp_last;
        if (!g.zp_query) g.zp_query = g.zp_last = g.zp_first = 1;   /* wrapped */
    }
    return g.zp_query;
}

u32 rsx_draw_engine_zpass_report(void)
{
    if (!rsx_draw_engine_zpass_supported()) return 0;
    u32 t = ++g.zp_ticket;
    if (!t) t = g.zp_ticket = 1;
    const u32 i = t % ENG_ZPASS_TICKETS;
    g.zp_tickets[i].ticket = t;
    g.zp_tickets[i].first  = g.zp_first ? g.zp_first : 1u;
    g.zp_tickets[i].last   = g.zp_last;
    /* Later draws are not this report's: they go into a query of their own,
     * which a report after them sums together with these unless a clear
     * comes first -- the counter itself only restarts at a clear. */
    g.zp_query = 0;
    return t;
}

int rsx_draw_engine_zpass_result(u32 ticket, int submit, u32* value)
{
    if (!ticket || !rsx_draw_engine_zpass_supported()) return -1;
    const u32 i = ticket % ENG_ZPASS_TICKETS;
    if (g.zp_tickets[i].ticket != ticket) return -1;
    const u32 first = g.zp_tickets[i].first, last = g.zp_tickets[i].last;
    if (last < first) { *value = 0; return 1; }      /* nothing was drawn */
    if (last - first >= ENG_ZPASS_MAX_RANGE) return -1;
    for (int pass = 0; pass < 2; pass++) {
        u64 sum = 0;
        int pending = 0, unsubmitted = 0;
        for (u32 q = first; q <= last; q++) {
            u64 n = 0;
            const int r = g.be->occlusion_result(g.be->user, q, &n);
            if (r < 0) return -1;
            if (r == 0) pending = 1;
            else if (r == 2) unsubmitted = 1;
            else sum += n;
        }
        if (!pending && !unsubmitted) {
            *value = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)sum;
            return 1;
        }
        if (pass || submit == RSX_ZPASS_POLL || !g.be->submit_and_wait) return 0;
        if (submit == RSX_ZPASS_KICK && !unsubmitted) return 0;
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    }
    return 0;
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (!g.ready || !dc.n_packets || !dc.fetch_ok) return;
    eng_arena_reset();
    /* Mixed geometry sources have no defined concatenation here. Reject the
     * whole group instead of drawing partial/misordered geometry. */
    if (dc.n_immediate && (dc.n_arr || dc.n_idx || dc.inl_bytes ||
                          r->inline_dropped != dc.inline_dropped_at_begin)) return;

    const u32 prim = g.rsx.current_primitive;
    /* Everything that becomes triangles -- lists, strips, fans, quads, quad
     * strips and polygons -- is REBUILT into one triangle list through an
     * index buffer. A host strip topology cannot express a restart cut, which
     * is the whole reason the reference engine rebuilds them, and rebuilding
     * is also what lets repeated references share an uploaded vertex. Points
     * and lines pass through as the guest issued them, and anything left that
     * would need an expansion this engine does not have is dropped rather
     * than drawn as something else. */
    rsx_topology topology = RSX_TOPOLOGY_TRIANGLES;
    int scratch = 0;
    const int rebuild = eng_topology_rebuild(prim, 0, &scratch) != 0;
    if (!rebuild) {
        if (rsx_primitive_needs_expansion(prim)) return;
        topology = rsx_primitive_topology(prim);
        if (topology == RSX_TOPOLOGY_UNSUPPORTED) return;
    }

    rsx_vertex_layout_plan layout;
    eng_vertex_layout(&layout);
    dc_fetch(&layout, rebuild);
    if (!dc.n_verts || !dc.fetch_ok) return;
    if (dc.n_streams > 1) {            /* the pipeline binds the streams */
        layout.stream_map = dc.stream_map;
        layout.n_streams = (u8)dc.n_streams;
    }

    int indexed = 0;
    u32 n_draw = dc.n_source_refs;
    if (rebuild) {
        if (!eng_topology_rebuild(prim, dc.refs_remapped, &indexed)) return;
        n_draw = indexed
            ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs,
                                                   dc.cuts, dc.n_cuts)
            : dc.n_source_refs - dc.n_source_refs % 3u;
    }
    if (!n_draw) return;

    rsx_be_render_state rs;
    rsx_draw_engine_decode_render_state(&g.rsx, &rs);

    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;
    const u32 target = targets[0];

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);

    /* A guest program pair that will not translate has no fallback: the draw
     * is dropped, as the reference engine drops it. Substituting the built-in
     * program would draw the geometry in the wrong colours, which is harder to
     * see than a missing object and hides the translation failure. */
    int pipeline_is_fixed = 1;
    u32 tex_used = 0;
    u64 fp_hash = 0;
    eng_depth_bounds_prepare(zslot, &rs);
    const u32 pipeline = eng_pipeline_get(&layout, &rs,
                                          eng_surface_format(sf.color_format),
                                          n_targets, &pipeline_is_fixed, &tex_used,
                                          &fp_hash);
    if (!pipeline) return;
    if (fp_hash && eng_debug_fp(fp_hash, pipeline)) return;

    /* Textures before any pipeline or target binding: a cache refresh can
     * replace a resource, and the backend is free to submit while doing it. */
    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(textures, 0, sizeof textures);
    memset(samplers, 0, sizeof samplers);
    memset(vtextures, 0, sizeof vtextures);
    memset(vsamplers, 0, sizeof vsamplers);
    /* The depth-bounds copy's unit is not the guest's: its texture state,
     * whatever an earlier draw left there, is not resolved. */
    const u32 dbz_bit = s_dbz.unit ? 1u << (s_dbz.unit - 1u) : 0u;
    u32 tex_mask = sink_bind_textures(targets, n_targets, zslot, tex_used & ~dbz_bit,
                                      textures, samplers);
    if (tex_mask == ENG_INVALID) return;
    if (dbz_bit && (tex_used & dbz_bit)) {
        textures[s_dbz.unit - 1u] = s_dbz.copy;
        memset(&samplers[s_dbz.unit - 1u], 0, sizeof samplers[0]);
        tex_mask |= dbz_bit;
    }
    const u32 vtex_mask = sink_bind_vertex_textures(eng_vtex_mask(), vtextures,
                                                    vsamplers);

    if (indexed) {
        if (!dc_reserve_indices(n_draw)) return;
        rsx_draw_engine_write_topology_indices(
            prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
            dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL,
            g.indices);
    }

    /* The transform constant block, then the viewport epilogue the vertex
     * program's tail multiplies by: RSX window coordinates mapped into the
     * host's clip space (rsx_live_draw.c:6143-6156). */
    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    /* The guest viewport is in pixels; an antialiased surface's clip is in
     * samples (aa_sx x aa_sy per pixel), so map NDC over the pixel extent. */
    const float Wv = W / (float)sf.aa_sx;
    const float Hv = H / (float)sf.aa_sy;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    memcpy(g.vp_cb, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16u);
    memcpy(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf);

    /* The buffered fragment constants, then fp_alpha: the layout
     * rsx_fp_decompile_buffered_ex compiled the shader against. */
    const u32 nslots = g.fp_constants->count ? g.fp_constants->count : 1u;
    /* + fp_dbounds after fp_alpha for a depth-bounded draw. */
    const u32 fp_bytes = (nslots + (s_dbz.unit ? 2u : 1u)) * 16u;
    if (g.fp_cb_cap < fp_bytes) {
        u8* n = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!n) return;
        g.fp_cb = n;
        g.fp_cb_cap = fp_bytes;
    }
    memset(g.fp_cb, 0, fp_bytes);
    if (g.fp_constants->count)
        memcpy(g.fp_cb, g.fp_constants->values, g.fp_constants->count * 16u);
    { float* alpha = (float*)(g.fp_cb + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
      /* Window convention for WPOS (rsx_fp_decompiler.c, _rsx_wpos): SET_SHADER_WINDOW
       * height in bits 0-11, origin (1 = BOTTOM) in 12-15, pixel centers (1 = INTEGER)
       * in 16-19. */
      const u32 win = rsx_dsp_reg(&g.rsx, 0x1D88);
      alpha[1] = ((win >> 12) & 0xFu) ? (float)(win & 0xFFFu) : 0.0f;
      alpha[2] = ((win >> 16) & 0xFu) ? -0.5f : 0.0f;
      /* The alpha test compares 8-bit values on an integer target
       * (rsx_fp_apply_alpha_test_buffered, rsx_fp_alpha_quantised). */
      alpha[3] = rsx_fp_alpha_quantised(rs.alpha_ref_format) ? 1.0f : 0.0f;
      if (s_dbz.unit) { alpha[4] = s_dbz.mn; alpha[5] = s_dbz.mx; alpha[6] = alpha[7] = 0.0f; } }

    pd_note(targets, n_targets, zslot, &rs, pipeline, tex_mask);

    /* The depth attachment is cleared the first time it is bound, so a title
     * that never clears its shadow zeta still tests against something. */
    const u32 depth_handle = (zslot != ENG_INVALID) ? g.zdepths[zslot].handle : 0;
    if (zslot != ENG_INVALID && !g.zdepths[zslot].cleared) {
        g.be->clear_depth_stencil(g.be->user, depth_handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                  1.0f, 0);
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].had_write = 0;
    }

    u32 handles[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n_targets; i++) handles[i] = g.surfaces[targets[i]].handle;
    g.be->bind_targets(g.be->user, handles, n_targets, depth_handle);
    g.be->bind_pipeline(g.be->user, pipeline);
    g.be->bind_vs_constants(g.be->user, g.vp_cb, ENG_VP_CB_BYTES);
    g.be->bind_ps_constants(g.be->user, g.fp_cb, fp_bytes);
    g.be->bind_textures(g.be->user, textures, samplers, tex_mask);
    g.be->bind_vertex_textures(g.be->user, vtextures, vsamplers, vtex_mask);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, W, H);

    {
        u32 sc[4];
        eng_scissor_rect(&sf, g.surfaces[target].w, g.surfaces[target].h, sc);
        g.be->set_scissor(g.be->user, sc[0], sc[1], sc[2], sc[3]);
    }
    /* Dynamic, and deliberately outside the pipeline key. */
    g.be->set_stencil_ref(g.be->user,
                          rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);
    /* SET_BLEND_COLOR (0x031C, and 0x0390 for the FP16 target's upper half),
     * read by the CONSTANT_* blend factors. It was never passed on, so those
     * draws blended against a constant of 1 (Folklore's D3D12 patch 0038). */
    if (g.be->set_blend_color) {
        const u32 bc = rsx_dsp_reg(&g.rsx, 0x031Cu);
        float rgba[4];
        if (sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16) {
            const u32 bc2 = rsx_dsp_reg(&g.rsx, 0x0390u);
            const u16 h[4] = { (u16)(bc & 0xFFFFu), (u16)(bc >> 16),
                               (u16)(bc2 & 0xFFFFu), (u16)(bc2 >> 16) };
            for (int c = 0; c < 4; c++) {
                const u32 sign = (u32)(h[c] >> 15), exp = (h[c] >> 10) & 0x1Fu, man = h[c] & 0x3FFu;
                float v = exp == 0 ? ldexpf((float)man, -24)
                        : exp == 31 ? (man ? NAN : INFINITY)
                        : ldexpf((float)(man | 0x400u), (int)exp - 25);
                rgba[c] = sign ? -v : v;
            }
        } else {
            rgba[0] = (float)((bc >> 16) & 0xFFu) / 255.0f;
            rgba[1] = (float)((bc >> 8) & 0xFFu) / 255.0f;
            rgba[2] = (float)(bc & 0xFFu) / 255.0f;
            rgba[3] = (float)(bc >> 24) / 255.0f;
        }
        g.be->set_blend_color(g.be->user, rgba);
    }
    /* The bounds too: a deferred renderer moves them between its light and
     * sky passes without changing anything else. Raw floats, as written. */
    if (g.be->set_depth_bounds) {
        const u32 lo = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MIN);
        const u32 hi = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MAX);
        float fmin, fmax;
        memcpy(&fmin, &lo, sizeof fmin);
        memcpy(&fmax, &hi, sizeof fmax);
        g.be->set_depth_bounds(g.be->user, fmin, fmax);
    }

    /* Polygon offset: dynamic state, like the stencil reference. The
     * enable that applies is the one for what the primitive rasterises as. */
    if (g.be->set_depth_bias) {
        float slope = 0.0f, units = 0.0f;
        const u32 en = topology == RSX_TOPOLOGY_POINTS ? M_POLY_OFFSET_POINT
                     : (topology == RSX_TOPOLOGY_LINES || topology == RSX_TOPOLOGY_LINE_STRIP)
                       ? M_POLY_OFFSET_LINE : M_POLY_OFFSET_FILL;
        if (eng_poly_offset_on() && (rsx_dsp_reg(&g.rsx, en) & 1u)) {
            const u32 f = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FACTOR);
            const u32 b = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
            memcpy(&slope, &f, sizeof slope);
            memcpy(&units, &b, sizeof units);
            if (slope != slope) slope = 0.0f;   /* NaN */
            if (units != units) units = 0.0f;
        }
        g.be->set_depth_bias(g.be->user, slope, units,
                             /* SURFACE_FORMAT depth: 1 Z16, 2 Z24S8 */
                             sf.depth_format == 1u ? 16u : 24u);
    }
    if (g.be->set_occlusion) g.be->set_occlusion(g.be->user, eng_zpass_query());

    const u32 uploaded = indexed ? dc.n_verts : n_draw;
    if (dc.n_streams > 1)
        g.be->draw_vbufs(g.be->user, topology, dc.streams, dc.n_streams, uploaded,
                         indexed ? g.indices : NULL, indexed ? n_draw : 0);
    else if (dc.vbuf)
        g.be->draw_vbuf(g.be->user, topology, dc.vbuf, uploaded, layout.stride,
                        indexed ? g.indices : NULL, indexed ? n_draw : 0);
    else
        g.be->draw(g.be->user, topology, dc.verts, uploaded, layout.stride,
                   indexed ? g.indices : NULL, indexed ? n_draw : 0);
    /* Counted only when the draw ran the guest's OWN programs, which is what
     * the test hook means: a draw through the built-in pair is a draw, not
     * evidence that the decompile-translate-compile path worked. */
    if (!pipeline_is_fixed) g.guest_draws++;
    eng_vpc_log_draw();

    if (rs.color_mask) eng_surfaces_wrote(targets, n_targets);
    if (zslot != ENG_INVALID && rs.depth_test && rs.depth_write) {
        g.zdepths[zslot].had_write = 1;
        /* A published depth image is only good until the next depth write,
         * as after a clear: a reader later in the stream resolves again.
         * Kept across writes, a pass that reconstructs positions from depth
         * after more geometry was drawn (occlusion volumes, decals, soft
         * particles) sampled a snapshot an earlier reader had frozen.
         * eng_zdepth_snapshot releases the previous snapshot when it takes
         * the next one, so this does not grow the object table. */
        g.zdepths[zslot].snapshot_valid = 0;
        eng_zdepth_wrote(zslot, &sf);
    }
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (!g.ready) return;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;
    if (pd_capturing()) {
        rsx_dsp_surface zs;
        rsx_dsp_get_surface(&g.rsx, &zs);
        pd_note(targets, n_targets,
                eng_zdepth_get(zs.zeta_location, zs.zeta_offset, zs.clip_w, zs.clip_h),
                NULL, 0, 0);
    }

    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G |
                RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float rgba[4] = {
            (float)((c >> 16) & 0xFF) / 255.0f,
            (float)((c >>  8) & 0xFF) / 255.0f,
            (float)( c        & 0xFF) / 255.0f,
            (float)((c >> 24) & 0xFF) / 255.0f,
        };
        /* Every target the set names, not only A: a deferred pass clears its
         * whole G-buffer in one CLEAR_SURFACE, and B, C and D would otherwise
         * keep whatever the last frame left in them. Each only where the
         * clip and the scissor let the clear through (eng_clear_rect). */
        int wrote = 0;
        for (u32 i = 0; i < n_targets; i++) {
            const eng_surface* s = &g.surfaces[targets[i]];
            u32 rc[4];
            const int part = eng_clear_rect(&sf, s->w, s->h, rc);
            if (!part) continue;
            if (part == 2 && g.be->clear_color_rect) {
                eng_log_rect_clear("colour", rc, s->w, s->h);
                g.be->clear_color_rect(g.be->user, s->handle, rgba,
                                       rc[0], rc[1], rc[2], rc[3]);
            } else
                g.be->clear_color(g.be->user, s->handle, rgba);
            wrote = 1;
        }
        if (wrote) eng_surfaces_wrote(targets, n_targets);
    }
    if (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) {
        const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                         sf.clip_w, sf.clip_h);
        if (zslot == ENG_INVALID) return;
        /* A depth target is at least the canvas (eng_zdepth_get), so a
         * shadow map's clip is smaller than it all the same; a clear that
         * covers everything the zeta was ever bound with is a whole clear,
         * which the backend folds into the next pass. */
        const eng_zdepth* z = &g.zdepths[zslot];
        u32 rc[4];
        const int part = eng_clear_rect(&sf, z->w, z->h, rc);
        if (!part) return;
        const int partial = part == 2 && g.be->clear_depth_stencil_rect &&
                            !(rc[0] == 0 && rc[1] == 0 &&
                              rc[2] >= z->ext_w && rc[3] >= z->ext_h);
        u32 flags = 0;
        if (mask & RSX_CLEAR_DEPTH)   flags |= RSX_BE_CLEAR_DEPTH;
        if (mask & RSX_CLEAR_STENCIL) flags |= RSX_BE_CLEAR_STENCIL;
        /* ZSTENCIL_CLEAR_VALUE is laid out as the zeta is: depth24 << 8 |
         * stencil8 for Z24S8, a bare depth16 for Z16 (SURFACE_FORMAT depth
         * field 1). Read as Z24S8, a Z16 surface's 0xFFFF -- its far plane --
         * would clear it to 255/16777215, nearly the near plane, and every
         * LESS or LEQUAL draw into it would then fail. The nv40 reset
         * 0xFFFFFF00 seeded by rsx_dispatch_init clears to 1.0 / 0, and the
         * FIFO walker writes it again at every flip, as the RSX does. A 0 is
         * the near plane: a title that sets 0 and never writes the register
         * again gets the far plane back from the next flip, not from here. */
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        const int z16 = sf.depth_format == 1u;
        const float depth = z16 ? (float)(zs & 0xFFFFu) / 65535.0f
                                : (float)(zs >> 8) / 16777215.0f;
        const u32 handle = g.zdepths[zslot].handle;
        if (partial) {
            eng_log_rect_clear("depth", rc, z->w, z->h);
            /* The rest of a target nothing initialised yet starts as the
             * first bind would leave it (sink_end). */
            if (!g.zdepths[zslot].cleared)
                g.be->clear_depth_stencil(g.be->user, handle,
                                          RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                          1.0f, 0);
            g.be->clear_depth_stencil_rect(g.be->user, handle, flags, depth,
                                           z16 ? 0 : (u8)(zs & 0xFFu),
                                           rc[0], rc[1], rc[2], rc[3]);
        } else {
            g.be->clear_depth_stencil(g.be->user, handle, flags,
                                      depth, z16 ? 0 : (u8)(zs & 0xFFu));
        }
        g.zdepths[zslot].cleared = 1;
        if (mask & RSX_CLEAR_DEPTH) {
            /* A clear invalidates the older published depth image; the next
             * texture consumer resolves the newly written pass exactly once.
             * Past a partial clear the rest still holds what was drawn. */
            if (!partial) g.zdepths[zslot].had_write = 0;
            g.zdepths[zslot].snapshot_valid = 0;
            eng_zdepth_wrote(zslot, &sf);
        }
    }
}

/* Resolve a flip's buffer id to a REGISTERED surface rather than whatever is
 * currently bound: the live target at that instant is often an offscreen
 * shadow or post-process surface, and copying it presents black despite the
 * scene's draws having executed (rsx_live_draw.c:7557-7570). */
static u32 eng_present_surface(u32 buffer_id)
{
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const eng_display_buffer* d = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == d->location &&
                g.surfaces[i].offset == d->offset)
                return i;
    }
    return eng_current_surface();
}

static void eng_present(u32 buffer_id)
{
    if (!g.ready) return;
    const u32 target = eng_present_surface(buffer_id);
    if (target == ENG_INVALID) return;
    ENG_TOUCH(&g.surfaces[target]);
    static const char* s_frame_dump = (const char*)1;   /* per present: read once */
    if (s_frame_dump == (const char*)1) s_frame_dump = getenv("PS3RECOMP_METAL_FRAME_DUMP");
    if (s_frame_dump && g.frames % 120 == 0) {
        u32 current = eng_current_surface();
        fprintf(stderr, "[rsx capture] frame=%u buffer=%u selected=%u current=%u draws=%u surfaces=%u\n",
                g.frames, buffer_id, target, current, g.guest_draws, g.n_surfaces);
        for (u32 i = 0; i < g.n_surfaces; ++i)
            fprintf(stderr, "[rsx capture] surface %u loc=%u offset=%08X size=%ux%u handle=%u\n",
                    i, g.surfaces[i].location, g.surfaces[i].offset,
                    g.surfaces[i].w, g.surfaces[i].h, g.surfaces[i].handle);
    }
    pd_present(target);
    g.last_present_surface = target;
    g.be->present(g.be->user, g.surfaces[target].handle);
    g.frames++;
    eng_vc_frame_end();
    eng_vpc_log_frame();
    g.last_guest_draws = g.guest_draws;
    g.guest_draws = 0;
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    g.last_flip_buffer = arg & 7u;
    eng_present(g.last_flip_buffer);
}

/* ---- public API ---------------------------------------------------------- */

void rsx_draw_engine_set_backend(const rsx_draw_backend* backend)
{
    g.be = backend;
}

void rsx_draw_engine_set_default(int on)
{
    g.default_on = on;
}

int rsx_draw_engine_enabled(void)
{
    static int cached = -1;
    static int env_seen = 0;
    static int env_on = 0;
    if (!env_seen) {
        const char* e = getenv("PS3RECOMP_RSX_ENGINE");
        env_seen = 1;
        if (e && *e) env_on = (strcmp(e, "dispatch") == 0) ? 1 : -1;
    }
    /* The environment is authoritative in both directions; without it the
     * backend's own default decides. Cached after the first backend has
     * registered, so the FIFO walker pays one compare. */
    const int on = env_on > 0 ? 1 : (env_on < 0 ? 0 : g.default_on);
    if (!g.be) return 0;
    if (cached < 0) cached = on;
    return cached;
}

int rsx_draw_engine_init(u32 width, u32 height)
{
    if (!g.be) return -1;
    g.width  = width  ? width  : 1280;
    g.height = height ? height : 720;
    { const char* e = getenv("PS3RECOMP_ENG_TEXHASH_FULL");
      s_texhash_full_always = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3RECOMP_ENG_DEPTH_VIA_GUEST");
      s_depth_via_guest = e && *e && strcmp(e, "0") != 0; }
    /* The vertex cache's knobs are read again at each init. */
    s_vc_enabled = s_vc_prune_on = s_vc_stats_on = s_vc_split_enabled = -1;
    s_vc_hit = s_vc_miss = s_vc_skip = s_vc_new = s_vc_hashed = 0;
    s_vc_split = s_vc_stream_hit = s_vc_stream_miss = 0;
    g.vp_cb = (u8*)calloc(1, ENG_VP_CB_BYTES);
    if (!g.vp_cb) return -1;
    if (g.be->init && g.be->init(g.be->user, g.width, g.height) != 0) {
        free(g.vp_cb); g.vp_cb = NULL;
        return -1;
    }
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear            = sink_clear;
    sink.begin            = sink_begin;
    sink.end              = sink_end;
    sink.draw_arrays      = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.inline_array     = sink_inline_array;
    sink.immediate_vertex = sink_immediate_vertex;
    sink.flip             = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);
    g.ready = 1;
    /* Display buffers the title registered before the backend came up. */
    cellGcm_replay_display_buffers_to_engine();
    fprintf(stderr, "[rsx engine] register-file draw engine up (%ux%u)\n",
            g.width, g.height);
    if (eng_vc_on())
        fprintf(stderr, "[rsx engine] vertex cache on (PS3RECOMP_ENG_VCACHE=0 turns it off)\n");
    return 0;
}

void rsx_draw_engine_shutdown(void)
{
    if (!g.be) return;
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_SHUTDOWN);
    eng_vc_clear();
    for (u32 i = 0; i < g.n_textures; i++)
        if (g.textures[i].handle) g.be->texture_release(g.be->user, g.textures[i].handle);
    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].handle) g.be->pipeline_release(g.be->user, g.pipelines[i].handle);
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].feedback) g.be->texture_release(g.be->user, g.surfaces[i].feedback);
        if (g.surfaces[i].handle) g.be->color_target_release(g.be->user, g.surfaces[i].handle);
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (g.zdepths[i].conv) g.be->texture_release(g.be->user, g.zdepths[i].conv);
        if (g.zdepths[i].dbz_copy) g.be->texture_release(g.be->user, g.zdepths[i].dbz_copy);
        if (g.zdepths[i].handle) g.be->depth_target_release(g.be->user, g.zdepths[i].handle);
    }
    if (g.ready && g.be->shutdown) g.be->shutdown(g.be->user);

    rsx_draw_engine_set_guest_memory(NULL, NULL);
    free(dc.refs); free(dc.cuts); free(dc.verts); free(dc.immediate);
    rsx_vertex_remap_destroy(&dc.ref_remap);
    memset(&dc, 0, sizeof dc);
    free(g.tex_staging); free(g.vp_cb); free(g.fp_cb); free(g.indices);
    const rsx_draw_backend* be = g.be;
    memset(&g, 0, sizeof g);
    g.be = be;
    eng_memo_reset();
}

void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (!g.ready) return;
    /* The subchannel is a binding slot, not an engine selector, and a title
     * whose SPU-built command lists bind NV4097 elsewhere would otherwise
     * store its state in the wrong register bank.  Standard NV4097 methods
     * (< 0x2000) strip the subchannel with & 0x1FFC.  Driver methods
     * (0xE9xx, 0xEBxx) encode the subchannel bits as part of the address;
     * preserve them with the wider mask. */
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    /* CLEAR_REPORT_VALUE(ZPASS_PIXEL_CNT) restarts the count: the next draw
     * opens a new query and the next report sums from there. */
    if (m == M_CLEAR_REPORT_VALUE && arg == 1u) {
        g.zp_query = 0;
        g.zp_first = g.zp_last + 1u;
    }
    rsx_dispatch_method(&g.rsx, m, arg);
}

void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    eng_display_buffer* d = &g.display_buffers[buffer_id];
    d->location = location;
    d->offset = offset;
    d->pitch = pitch;
    d->width = width;
    d->height = height;
    d->valid = width && height;
}

void rsx_draw_engine_flush(void)
{
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

void rsx_draw_engine_present(void)
{
    /* A host that drives the flip itself gets whichever buffer the guest's
     * own last flip named, and display buffer 0 before there has been one:
     * a runner that both consumes 0xE944 and calls here would otherwise
     * re-present a double-buffered title's OTHER scanout. Presenting twice is
     * harmless either way -- the engine never clears the surface, so a second
     * present just blits the same image again. */
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_present_buffer(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_note_flip(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
}

u32 rsx_draw_engine_guest_draws(void)
{
    return g.last_guest_draws;
}

u32 rsx_draw_engine_readback_center(void)
{
    if (!g.ready || !g.be->readback) return 0;
    const u32 slot = g.last_present_surface;
    if (slot >= g.n_surfaces) return 0;
    const eng_surface* s = &g.surfaces[slot];
    if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->w || !s->h) return 0;
    u8 px[4] = { 0, 0, 0, 0 };
    g.be->readback(g.be->user, s->handle, s->w / 2u, s->h / 2u, 1, 1, px, 4);
    /* Decoded rows are R,G,B,A; the guest's own clear colour is A8R8G8B8, and
     * a host comparing the two wants them in the same order. */
    return ((u32)px[3] << 24) | ((u32)px[0] << 16) |
           ((u32)px[1] << 8)  |  (u32)px[2];
}
