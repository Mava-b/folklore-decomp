/*
 * ps3recomp - Track B live NV4097 draw path (implementation)
 *
 * See rsx_live_draw.h. This is the validated capture-replay D3D12 engine
 * (libs/video/tests/replay_main.c) lifted into a runtime module and driven by
 * the live FIFO consumer instead of an .rxs file:
 *   - rsx_dispatch register-file model (shared, unchanged)
 *   - NV40 VP/FP -> HLSL decompilers (shared, unchanged)
 *   - B1 render/sampler state -> D3D12 PSO + dynamic samplers + mip chains
 *
 * Differences from the harness:
 *   - guest memory comes from an injected resolver (the runtime's vm_base map),
 *     not a private arena;
 *   - present goes to a swap chain bound to the runtime's window, not a PPM
 *     readback;
 *   - the whole engine is gated behind RSX_DRAW (default ON; "0" = off).
 *
 * Clean-room: NV40 ISA/register facts from envytools rnndb + Mesa nv30 +
 * psdevwiki; RPCS3 as a read-only fact oracle only.
 */

#include "rsx_live_draw.h"
#include "ps3emu/yz_runtime_config.h"
#include "ps3emu/yz_frontier_trace.h"

#if !defined(_WIN32)

/* Non-Windows: the whole path is a no-op (D3D12 is Windows-only). */
int  rsx_live_draw_enabled(void) { return 0; }
int  rsx_live_draw_init(void* hwnd, u32 w, u32 h, rsx_live_guest_ptr_fn f, void* u)
{ (void)hwnd; (void)w; (void)h; (void)f; (void)u; return 0; }
void rsx_live_draw_seed_registers(const u32* r, u32 n) { (void)r; (void)n; }
void rsx_live_draw_seed_transform_program(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_seed_transform_constants(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_set_display_buffer(
    u32 b, u32 l, u32 o, u32 p, u32 w, u32 h)
{ (void)b; (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_live_draw_method(u32 m, u32 a) { (void)m; (void)a; }
void rsx_live_draw_set_fifo_position(u32 g, u32 p) { (void)g; (void)p; }
void rsx_live_draw_note_inline_transfer(u32 d, u32 o, u32 v)
{ (void)d; (void)o; (void)v; }
int  rsx_live_draw_blit(u32 sl, u32 sa, u32 sp, u32 dl, u32 da, u32 dp, u32 w, u32 h)
{ (void)sl; (void)sa; (void)sp; (void)dl; (void)da; (void)dp; (void)w; (void)h; return 0; }
int  rsx_live_draw_resolve_blit(u32 sl, u32 sa, u32 sp, u32 ds, u32 dt, u32 dl, u32 da,
                                u32 dp, u32 x, u32 y, u32 w, u32 h)
{ (void)sl; (void)sa; (void)sp; (void)ds; (void)dt; (void)dl; (void)da; (void)dp;
  (void)x; (void)y; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 sl, u32 sa, u32 dl, u32 da)
{ (void)sl; (void)sa; (void)dl; (void)da; }
void rsx_live_draw_flush(void) {}
void rsx_live_draw_present(u32 b) { (void)b; }
void rsx_live_draw_set_movie_mode(int on) { (void)on; }
void rsx_live_draw_present_rgba(const uint8_t* r, u32 w, u32 h) { (void)r; (void)w; (void)h; }
void rsx_live_draw_set_overlay(rsx_live_overlay_fn fn) { (void)fn; }
void rsx_live_draw_set_render_scale(u32 s) { (void)s; }
u32  rsx_live_draw_get_render_scale(void) { return 1; }
void rsx_live_draw_set_post_aa(int m) { (void)m; }
void rsx_live_draw_set_aspect(float a) { (void)a; }
float rsx_live_draw_get_aspect(void) { return 16.0f / 9.0f; }
void rsx_live_draw_set_async_shaders(int e) { (void)e; }
void rsx_live_draw_set_anisotropy(u32 l) { (void)l; }
void rsx_live_draw_set_texture_lod_bias(int h) { (void)h; }
void rsx_live_draw_set_sharpening(u32 p) { (void)p; }
void rsx_live_draw_set_ps_replacement(u64 h, const char* s) { (void)h; (void)s; }
void rsx_live_draw_get_shader_stats(u32* p, u32* b, u32* s)
{ if (p) *p = 0; if (b) *b = 0; if (s) *s = 0; }
u32  rsx_live_draw_get_frames(void) { return 0; }
void* rsx_live_draw_get_present_thread_handle(void) { return NULL; }
u32  rsx_live_draw_get_last_draws(void) { return 0; }
double rsx_live_draw_get_present_fps(void) { return 0.0; }
void rsx_live_draw_dump_present_samples(void) {}
void rsx_live_draw_a010_probe_begin(void) {}
int  rsx_live_draw_a010_probe_active(void) { return 0; }
int  rsx_live_draw_a010_world_ready(void) { return 1; }
int  rsx_live_draw_debug_dump_surface(u32 l, u32 o, const char* p)
{ (void)l; (void)o; (void)p; return -1; }
void rsx_live_draw_set_a010_camera_matrix(const float* m) { (void)m; }
void rsx_live_draw_shutdown(void) {}

#else /* _WIN32 */

#define _CRT_SECURE_NO_WARNINGS

#include "rsx_dispatch.h"
#include "rsx_fp_decompiler.h"
static unsigned long long g_ld_bind_white = 0, g_ld_bind_real = 0, g_ld_bind_surf = 0;
/* Set once PS1 VRAM actually holds a drawn frame (see the PS1_PC heartbeat).
 * Every surface dump before this is an early-boot snapshot of a framebuffer the
 * PS1 has not drawn into yet -- which is what made nonblack=0 look like a broken
 * pipeline when it was the correct reading of an empty source. */
static volatile long g_ld_ps1_vram_ready = 0;
#include "rsx_restart_cuts.h"
#include "rsx_vertex_compact.h"
#include "rsx_geometry_cache.h"
#include "rsx_vp_decompiler.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(YZ_PERF_CLEAN)
#define LD_DIAG_ENABLED(name) 0
#else
/* Cached per call site: several of these sit on per-draw paths. */
#define LD_DIAG_ENABLED(name) ({ static int _ld_diag = -1; \
    if (_ld_diag < 0) _ld_diag = getenv(name) != NULL; _ld_diag; })
#endif
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include "rsx_vertex_formats.h"
#include <sys/stat.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* Armed by the a010 AUTH repair only after its animation palette is resident. */
extern volatile LONG g_yz_a010_reference_camera_active;
/* Published when the Sunshine Orphanage AUTH root becomes live. */
extern volatile LONG g_yz_a010_root_active;
/* Opt-in headless gameplay-proof handshake, owned jointly with cellPad.c. */
extern volatile LONG g_yz_movement_proof_phase;
extern volatile unsigned long long g_yz_auto_start_tick;

/* ---------------------------------------------------------------------------
 * Engine state (module-static; single live RSX)
 * -----------------------------------------------------------------------*/

#define LD_SWAP_BUFFERS  2
#define MAX_SURFACES     64
#define MAX_TEXTURES     1024
#define MAX_RETIRED_TEXTURES 4096
#define MAX_VTEX         64
#define MAX_SAMPLERS     128
/*
 * A full live boot carries shader pairs from startup movies, menus, and AUTH
 * into the orphanage.  The old 256-entry cap was already exhausted there,
 * causing 155/328 sampled a010 groups to return NULL from get_pso().  The
 * one-frame reference alone uses 197 pairs.  A full visible boot through
 * orphanage and into Akiyama measured 2,296 distinct requested PSO keys before
 * the first presentation stall, so use the next power of two with headroom.
 */
#define MAX_PSOS         8192
#define FORMER_MAX_SHADER_BLOBS 2048
#define MAX_SHADER_BLOBS 8192
#define MAX_REJECTED_PSO_KEYS 8192
/* Frame pipelining (ld_submit_frame): the transient rings are split into two
 * banks, one per frame in flight, so the CPU records frame N+1 into one bank
 * while the GPU still reads frame N from the other. Every ring keeps the
 * capacity a single frame had before (the totals below are doubled). */
#define LD_BANKS         2
#define UPLOAD_SIZE      (128u * 1024 * 1024)
#define UPLOAD_BANK_SIZE (UPLOAD_SIZE / LD_BANKS)
/* PSO lookup: open-addressed index over psos[] (the linear scan cost a few
 * hundred probes per draw once a scene had built up its PSOs). */
#define PSO_INDEX_SLOTS  (MAX_PSOS * 2)
#define SHADER_DISK_CACHE_MAGIC   0x43535A59u /* "YZSC" */
/* v2: OP_KIL now emits discard (was a dropped TODO) — cached v1 pixel
 * shaders would silently keep the old no-discard codegen. */
#define SHADER_DISK_CACHE_VERSION 2u
#define SHADER_DISK_CACHE_MAX_BLOB (4u * 1024u * 1024u)

#define SRV_WHITE        0
#define SRV_SURFACE_BASE 1
#define SRV_ZDEPTH_BASE  (SRV_SURFACE_BASE + MAX_SURFACES)
#define SRV_TEXTURE_BASE (SRV_ZDEPTH_BASE + MAX_SURFACES)
#define SRV_VTEX_BASE    (SRV_TEXTURE_BASE + MAX_TEXTURES)
#define SRV_HEAP_SLOTS   (SRV_VTEX_BASE + MAX_VTEX)
#define SRV_DEPTH_SOURCE_BASE SRV_HEAP_SLOTS
#define UAV_ZDEPTH_BASE   (SRV_DEPTH_SOURCE_BASE + MAX_SURFACES)
/* Per-surface copy a draw samples when it reads its own render target. */
#define SRV_FEEDBACK_BASE (UAV_ZDEPTH_BASE + MAX_SURFACES)
#define SRV_CPU_HEAP_SLOTS (SRV_FEEDBACK_BASE + MAX_SURFACES)
/* The host movie frame the present pass scales to the window. */
#define SRV_PRESENT_MOVIE SRV_CPU_HEAP_SLOTS
#define SRV_CPU_HEAP_TOTAL (SRV_CPU_HEAP_SLOTS + 1)
#define SRV_TABLE_SIZE   16
/* Both banks together; each frame gets SRV_BANK_TABLES. */
#define SRV_RING_TABLES  8192
#define SRV_BANK_TABLES  (SRV_RING_TABLES / LD_BANKS)

#define SMP_DEFAULT      0
#define SMP_CACHE_SLOTS  MAX_SAMPLERS
#define SMP_TABLE_SIZE   16
/* A shader-visible SAMPLER heap is hard-capped at 2048 descriptors by D3D12
 * (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE). SMP_RING_TABLES*SMP_TABLE_SIZE
 * must stay <= 2048, else CreateDescriptorHeap fails. Identical sampler
 * tables share entries, so this is 128 unique tables between GPU flushes. */
#define SMP_RING_TABLES  128

#define CB_BLOCK_BYTES   ((512 + 2) * 16)
#define CB_BLOCK_ALIGNED ((CB_BLOCK_BYTES + 255) & ~255u)
#define CB_RING_BYTES    (CB_BLOCK_ALIGNED * SRV_RING_TABLES)
#define PS_CB_RING_BYTES CB_RING_BYTES
#define CB_BANK_BYTES    (CB_RING_BYTES / LD_BANKS)
#define PS_CB_BANK_BYTES (PS_CB_RING_BYTES / LD_BANKS)
#define LD_VARIANT_SET_CAPACITY (MAX_SHADER_BLOBS * 2u)

#define VERT_STRIDE      (16 * 4 * 4)   /* 16 attrs * float4                  */
#define VERT_BUFFER_SIZE (512u * 1024u * 1024u)
#define INDEX_BUFFER_SIZE (128u * 1024u * 1024u)
#define VERT_BANK_SIZE   (VERT_BUFFER_SIZE / LD_BANKS)
#define INDEX_BANK_SIZE  (INDEX_BUFFER_SIZE / LD_BANKS)
#define LD_INVALID_SURFACE 0xFFFFFFFFu

/* gcm texture format bytes (mirror rsx_dispatch.h) */
#define TEX_FMT_B8         0x81
#define TEX_FMT_A1R5G5B5   0x82
#define TEX_FMT_A4R4G4B4   0x83
#define TEX_FMT_R5G6B5     0x84
#define TEX_FMT_A8R8G8B8   0x85
#define TEX_FMT_DXT1       0x86
#define TEX_FMT_DXT23      0x87
#define TEX_FMT_DXT45      0x88
#define TEX_FMT_G8B8       0x8B
#define TEX_FMT_DEPTH24_D8 0x90
#define TEX_FMT_LINEAR     0x20
#define TEX_FMT_UNNORM     0x40
#define TEX_FMT_BASE_MASK  0x9F

typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    u32 w, h;
    DXGI_FORMAT fmt;
    u32 scale;                /* host pixels per guest pixel, vertically (ld_surface_scale_for) */
    float scale_x;            /* ... and horizontally: scale x the ultrawide widening */
    ID3D12Resource* fb_tex;   /* feedback copy, see surface_feedback_srv */
#if !defined(YZ_PERF_CLEAN)
    u32 resource_serial;
    u32 last_write_generation;
    u32 last_write_kind;
    u32 last_create_generation;
    u32 last_draw_generation;
    u32 last_clear_generation;
    u32 last_copy_generation;
    u32 last_blit_generation;
    u32 last_resolve_generation;
    u32 last_other_generation;
    u32 last_guest_blit_generation;
    u32 last_present_copy_generation;
#endif
} surface_t;
typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
    int resolve_src;   /* surface slot + 1 a scaled blit resolves into this buffer; 0 = none */
} display_buffer_t;
typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    ID3D12Resource* snapshot;
    u32 w, h;
    u32 snapshot_w, snapshot_h;
    D3D12_RESOURCE_STATES snapshot_state;
    int cleared;
    int had_write;
    int snapshot_valid;
    u64 draws;
    u64 depth_test_draws;
    u64 depth_write_draws;
    u64 depth_both_draws;
    int reject_logged;
} zdepth_t;
typedef struct {
    u32 location, offset, format, width, height, pitch, remap;
    u32 cubemap;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
    u64 last_use_serial;
} texcache_t;
typedef struct {
    u32 location, offset, format, width, height, pitch;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
} vtexcache_t;
/* pending: a background compile (ld_pso_job) is still building it. The
 * worker publishes pso before it clears pending. */
typedef struct {
    u64 key;
    ID3D12PipelineState* volatile pso;
    volatile LONG pending;
    u32 ps_unit_mask;   /* texture units the fragment program samples */
} psocache_t;
typedef struct {
    u64 hash;
    u32 source_length;
    char* source;
    ID3DBlob* blob;
} shader_blob_t;
typedef struct {
    shader_blob_t entries[MAX_SHADER_BLOBS];
    u32 count;
    u64 retained_source_bytes;
    u64 retained_blob_bytes;
} shader_blob_cache_t;
typedef struct {
    u32 magic;
    u32 version;
    u32 stage;
    u32 source_length;
    u32 blob_length;
    u32 reserved;
    u64 source_hash;
} shader_disk_cache_header;

typedef struct {
    int              enabled;    /* RSX_DRAW resolved                     */
    int              ready;      /* device + resources up                    */

    ID3D12Device*              dev;
    ID3D12CommandQueue*        queue;
    ID3D12CommandAllocator*    alloc;      /* allocs[bank] */
    ID3D12GraphicsCommandList* list;
    ID3D12Fence*               fence;
    HANDLE                     fence_event;
    u64                        fence_value;
    /* Frame pipelining (ld_submit_frame). bank selects the half of every
     * transient ring the open command list writes; bank_fence is the fence
     * value of the last submission that used each bank. */
    ID3D12CommandAllocator*    allocs[LD_BANKS];
    u32                        bank;
    u64                        bank_fence[LD_BANKS];
    ID3D12Resource*            bank_retired[LD_BANKS][MAX_RETIRED_TEXTURES];
    u32                        bank_n_retired[LD_BANKS];
    int                        pipelined;  /* 0: every present waits, as before */

    IDXGISwapChain3*           swap;
    ID3D12Resource*            backbuf[LD_SWAP_BUFFERS];
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_step;

    surface_t                  surfaces[MAX_SURFACES];
    u32                        n_surfaces;
    display_buffer_t           display_buffers[8];

    ID3D12DescriptorHeap*      dsv_heap;
    u32                        dsv_step;
    ID3D12Resource*            depth;
    int                        depth_cleared;
    zdepth_t                   zdepths[MAX_SURFACES];
    u32                        n_zdepths;

    ID3D12DescriptorHeap*      srv_cpu_heap;
    ID3D12DescriptorHeap*      srv_heap;
    u32                        srv_step, srv_ring_used;
    ID3D12Resource*            white_tex;
    texcache_t                 textures[MAX_TEXTURES];
    u32                        n_textures;
    vtexcache_t                vtex[MAX_VTEX];
    u32                        n_vtex;
    ID3D12Resource*            retired_textures[MAX_RETIRED_TEXTURES];
    u32                        n_retired_textures;
    ID3D12Resource*            upload;
    u8*                        upload_mapped;
    u32                        upload_used;

    /* Optional host-movie UI compositor. The guest keeps rendering captions
     * into its ordinary offscreen surface. Each guest flip reads back the
     * latest sparse overlay for blending over each 30 Hz host movie frame. */
    ID3D12Resource*            movie_upload;
    u8*                        movie_upload_mapped;
    ID3D12Resource*            movie_overlay_readback;
    u8*                        movie_overlay_rgba;
    u8*                        movie_overlay_mask;
    u32                        movie_overlay_pitch;
    int                        movie_overlay_valid;
    u64                        movie_overlay_frames;

    ID3D12DescriptorHeap*      smp_cpu_heap;
    ID3D12DescriptorHeap*      smp_heap;
    u32                        smp_step, smp_ring_used;
    u32                        smp_ring_slots[SMP_RING_TABLES][SMP_TABLE_SIZE];
    /* Sampler tables persist across frames: a table may be rewritten only
     * when the last submission that used it has completed. */
    u64                        smp_table_fence[SMP_RING_TABLES];
    u64                        smp_keys[SMP_CACHE_SLOTS];
    u32                        n_samplers;

    ID3D12RootSignature*       rootsig_x;
    ID3D12RootSignature*       depth_snapshot_rootsig;
    ID3D12PipelineState*       depth_snapshot_pso;
    psocache_t                 psos[MAX_PSOS];
    u32                        n_psos;
    u32                        pso_index[PSO_INDEX_SLOTS];   /* psos index + 1, 0 = empty */
    shader_blob_cache_t        vs_blobs;
    shader_blob_cache_t        ps_blobs;

    ID3D12Resource*            cb;
    u8*                        cb_mapped;
    u32                        cb_used;

    /* Separate transient b1 arena for buffered fragment constants. It uses
     * the existing constant-buffer ring budget rather than changing any
     * texture or vertex upload-ring capacity. */
    ID3D12Resource*            ps_cb;
    u8*                        ps_cb_mapped;
    u32                        ps_cb_used;
    rsx_fp_constant_block      fp_constants;
    float                      fp_alpha_ref;
    char                       fp_constant_mode;

    ID3D12Resource*            vb;
    u8*                        vb_mapped;
    u32                        vb_used;
    ID3D12Resource*            ib;
    u32*                       ib_mapped;
    u32                        ib_used;

    u32                        width, height;   /* guest canvas */
    /* Render scale: every colour/depth surface is created scale x its guest
     * size; everything the guest sees (sizes, offsets) stays in guest pixels
     * and only GPU-side rectangles are multiplied (ld_scaled). */
    u32                        scale;
    /* Ultrawide: canvas-sized surfaces are this much wider again than the
     * render scale makes them (target aspect / 16:9, 1 = off). The game draws
     * its usual 1280x720 frame with a projection widened to the target
     * aspect (the port patches it), so the frame is horizontally squeezed in
     * guest pixels; the wider host surfaces undo the squeeze at full
     * resolution and the present pass shows it at the target aspect. */
    float                      widen;
    /* Swap chain: follows the window's client area (present pass). */
    HWND                       hwnd;
    u32                        bb_w, bb_h;
    ID3D12RootSignature*       present_rootsig;
    ID3D12PipelineState*       present_pso[3];   /* scale, FXAA, box downsample */
    ID3D12Resource*            present_movie;    /* host movie frame, SRV_PRESENT_MOVIE */
    u32                        present_movie_w, present_movie_h;
    rsx_live_guest_ptr_fn      guest_ptr;
    void*                      guest_user;

    rsx_dispatch               rsx;
    /*
     * Startup-selected Stage-1 isolation mode.  The hot draw path only reads
     * these bits; environment parsing is completed once in init.
     *
     * L = legacy
     * H = hoisted descriptor/base fetch
     * M = H + masked shader signature/input layout
     * C = M + compact payload/stride
     */
    u32                        vertex_features;
    char                       vertex_mode;
    char                       vertex_diag_dir[MAX_PATH];
} ld_state;

static ld_state g;

/* Graphics settings the port sets (rsx_live_draw_set_*). The render scale is
 * taken once, at init; the others are read every frame. */
static u32 g_ld_requested_scale = 1;
static float g_ld_requested_aspect = 0.0f;      /* 0 or <= 16:9: original */
static volatile LONG g_ld_post_aa = 0;          /* 0 off, 1 FXAA */
static volatile LONG g_ld_async_shaders = 1;    /* compile new shaders in the background */
static volatile LONG g_ld_anisotropy = 1;       /* requested max anisotropy: 1 (off), 2, 4, 8, 16 */
static u32 s_ld_sampler_anisotropy = 1;         /* what the cached samplers were built with (render thread) */
static volatile LONG g_ld_texture_lod_bias = 0;  /* requested mip bias in hundredths (0 = the game's choice) */
static LONG s_ld_sampler_lod_bias = 0;           /* what the cached samplers were built with (render thread) */
static volatile LONG g_ld_sharpen_percent = 0;  /* present-pass sharpening, 0-100 */
static volatile LONG g_ld_pso_jobs_pending = 0;
static volatile LONG g_ld_pso_jobs_done = 0;
static volatile LONG g_ld_pso_draws_skipped = 0;
static SRWLOCK g_ld_shader_cache_lock = SRWLOCK_INIT;

/* A guest-pixel length in host pixels. */
static u32 ld_scaled(u32 guestPixels)
{
    return guestPixels * (g.scale ? g.scale : 1u);
}

/* Horizontal host pixels per guest pixel on a canvas-sized surface. */
static float ld_canvas_scale_x(void)
{
    const float widen = g.widen > 1.0f ? g.widen : 1.0f;
    return (float)(g.scale ? g.scale : 1u) * widen;
}

static u32 ld_scale_pixels(u32 guestPixels, float scale)
{
    return (u32)((float)guestPixels * scale + 0.5f);
}

/* Canvas-sized lengths (zetas, the shared depth, the movie overlay):
 * ld_scaled_x horizontally, ld_scaled vertically. */
static u32 ld_scaled_x(u32 guestPixels)
{
    return ld_scale_pixels(guestPixels, ld_canvas_scale_x());
}

/* The render scale a colour surface of this guest size gets. Only surfaces
 * at least as large as the guest canvas (the scene, its 2x-wide AA buffer,
 * the display buffers, a 2048x2048 shadow map) are rendered at the render
 * scale. Smaller ones are the post-effect chain -- Folklore's bloom blurs a
 * 640x480 buffer, its luminance pass works at 256x256 -- and stay at the
 * PS3's own size: their shaders step their taps by fixed guest-texel offsets,
 * so at 3-4x the same taps covered a third or a quarter of the footprint and
 * the blur fell apart into stripes. They are soft images anyway, so keeping
 * them small loses nothing (and saves fill). LD_SCALE_ALL_SURFACES=1 scales
 * every surface again. */
static int ld_surface_is_canvas(u32 guestWidth, u32 guestHeight)
{
    static int scaleAll = -1;
    if (scaleAll < 0)
    {
        const char* text = getenv("LD_SCALE_ALL_SURFACES");
        scaleAll = (text && *text == '1') ? 1 : 0;
    }
    if (scaleAll)
    {
        return 1;
    }
    if (!guestWidth)
    {
        guestWidth = g.width;
    }
    if (!guestHeight)
    {
        guestHeight = g.height;
    }
    return guestWidth >= g.width && guestHeight >= g.height;
}

static u32 ld_surface_scale_for(u32 guestWidth, u32 guestHeight)
{
    return ld_surface_is_canvas(guestWidth, guestHeight) ? (g.scale ? g.scale : 1u) : 1u;
}

static float ld_surface_scale_x_for(u32 guestWidth, u32 guestHeight)
{
    return ld_surface_is_canvas(guestWidth, guestHeight) ? ld_canvas_scale_x() : 1.0f;
}

/* A guest-pixel length in a surface's host pixels: ld_surface_x across,
 * ld_surface_scaled (= ld_surface_y) down. */
static u32 ld_surface_scaled(const surface_t* surface, u32 guestPixels)
{
    return guestPixels * (surface->scale ? surface->scale : 1u);
}

static u32 ld_surface_x(const surface_t* surface, u32 guestPixels)
{
    return ld_scale_pixels(guestPixels, surface->scale_x > 0.0f ? surface->scale_x : 1.0f);
}

static int ld_surface_same_scale(const surface_t* first, const surface_t* second)
{
    return first->scale == second->scale && first->scale_x == second->scale_x;
}

static int ld_surface_is_canvas_scale(const surface_t* surface)
{
    return surface->scale == (g.scale ? g.scale : 1u) && surface->scale_x == ld_canvas_scale_x();
}

/* Bounds of the open bank in each transient ring (absolute offsets). */
static u32 ld_upload_base(void) { return g.bank * UPLOAD_BANK_SIZE; }
static u32 ld_upload_end(void) { return ld_upload_base() + UPLOAD_BANK_SIZE; }
static u32 ld_vb_base(void) { return g.bank * VERT_BANK_SIZE; }
static u32 ld_vb_end(void) { return ld_vb_base() + VERT_BANK_SIZE; }
static u32 ld_ib_base(void) { return g.bank * INDEX_BANK_SIZE; }
static u32 ld_ib_end(void) { return ld_ib_base() + INDEX_BANK_SIZE; }
static u32 ld_cb_base(void) { return g.bank * CB_BANK_BYTES; }
static u32 ld_cb_end(void) { return ld_cb_base() + CB_BANK_BYTES; }
static u32 ld_ps_cb_base(void) { return g.bank * PS_CB_BANK_BYTES; }
static u32 ld_ps_cb_end(void) { return ld_ps_cb_base() + PS_CB_BANK_BYTES; }
static u32 ld_srv_base(void) { return g.bank * SRV_BANK_TABLES; }
static u32 ld_srv_end(void) { return ld_srv_base() + SRV_BANK_TABLES; }

/* Every per-draw ring back to the start of the open bank. The upload ring is
 * reset by ld_flush / ld_submit_frame (texture uploads may be recorded
 * between draws), and sampler tables are not a ring any more (sampler_table). */
static void ld_rings_reset(void)
{
    g.vb_used = ld_vb_base();
    g.ib_used = ld_ib_base();
    g.cb_used = ld_cb_base();
    g.ps_cb_used = ld_ps_cb_base();
    g.srv_ring_used = ld_srv_base();
}

/* The fence value the open command list will signal when it is submitted. */
static u64 ld_open_list_fence(void)
{
    return g.fence_value + 1u;
}

#define LD_VERTEX_HOIST_FETCH       (1u << 0)
#define LD_VERTEX_MASK_SIGNATURE    (1u << 1)
#define LD_VERTEX_COMPACT_PAYLOAD   (1u << 2)

static int ld_vertex_hoist_fetch(void)
{
    return (g.vertex_features & LD_VERTEX_HOIST_FETCH) != 0;
}

static int ld_vertex_mask_signature(void)
{
    return (g.vertex_features & LD_VERTEX_MASK_SIGNATURE) != 0;
}

static int ld_vertex_compact_payload(void)
{
    return (g.vertex_features & LD_VERTEX_COMPACT_PAYLOAD) != 0;
}

static const char* ld_vertex_mode_name(void)
{
    switch (g.vertex_mode) {
    case 'H': return "H-hoist";
    case 'M': return "M-mask";
    case 'C': return "C-compact";
    default:  return "L-legacy";
    }
}

static int ld_fp_constants_buffered(void)
{
    return g.fp_constant_mode == 'B';
}

static const char* ld_fp_constant_mode_name(void)
{
    return ld_fp_constants_buffered() ? "buffered" : "literal";
}

static int g_ld_dred_dumped = 0;
static u32 g_ld_frames = 0;
static ID3D12InfoQueue* g_ld_info_queue = NULL;
static int g_ld_debug_layer_enabled = 0;

typedef struct {
    int valid;
    u64 key;
    u64 vp_hash;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask;
    u32 input_mask, input_stride;
    u32 packed_offsets;
    u32 ps_unit_mask;
} ld_pso_metadata;

static ld_pso_metadata g_ld_current_pso;
static u64 g_ld_flip_requested = 0;
static u64 g_ld_flip_consumed = 0;
static u32 g_ld_last_requested_buffer = UINT32_MAX;
static u32 g_ld_last_consumed_buffer = UINT32_MAX;
static u32 g_ld_last_present_target = UINT32_MAX;
static u64 g_ld_last_dump_fingerprint = 0;
#if !defined(YZ_PERF_CLEAN)
static u32 g_ld_surface_generation = 0;
static u32 g_ld_surface_resource_serial = 0;
static u32 g_ld_guest_blit_generation = 0;
static u32 g_ld_present_copy_generation = 0;
static u64 g_ld_vertex_constant_ring_recycles = 0;
static u32 g_ld_frame_vertex_constant_ring_recycles = 0;
static u32 g_ld_fifo_get = 0;
static u32 g_ld_fifo_put = 0;
static volatile LONG g_ld_diag_post_movie_pending = 0;
static volatile LONG g_ld_diag_post_movie_presents = 0;

enum {
    LD_SURFACE_WRITE_NONE = 0,
    LD_SURFACE_WRITE_CREATE = 1,
    LD_SURFACE_WRITE_DRAW = 2,
    LD_SURFACE_WRITE_CLEAR = 3,
    LD_SURFACE_WRITE_COPY = 4,
    LD_SURFACE_WRITE_BLIT = 5,
    LD_SURFACE_WRITE_RESOLVE = 6,
    LD_SURFACE_WRITE_OTHER = 7
};

static void ld_surface_note_write(u32 target, u32 kind)
{
    if (target >= MAX_SURFACES)
        return;
    surface_t* surface = &g.surfaces[target];
    const u32 generation = ++g_ld_surface_generation;
    surface->last_write_generation = generation;
    surface->last_write_kind = kind;
    switch (kind) {
    case LD_SURFACE_WRITE_CREATE:
        surface->last_create_generation = generation;
        break;
    case LD_SURFACE_WRITE_DRAW:
        surface->last_draw_generation = generation;
        break;
    case LD_SURFACE_WRITE_CLEAR:
        surface->last_clear_generation = generation;
        break;
    case LD_SURFACE_WRITE_COPY:
        surface->last_copy_generation = generation;
        break;
    case LD_SURFACE_WRITE_BLIT:
        surface->last_blit_generation = generation;
        break;
    case LD_SURFACE_WRITE_RESOLVE:
        surface->last_resolve_generation = generation;
        break;
    default:
        surface->last_other_generation = generation;
        break;
    }
}
#else
#define ld_surface_note_write(target, kind) ((void)0)
#endif

#define LD_LAYOUT_CACHE_CAP 256u
typedef struct {
    int valid;
    rsx_vertex_layout_plan plan;
} ld_layout_cache_entry;

static ld_layout_cache_entry g_ld_layout_cache[LD_LAYOUT_CACHE_CAP];
static u32 g_ld_layout_cache_count = 0;

static void ld_layout_plan_get(u32 mask, rsx_vertex_layout_plan* out)
{
    mask &= 0xFFFFu;
    for (u32 i = 0; i < g_ld_layout_cache_count; i++) {
        if (g_ld_layout_cache[i].valid &&
            g_ld_layout_cache[i].plan.mask == mask) {
            *out = g_ld_layout_cache[i].plan;
            return;
        }
    }
    rsx_vertex_layout_plan_init(out, mask);
    if (g_ld_layout_cache_count < LD_LAYOUT_CACHE_CAP) {
        ld_layout_cache_entry* entry =
            &g_ld_layout_cache[g_ld_layout_cache_count++];
        entry->valid = 1;
        entry->plan = *out;
    }
}

#define LD_RECENT_DRAW_CAP 128u
typedef struct {
    u64 serial;
    u64 pso_key;
    u64 descriptor_signature;
    u32 frame;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask, texture_mask;
    u32 primitive, vertices, target, zslot;
    u32 clip_x, clip_y, clip_w, clip_h;
    u32 srv_ring_used, sampler_ring_used, cb_used, vb_used;
} ld_recent_draw;

static ld_recent_draw g_ld_recent_draws[LD_RECENT_DRAW_CAP];
static u64 g_ld_recent_draw_total = 0;

/*
 * Isolated pre-cleanup benchmark measurement. Record only successful
 * swap-chain presents: one QPC call and fixed-ring stores, with no allocation,
 * configuration lookup, sorting, or logging in the presentation path.
 */
#define LD_PRESENT_RING_CAP 16384u
typedef struct {
    u64 present_id;
    u32 guest_frame;
    LONGLONG qpc;
    u64 process_kernel_100ns;
    u64 process_user_100ns;
    u64 present_thread_kernel_100ns;
    u64 present_thread_user_100ns;
    u32 present_thread_id;
} ld_present_sample;
static ld_present_sample g_ld_present_ring[LD_PRESENT_RING_CAP];
static u64 g_ld_present_total = 0;
static LONGLONG g_ld_qpc_frequency = 0;
static int g_ld_present_dumped = 0;
static int g_ld_schedule_diag = 0;
#if defined(YZ_PPU_SAMPLE)
static HANDLE g_ld_present_thread_handle = NULL;
#endif

static void ld_present_measure_dump(void);
#if defined(YZ_PERF_PROFILE)
extern void spu_perf_window_begin(u32 guest_frame);
extern void spu_perf_window_dump(u32 guest_frame);
extern void spu_perf_frame_sample_record(u32 guest_frame);
extern void spu_perf_frame_sample_dump(u32 guest_start, u32 guest_end);
#endif
#if defined(YZ_PPU_SAMPLE)
extern void yz_ppu_perf_window_begin(u32 guest_frame);
extern void yz_ppu_perf_window_dump(u32 guest_frame);
#endif

static void ld_present_measure_init(void)
{
    LARGE_INTEGER frequency;
    memset(g_ld_present_ring, 0, sizeof(g_ld_present_ring));
    g_ld_present_total = 0;
    g_ld_present_dumped = 0;
    g_ld_schedule_diag = getenv("RSX_SCHEDULE_DIAG") != NULL;
    if (QueryPerformanceFrequency(&frequency))
        g_ld_qpc_frequency = frequency.QuadPart;
    else
        g_ld_qpc_frequency = 0;
}

static void ld_present_measure_record(u32 guest_frame)
{
#if defined(YZ_PPU_SAMPLE)
    if (!g_ld_present_thread_handle) {
        HANDLE duplicate = NULL;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                            GetCurrentProcess(), &duplicate, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            HANDLE previous = (HANDLE)InterlockedCompareExchangePointer(
                (PVOID volatile*)&g_ld_present_thread_handle, duplicate, NULL);
            if (previous)
                CloseHandle(duplicate);
        }
    }
#endif
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now))
        return;
    const u64 present_id = g_ld_present_total + 1u;
    ld_present_sample* sample =
        &g_ld_present_ring[(present_id - 1u) & (LD_PRESENT_RING_CAP - 1u)];
    sample->present_id = present_id;
    sample->guest_frame = guest_frame;
    sample->qpc = now.QuadPart;
    if (g_ld_schedule_diag) {
        FILETIME creation = {0}, exit = {0}, kernel = {0}, user = {0};
        ULARGE_INTEGER value;
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit,
                            &kernel, &user)) {
            value.LowPart = kernel.dwLowDateTime;
            value.HighPart = kernel.dwHighDateTime;
            sample->process_kernel_100ns = value.QuadPart;
            value.LowPart = user.dwLowDateTime;
            value.HighPart = user.dwHighDateTime;
            sample->process_user_100ns = value.QuadPart;
        }
        if (GetThreadTimes(GetCurrentThread(), &creation, &exit,
                           &kernel, &user)) {
            value.LowPart = kernel.dwLowDateTime;
            value.HighPart = kernel.dwHighDateTime;
            sample->present_thread_kernel_100ns = value.QuadPart;
            value.LowPart = user.dwLowDateTime;
            value.HighPart = user.dwHighDateTime;
            sample->present_thread_user_100ns = value.QuadPart;
        }
        sample->present_thread_id = GetCurrentThreadId();
    }
    g_ld_present_total = present_id;
#if defined(YZ_PPU_SAMPLE)
    {
        static int sample_present = -1;
        static u32 sample_start = 1578u;
        static u32 sample_end = 2178u;
        if (sample_present < 0) {
            sample_present =
                getenv("PPU_SAMPLE_RUN") != NULL &&
                getenv("PPU_SAMPLE_PRESENT") != NULL;
            const char* start_text = getenv("PPU_SAMPLE_START");
            const char* frames_text = getenv("PPU_SAMPLE_FRAMES");
            if (start_text) {
                const unsigned long parsed = strtoul(start_text, NULL, 0);
                if (parsed <= UINT32_MAX)
                    sample_start = (u32)parsed;
            }
            if (frames_text) {
                unsigned long parsed = strtoul(frames_text, NULL, 0);
                if (parsed < 4u) parsed = 4u;
                if (parsed > 600u) parsed = 600u;
                sample_end = sample_start + (u32)parsed;
            }
        }
        const int game_present = guest_frame == g_ld_frames + 1u;
#if defined(YZ_PERF_PROFILE)
        if (sample_present && game_present &&
            guest_frame >= sample_start && guest_frame <= sample_end)
            spu_perf_frame_sample_record(guest_frame);
#endif
        if (sample_present && game_present && guest_frame == sample_start)
            yz_ppu_perf_window_begin(guest_frame);
        else if (sample_present && game_present && guest_frame == sample_end) {
            yz_ppu_perf_window_dump(guest_frame);
#if defined(YZ_PERF_PROFILE)
            spu_perf_frame_sample_dump(sample_start, sample_end);
#endif
        }
    }
#endif
#if defined(YZ_PERF_PROFILE)
    if (guest_frame == 1578u)
        spu_perf_window_begin(guest_frame);
    else if (guest_frame == 1582u)
        spu_perf_window_dump(guest_frame);
#endif
    /*
     * The controlled A010 benchmark ends at guest frame 2293.  Flush the
     * already-recorded fixed ring exactly at that endpoint so an unrelated
     * host-window teardown cannot truncate the authoritative raw samples.
     * This is outside the measured timestamp operation and runs once.
     */
    if (guest_frame == 2293u)
        ld_present_measure_dump();
}

static void ld_present_measure_dump(void)
{
    if (g_ld_present_dumped || g_ld_present_total == 0 ||
        g_ld_qpc_frequency <= 0)
        return;
    g_ld_present_dumped = 1;

    char path[256];
    snprintf(path, sizeof(path), "scratch\\present_qpc_%lu.csv",
             (unsigned long)GetCurrentProcessId());
    FILE* f = fopen(path, "wb");
    if (!f)
        return;

    fprintf(f, "# qpc_frequency=%lld\n", g_ld_qpc_frequency);
    if (g_ld_schedule_diag) {
        fprintf(f, "present_id,guest_frame,qpc,process_kernel_100ns,"
                   "process_user_100ns,present_thread_kernel_100ns,"
                   "present_thread_user_100ns,present_thread_id\n");
    } else {
        fprintf(f, "present_id,guest_frame,qpc\n");
    }
    const u64 first =
        g_ld_present_total > LD_PRESENT_RING_CAP
            ? g_ld_present_total - LD_PRESENT_RING_CAP + 1u : 1u;
    for (u64 present_id = first; present_id <= g_ld_present_total;
         ++present_id) {
        const ld_present_sample* sample =
            &g_ld_present_ring[(present_id - 1u) &
                               (LD_PRESENT_RING_CAP - 1u)];
        if (sample->present_id == present_id) {
            if (g_ld_schedule_diag) {
                fprintf(f, "%llu,%u,%lld,%llu,%llu,%llu,%llu,%u\n",
                        (unsigned long long)sample->present_id,
                        sample->guest_frame, sample->qpc,
                        (unsigned long long)sample->process_kernel_100ns,
                        (unsigned long long)sample->process_user_100ns,
                        (unsigned long long)sample->present_thread_kernel_100ns,
                        (unsigned long long)sample->present_thread_user_100ns,
                        sample->present_thread_id);
            } else {
                fprintf(f, "%llu,%u,%lld\n",
                        (unsigned long long)sample->present_id,
                        sample->guest_frame, sample->qpc);
            }
        }
    }
    fclose(f);
    fprintf(stderr,
            "[present-qpc] preserved %llu successful presents at %lld Hz in %s\n",
            (unsigned long long)(g_ld_present_total - first + 1u),
            g_ld_qpc_frequency, path);
}

static u64 g_ld_texture_cache_full = 0;
static u64 g_ld_texture_use_serial = 0;

static const char* ld_d3d_severity_name(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
        case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D12_MESSAGE_SEVERITY_INFO: return "info";
        default: return "message";
    }
}

static void ld_drain_info_queue(const char* where)
{
    if (!g_ld_info_queue) return;
    const u64 count =
        g_ld_info_queue->lpVtbl->GetNumStoredMessages(g_ld_info_queue);
    for (u64 i = 0; i < count; i++) {
        SIZE_T bytes = 0;
        if (FAILED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, NULL, &bytes)) ||
            !bytes || bytes > 1024u * 1024u)
            continue;
        D3D12_MESSAGE* message = (D3D12_MESSAGE*)malloc(bytes);
        if (!message) break;
        if (SUCCEEDED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, message, &bytes))) {
            fprintf(stderr,
                    "[d3d-debug] where=%s frame=%u severity=%s id=%u %s\n",
                    where, g_ld_frames,
                    ld_d3d_severity_name(message->Severity),
                    (unsigned)message->ID,
                    message->pDescription
                        ? message->pDescription : "<no description>");
        }
        free(message);
    }
    if (count)
        g_ld_info_queue->lpVtbl->ClearStoredMessages(g_ld_info_queue);
}

static void ld_dump_recent_draws(void)
{
    const u64 first =
        g_ld_recent_draw_total > LD_RECENT_DRAW_CAP
            ? g_ld_recent_draw_total - LD_RECENT_DRAW_CAP + 1u : 1u;
    fprintf(stderr,
            "[draw-history] total=%llu retained=%llu..%llu\n",
            (unsigned long long)g_ld_recent_draw_total,
            (unsigned long long)first,
            (unsigned long long)g_ld_recent_draw_total);
    for (u64 serial = first; serial <= g_ld_recent_draw_total; serial++) {
        const ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        if (draw->serial != serial) continue;
        fprintf(stderr,
                "[draw-history] serial=%llu frame=%u key=%016llX "
                "vp=%u+%u fp=%u:0x%X+%u ctrl=0x%X "
                "mask{tex=%04X cube=%04X vtex=%04X txl=%04X} "
                "prim=%u verts=%u target=%u z=%u "
                "clip=%u,%u %ux%u desc=%016llX "
                "ring{srv=%u smp=%u cb=%u vb=%u}\n",
                (unsigned long long)draw->serial, draw->frame,
                (unsigned long long)draw->pso_key,
                draw->vp_start, draw->vp_instrs,
                draw->fp_location, draw->fp_offset, draw->fp_size,
                draw->fp_control, draw->texture_mask, draw->cube_mask,
                draw->vtex_mask, draw->txl_mask, draw->primitive,
                draw->vertices, draw->target, draw->zslot,
                draw->clip_x, draw->clip_y, draw->clip_w, draw->clip_h,
                (unsigned long long)draw->descriptor_signature,
                draw->srv_ring_used, draw->sampler_ring_used,
                draw->cb_used, draw->vb_used);
    }
}

static const char* ld_dred_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRTV";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDSV";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
        default: return "other";
    }
}

static void ld_enable_dred(void)
{
    ID3D12DeviceRemovedExtendedDataSettings* settings = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&settings);
    if (FAILED(hr) || !settings) {
        fprintf(stderr, "[dred] settings unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    settings->lpVtbl->SetAutoBreadcrumbsEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->SetPageFaultEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->Release(settings);
    fprintf(stderr, "[dred] auto-breadcrumbs and page-fault tracking enabled\n");
}

static void ld_enable_debug_layer(void)
{
    if (!getenv("RSX_D3D_DEBUG")) return;
    ID3D12Debug* debug = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12Debug, (void**)&debug);
    if (FAILED(hr) || !debug) {
        fprintf(stderr,
                "[d3d-debug] validation layer unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    debug->lpVtbl->EnableDebugLayer(debug);
    debug->lpVtbl->Release(debug);
    g_ld_debug_layer_enabled = 1;
    fprintf(stderr, "[d3d-debug] validation layer enabled\n");
}

static void ld_open_info_queue(void)
{
    if (!g_ld_debug_layer_enabled || !g.dev) return;
    const HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12InfoQueue, (void**)&g_ld_info_queue);
    if (FAILED(hr) || !g_ld_info_queue) {
        fprintf(stderr,
                "[d3d-debug] info queue unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    D3D12_MESSAGE_SEVERITY severities[] = {
        D3D12_MESSAGE_SEVERITY_CORRUPTION,
        D3D12_MESSAGE_SEVERITY_ERROR,
        D3D12_MESSAGE_SEVERITY_WARNING
    };
    D3D12_INFO_QUEUE_FILTER filter = {0};
    filter.AllowList.NumSeverities =
        (UINT)(sizeof(severities) / sizeof(severities[0]));
    filter.AllowList.pSeverityList = severities;
    g_ld_info_queue->lpVtbl->AddStorageFilterEntries(
        g_ld_info_queue, &filter);
    g_ld_info_queue->lpVtbl->SetMessageCountLimit(
        g_ld_info_queue, 65536u);
    fprintf(stderr,
            "[d3d-debug] info queue attached (warning/error/corruption)\n");
}

static void ld_dump_dred(const char* where, HRESULT trigger_hr)
{
    if (g_ld_dred_dumped++ || !g.dev) return;
    ld_drain_info_queue(where);
    ld_dump_recent_draws();
    const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
    fprintf(stderr,
            "[dred] trigger=%s hr=0x%08lX removed=0x%08lX frame=%u "
            "surfaces=%u zetas=%u textures=%u psos=%u\n",
            where, (unsigned long)trigger_hr, (unsigned long)removed,
            g_ld_frames, g.n_surfaces, g.n_zdepths, g.n_textures, g.n_psos);

    ID3D12DeviceRemovedExtendedData1* dred = NULL;
    HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12DeviceRemovedExtendedData1, (void**)&dred);
    if (FAILED(hr) || !dred) {
        fprintf(stderr, "[dred] query failed hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {0};
    hr = dred->lpVtbl->GetAutoBreadcrumbsOutput1(dred, &breadcrumbs);
    fprintf(stderr, "[dred] breadcrumbs hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        const D3D12_AUTO_BREADCRUMB_NODE1* node =
            breadcrumbs.pHeadAutoBreadcrumbNode;
        for (u32 node_index = 0; node && node_index < 16;
             node = node->pNext, node_index++) {
            const u32 last = node->pLastBreadcrumbValue
                ? *node->pLastBreadcrumbValue : 0;
            fprintf(stderr,
                    "[dred] node=%u list=%s queue=%s count=%u last=%u\n",
                    node_index,
                    node->pCommandListDebugNameA
                        ? node->pCommandListDebugNameA : "<unnamed>",
                    node->pCommandQueueDebugNameA
                        ? node->pCommandQueueDebugNameA : "<unnamed>",
                    node->BreadcrumbCount, last);
            if (!node->pCommandHistory || !node->BreadcrumbCount) continue;
            const u32 first = last > 8 ? last - 8 : 0;
            u32 end = last + 8;
            if (end >= node->BreadcrumbCount)
                end = node->BreadcrumbCount - 1;
            for (u32 i = first; i <= end; i++) {
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                fprintf(stderr,
                        "[dred]   op[%u]%s=%u(%s)\n",
                        i, i == last ? "*" : "",
                        (unsigned)op, ld_dred_op_name(op));
            }
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {0};
    hr = dred->lpVtbl->GetPageFaultAllocationOutput1(dred, &fault);
    fprintf(stderr,
            "[dred] page-fault hr=0x%08lX va=0x%016llX\n",
            (unsigned long)hr,
            (unsigned long long)fault.PageFaultVA);
    if (SUCCEEDED(hr)) {
        const D3D12_DRED_ALLOCATION_NODE1* lists[2] = {
            fault.pHeadExistingAllocationNode,
            fault.pHeadRecentFreedAllocationNode
        };
        const char* labels[2] = {"existing", "recent-freed"};
        for (u32 list = 0; list < 2; list++) {
            const D3D12_DRED_ALLOCATION_NODE1* allocation = lists[list];
            for (u32 i = 0; allocation && i < 16;
                 allocation = allocation->pNext, i++) {
                fprintf(stderr,
                        "[dred] %s[%u] type=%u name=%s object=%p\n",
                        labels[list], i, (unsigned)allocation->AllocationType,
                        allocation->ObjectNameA
                            ? allocation->ObjectNameA : "<unnamed>",
                        (const void*)allocation->pObject);
            }
        }
    }
    dred->lpVtbl->Release(dred);
    fflush(stderr);
}
static u64 g_ld_texture_cache_evictions = 0;
static u64 g_ld_texture_decode_fail = 0;
static u64 g_ld_zdepth_srv_binds = 0;
static u64 g_ld_zdepth_srv_reject_no_write = 0;
static u64 g_ld_vtex_binds = 0;
static u64 g_ld_vtex_uploads = 0;
static u64 g_ld_vtex_refreshes = 0;
static u64 g_ld_vtex_unsupported = 0;
static u64 g_ld_vtex_enabled = 0;
static u64 g_ld_vtex_missing_for_txl = 0;
static u64 g_ld_divider_fetches = 0;
static char g_ld_shader_disk_dir[MAX_PATH];
static int g_ld_shader_disk_ready = -1;
static u64 g_ld_shader_disk_hits[2] = {0, 0};
static u64 g_ld_shader_disk_misses[2] = {0, 0};
static u64 g_ld_shader_disk_writes[2] = {0, 0};
static u64 g_ld_shader_disk_rejects = 0;
/* Texture bytes copied to the GPU, for the [hitch] line. */
static u64 g_ld_texture_upload_bytes = 0;

/*
 * Profile-lane-only renderer accounting.  These counters deliberately live
 * outside ld_state so the clean benchmark lane keeps the exact same state
 * layout and hot path.  Emit only slow/interesting frames plus a sparse
 * heartbeat; the orphanage window is therefore visible without turning the
 * log itself into a benchmark cost.
 */
typedef enum {
    LD_FLUSH_PRESENT = 0,
    LD_FLUSH_GUEST_REFERENCE,
    LD_FLUSH_VERTEX_RING,
    LD_FLUSH_VERTEX_CONSTANT_RING,
    LD_FLUSH_RETIRE_QUEUE,
    LD_FLUSH_MOVIE,
    LD_FLUSH_MOVIE_PRESENT,
    LD_FLUSH_READBACK,
    LD_FLUSH_PIXEL_CONSTANT_RING,
    LD_FLUSH_DESCRIPTOR_RING,
    LD_FLUSH_SHUTDOWN,
    LD_FLUSH_REASON_COUNT
} ld_flush_reason;

#if defined(YZ_PERF_PROFILE)
typedef enum {
    LD_REJECT_WORLD = 0,
    LD_REJECT_CHARACTER,
    LD_REJECT_UI,
    LD_REJECT_OTHER,
    LD_REJECT_CLASS_COUNT
} ld_reject_class;

typedef struct {
    u64 pso_lookups;
    u64 pso_hits;
    u64 pso_misses;
    u64 pso_probes;
    u64 pso_full;
    u64 vs_blob_lookups;
    u64 vs_blob_hits;
    u64 vs_blob_misses;
    u64 vs_blob_inserts;
    u64 vs_blob_full_rejects;
    u64 ps_blob_lookups;
    u64 ps_blob_hits;
    u64 ps_blob_misses;
    u64 ps_blob_inserts;
    u64 ps_blob_full_rejects;
    u64 vs_compile_calls;
    u64 ps_compile_calls;
    u64 vs_unique;
    u64 ps_unique;
    u64 vs_post_boundary_distinct;
    u64 vs_post_boundary_repeats;
    u64 ps_post_boundary_distinct;
    u64 ps_post_boundary_repeats;
    u64 create_pso_calls;
    u64 decompile_qpc;
    u64 vs_blob_lookup_qpc;
    u64 ps_blob_lookup_qpc;
    u64 vs_compile_qpc;
    u64 ps_compile_qpc;
    u64 create_pso_qpc;
    u64 flush_qpc;
    u64 fence_wait_qpc;
    u64 flush_reason[LD_FLUSH_REASON_COUNT];
    u64 flush_reason_qpc[LD_FLUSH_REASON_COUNT];
    u64 fence_reason_qpc[LD_FLUSH_REASON_COUNT];
    u64 input_vertices;
    u64 expanded_vertices;
    u64 used_attribute_draws;
    u64 used_attribute_sum;
    u64 legacy_vertex_upload_bytes;
    u64 vertex_upload_bytes;
    u64 vertex_fetch_pack_qpc;
    u64 sink_end_qpc;
    u64 texture_upload_bytes;
    u64 texture_decode_calls;
    u64 texture_decode_qpc;
    u64 ps_constant_allocations;
    u64 ps_constant_upload_bytes;
    u64 ps_constant_upload_qpc;
    u64 ps_constant_capacity_failures;
} ld_profile_counts;

typedef struct {
    ld_profile_counts total;
    ld_profile_counts previous;
    u64 vs_hashes[MAX_SHADER_BLOBS];
    u64 ps_hashes[MAX_SHADER_BLOBS];
    u64 ps_raw_exact_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_raw_constant_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_exact_source_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_constant_specialized_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_canonical_hashes[LD_VARIANT_SET_CAPACITY];
    u32 n_ps_raw_exact_hashes;
    u32 n_ps_raw_constant_hashes;
    u32 n_vs_hashes;
    u32 n_ps_hashes;
    u32 n_ps_exact_source_hashes;
    u32 n_ps_constant_specialized_hashes;
    u32 n_ps_canonical_hashes;
    u64 ps_variant_tracking_overflow;
    u32 frame_upload_high;
    u32 frame_vb_high;
    u32 frame_retired_high;
    u32 total_upload_high;
    u32 total_vb_high;
    u32 total_retired_high;
    u32 frame_used_attribute_max;
    u32 total_used_attribute_max;
    u64 measured_frames;
    u64 compile_free_frames;
    double total_frame_ms;
    double compile_free_frame_ms;
    u64 previous_packets;
    u64 previous_groups;
    u64 previous_executed;
    u64 previous_evictions;
    u64 previous_refreshes;
    LONGLONG previous_present_qpc;
    LONGLONG qpc_frequency;
    IDXGIAdapter3* adapter;
    u64 rejected_pso_keys[MAX_REJECTED_PSO_KEYS];
    u8 rejected_pso_class_masks[MAX_REJECTED_PSO_KEYS];
    u32 n_rejected_pso_keys;
    u64 rejected_pso_requests[LD_REJECT_CLASS_COUNT];
    u32 rejected_pso_unique[LD_REJECT_CLASS_COUNT];
    u32 first_pso_full_frame;
} ld_profile_state;

static ld_profile_state g_ld_profile;

static LONGLONG ld_profile_qpc(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static void ld_profile_note_ring_highwater(void)
{
    if (g.upload_used > g_ld_profile.frame_upload_high)
        g_ld_profile.frame_upload_high = g.upload_used;
    if (g.upload_used > g_ld_profile.total_upload_high)
        g_ld_profile.total_upload_high = g.upload_used;
    if (g.vb_used > g_ld_profile.frame_vb_high)
        g_ld_profile.frame_vb_high = g.vb_used;
    if (g.vb_used > g_ld_profile.total_vb_high)
        g_ld_profile.total_vb_high = g.vb_used;
    if (g.n_retired_textures > g_ld_profile.frame_retired_high)
        g_ld_profile.frame_retired_high = g.n_retired_textures;
    if (g.n_retired_textures > g_ld_profile.total_retired_high)
        g_ld_profile.total_retired_high = g.n_retired_textures;
}
#else
static void ld_profile_note_ring_highwater(void) {}
#endif

static volatile LONG g_ld_a010_probe_active = 0;
static u32 g_ld_a010_probe_start_frame = 0;
static u32 g_ld_a010_probe_sample = 0;
static u64 g_ld_a010_probe_touched = 0;
static volatile LONG g_ld_a010_camera_ready = 0;
static volatile LONG g_ld_a010_world_ready = 0;
static u32 g_ld_a010_camera_bits[16];

static void ld_movement_camera_snapshot(LONG phase)
{
    fprintf(stderr,
            "[movement-frontier-rsx] phase=%ld frame=%u root=%ld "
            "reference_active=%ld camera_ready=%ld\n",
            phase, g_ld_frames,
            InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0),
            InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0),
            InterlockedCompareExchange(&g_ld_a010_camera_ready, 0, 0));
    for (u32 row = 0; row < 4u; ++row) {
        fprintf(stderr,
                "[movement-frontier-rsx-camera] phase=%ld row=%u "
                "constant=%08X/%08X/%08X/%08X "
                "fallback=%08X/%08X/%08X/%08X\n",
                phase, row,
                g.rsx.constants[108u + row][0],
                g.rsx.constants[108u + row][1],
                g.rsx.constants[108u + row][2],
                g.rsx.constants[108u + row][3],
                g_ld_a010_camera_bits[row * 4u + 0u],
                g_ld_a010_camera_bits[row * 4u + 1u],
                g_ld_a010_camera_bits[row * 4u + 2u],
                g_ld_a010_camera_bits[row * 4u + 3u]);
    }
    fflush(stderr);
}
/* Host movie presentation and the FIFO consumer live on different threads but
 * share one D3D12 command list. Normal gameplay remains single-producer and
 * bypasses this lock; the active-reader handshake closes the movie-mode
 * transition race without putting every ordinary RSX method behind an SRW
 * lock. Host-frame priority prevents a flood of guest caption commands from
 * starving Present when the window is backgrounded. */
static SRWLOCK g_ld_access_lock = SRWLOCK_INIT;
static volatile LONG g_ld_guest_active = 0;
static volatile LONG g_ld_host_waiting = 0;

static const u8* guest_ptr(u32 location, u32 offset, u32 min_bytes)
{
    if (!g.guest_ptr) return NULL;
    return g.guest_ptr(g.guest_user, location, offset, min_bytes);
}
static u64 fnv1a(const void* data, u32 n, u64 h);
static u64 ld_dump_surface_ppm(const char* path, const surface_t* surface);
static u64 live_legacy_pso_key(void);
static void ld_vertex_diag_emit(const char* reason, int dump_surface);

/* ---------------------------------------------------------------------------
 * enable gate
 * -----------------------------------------------------------------------*/
int rsx_live_draw_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        /* Opt-in in this tree. Upstream (caner's Yakuza fork) defaults this ON
         * because that runner has no other renderer; here rsx_d3d12_backend.c
         * is still the default for every other title, so the live engine has
         * to be asked for. (The YZ_RSX_DRAW alias his scripts used went with
         * the title-hack drain; RSX_LIVE_DRAW is the one spelling.) */
        const char* e = getenv("RSX_LIVE_DRAW");
        cached = (e && e[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* ---------------------------------------------------------------------------
 * B1 render/sampler state decode (identical facts to replay_main.c)
 * -----------------------------------------------------------------------*/
#define M_BLEND_ENABLE       0x0310
#define M_ALPHA_TEST_ENABLE  0x0304
#define M_ALPHA_FUNC         0x0308
#define M_ALPHA_REF          0x030C
#define M_ANTI_ALIASING_CONTROL 0x1D7C
#define M_BLEND_SFACTOR      0x0314
#define M_BLEND_DFACTOR      0x0318
#define M_BLEND_EQUATION     0x0320
#define M_DEPTH_FUNC         0x0A6C
#define M_DEPTH_WRITE        0x0A70
#define M_DEPTH_TEST_ENABLE  0x0A74
#define M_CULL_FACE          0x1830
#define M_FRONT_FACE         0x1834
#define M_CULL_FACE_ENABLE   0x183C
#define M_COLOR_MASK         0x0324
/* Stencil + scissor (raw NV4097 offsets, same convention as above; RPCS3
 * gcm_enums.h consulted as a read-only numbering oracle). */
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
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_ZSTENCIL_CLEAR      0x1D8C
#define M_BLEND_COLOR         0x031C
#define M_BLEND_COLOR2        0x0390
#define M_POLY_OFFSET_FILL_ENABLE 0x0A68
#define M_POLYGON_OFFSET_SCALE    0x0A78
#define M_POLYGON_OFFSET_BIAS     0x0A7C

static D3D12_COMPARISON_FUNC gcm_cmp(u32 f)
{
    switch (f) {
    case 0x0200: return D3D12_COMPARISON_FUNC_NEVER;
    case 0x0201: return D3D12_COMPARISON_FUNC_LESS;
    case 0x0202: return D3D12_COMPARISON_FUNC_EQUAL;
    case 0x0203: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 0x0204: return D3D12_COMPARISON_FUNC_GREATER;
    case 0x0205: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 0x0206: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default:     return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}
static D3D12_BLEND gcm_blend_factor(u32 f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static D3D12_BLEND_OP gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;
    }
}

/* SURFACE_COLOR_TARGET selector -> number of colour targets written, A first.
 * 0x02 (B alone) and anything unknown draw to A only, as before. */
static u32 ld_rt_count(u32 sel)
{
    switch (sel) {
    case 0x13: return 2;
    case 0x17: return 3;
    case 0x1F: return 4;
    default:   return 1;
    }
}

typedef struct {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
    /* TM: this title renders HDR into FP16 (SURFACE_FORMAT 0xB) targets;
     * the RTV format is PSO state, so it belongs in the key. */
    u32 rt_fp16;
    /* Colour targets the draw writes (SURFACE_COLOR_TARGET: A, AB, ABC,
     * ABCD). GH3's venue pass writes A and B; the post pass then samples B.
     * With only A bound, B stayed empty and that pass painted the scene
     * black. NumRenderTargets is PSO state, so it is keyed too. */
    u32 rt_count;
    /* Stencil is PSO state in D3D12, so it lives here and feeds the PSO key
     * like every other field. The reference value is dynamic
     * (OMSetStencilRef) and deliberately NOT part of this struct. */
    u32 stencil_enable, stencil_two_sided;
    u32 s_func, s_func_mask, s_write_mask, s_fail, s_zfail, s_zpass;
    u32 bs_func, bs_fail, bs_zfail, bs_zpass;
    /* POLY_OFFSET_FILL_ENABLE and the factor / units it applies (float
     * bits, zero while disabled). Depth bias is rasterizer state: keyed. */
    u32 poly_offset_fill, poly_offset_scale, poly_offset_bias;
    /* Render scale of the colour target (ld_surface_scale_for): WPOS and the
     * slope bias are baked per scale, so it is keyed. */
    u32 draw_scale;
    u32 draw_scale_x_bits;    /* float: horizontal scale (ultrawide) */
} render_state_t;

static void decode_render_state(render_state_t* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(&g.rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func = rsx_dsp_reg(&g.rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(&g.rsx, M_ALPHA_REF);
    rsx_dsp_surface alpha_surface;
    rsx_dsp_get_surface(&g.rsx, &alpha_surface);
    rs->alpha_ref_format = alpha_surface.color_format;
    rs->draw_scale = ld_surface_scale_for(alpha_surface.clip_w, alpha_surface.clip_h);
    {
        const float drawScaleX = ld_surface_scale_x_for(alpha_surface.clip_w, alpha_surface.clip_h);
        memcpy(&rs->draw_scale_x_bits, &drawScaleX, sizeof drawScaleX);
    }
    /* Alpha to coverage (ANTI_ALIASING_CONTROL: bit 0 enable, bit 4
     * alpha-to-coverage). Foliage and wire fences are drawn with it and no
     * alpha test, so without it their cut-out texels come out solid. It only
     * applies to an antialiased surface (more than one sample). AA
     * surfaces are kept at sample resolution here, not as D3D12 MSAA, so
     * coverage cannot be produced from alpha; a fragment whose alpha is below
     * one half covers no sample in the common case, so it is emulated as an
     * alpha test GEQUAL 0.5 when the title's own alpha test is off.
     * PS3_RSX_NO_A2C=1 turns the emulation off. */
    {
        static int alphaToCoverageOff = -1;
        if (alphaToCoverageOff < 0)
        {
            alphaToCoverageOff = getenv("PS3_RSX_NO_A2C") ? 1 : 0;
        }
        const u32 antiAliasing = rsx_dsp_reg(&g.rsx, M_ANTI_ALIASING_CONTROL);
        if (!alphaToCoverageOff && !rs->alpha_test_enable && (antiAliasing & 0x11u) == 0x11u &&
            alpha_surface.antialias != 0)
        {
            rs->alpha_test_enable = 1;
            rs->alpha_func = 0x0206u;   /* GEQUAL */
            if (alpha_surface.color_format == 14u)
            {
                rs->alpha_ref_raw = 0x3800u;        /* half 0.5 */
            }
            else if (alpha_surface.color_format == 15u || alpha_surface.color_format == 16u)
            {
                rs->alpha_ref_raw = 0x3F000000u;    /* float 0.5 */
            }
            else
            {
                rs->alpha_ref_raw = 0x80u;          /* 128/255 */
            }
        }
    }
    rs->rt_fp16 = alpha_surface.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->rt_count = ld_rt_count(alpha_surface.color_target);
    rs->blend_enable = rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1;
    const u32 sf = rsx_dsp_reg(&g.rsx, M_BLEND_SFACTOR);
    const u32 df = rsx_dsp_reg(&g.rsx, M_BLEND_DFACTOR);
    const u32 eq = rsx_dsp_reg(&g.rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sf & 0xFFFF; rs->sf_a = sf >> 16;
    rs->df_rgb = df & 0xFFFF; rs->df_a = df >> 16;
    rs->eq_rgb = eq & 0xFFFF; rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1;
    /* LD_NO_DEPTH=1: disable the depth test on every draw. A probe for draws
     * that vanish -- Tornado Outbreak's UI text fails its depth test here (not
     * yet understood), and this is how its menus are readable meanwhile. */
    { static int nd = -1; if (nd < 0) nd = getenv("LD_NO_DEPTH") ? 1 : 0; if (nd) rs->depth_test = 0; }
    rs->depth_write = rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(&g.rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(&g.rsx, M_CULL_FACE_ENABLE) & 1;
    rs->cull_face   = rsx_dsp_reg(&g.rsx, M_CULL_FACE);
    rs->front_face  = rsx_dsp_reg(&g.rsx, M_FRONT_FACE);
    /* s31 (scratch/s31_blue_emitter.md): honor the RAW register — 0 is a
     * legitimate game-written "write no color channels" (the character
     * shadow-mask depth-prime pass). rsx_dispatch_init seeds the nv40
     * reset default (0x01010101), so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(&g.rsx, M_COLOR_MASK);
    rs->stencil_enable    = rsx_dsp_reg(&g.rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(&g.rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(&g.rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_ZPASS);
    rs->poly_offset_fill = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FILL_ENABLE) & 1;
    if (rs->poly_offset_fill)
    {
        rs->poly_offset_scale = rsx_dsp_reg(&g.rsx, M_POLYGON_OFFSET_SCALE);
        rs->poly_offset_bias = rsx_dsp_reg(&g.rsx, M_POLYGON_OFFSET_BIAS);
    }
}

/* GL stencil-op enums (the NV4097 payload) -> D3D12. Zero (never seeded by
 * the game before first use) and GL_KEEP both map to KEEP. */
static D3D12_STENCIL_OP gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return D3D12_STENCIL_OP_ZERO;      /* GL_ZERO */
    case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;  /* GL_INCR (clamped) */
    case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;  /* GL_DECR (clamped) */
    case 0x150A: return D3D12_STENCIL_OP_INVERT;
    case 0x8507: return D3D12_STENCIL_OP_INCR;      /* GL_INCR_WRAP */
    case 0x8508: return D3D12_STENCIL_OP_DECR;      /* GL_DECR_WRAP */
    case 0x1E00: default: return D3D12_STENCIL_OP_KEEP;
    }
}

static void apply_render_state(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                               const render_state_t* rs)
{
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd->BlendState.RenderTarget[0];
    /* nv40_3d COLOR_MASK byte layout (Mesa/nouveau nv40_3d.xml.h, MIT/X11):
     * B=[0:7] G=[8:15] R=[16:23] A=[24:31]; any nonzero byte = channel on.
     * (s31: was hardcoded ENABLE_ALL — the blue-character class' live twin.) */
    b->RenderTargetWriteMask =
        (((rs->color_mask       ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_BLUE  : 0) |
        (((rs->color_mask >>  8 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        (((rs->color_mask >> 16 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_RED   : 0) |
        (((rs->color_mask >> 24 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0);
    if (rs->blend_enable) {
        b->BlendEnable   = TRUE;
        b->SrcBlend      = gcm_blend_factor(rs->sf_rgb, 0);
        b->DestBlend     = gcm_blend_factor(rs->df_rgb, 0);
        b->BlendOp       = gcm_blend_op(rs->eq_rgb);
        b->SrcBlendAlpha = gcm_blend_factor(rs->sf_a, 1);
        b->DestBlendAlpha= gcm_blend_factor(rs->df_a, 1);
        b->BlendOpAlpha  = gcm_blend_op(rs->eq_a);
    }
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    if (rs->cull_enable && rs->cull_face) {
        const u32 f = rs->cull_face;
        pd->RasterizerState.CullMode = (f == 0x0404) ? D3D12_CULL_MODE_FRONT
                                     : (f == 0x0405) ? D3D12_CULL_MODE_BACK
                                                     : D3D12_CULL_MODE_NONE;
    } else {
        pd->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    /* Y-negating viewport epilogue mirrors winding; take the front sense
     * straight from the guest value (validated in B1). */
    pd->RasterizerState.FrontCounterClockwise = (rs->front_face == 0x0901);
    /* Polygon offset (glPolygonOffset): units of the RSX's 24-bit depth,
     * which a float depth buffer's bias unit equals for depths in [0.5, 1),
     * where nearly all of a perspective scene lies. The slope term is per
     * pixel, so it grows with the render scale to stay per guest pixel.
     * It was not applied at all: decals and effects drawn onto surfaces
     * with an offset lost the depth test to the surface beneath. */
    if (rs->poly_offset_fill)
    {
        float slopeFactor = 0.0f;
        float constantUnits = 0.0f;
        memcpy(&slopeFactor, &rs->poly_offset_scale, sizeof slopeFactor);
        memcpy(&constantUnits, &rs->poly_offset_bias, sizeof constantUnits);
        if (constantUnits == constantUnits && constantUnits > -1.0e6f && constantUnits < 1.0e6f)
        {
            pd->RasterizerState.DepthBias = (INT)(constantUnits < 0.0f ? constantUnits - 0.5f
                                                                       : constantUnits + 0.5f);
        }
        if (slopeFactor == slopeFactor && slopeFactor > -1.0e6f && slopeFactor < 1.0e6f)
        {
            pd->RasterizerState.SlopeScaledDepthBias = slopeFactor * (float)(rs->draw_scale ? rs->draw_scale : 1u);
        }
        pd->RasterizerState.DepthBiasClamp = 0.0f;
    }
    if (g.depth) {
        pd->DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        pd->DepthStencilState.DepthEnable = rs->depth_test ? TRUE : FALSE;
        pd->DepthStencilState.DepthWriteMask =
            rs->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd->DepthStencilState.DepthFunc = gcm_cmp(rs->depth_func);
        if (rs->stencil_enable) {
            pd->DepthStencilState.StencilEnable    = TRUE;
            pd->DepthStencilState.StencilReadMask  = (UINT8)rs->s_func_mask;
            pd->DepthStencilState.StencilWriteMask = (UINT8)rs->s_write_mask;
            pd->DepthStencilState.FrontFace.StencilFunc = gcm_cmp(rs->s_func);
            pd->DepthStencilState.FrontFace.StencilFailOp = gcm_stencil_op(rs->s_fail);
            pd->DepthStencilState.FrontFace.StencilDepthFailOp = gcm_stencil_op(rs->s_zfail);
            pd->DepthStencilState.FrontFace.StencilPassOp = gcm_stencil_op(rs->s_zpass);
            /* Two-sided off: nv40 applies the front state to both faces. */
            if (rs->stencil_two_sided) {
                pd->DepthStencilState.BackFace.StencilFunc = gcm_cmp(rs->bs_func);
                pd->DepthStencilState.BackFace.StencilFailOp = gcm_stencil_op(rs->bs_fail);
                pd->DepthStencilState.BackFace.StencilDepthFailOp = gcm_stencil_op(rs->bs_zfail);
                pd->DepthStencilState.BackFace.StencilPassOp = gcm_stencil_op(rs->bs_zpass);
            } else {
                pd->DepthStencilState.BackFace = pd->DepthStencilState.FrontFace;
            }
        } else {
            pd->DepthStencilState.StencilEnable = FALSE;
        }
    }
}

static D3D12_TEXTURE_ADDRESS_MODE gcm_wrap(u32 w)
{
    switch (w & 0xF) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 6:
    case 7:
    case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

/* LD_CONVOLUTION=0: plain bilinear for the RSX convolution filters, without
 * the kernel in the shader (rsx_fp_apply_convolution). For comparison. */
static int ld_convolution_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char* text = getenv("LD_CONVOLUTION");
        enabled = (text && *text == '0') ? 0 : 1;
    }
    return enabled;
}

static D3D12_SAMPLER_DESC decode_sampler(const rsx_dsp_texture* t)
{
    D3D12_SAMPLER_DESC sd = {0};
    const u32 minf = (t->filter >> 16) & 0x7;
    const u32 magf = (t->filter >> 24) & 0x7;
    /* 7 (CONVOLUTION_MIN) and mag 4 (CONVOLUTION_MAG) are the RSX's
     * convolution filters: a quincunx or Gaussian kernel over neighbouring
     * texels, used to resolve the 2x-wide AA surfaces and to soften
     * upscaled blur buffers. Read as POINT they were the hardest filter
     * there is -- Folklore's bloom came out as blocks and its resolved
     * frame stippled. Bilinear is the nearest D3D12 sampler state (the same
     * choice RPCS3 makes). */
    const int min_linear = (minf == 2 || minf == 4 || minf == 6 || minf == 7);
    const int mag_linear = (magf == 2 || magf == 4);
    { static unsigned reported = 0;
      if ((minf == 7 || magf == 4) && reported < 4) {
          reported++;
          fprintf(stderr, "[live-draw] convolution filter on a %ux%u texture (min %u, mag %u, kernel %u): sampled bilinear\n",
                  t->width, t->height, minf, magf, (t->filter >> 13) & 0x7);
      } }
    const int mip_linear = (minf == 5 || minf == 6);
    const int mip_present = (minf >= 3);
    D3D12_FILTER_TYPE mnf = min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mgf = mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mpf = mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(mnf, mgf, mpf, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.AddressU = gcm_wrap(t->wrap);
    sd.AddressV = gcm_wrap(t->wrap >> 8);
    sd.AddressW = gcm_wrap(t->wrap >> 16);
    const u32 max_lod_fx = (t->control0 >> 7)  & 0xFFF;
    const u32 min_lod_fx = (t->control0 >> 19) & 0xFFF;
    sd.MinLOD = (float)min_lod_fx / 256.0f;
    sd.MaxLOD = mip_present ? (float)max_lod_fx / 256.0f : 0.0f;
    if (sd.MaxLOD < sd.MinLOD) sd.MaxLOD = sd.MinLOD;
    sd.MaxAnisotropy = 1;
    /* Anisotropic filtering (engine setting): only for mip-mapped textures
     * the game already filters linearly -- the world's surfaces. Render
     * targets read back by post effects have no mips and point-sampled
     * lookups keep their filter, so nothing that relies on exact texels
     * changes. Anisotropic implies trilinear between mips. */
    if (s_ld_sampler_anisotropy > 1u && mip_present && min_linear && mag_linear)
    {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = s_ld_sampler_anisotropy;
    }
    /* Texture detail (engine setting): a negative mip bias for the same
     * textures, so a finer mip level is chosen. The PS3 picked mips for a
     * 720-line picture with no other filtering to keep the finer ones from
     * shimmering; with supersampling, anisotropic filtering and
     * anti-aliasing here they hold up, and surfaces keep their detail
     * further away. Render targets and point-sampled lookups are left as
     * they are. */
    if (s_ld_sampler_lod_bias != 0 && mip_present && min_linear)
    {
        sd.MipLODBias = (float)s_ld_sampler_lod_bias / 100.0f;
    }
    return sd;
}

/* A changed anisotropy setting rebuilds the sampler cache at a frame
 * boundary, once the GPU has finished with every table that holds the old
 * descriptors. */
static void ld_wait_idle(void);
static void ld_refresh_sampler_settings(void)
{
    u32 requested = (u32)InterlockedCompareExchange(&g_ld_anisotropy, 0, 0);
    if (requested != 2u && requested != 4u && requested != 8u && requested != 16u)
    {
        requested = 1u;
    }
    LONG requestedBias = InterlockedCompareExchange(&g_ld_texture_lod_bias, 0, 0);
    if (requestedBias < -200 || requestedBias > 0)
    {
        requestedBias = 0;
    }
    if (requested == s_ld_sampler_anisotropy && requestedBias == s_ld_sampler_lod_bias)
    {
        return;
    }
    ld_wait_idle();
    s_ld_sampler_anisotropy = requested;
    s_ld_sampler_lod_bias = requestedBias;
    g.n_samplers = 0;
    g.smp_ring_used = 0;
    memset(g.smp_table_fence, 0, sizeof(g.smp_table_fence));
    fprintf(stderr, "[live-draw] anisotropic filtering %ux, texture mip bias %.2f\n", requested,
            (double)requestedBias / 100.0);
}
/* Cache key of a sampler: exactly the fields decode_sampler reads, so two states share a descriptor only when
 * they decode alike. The previous key kept wrap bits 0-11 (U and V, none of W at bits 16-19) and shifted the 21-bit
 * LOD field of control0 left by 18, which drops its top seven bits: min LOD (control0 bits 19-30) above bit 20
 * and the W address mode were lost, so states differing only there reused another state's descriptor. */
static u64 sampler_key(const rsx_dsp_texture* t)
{
    const u64 minf    = (t->filter >> 16) & 0x7;
    const u64 magf    = (t->filter >> 24) & 0x7;
    const u64 wrap_u  = t->wrap & 0xF;
    const u64 wrap_v  = (t->wrap >> 8) & 0xF;
    const u64 wrap_w  = (t->wrap >> 16) & 0xF;
    const u64 max_lod = (t->control0 >> 7) & 0xFFF;
    const u64 min_lod = (t->control0 >> 19) & 0xFFF;
    return minf | (magf << 3) | (wrap_u << 6) | (wrap_v << 10) | (wrap_w << 14) | (max_lod << 18) | (min_lod << 30);
}

/* ---------------------------------------------------------------------------
 * descriptor heap helpers
 * -----------------------------------------------------------------------*/
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(u32 idx)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.rtv_heap, &h);
    h.ptr += (size_t)idx * g.rtv_step;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.srv_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_cpu_heap, &h);
    h.ptr += (size_t)slot * g.srv_step;
    return h;
}
static void srv_write(u32 slot, ID3D12Resource* tex)
{
    g.dev->lpVtbl->CreateShaderResourceView(g.dev, tex, NULL, srv_cpu(slot));
}
static void srv_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* The replay-proven depth snapshot broadcasts depth to RGB. Mirror that
     * mapping directly in the native SRV and force alpha to one. */
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void srv_write_depth_source(u32 slot, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void uav_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateUnorderedAccessView(
        g.dev, tex, NULL, &ud, srv_cpu(slot));
}
static D3D12_GPU_DESCRIPTOR_HANDLE srv_table(const u32 slots[SRV_TABLE_SIZE])
{
    if (g.srv_ring_used >= ld_srv_end()) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    const u32 base = g.srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SRV_TABLE_SIZE];
    const UINT dst_size = SRV_TABLE_SIZE;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_heap, &dst);
    dst.ptr += (size_t)base * g.srv_step;
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++)
        src[i] = srv_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SRV_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.srv_heap, &h);
    h.ptr += (u64)base * g.srv_step;
    return h;
}
static int depth_snapshot_descriptors(
    u32 zindex, D3D12_GPU_DESCRIPTOR_HANDLE* out_source,
    D3D12_GPU_DESCRIPTOR_HANDLE* out_destination)
{
    if (zindex >= MAX_SURFACES || g.srv_ring_used >= ld_srv_end())
        return 0;
    const u32 base = g.srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        g.srv_heap, &cpu);
    cpu.ptr += (size_t)base * g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(SRV_DEPTH_SOURCE_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu.ptr += g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(UAV_ZDEPTH_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        g.srv_heap, &gpu);
    gpu.ptr += (u64)base * g.srv_step;
    *out_source = gpu;
    gpu.ptr += g.srv_step;
    *out_destination = gpu;
    return 1;
}
static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.smp_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_cpu_heap, &h);
    h.ptr += (size_t)slot * g.smp_step;
    return h;
}
static u32 sampler_slot(const rsx_dsp_texture* t, u64 key)
{
    for (u32 i = 0; i < g.n_samplers; i++)
        if (g.smp_keys[i] == key) return 1 + i;
    if (g.n_samplers >= SMP_CACHE_SLOTS) return SMP_DEFAULT;
    D3D12_SAMPLER_DESC sd = decode_sampler(t);
    const u32 slot = 1 + g.n_samplers;
    g.dev->lpVtbl->CreateSampler(g.dev, &sd, smp_cpu(slot));
    g.smp_keys[g.n_samplers++] = key;
    return slot;
}
/* A sampler table no longer referenced by any submission that can still run:
 * free, or used last by a list whose fence has completed. Returns the slot,
 * or SMP_RING_TABLES when every table may still be read by the GPU. */
static u32 sampler_table_free_slot(void)
{
    if (g.smp_ring_used < SMP_RING_TABLES)
    {
        return g.smp_ring_used;
    }
    const u64 completed = g.fence->lpVtbl->GetCompletedValue(g.fence);
    u32 oldestSlot = SMP_RING_TABLES;
    for (u32 slotIndex = 0; slotIndex < SMP_RING_TABLES; slotIndex++)
    {
        if (g.smp_table_fence[slotIndex] > completed)
        {
            continue;
        }
        if (oldestSlot == SMP_RING_TABLES ||
            g.smp_table_fence[slotIndex] < g.smp_table_fence[oldestSlot])
        {
            oldestSlot = slotIndex;
        }
    }
    return oldestSlot;
}

/* Sampler descriptor tables persist across frames (a scene reuses the same
 * few combinations every frame), each stamped with the fence of the last
 * list that used it, so with frames in flight a table is only rewritten
 * once the GPU is done with it. */
static D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(const u32 slots[SMP_TABLE_SIZE])
{
    for (u32 i = 0; i < g.smp_ring_used; i++) {
        if (memcmp(g.smp_ring_slots[i], slots, sizeof(g.smp_ring_slots[i])) == 0) {
            g.smp_table_fence[i] = ld_open_list_fence();
            D3D12_GPU_DESCRIPTOR_HANDLE cached;
            g.smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
                g.smp_heap, &cached);
            cached.ptr += (u64)(i * SMP_TABLE_SIZE) * g.smp_step;
            return cached;
        }
    }
    /* The caller preflights capacity before recording render-state commands,
     * because a command-list flush here would discard that state. */
    const u32 slot = sampler_table_free_slot();
    if (slot >= SMP_RING_TABLES) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    if (slot == g.smp_ring_used)
    {
        g.smp_ring_used++;
    }
    g.smp_table_fence[slot] = ld_open_list_fence();
    const u32 base = slot * SMP_TABLE_SIZE;
    memcpy(g.smp_ring_slots[slot], slots, sizeof(g.smp_ring_slots[0]));
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SMP_TABLE_SIZE];
    const UINT dst_size = SMP_TABLE_SIZE;
    g.smp_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_heap, &dst);
    dst.ptr += (size_t)base * g.smp_step;
    for (u32 i = 0; i < SMP_TABLE_SIZE; i++)
        src[i] = smp_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SMP_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.smp_heap, &h);
    h.ptr += (u64)base * g.smp_step;
    return h;
}

/* ---------------------------------------------------------------------------
 * command list submit/wait (simple synchronous model, like the harness)
 * -----------------------------------------------------------------------*/
u64 ps3_fs_read_ns(void);      /* [hitch] line, runtime/ppu/ppu_fs.cpp */
u64 ps3_fs_read_bytes(void);
extern volatile u64 g_gcm_perf_gpu_wait_ns;   /* [fps] breakdown, cellGcmSys.c */
extern volatile u64 g_gcm_perf_swap_ns;
extern volatile u64 g_gcm_perf_waitflip_ns;
extern volatile u64 g_gcm_perf_present_idle_ns;
extern volatile u64 g_gcm_perf_main_usleep_ns;
extern volatile u64 g_gcm_perf_main_syscall_ns;
extern volatile u64 g_gcm_perf_main_hle_ns;
extern volatile u64 g_gcm_perf_ring_wait_ns;
extern volatile u32 g_gcm_ring_frame_peak;
extern volatile u32 g_gcm_ring_bytes;
extern volatile u64 g_gcm_perf_drain_ns;
extern volatile u64 g_gcm_perf_present_ns;
extern volatile u64 g_gcm_perf_flip_drain_lat_ns;
extern volatile u64 g_gcm_perf_flip_show_lat_ns;
extern volatile u32 g_gcm_perf_flips_shown;
extern volatile u32 g_gcm_perf_waitflip_timeouts;
extern u64 gcm_perf_now_ns(void);
static void ld_flush(ld_flush_reason reason)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG flush_begin = ld_profile_qpc();
    g_ld_profile.total.flush_reason[reason]++;
    ld_profile_note_ring_highwater();
#else
    (void)reason;
#endif
    const HRESULT close_hr = g.list->lpVtbl->Close(g.list);
    if (FAILED(close_hr)) {
        fprintf(stderr,
                "[d3d-fail] command-list Close hr=0x%08lX frame=%u\n",
                (unsigned long)close_hr, g_ld_frames);
        ld_dump_dred("Close", close_hr);
        g.ready = 0;
        return;
    }
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    const u64 v = ++g.fence_value;
    const HRESULT signal_hr =
        g.queue->lpVtbl->Signal(g.queue, g.fence, v);
    if (FAILED(signal_hr)) {
        fprintf(stderr,
                "[d3d-fail] queue Signal hr=0x%08lX frame=%u\n",
                (unsigned long)signal_hr, g_ld_frames);
        ld_dump_dred("Signal", signal_hr);
        g.ready = 0;
        return;
    }
    if (g.fence->lpVtbl->GetCompletedValue(g.fence) < v) {
#if defined(YZ_PERF_PROFILE)
        const LONGLONG wait_begin = ld_profile_qpc();
#endif
        const HRESULT event_hr =
            g.fence->lpVtbl->SetEventOnCompletion(
                g.fence, v, g.fence_event);
        if (FAILED(event_hr)) {
            fprintf(stderr,
                    "[d3d-fail] fence SetEventOnCompletion "
                    "hr=0x%08lX frame=%u\n",
                    (unsigned long)event_hr, g_ld_frames);
            ld_dump_dred("SetEventOnCompletion", event_hr);
            g.ready = 0;
            return;
        }
        {
            const u64 gpuWaitBegin = gcm_perf_now_ns();
            WaitForSingleObject(g.fence_event, INFINITE);
            g_gcm_perf_gpu_wait_ns += gcm_perf_now_ns() - gpuWaitBegin;
        }
#if defined(YZ_PERF_PROFILE)
        const u64 wait_ticks = (u64)(ld_profile_qpc() - wait_begin);
        g_ld_profile.total.fence_wait_qpc += wait_ticks;
        g_ld_profile.total.fence_reason_qpc[reason] += wait_ticks;
#endif
    }
    ld_drain_info_queue("flush");
    /* Dynamic guest textures can replace a cached D3D resource while an
     * earlier draw in this command list still references the old one.  The
     * fence above is the first safe point at which those old resources may be
     * released. It covers every earlier submission too, so the resources
     * frames in flight retired go now as well. */
    for (u32 i = 0; i < g.n_retired_textures; i++)
        g.retired_textures[i]->lpVtbl->Release(g.retired_textures[i]);
    g.n_retired_textures = 0;
    for (u32 bankIndex = 0; bankIndex < LD_BANKS; bankIndex++)
    {
        for (u32 retiredIndex = 0; retiredIndex < g.bank_n_retired[bankIndex]; retiredIndex++)
        {
            g.bank_retired[bankIndex][retiredIndex]->lpVtbl->Release(
                g.bank_retired[bankIndex][retiredIndex]);
        }
        g.bank_n_retired[bankIndex] = 0;
    }
    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    g.upload_used = ld_upload_base();
#if defined(YZ_PERF_PROFILE)
    const u64 flush_ticks = (u64)(ld_profile_qpc() - flush_begin);
    g_ld_profile.total.flush_qpc += flush_ticks;
    g_ld_profile.total.flush_reason_qpc[reason] += flush_ticks;
#endif
}

/* Block until the GPU has reached fence value `value`. */
static void ld_wait_fence(u64 value)
{
    if (!value || g.fence->lpVtbl->GetCompletedValue(g.fence) >= value)
    {
        return;
    }
    if (FAILED(g.fence->lpVtbl->SetEventOnCompletion(g.fence, value, g.fence_event)))
    {
        g.ready = 0;
        return;
    }
    const u64 gpuWaitBegin = gcm_perf_now_ns();
    WaitForSingleObject(g.fence_event, INFINITE);
    g_gcm_perf_gpu_wait_ns += gcm_perf_now_ns() - gpuWaitBegin;
}

/* Every submitted command list has finished on the GPU. */
static void ld_wait_idle(void)
{
    ld_wait_fence(g.fence_value);
}

/* End of a presented frame. Submits the frame without waiting for it and
 * moves on to the other bank, so the CPU decodes and records the next frame
 * while the GPU renders this one; only if the GPU is still on the frame
 * before (the one that last used the other bank) does it wait. The old
 * model waited for every frame to finish on the GPU before starting the
 * next (ld_flush), so the two never overlapped. LD_NO_PIPELINE=1 keeps it. */
static void ld_submit_frame(void)
{
    if (!g.pipelined ||
        g.bank_n_retired[g.bank] + g.n_retired_textures > MAX_RETIRED_TEXTURES)
    {
        ld_flush(LD_FLUSH_PRESENT);
        ld_rings_reset();
        ld_refresh_sampler_settings();
        return;
    }
    const HRESULT closeResult = g.list->lpVtbl->Close(g.list);
    if (FAILED(closeResult))
    {
        fprintf(stderr, "[d3d-fail] command-list Close hr=0x%08lX frame=%u\n",
                (unsigned long)closeResult, g_ld_frames);
        ld_dump_dred("Close", closeResult);
        g.ready = 0;
        return;
    }
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    const u64 submitted = ++g.fence_value;
    const HRESULT signalResult = g.queue->lpVtbl->Signal(g.queue, g.fence, submitted);
    if (FAILED(signalResult))
    {
        fprintf(stderr, "[d3d-fail] queue Signal hr=0x%08lX frame=%u\n",
                (unsigned long)signalResult, g_ld_frames);
        ld_dump_dred("Signal", signalResult);
        g.ready = 0;
        return;
    }
    g.bank_fence[g.bank] = submitted;
    /* Resources this frame retired go when its fence completes. */
    for (u32 retiredIndex = 0; retiredIndex < g.n_retired_textures; retiredIndex++)
    {
        g.bank_retired[g.bank][g.bank_n_retired[g.bank]++] = g.retired_textures[retiredIndex];
    }
    g.n_retired_textures = 0;

    g.bank = (g.bank + 1u) % LD_BANKS;
    ld_wait_fence(g.bank_fence[g.bank]);
    ld_drain_info_queue("submit");
    for (u32 retiredIndex = 0; retiredIndex < g.bank_n_retired[g.bank]; retiredIndex++)
    {
        g.bank_retired[g.bank][retiredIndex]->lpVtbl->Release(g.bank_retired[g.bank][retiredIndex]);
    }
    g.bank_n_retired[g.bank] = 0;
    g.alloc = g.allocs[g.bank];
    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    g.upload_used = ld_upload_base();
    ld_rings_reset();
    ld_refresh_sampler_settings();
}

/* Public wrapper for the RSX SET_REFERENCE / sync fence: block until the GPU
 * has finished all queued draws (mirrors RPCS3 nv406e::set_reference's sync(),
 * RSXThread.cpp), so the game's REF poll advances only after the GPU has really
 * caught up. Without it our async consumer writes REF instantly and races ahead
 * of real GPU time (measured: ours skips the fence wait RPCS3 performs). Gated
 * at the call site by RSX_FENCE_SYNC. */
void rsx_live_draw_flush(void)
{
    if (g.ready) ld_flush(LD_FLUSH_GUEST_REFERENCE);
}

/* Port overlay (rsx_live_draw.h). */
static rsx_live_overlay_fn s_ld_overlay = NULL;
void rsx_live_draw_set_overlay(rsx_live_overlay_fn fn)
{
    s_ld_overlay = fn;
}

/* Graphics settings (rsx_live_draw.h). */
void rsx_live_draw_set_render_scale(u32 scale)
{
    if (scale < 1u || scale > 4u)
    {
        scale = 1u;
    }
    g_ld_requested_scale = scale;
}

u32 rsx_live_draw_get_render_scale(void)
{
    return g.ready ? g.scale : g_ld_requested_scale;
}

void rsx_live_draw_set_aspect(float aspect)
{
    g_ld_requested_aspect = aspect;
}

float rsx_live_draw_get_aspect(void)
{
    if (!g.ready)
    {
        return g_ld_requested_aspect > 16.0f / 9.0f ? g_ld_requested_aspect : 16.0f / 9.0f;
    }
    return (float)g.width / (float)(g.height ? g.height : 1u) * (g.widen > 1.0f ? g.widen : 1.0f);
}

void rsx_live_draw_set_post_aa(int mode)
{
    InterlockedExchange(&g_ld_post_aa, mode == 1 ? 1 : 0);
}

void rsx_live_draw_set_async_shaders(int enabled)
{
    InterlockedExchange(&g_ld_async_shaders, enabled ? 1 : 0);
}

void rsx_live_draw_set_anisotropy(u32 level)
{
    InterlockedExchange(&g_ld_anisotropy, (LONG)level);
}

void rsx_live_draw_set_texture_lod_bias(int hundredths)
{
    InterlockedExchange(&g_ld_texture_lod_bias, (LONG)hundredths);
}

/* Pixel shader replacements (rsx_live_draw_set_ps_replacement). */
typedef struct
{
    u64 sourceHash;
    const char* hlsl;
} ld_ps_replacement;
static ld_ps_replacement g_ld_ps_replacements[16];
static volatile LONG g_ld_ps_replacement_count = 0;
static SRWLOCK g_ld_ps_replacement_lock = SRWLOCK_INIT;

void rsx_live_draw_set_ps_replacement(u64 sourceHash, const char* hlsl)
{
    AcquireSRWLockExclusive(&g_ld_ps_replacement_lock);
    LONG count = g_ld_ps_replacement_count;
    LONG found = -1;
    for (LONG entryIndex = 0; entryIndex < count; entryIndex++)
    {
        if (g_ld_ps_replacements[entryIndex].sourceHash == sourceHash)
        {
            found = entryIndex;
        }
    }
    if (hlsl)
    {
        if (found < 0 && count < (LONG)(sizeof(g_ld_ps_replacements) / sizeof(g_ld_ps_replacements[0])))
        {
            found = count;
            count++;
        }
        if (found >= 0)
        {
            g_ld_ps_replacements[found].sourceHash = sourceHash;
            g_ld_ps_replacements[found].hlsl = hlsl;
        }
    }
    else if (found >= 0)
    {
        count--;
        g_ld_ps_replacements[found] = g_ld_ps_replacements[count];
    }
    InterlockedExchange(&g_ld_ps_replacement_count, count);
    ReleaseSRWLockExclusive(&g_ld_ps_replacement_lock);
}

/* Swap a translated pixel shader for a registered replacement. Returns 1 when
 * it did. */
static int ld_apply_ps_replacement(char* hlsl, size_t capacity)
{
    if (InterlockedCompareExchange(&g_ld_ps_replacement_count, 0, 0) == 0)
    {
        return 0;
    }
    /* Normalised so one fragment program has one hash whatever is bound
     * with it: from the constants block on (the texture declarations before
     * it depend on which units hold cube maps), and with rsx_tex[N] read as
     * rsx_texN (the form the translator uses when a cube map is bound). */
    const char* start = strstr(hlsl, "cbuffer PSConstants");
    u64 sourceHash = 0xCBF29CE484222325ull;
    for (const unsigned char* cursor = (const unsigned char*)(start ? start : hlsl); *cursor; cursor++)
    {
        if (cursor[0] == '[' && cursor > (const unsigned char*)hlsl + 7 &&
            memcmp(cursor - 7, "rsx_tex", 7) == 0 && cursor[1] >= '0' && cursor[1] <= '9')
        {
            const unsigned char* digit = cursor + 1;
            while (*digit >= '0' && *digit <= '9')
            {
                sourceHash ^= *digit;
                sourceHash *= 0x100000001B3ull;
                digit++;
            }
            cursor = (*digit == ']') ? digit : digit - 1;
            continue;
        }
        sourceHash ^= *cursor;
        sourceHash *= 0x100000001B3ull;
    }
    int replaced = 0;
    AcquireSRWLockShared(&g_ld_ps_replacement_lock);
    for (LONG entryIndex = 0; entryIndex < g_ld_ps_replacement_count; entryIndex++)
    {
        const ld_ps_replacement* entry = &g_ld_ps_replacements[entryIndex];
        if (entry->sourceHash == sourceHash && strlen(entry->hlsl) < capacity)
        {
            memcpy(hlsl, entry->hlsl, strlen(entry->hlsl) + 1);
            replaced = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_ld_ps_replacement_lock);
    if (replaced)
    {
        static int replacementLogs = 0;
        if (replacementLogs < 16)
        {
            replacementLogs++;
            fprintf(stderr, "[ps-replace] pso %016llx: shader %016llx replaced by the title's version\n",
                    (unsigned long long)live_legacy_pso_key(), (unsigned long long)sourceHash);
        }
    }
    return replaced;
}

void rsx_live_draw_set_sharpening(u32 percent)
{
    InterlockedExchange(&g_ld_sharpen_percent, (LONG)(percent > 100u ? 100u : percent));
}

void rsx_live_draw_get_shader_stats(u32* pending, u32* built, u32* skippedDraws)
{
    if (pending)
    {
        const LONG value = InterlockedCompareExchange(&g_ld_pso_jobs_pending, 0, 0);
        *pending = value > 0 ? (u32)value : 0u;
    }
    if (built)
    {
        *built = (u32)InterlockedCompareExchange(&g_ld_pso_jobs_done, 0, 0);
    }
    if (skippedDraws)
    {
        *skippedDraws = (u32)InterlockedCompareExchange(&g_ld_pso_draws_skipped, 0, 0);
    }
}

/* The backbuffer `bb` (index `index`) has just been written, by a copy
 * (COPY_DEST) or by the present pass (RENDER_TARGET), and is in `state`: run
 * the overlay over it, if one is set, and leave it in PRESENT either way. */
static void ld_overlay_then_present_state(ID3D12Resource* bb, u32 index,
                                          D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = bb;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state;
    if (s_ld_overlay)
    {
        if (state != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g.list->lpVtbl->ResourceBarrier(g.list, 1, &barrier);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE target = rtv_handle(index);
        g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &target, FALSE, NULL);
        {
            const D3D12_VIEWPORT viewport = {0.0f, 0.0f, (float)g.bb_w, (float)g.bb_h, 0.0f, 1.0f};
            const D3D12_RECT scissor = {0, 0, (LONG)g.bb_w, (LONG)g.bb_h};
            g.list->lpVtbl->RSSetViewports(g.list, 1, &viewport);
            g.list->lpVtbl->RSSetScissorRects(g.list, 1, &scissor);
        }
        s_ld_overlay(g.dev, g.queue, g.list, (unsigned long long)target.ptr,
                     (unsigned)DXGI_FORMAT_R8G8B8A8_UNORM, g.bb_w, g.bb_h);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_PRESENT)
    {
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &barrier);
    }
}

static void retire_texture(ID3D12Resource* tex)
{
    if (!tex) return;
    if (g.n_retired_textures >= MAX_RETIRED_TEXTURES)
        ld_flush(LD_FLUSH_RETIRE_QUEUE);
    g.retired_textures[g.n_retired_textures++] = tex;
    ld_profile_note_ring_highwater();
}

/* ---------------------------------------------------------------------------
 * texture upload (single-level + mip)
 * -----------------------------------------------------------------------*/
static u32 log2_u32(u32 v) { u32 n = 0; while (v > 1) { v >>= 1; n++; } return n; }
static u32 morton_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 idx = 0, shift = 0;
    while (lw || lh) {
        if (lw) { idx |= (x & 1) << shift; x >>= 1; shift++; lw--; }
        if (lh) { idx |= (y & 1) << shift; y >>= 1; shift++; lh--; }
    }
    return idx;
}
static u8 remap_comp(const u8 s[4], u32 remap, u32 comp)
{
    const u32 op  = (remap >> (8 + comp * 2)) & 3;
    const u32 sel = (remap >> (comp * 2)) & 3;
    if (op == 0) return 0;
    if (op == 1) return 255;
    return s[sel];
}
static void decode_texel(u32 base_fmt, const u8* p, u32 remap, u8 d[4])
{
    u8 s[4];
    switch (base_fmt) {
    /* B8 is one channel. Broadcasting it to alpha as well is arguably more
     * faithful (the YUV movie combine shader samples each plane's .w), but
     * DO NOT do it until the USM decode actually fills those planes: while
     * they are empty it only makes the garbage VISIBLE, putting green noise
     * behind the title screen and every other movie-backed UI. Tried
     * 2026-09-01, reverted the same session. See ydkj-live-draw-rebuild. */
    case TEX_FMT_B8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    case TEX_FMT_A4R4G4B4: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (u8)(((v >> 12) & 0xF) * 17); s[1] = (u8)(((v >> 8) & 0xF) * 17);
        s[2] = (u8)(((v >> 4) & 0xF) * 17);  s[3] = (u8)((v & 0xF) * 17); break;
    }
    case TEX_FMT_A1R5G5B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        /* LD_FORCE_A1_OPAQUE=1: experiment, not a fix. A PS1 15-bit pixel uses
         * the top bit as a semi-transparency FLAG, and it is clear for ordinary
         * opaque pixels -- so decoding it as A1R5G5B5 alpha gives alpha=0 for
         * essentially the whole framebuffer. If the pipeline then blends on
         * source alpha, every pixel is discarded and the surface reads black,
         * which is exactly what LD_SURF_DUMP reports for all nine surfaces.
         * This flips alpha to opaque so that theory can be tested in one run
         * instead of argued about. */
        { static int fo = -1;
          if (fo < 0) fo = getenv("LD_FORCE_A1_OPAQUE") ? 1 : 0;
          s[0] = fo ? 255 : ((v & 0x8000) ? 255 : 0); }
        s[1] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x1F) * 255 / 31);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_R5G6B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = 255;
        s[1] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_G8B8:
        s[0] = 255;
        s[1] = s[2] = p[0];
        s[3] = p[1];
        break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: {
        /* Four big-endian halves, X Y Z W -> R G B A. Values are linear and
         * may exceed 1.0 (these are render targets), so clamp rather than
         * wrap. Decoded to 8-bit because the cache is RGBA8 throughout. */
        for (int c = 0; c < 4; c++) {
            const u16 hbits = (u16)((p[c * 2] << 8) | p[c * 2 + 1]);
            const int  exp  = (hbits >> 10) & 0x1F;
            const int  man  = hbits & 0x3FF;
            float f;
            if (exp == 0)        f = (float)man * (1.0f / 16384.0f) * (1.0f / 64.0f);
            else if (exp == 31)  f = 1.0f;
            else {
                float m = 1.0f + (float)man / 1024.0f;
                int   e = exp - 15;
                while (e > 0) { m *= 2.0f; e--; }
                while (e < 0) { m *= 0.5f; e++; }
                f = m;
            }
            if (hbits & 0x8000) f = 0.0f;   /* negative -> clamp to zero */
            if (f > 1.0f) f = 1.0f;
            /* s[] is A,R,G,B; the halves arrive X,Y,Z,W = R,G,B,A. */
            const u8 v = (u8)(f * 255.0f + 0.5f);
            if (c == 3) s[0] = v; else s[c + 1] = v;
        }
        break;
    }
    case TEX_FMT_DEPTH24_D8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    default: s[0] = p[0]; s[1] = p[1]; s[2] = p[2]; s[3] = p[3]; break;
    }
    d[0] = remap_comp(s, remap, 1);
    d[1] = remap_comp(s, remap, 2);
    d[2] = remap_comp(s, remap, 3);
    d[3] = remap_comp(s, remap, 0);
}

typedef struct { u32 w, h; const u8* data; u32 row_bytes, rows; } tex_level_t;

static ID3D12Resource* create_texture_mipped(DXGI_FORMAT fmt, const tex_level_t* lv, u32 n)
{
    if (n == 0) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 1;
    rd.MipLevels = (u16)n; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&tex)))
        return NULL;
    for (u32 m = 0; m < n; m++) {
        const u32 pitch = (lv[m].row_bytes + 255) & ~255u;
        const u32 start = (g.upload_used + 511) & ~511u;
        if ((u64)start + (u64)pitch * lv[m].rows > ld_upload_end()) break;
        for (u32 y = 0; y < lv[m].rows; y++)
            memcpy(g.upload_mapped + start + (size_t)y * pitch,
                   lv[m].data + (size_t)y * lv[m].row_bytes, lv[m].row_bytes);
        D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
        src.pResource = g.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = start;
        src.PlacedFootprint.Footprint.Format = fmt;
        src.PlacedFootprint.Footprint.Width = lv[m].w;
        src.PlacedFootprint.Footprint.Height = lv[m].h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        g.upload_used = start + pitch * lv[m].rows;
        g_ld_texture_upload_bytes += (u64)pitch * lv[m].rows;
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.texture_upload_bytes +=
            (u64)pitch * lv[m].rows;
#endif
        ld_profile_note_ring_highwater();
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return tex;
}

/* Cubemap faces are stored face-major in guest memory. D3D12 subresources use
 * that same order: face * mip_count + mip. */
static ID3D12Resource* create_texture_cube(
    DXGI_FORMAT fmt, const tex_level_t* lv, u32 n_mips)
{
    if (!n_mips) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 6;
    rd.MipLevels = (u16)n_mips; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&tex)))
        return NULL;
    for (u32 face = 0; face < 6; face++) {
        for (u32 mip = 0; mip < n_mips; mip++) {
            const tex_level_t* level = &lv[face * n_mips + mip];
            const u32 pitch = (level->row_bytes + 255) & ~255u;
            const u32 start = (g.upload_used + 511) & ~511u;
            if ((u64)start + (u64)pitch * level->rows > ld_upload_end()) {
                retire_texture(tex);     /* earlier faces' copies are recorded */
                return NULL;
            }
            for (u32 y = 0; y < level->rows; y++)
                memcpy(g.upload_mapped + start + (size_t)y * pitch,
                       level->data + (size_t)y * level->row_bytes,
                       level->row_bytes);
            D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
            src.pResource = g.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = start;
            src.PlacedFootprint.Footprint.Format = fmt;
            src.PlacedFootprint.Footprint.Width = level->w;
            src.PlacedFootprint.Footprint.Height = level->h;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = pitch;
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = face * n_mips + mip;
            g.list->lpVtbl->CopyTextureRegion(
                g.list, &dst, 0, 0, 0, &src, NULL);
            g.upload_used = start + pitch * level->rows;
            g_ld_texture_upload_bytes += (u64)pitch * level->rows;
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.texture_upload_bytes +=
                (u64)pitch * level->rows;
#endif
            ld_profile_note_ring_highwater();
        }
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return tex;
}

static ID3D12Resource* create_texture_rgba(const u8* rgba, u32 w, u32 h)
{
    tex_level_t lv = { w, h, rgba, w * 4, h };
    return create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, &lv, 1);
}

static u32 texture_source_span(const rsx_dsp_texture* t)
{
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    u32 texel_size = 0, block_size = 0;
    switch (base_fmt) {
    case TEX_FMT_DXT1:  block_size = 8; break;
    case TEX_FMT_DXT23:
    case TEX_FMT_DXT45: block_size = 16; break;
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8: texel_size = 2; break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: texel_size = 8; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    default: return 0;
    }
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096 ||
        t->dimension != 2)
        return 0;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;
    /* A pitch-linear (LN) texture cannot carry a mip chain -- the RSX samples
     * only its base level and ignores the mipmap count in SET_TEXTURE_FORMAT.
     * Honouring that count instead walks `off += pitch * mh` through whatever
     * guest memory follows the base image and builds every lower level out of
     * it, and a minified draw then samples that garbage.
     *
     * Cherry-picked by hand from flow/live-draw 3f4bdb9, where flOw's EULA
     * showed "banded olive stripes with a ghosted second copy of the page".
     * Twisted Metal binds PS1 VRAM pitch-linear (fmt 0xE2/0xE1 carry
     * TEX_FMT_LINEAR) and shows banded green/magenta stripes, which is the same
     * symptom on the same mechanism. */
    if (linear) n_mips = 1;
    if (t->cubemap && block_size) {
        n_mips = 1;
        for (u32 d = (t->width < t->height ? t->width : t->height) / 4;
             d > 1; d >>= 1)
            n_mips++;
        if (t->mipmaps && n_mips > t->mipmaps)
            n_mips = t->mipmaps;
        if (n_mips > 14) n_mips = 14;
    }
    u32 mw = t->width, mh = t->height, span = 0;
    for (u32 m = 0; m < n_mips; m++) {
        if (block_size)
            span += ((mw + 3) / 4) * block_size * ((mh + 3) / 4);
        else {
            const u32 pitch = (m == 0 && linear && t->pitch)
                ? t->pitch : mw * texel_size;
            span += pitch * mh;
        }
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    return t->cubemap ? span * 6 : span;
}

static u64 texture_content_hash(const rsx_dsp_texture* t, int* readable)
{
    const u32 span = texture_source_span(t);
    const u8* src = span ? guest_ptr(t->location, t->offset, span) : NULL;
    if (!src) {
        *readable = 0;
        return 0;
    }
    /* LD_TEXSRC_DBG=1: is the guest data there AT ALL?
     *
     * "Draws execute, forced green fills the screen, the sampled texture is
     * black" keeps arriving as the same question, and the decoded-RGBA dump
     * cannot answer it: that dump lives on the uncompressed path, so a DXT
     * texture -- which is most of a real game's atlas -- never reaches it.
     * Counting non-zero SOURCE bytes covers every format and separates "the
     * upload never happened" from "decode ate it". */
    { static int ts = -1; static int tsn = 0;
      if (ts < 0) ts = getenv("LD_TEXSRC_DBG") ? 1 : 0;
      if (ts && tsn < 16) {
          u32 nz = 0;
          for (u32 q = 0; q < span; q++) if (src[q]) nz++;
          char head[80]; int hn = 0;
          for (u32 q = 0; q < 16 && q < span; q++)
              hn += snprintf(head + hn, sizeof head - hn, "%02X", src[q]);
          fprintf(stderr, "[tex-src] loc=%u off=0x%08X %ux%u fmt=0x%02X mips=%u"
                          " remap=0x%08X filter=0x%08X head=%s"
                          " span=%u nonzero=%u\n",
                  t->location, t->offset, t->width, t->height,
                  t->format & 0xFF, t->mipmaps, t->remap, t->filter, head,
                  span, nz);
          fflush(stderr);
          tsn++;
      } }

    /* One hash per cached texture per presented frame -- a mutation detector,
     * not a content ID. Every bound texture is re-read in full each frame, so
     * this is a hot loop (a busy scene binds tens of megabytes of texels): four
     * independent FNV-style lanes over 32-byte blocks let the multiplies
     * overlap instead of forming one serial chain, about four times the
     * throughput of the single-lane version for the same detection. */
    u64 laneA = 1469598103934665603ull;
    u64 laneB = 0x9E3779B97F4A7C15ull;
    u64 laneC = 0xC2B2AE3D27D4EB4Full;
    u64 laneD = 0x165667B19E3779F9ull;
    const u64 prime = 1099511628211ull;
    u32 position = 0;
    for (; position + 32 <= span; position += 32)
    {
        u64 words[4];
        memcpy(words, src + position, sizeof(words));
        laneA = (laneA ^ words[0]) * prime;
        laneB = (laneB ^ words[1]) * prime;
        laneC = (laneC ^ words[2]) * prime;
        laneD = (laneD ^ words[3]) * prime;
    }
    u64 hash = laneA ^ (laneB * 31u) ^ (laneC * 961u) ^ (laneD * 29791u);
    for (; position < span; position++)
    {
        hash ^= src[position];
        hash *= prime;
    }
    hash ^= span;
    hash *= prime;
    *readable = 1;
    return hash;
}

static ID3D12Resource* decode_guest_texture(const rsx_dsp_texture* t, u32 remap)
{
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    const u32 w = t->width, h = t->height;
    if (!w || !h || w > 4096 || h > 4096 || t->dimension != 2)
        return NULL;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;
    /* A pitch-linear (LN) texture cannot carry a mip chain -- the RSX samples
     * only its base level and ignores the mipmap count in SET_TEXTURE_FORMAT.
     * Honouring that count instead walks `off += pitch * mh` through whatever
     * guest memory follows the base image and builds every lower level out of
     * it, and a minified draw then samples that garbage.
     *
     * Cherry-picked by hand from flow/live-draw 3f4bdb9, where flOw's EULA
     * showed "banded olive stripes with a ghosted second copy of the page".
     * Twisted Metal binds PS1 VRAM pitch-linear (fmt 0xE2/0xE1 carry
     * TEX_FMT_LINEAR) and shows banded green/magenta stripes, which is the same
     * symptom on the same mechanism. */
    if (linear) n_mips = 1;

    if (base_fmt == TEX_FMT_DXT1 || base_fmt == TEX_FMT_DXT23 ||
        base_fmt == TEX_FMT_DXT45) {
        const DXGI_FORMAT dxgi = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                               : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                           : DXGI_FORMAT_BC3_UNORM;
        const u32 block = base_fmt == TEX_FMT_DXT1 ? 8 : 16;
        const u32 total = texture_source_span(t);
        const u8* src = guest_ptr(t->location, t->offset, total);
        if (!src) return NULL;
        if (t->cubemap) {
            n_mips = 1;
            for (u32 d = (w < h ? w : h) / 4; d > 1; d >>= 1)
                n_mips++;
            if (t->mipmaps && n_mips > t->mipmaps)
                n_mips = t->mipmaps;
            if (n_mips > 14) n_mips = 14;
            const u32 face_span = total / 6;
            tex_level_t cube_levels[6 * 14];
            u32 level = 0;
            for (u32 face = 0; face < 6; face++) {
                u32 mw = w, mh = h, off = 0;
                for (u32 mip = 0; mip < n_mips; mip++) {
                    const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
                    cube_levels[level].w = (mw + 3) & ~3u;
                    cube_levels[level].h = (mh + 3) & ~3u;
                    cube_levels[level].data =
                        src + (size_t)face * face_span + off;
                    cube_levels[level].row_bytes = bw * block;
                    cube_levels[level].rows = bh;
                    level++;
                    off += bw * block * bh;
                    mw = mw > 1 ? mw >> 1 : 1;
                    mh = mh > 1 ? mh >> 1 : 1;
                }
            }
            return create_texture_cube(dxgi, cube_levels, n_mips);
        }
        tex_level_t levels[14];
        u32 mw = w, mh = h, off = 0, n = 0;
        for (u32 m = 0; m < n_mips; m++) {
            const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
            levels[n].w = (mw + 3) & ~3u;
            levels[n].h = (mh + 3) & ~3u;
            levels[n].data = src + off;
            levels[n].row_bytes = bw * block;
            levels[n].rows = bh;
            n++;
            off += bw * block * bh;
            if (mw == 1 && mh == 1) break;
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        return create_texture_mipped(dxgi, levels, n);
    }

    u32 texel_size;
    switch (base_fmt) {
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8: texel_size = 2; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: texel_size = 8; break;
    default: return NULL;
    }
    if (!linear && ((w & (w - 1)) || (h & (h - 1)))) return NULL;
    const u32 span = texture_source_span(t);
    const u8* src = guest_ptr(t->location, t->offset, span);
    if (!src) return NULL;

    if (t->cubemap) {
        u32 available_mips = 1;
        for (u32 mw = w, mh = h; mw > 1 || mh > 1; available_mips++) {
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        if (n_mips > available_mips) n_mips = available_mips;
        const u32 face_span = span / 6;
        u8* rgba[6 * 14] = {0};
        tex_level_t cube_levels[6 * 14];
        u32 level = 0;
        int oom = 0;
        for (u32 face = 0; face < 6 && !oom; face++) {
            u32 mw = w, mh = h, off = 0;
            for (u32 mip = 0; mip < n_mips; mip++) {
                const u32 pitch = (mip == 0 && linear && t->pitch)
                    ? t->pitch : mw * texel_size;
                rgba[level] = (u8*)malloc((size_t)mw * mh * 4);
                if (!rgba[level]) {
                    oom = 1;
                    break;
                }
                const u32 lw = log2_u32(mw), lh = log2_u32(mh);
                const u8* level_src =
                    src + (size_t)face * face_span + off;
                for (u32 y = 0; y < mh; y++)
                    for (u32 x = 0; x < mw; x++) {
                        const u8* pixel = linear
                            ? level_src + (size_t)y * pitch +
                                (size_t)x * texel_size
                            : level_src +
                                (size_t)morton_index(x, y, lw, lh) *
                                    texel_size;
                        decode_texel(
                            base_fmt, pixel, remap,
                            rgba[level] + ((size_t)y * mw + x) * 4);
                    }
                cube_levels[level].w = mw;
                cube_levels[level].h = mh;
                cube_levels[level].data = rgba[level];
                cube_levels[level].row_bytes = mw * 4;
                cube_levels[level].rows = mh;
                level++;
                off += pitch * mh;
                if (mw == 1 && mh == 1) break;
                mw = mw > 1 ? mw >> 1 : 1;
                mh = mh > 1 ? mh >> 1 : 1;
            }
        }
        ID3D12Resource* resource = (!oom && level == 6 * n_mips)
            ? create_texture_cube(
                DXGI_FORMAT_R8G8B8A8_UNORM, cube_levels, n_mips)
            : NULL;
        for (u32 i = 0; i < level; i++) free(rgba[i]);
        return resource;
    }

    u8* rgba[14] = {0};
    tex_level_t levels[14];
    u32 mw = w, mh = h, off = 0, n = 0;
    int oom = 0;
    for (u32 m = 0; m < n_mips; m++) {
        const u32 pitch = (m == 0 && linear && t->pitch)
            ? t->pitch : mw * texel_size;
        rgba[n] = (u8*)malloc((size_t)mw * mh * 4);
        if (!rgba[n]) { oom = 1; break; }
        const u32 lw = log2_u32(mw), lh = log2_u32(mh);
        const u8* level_src = src + off;
        /* LD_PLANE_DUMP=1: write the first few big single-channel (B8) planes
         * straight out as PGM, exactly as the guest laid them down. Settles
         * "is the video decode producing an image?" without guessing from the
         * composited frame. ponytail: one-shot debug, costs nothing when off. */
        { static int pd = -1; static int pdn = 0;
          if (pd < 0) { const char* e = getenv("LD_PLANE_DUMP"); pd = e ? atoi(e) : 0; }
          if (pd && (int)g_ld_frames >= pd && m == 0 && base_fmt == TEX_FMT_B8 && mw >= 320 && pdn < 6) {
              char fn[128];
              snprintf(fn, sizeof fn, "scratch/plane_%02d_%ux%u_p%u.pgm", pdn, mw, mh, pitch);
              FILE* pf = fopen(fn, "wb");
              if (pf) {
                  fprintf(pf, "P5\n%u %u\n255\n", mw, mh);
                  for (u32 yy = 0; yy < mh; yy++)
                      fwrite(level_src + (size_t)yy * pitch, 1, mw, pf);
                  fclose(pf);
                  fprintf(stderr, "[plane-dump] %s linear=%d\n", fn, linear);
              }
              pdn++;
          } }
        for (u32 y = 0; y < mh; y++)
            for (u32 x = 0; x < mw; x++) {
                const u8* pixel = linear
                    ? level_src + (size_t)y * pitch + (size_t)x * texel_size
                    : level_src + (size_t)morton_index(x, y, lw, lh) * texel_size;
                decode_texel(base_fmt, pixel, remap,
                             rgba[n] + ((size_t)y * mw + x) * 4);
            }
        /* LD_TEXRGBA_DUMP=1: the DECODED level-0 RGBA, with a non-black count
         * and the remap that produced it.
         *
         * Forcing the fragment shader to green fills every surface completely
         * (368,640 = 720x512 and 921,600 = 1280x720), so the draws reach the
         * GPU and write every pixel -- the only thing left is that the sampled
         * texture comes back black. The guest MEMORY has content (PS1 VRAM
         * carries the BIOS boot screen), so the question is whether decode
         * preserves it. remap is applied here, and a remap that zeroes channels
         * would produce exactly this. */
        { static int td = -1; static int tdn = 0; static u32 tdf = 0;
          /* LD_TEXRGBA_DUMP=<hex fmt>: only that base format -- the 12-dump
           * cap otherwise fills with whatever decodes first. */
          if (td < 0) { const char* e = getenv("LD_TEXRGBA_DUMP");
                        td = e ? 1 : 0; tdf = e ? (u32)strtoul(e, 0, 16) : 0; }
          const int td_this = td && !(tdf > 1 && base_fmt != tdf);
          /* No size floor: the floor was picked for one title's full-screen
           * textures and silently skipped every UI atlas, which is the size
           * a 2D layer is made of. The dump is opt-in; dump what was asked. */
          if (td_this && m == 0 && tdn < 12 && mw && mh) {
              u64 nb = 0, na = 0;
              for (u32 q = 0; q < mw * mh; q++) {
                  const u8* px = rgba[n] + (size_t)q * 4;
                  if (px[0] || px[1] || px[2]) nb++;
                  if (px[3] > 0x1A) na++;
              }
              char fn[160];
              snprintf(fn, sizeof fn, "scratch/tex_%02d_%ux%u_f%02X.ppm",
                       tdn, mw, mh, base_fmt);
              FILE* tf = fopen(fn, "wb");
              if (tf) {
                  fprintf(tf, "P6\n%u %u\n255\n", mw, mh);
                  for (u32 q = 0; q < mw * mh; q++)
                      fwrite(rgba[n] + (size_t)q * 4, 1, 3, tf);
                  fclose(tf);
              }
              fprintf(stderr, "[tex-rgba] %s fmt=0x%02X remap=0x%04X linear=%d"
                              " pitch=%u nonblack=%llu/%u alpha>0x1A=%llu\n",
                      fn, base_fmt, remap, linear, pitch,
                      (unsigned long long)nb, mw * mh, (unsigned long long)na);
              tdn++;
          } }
        levels[n].w = mw;
        levels[n].h = mh;
        levels[n].data = rgba[n];
        levels[n].row_bytes = mw * 4;
        levels[n].rows = mh;
        n++;
        off += pitch * mh;
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    ID3D12Resource* resource = (!oom && n)
        ? create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, levels, n) : NULL;
    for (u32 m = 0; m < n; m++) free(rgba[m]);
    return resource;
}

static ID3D12Resource* decode_guest_texture_profiled(
    const rsx_dsp_texture* texture, u32 remap)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin = ld_profile_qpc();
    g_ld_profile.total.texture_decode_calls++;
#endif
    ID3D12Resource* result = decode_guest_texture(texture, remap);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.texture_decode_qpc +=
        (u64)(ld_profile_qpc() - begin);
#endif
    return result;
}

static void write_texture_srv(u32 index, const texcache_t* entry)
{
    const u32 base_fmt = entry->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int compressed = base_fmt == TEX_FMT_DXT1 ||
                           base_fmt == TEX_FMT_DXT23 ||
                           base_fmt == TEX_FMT_DXT45;
    if (entry->cubemap) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = !compressed ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.Shader4ComponentMapping =
            !compressed || entry->remap == 0xAAE4
            ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
            : mapping[0] | (mapping[1] << 3) | (mapping[2] << 6) |
              (mapping[3] << 9) | (1u << 12);
        desc.TextureCube.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else if (compressed && entry->remap != 0xAAE4) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = mapping[0] | (mapping[1] << 3) |
                                       (mapping[2] << 6) | (mapping[3] << 9) |
                                       (1u << 12);
        desc.Texture2D.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else {
        srv_write(SRV_TEXTURE_BASE + index, entry->tex);
    }
}

/* Decode a guest texture descriptor into a cached SRV slot.  Unlike the old
 * descriptor-only cache, re-hash the source once per frame and refresh the
 * D3D resource when the game rewrites the same guest address. */
/* Texture cache index.
 *
 * texture_srv_slot used to walk every cached texture (630 of them by the
 * time Folklore reaches Doolin) comparing descriptors, for every enabled
 * unit of every draw -- eight units on each of 2,000 draws a frame there.
 * Cache keys are unique (an entry is only added when no entry matches), so
 * a chained hash on the same fields finds the one entry directly. Entries
 * are only ever added or replaced in place (LRU reuse), never removed. */
#define LD_TEXTURE_BUCKETS 2048u
static u16 g_ld_texture_bucket[LD_TEXTURE_BUCKETS];   /* entry index + 1, 0 = empty */
static u16 g_ld_texture_next[MAX_TEXTURES];           /* entry index + 1, 0 = end   */

static u32 ld_texture_key_bucket(u32 location, u32 offset, u32 format, u32 width, u32 height,
                                 u32 pitch, u32 remap, u32 cubemap)
{
    u64 hash = 1469598103934665603ull;
    const u32 fields[8] = { location, offset, format, width, height, pitch, remap, cubemap };
    for (u32 fieldIndex = 0; fieldIndex < 8; fieldIndex++)
    {
        hash ^= fields[fieldIndex];
        hash *= 1099511628211ull;
        hash ^= hash >> 29;
    }
    return (u32)(hash & (LD_TEXTURE_BUCKETS - 1u));
}

static u32 ld_texture_entry_bucket(const texcache_t* entry)
{
    return ld_texture_key_bucket(entry->location, entry->offset, entry->format, entry->width,
                                 entry->height, entry->pitch, entry->remap, entry->cubemap);
}

static void ld_texture_index_remove(u32 entryIndex)
{
    const u32 bucket = ld_texture_entry_bucket(&g.textures[entryIndex]);
    u16* link = &g_ld_texture_bucket[bucket];
    while (*link)
    {
        if ((u32)(*link - 1u) == entryIndex)
        {
            *link = g_ld_texture_next[entryIndex];
            g_ld_texture_next[entryIndex] = 0;
            return;
        }
        link = &g_ld_texture_next[*link - 1u];
    }
}

static void ld_texture_index_insert(u32 entryIndex)
{
    const u32 bucket = ld_texture_entry_bucket(&g.textures[entryIndex]);
    g_ld_texture_next[entryIndex] = g_ld_texture_bucket[bucket];
    g_ld_texture_bucket[bucket] = (u16)(entryIndex + 1u);
}

static u32 texture_srv_slot(const rsx_dsp_texture* t)
{
    const u32 remap = t->remap & 0xFFFF;
    static int refresh_enabled = -1;
    if (refresh_enabled < 0)
        refresh_enabled = getenv("RSX_NO_TEX_REFRESH") ? 0 : 1;

    const u32 lookupBucket = ld_texture_key_bucket(t->location, t->offset, t->format, t->width,
                                                   t->height, t->pitch, remap, t->cubemap);
    for (u32 link = g_ld_texture_bucket[lookupBucket]; link; link = g_ld_texture_next[link - 1u]) {
        const u32 i = link - 1u;
        texcache_t* entry = &g.textures[i];
        if (entry->location != t->location || entry->offset != t->offset ||
            entry->format != t->format || entry->width != t->width ||
            entry->height != t->height || entry->pitch != t->pitch ||
            entry->remap != remap || entry->cubemap != t->cubemap)
            continue;
        if (refresh_enabled && entry->tex &&
            entry->last_hash_frame != g_ld_frames) {
            int readable = 0;
            const u64 hash = texture_content_hash(t, &readable);
            /* LD_TEX_RACE=1: hash the SAME source twice, back to back. The two
             * reads are microseconds apart, so they can only disagree if a
             * guest thread is writing this texture right now -- i.e. we are
             * decoding a half-written image. Pure observation; it changes
             * nothing. Counts per source so one torn texture is attributable. */
            { static int race = -1;
              if (race < 0) race = getenv("LD_TEX_RACE") ? 1 : 0;
              if (race && readable) {
                  int r2 = 0;
                  const u64 again = texture_content_hash(t, &r2);
                  static unsigned long long checks = 0, torn = 0;
                  checks++;
                  if (r2 && again != hash) torn++;
                  if ((checks % 256) == 0)
                      fprintf(stderr,
                              "[tex-race] src=%u:0x%08X %ux%u  torn %llu/%llu reads"
                              " (%.1f%%)\n",
                              t->location, t->offset, t->width, t->height,
                              (unsigned long long)torn,
                              (unsigned long long)checks,
                              100.0 * (double)torn / (double)checks);
              } }
            /* LD_FBDBG=1: account for the PS1 framebuffer specifically
             * (location 1, offset 0x400000, the 1024x512 fmt 0xE2 texture the
             * PS3 side composites). Its guest memory demonstrably fills up --
             * PS1 VRAM goes from 0 to 75,527 non-zero words -- yet
             * [tex-refresh] fires only 6 times in 90 seconds. Three different
             * things produce that, and they need different fixes: the texture
             * is rarely BOUND, the span is unreadable so `readable` is 0, or
             * the hash genuinely does not change. Count all three. */
            { static int fbd = -1;
              if (fbd < 0) fbd = getenv("LD_FBDBG") ? 1 : 0;
              if (fbd && t->location == 1u && t->offset == 0x400000u) {
                  static unsigned long long checks = 0, unread = 0, changed = 0;
                  checks++;
                  if (!readable) unread++;
                  if (readable && hash != entry->content_hash) changed++;
                  if ((checks % 64) == 0 || checks < 4)
                      fprintf(stderr, "[fbdbg] ps1fb checks=%llu unreadable=%llu"
                                      " changed=%llu span=%u %ux%u pitch=%u\n",
                              checks, unread, changed,
                              texture_source_span(t), t->width, t->height,
                              t->pitch);
              } }
            entry->last_hash_frame = g_ld_frames;
            if (readable && hash != entry->content_hash) {
                ID3D12Resource* replacement =
                    decode_guest_texture_profiled(t, remap);
                if (replacement) {
                    ID3D12Resource* old = entry->tex;
                    entry->tex = replacement;
                    entry->content_hash = hash;
                    write_texture_srv(i, entry);
                    retire_texture(old);
                    static u32 refresh_count = 0;
                    refresh_count++;
                    if (refresh_count <= 64 || (refresh_count & 255) == 0)
                        fprintf(stderr,
                                "[tex-refresh] n=%u frame=%u unit-src=%u:0x%08X "
                                "fmt=0x%02X %ux%u\n",
                                refresh_count, g_ld_frames, t->location, t->offset,
                                t->format, t->width, t->height);
                }
            }
        }
        entry->last_use_serial = ++g_ld_texture_use_serial;
        return entry->tex ? SRV_TEXTURE_BASE + i : SRV_WHITE;
    }

    u32 index;
    ID3D12Resource* evicted = NULL;
    const int cache_was_full = g.n_textures >= MAX_TEXTURES;
    if (!cache_was_full) {
        index = g.n_textures++;
    } else {
        /*
         * A full boot plus a010 binds far more than 128 distinct guest
         * texture descriptors. Returning SRV_WHITE after the fixed cache
         * filled made the recovered orphanage render as flat green/black
         * geometry. Reuse the least-recently-used descriptor while keeping
         * its resource alive until the open command list reaches a fence.
         * The shader-visible draw table already contains a descriptor copy,
         * so rewriting this CPU cache slot cannot alter earlier draws.
         */
        g_ld_texture_cache_full++;
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial <
                g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].tex;
        g_ld_texture_cache_evictions++;
        if (g_ld_texture_cache_full <= 16 ||
            (g_ld_texture_cache_full & (g_ld_texture_cache_full - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] FULL cap=%u lru-evictions=%llu\n",
                    MAX_TEXTURES,
                    (unsigned long long)g_ld_texture_cache_evictions);
    }

    texcache_t replacement;
    memset(&replacement, 0, sizeof(replacement));
    replacement.location = t->location;
    replacement.offset = t->offset;
    replacement.format = t->format;
    replacement.width = t->width;
    replacement.height = t->height;
    replacement.pitch = t->pitch;
    replacement.remap = remap;
    replacement.cubemap = t->cubemap;
    replacement.last_hash_frame = g_ld_frames;
    replacement.last_use_serial = ++g_ld_texture_use_serial;
    {
        int readable = 0;
        replacement.content_hash = texture_content_hash(t, &readable);
    }
    replacement.tex = decode_guest_texture_profiled(t, remap);
    if (replacement.tex) {
        texcache_t* entry = &g.textures[index];
        if (evicted)
            retire_texture(evicted);
        if (cache_was_full)
        {
            ld_texture_index_remove(index);
        }
        *entry = replacement;
        ld_texture_index_insert(index);
        write_texture_srv(index, entry);
    } else {
        if (!cache_was_full)
        {
            g.textures[index] = replacement;
            ld_texture_index_insert(index);
        }
        g_ld_texture_decode_fail++;
        if (g_ld_texture_decode_fail <= 16 ||
            (g_ld_texture_decode_fail & (g_ld_texture_decode_fail - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] decode failed n=%llu src=%u:0x%08X "
                    "fmt=0x%02X %ux%u pitch=%u\n",
                    (unsigned long long)g_ld_texture_decode_fail,
                    t->location, t->offset, t->format, t->width, t->height,
                    t->pitch);
    }
    return replacement.tex ? SRV_TEXTURE_BASE + index : SRV_WHITE;
}

typedef struct {
    DXGI_FORMAT dxgi;
    u32 bytes_per_texel;
    u32 component_bytes;
} vertex_texture_format_t;

static int vertex_texture_format(
    const rsx_dsp_vertex_texture* vt, vertex_texture_format_t* out)
{
    if (!vt || vt->dimension != 2 || vt->cubemap)
        return 0;
    vertex_texture_format_t format = {DXGI_FORMAT_UNKNOWN, 0, 0};
    switch (vt->format & TEX_FMT_BASE_MASK) {
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
        format.bytes_per_texel = 8;
        format.component_bytes = 2;
        break;
    case RSX_TEX_FMT_W32Z32Y32X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT;
        format.bytes_per_texel = 16;
        format.component_bytes = 4;
        break;
    case RSX_TEX_FMT_X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32_FLOAT;
        format.bytes_per_texel = 4;
        format.component_bytes = 4;
        break;
    case RSX_TEX_FMT_Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16_FLOAT;
        format.bytes_per_texel = 4;
        format.component_bytes = 2;
        break;
    default:
        return 0;
    }
    if (out)
        *out = format;
    return 1;
}

static int vertex_texture_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("RSX_VTEX_TRACE") ? 1 : 0;
    return enabled;
}

static u32 vertex_texture_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (!vt.enabled) continue;
        g_ld_vtex_enabled++;
        if (vertex_texture_format(&vt, NULL)) {
            mask |= 1u << u;
        } else {
            g_ld_vtex_unsupported++;
            static u32 warned = 0;
            if (warned++ < 16 && vertex_texture_trace_enabled())
                fprintf(stderr,
                        "[vtex] enabled but unsupported unit=%u off=0x%08X "
                        "fmt=0x%02X dim=%u cube=%u %ux%u pitch=%u ctl=0x%08X\n",
                        u, vt.offset, vt.format, vt.dimension, vt.cubemap,
                        vt.width, vt.height, vt.pitch, vt.control0);
        }
    }
    return mask;
}

static u64 vertex_texture_hash(const rsx_dsp_vertex_texture* vt,
                               const u8** out_src, u32* out_pitch)
{
    vertex_texture_format_t format;
    if (!vertex_texture_format(vt, &format) || !vt->width || !vt->height ||
        vt->width > 4096 || vt->height > 4096)
        return 0;
    const u64 row_bytes64 = (u64)vt->width * format.bytes_per_texel;
    const u64 pitch64 = vt->pitch ? vt->pitch : row_bytes64;
    const u64 span64 = pitch64 * vt->height;
    if (row_bytes64 > UINT32_MAX || pitch64 < row_bytes64 ||
        pitch64 > UINT32_MAX || span64 > UINT32_MAX)
        return 0;
    const u32 pitch = (u32)pitch64;
    const u32 span = (u32)span64;
    const u8* src = span ? guest_ptr(vt->location, vt->offset, span) : NULL;
    if (out_src) *out_src = src;
    if (out_pitch) *out_pitch = pitch;
    return src ? fnv1a(src, span, 1469598103934665603ull) : 0;
}

static ID3D12Resource* decode_vertex_texture(
    const rsx_dsp_vertex_texture* vt, u64* out_hash)
{
    vertex_texture_format_t format;
    if (!vertex_texture_format(vt, &format) || !vt->width || !vt->height ||
        vt->width > 4096 || vt->height > 4096)
        return NULL;
    const u8* src = NULL;
    u32 pitch = 0;
    const u64 hash = vertex_texture_hash(vt, &src, &pitch);
    if (!src) return NULL;
    const u32 row_bytes = vt->width * format.bytes_per_texel;
    if (pitch < row_bytes)
        return NULL;
    u8* staging = (u8*)malloc((size_t)row_bytes * vt->height);
    if (!staging) return NULL;
    for (u32 y = 0; y < vt->height; y++) {
        const u8* srow = src + (size_t)y * pitch;
        u8* drow = staging + (size_t)y * row_bytes;
        const u32 components = row_bytes / format.component_bytes;
        for (u32 c = 0; c < components; c++) {
            const u8* source =
                srow + (size_t)c * format.component_bytes;
            u8* destination =
                drow + (size_t)c * format.component_bytes;
            for (u32 byte = 0; byte < format.component_bytes; byte++)
                destination[byte] =
                    source[format.component_bytes - 1u - byte];
        }
    }
    tex_level_t level = {
        vt->width, vt->height, staging, row_bytes, vt->height
    };
    ID3D12Resource* tex = create_texture_mipped(format.dxgi, &level, 1);
    free(staging);
    if (tex) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        if (out_hash) *out_hash = hash;
    }
    return tex;
}

static void write_vertex_texture_srv(u32 index, ID3D12Resource* tex)
{
    rsx_dsp_vertex_texture descriptor = {0};
    vertex_texture_format_t format;
    if (index >= g.n_vtex)
        return;
    descriptor.format = g.vtex[index].format;
    descriptor.dimension = 2;
    if (!vertex_texture_format(&descriptor, &format))
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = format.dxgi;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(SRV_VTEX_BASE + index));
}

static u32 vertex_texture_srv_slot(const rsx_dsp_vertex_texture* vt)
{
    for (u32 i = 0; i < g.n_vtex; i++) {
        vtexcache_t* e = &g.vtex[i];
        if (e->location != vt->location || e->offset != vt->offset ||
            e->format != vt->format || e->width != vt->width ||
            e->height != vt->height || e->pitch != vt->pitch)
            continue;
        if (e->tex && e->last_hash_frame != g_ld_frames) {
            const u64 hash = vertex_texture_hash(vt, NULL, NULL);
            e->last_hash_frame = g_ld_frames;
            if (hash && hash != e->content_hash) {
                u64 replacement_hash = 0;
                ID3D12Resource* replacement =
                    decode_vertex_texture(vt, &replacement_hash);
                if (replacement) {
                    ID3D12Resource* old = e->tex;
                    e->tex = replacement;
                    e->content_hash = replacement_hash;
                    write_vertex_texture_srv(i, e->tex);
                    retire_texture(old);
                    g_ld_vtex_refreshes++;
                }
            }
        }
        return e->tex ? SRV_VTEX_BASE + i : SRV_WHITE;
    }
    if (g.n_vtex >= MAX_VTEX) return SRV_WHITE;
    const u32 index = g.n_vtex++;
    vtexcache_t* e = &g.vtex[index];
    memset(e, 0, sizeof(*e));
    e->location = vt->location;
    e->offset = vt->offset;
    e->format = vt->format;
    e->width = vt->width;
    e->height = vt->height;
    e->pitch = vt->pitch;
    e->last_hash_frame = g_ld_frames;
    e->tex = decode_vertex_texture(vt, &e->content_hash);
    if (e->tex) {
        write_vertex_texture_srv(index, e->tex);
        g_ld_vtex_uploads++;
        if (vertex_texture_trace_enabled())
            fprintf(stderr,
                    "[vtex] upload unit-data %u:0x%08X fmt=0x%02X %ux%u pitch=%u\n",
                    vt->location, vt->offset, vt->format,
                    vt->width, vt->height, vt->pitch);
    }
    return e->tex ? SRV_VTEX_BASE + index : SRV_WHITE;
}

/* ---------------------------------------------------------------------------
 * surfaces (color RTs keyed by location/offset), rendered into then presented
 * -----------------------------------------------------------------------*/
/* fp constants are stored as raw bits; the shader reads them as floats. */
static float ld_c2f(u32 bits) { float f; memcpy(&f, &bits, sizeof f); return f; }

static u32 surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                       DXGI_FORMAT want_fmt)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;
    /* Key on the DIMENSIONS as well as location/offset.
     *
     * Keying on location/offset alone meant a redeclaration of the same offset
     * at a different size STOLE the slot and recreated the D3D12 resource,
     * throwing the old contents away. ps1_netemu renders two passes at two
     * sizes -- the PS1 composite at 720x512 and the PS3 output at 1280x720 --
     * and it alternates between them, so that path fired 9,573 times in a
     * 90-second run:
     *
     *     [surfsz] live surface 0x0 redeclared 720x512 -> 1280x720 (content dropped)
     *     [surfsz] live surface 0x1401C00 redeclared 1280x720 -> 720x512 (content dropped)
     *
     * Every frame of both passes was being destroyed by the other. Giving each
     * (offset, size) its own surface lets both keep their contents.
     *
     * ponytail: two surfaces over the same guest memory do not alias, so a pass
     * that renders at one size and samples at the other will not see the
     * other's pixels. That is strictly better than the old behaviour, which saw
     * NOBODY's pixels, and it is the smaller change. If a title ever needs true
     * aliasing, the fix is one surface plus a copy on redeclare -- not going
     * back to destroying content. */
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset &&
            g.surfaces[i].w == want_w && g.surfaces[i].h == want_h &&
            g.surfaces[i].fmt == want_fmt)
            return i;
    /* Never destroy a usable render target because a malformed live command
     * briefly decoded a guest pointer as clip dimensions.  The known-good
     * orphanage stream never exceeds 1280x1024; D3D12 rejects the observed
     * 1280x16452 declaration and the old path then dereferenced a null
     * resource.  Preserve the prior surface so diagnostics can capture the
     * actual world pass while the bad command link is isolated. */
    if (want_w > 8192u || want_h > 8192u) {
        static u32 invalid_surface_logs = 0;
        if (invalid_surface_logs++ < 32u)
            fprintf(stderr,
                    "[surface-guard] rejected implausible surface 0x%X "
                    "%ux%u; preserving slot=%s\n",
                    offset, want_w, want_h,
                    slot < MAX_SURFACES ? "existing" : "none");
        return slot < MAX_SURFACES ? slot : LD_INVALID_SURFACE;
    }
    if (g.n_surfaces >= MAX_SURFACES) {
        /* Out of slots is now the only way a surface can be lost, so say so
         * once rather than silently returning an invalid handle. */
        static u32 full_logs = 0;
        if (full_logs++ < 8)
            fprintf(stderr, "[surfsz] surface table full (%u); dropping "
                            "%u:0x%X %ux%u\n",
                    (unsigned)MAX_SURFACES, location, offset, want_w, want_h);
        return LD_INVALID_SURFACE;
    }
    slot = g.n_surfaces;
    { static u32 new_logs = 0;
      if (new_logs++ < 24)
          fprintf(stderr, "[surfsz] new live surface %u:0x%X %ux%u (slot %u)\n",
                  location, offset, want_w, want_h, slot); }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const u32 surfaceScale = ld_surface_scale_for(want_w, want_h);
    const float surfaceScaleX = ld_surface_scale_x_for(want_w, want_h);
    rd.Width = ld_scale_pixels(want_w, surfaceScaleX); rd.Height = want_h * surfaceScale; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = want_fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = rd.Format;
    surface_t* s = &g.surfaces[slot];
    ID3D12Resource* replacement = NULL;
    const HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        &IID_ID3D12Resource, (void**)&replacement);
    if (FAILED(create_hr)) {
        static u32 surface_fail_logs = 0;
        if (surface_fail_logs++ < 32) {
            const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
            fprintf(stderr,
                    "[surface-fail] color %u:0x%X %ux%u hr=0x%08lX "
                    "removed=0x%08lX slot=%u count=%u\n",
                    location, offset, want_w, want_h,
                    (unsigned long)create_hr, (unsigned long)removed,
                    slot, g.n_surfaces);
        }
        /* A resize failure must not poison an existing usable slot. */
        return slot < g.n_surfaces && s->tex ? slot : LD_INVALID_SURFACE;
    }
    /* Draws already recorded in the open command list can still reference
     * the old render target.  Release it only after ld_flush fences that list,
     * just like dynamic texture and zeta replacements. */
    if (s->tex)
        retire_texture(s->tex);
    if (s->fb_tex) {                 /* sized for the old resource */
        retire_texture(s->fb_tex);
        s->fb_tex = NULL;
    }
    s->tex = replacement;
    s->location = location; s->offset = offset; s->w = want_w; s->h = want_h;
    s->fmt = want_fmt;
    s->scale = surfaceScale;
    s->scale_x = surfaceScaleX;
#if !defined(YZ_PERF_CLEAN)
    s->resource_serial = ++g_ld_surface_resource_serial;
#endif
    ld_surface_note_write(slot, LD_SURFACE_WRITE_CREATE);
    /* RTVs for surfaces live above the swap-chain backbuffer RTVs */
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex,
        NULL, rtv_handle(LD_SWAP_BUFFERS + slot));
    srv_write(SRV_SURFACE_BASE + slot, s->tex);
    if (slot == g.n_surfaces) g.n_surfaces++;
    return slot;
}

/* A draw that samples its own render target. RSX reads and writes the same
 * surface in one pass (GH3's post pass reads the scene at 0x200000 while
 * drawing into 0x200000); D3D12 cannot, and the lookup used to fall back to
 * guest VRAM -- empty, so the pass painted the scene black. Snapshot the
 * target into a per-surface copy and sample that. Returns the SRV slot, or
 * SRV_WHITE if the copy cannot be made. */
static u32 surface_feedback_srv(u32 slot)
{
    surface_t* s = &g.surfaces[slot];
    if (!s->tex) return SRV_WHITE;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    int fresh = 0;
    if (!s->fb_tex) {
        D3D12_RESOURCE_DESC rd;
        s->tex->lpVtbl->GetDesc(s->tex, &rd);
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
                g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&s->fb_tex))) {
            s->fb_tex = NULL;
            return SRV_WHITE;
        }
        srv_write(SRV_FEEDBACK_BASE + slot, s->fb_tex);
        fresh = 1;
    }
    b[0].Transition.pResource = s->tex;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = s->fb_tex;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, fresh ? 1 : 2, b);
    g.list->lpVtbl->CopyResource(g.list, s->fb_tex, s->tex);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    return SRV_FEEDBACK_BASE + slot;
}

/* Find the live surface whose guest rows (at `pitch`) contain byte `abs`, with
 * room for a w x h rect there. Returns the slot and the rect's origin. */
static int surface_containing(u32 location, u32 abs, u32 pitch, u32 w, u32 h,
                              u32* ox, u32* oy)
{
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* s = &g.surfaces[i];
        if (!s->tex || s->location != location || abs < s->offset) continue;
        const u32 d = abs - s->offset, y = d / pitch, x = (d % pitch) / 4u;
        if (x + w <= s->w && y + h <= s->h) { *ox = x; *oy = y; return (int)i; }
    }
    return -1;
}

/* NV3089 scaled-image blit whose SOURCE is a live surface: the pixels exist
 * only on the GPU, so the guest-memory copy in cellGcmSys moves black. GH3
 * copies its 1040x592 scene out to 0x1651500 every frame, runs its post pass
 * sampling that copy, and copies it back -- all black, so attract mode showed
 * only its overlay. Do the copy on the GPU, creating a live surface for the
 * destination so later texture lookups at that address find it. 1:1 only.
 * Returns 1 when handled, <= 0 when not (reason codes in ld_blit_gpu). */
static FILE* s_csv_file;       /* defined with live_draw_csv_emit */
static int   s_csv_gate_open;
static u32   s_csv_gate_session;

static int ld_blit_gpu(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h);
int rsx_live_draw_blit(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h)
{
    const int r = ld_blit_gpu(src_loc, src_abs, src_pitch, dst_loc, dst_abs, dst_pitch, w, h);
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#blit,%u,%u:0x%X/%u,%u:0x%X/%u,%ux%u,gpu=%d\n", g_ld_frames,
                src_loc, src_abs, src_pitch, dst_loc, dst_abs, dst_pitch, w, h, r);
        fflush(s_csv_file);
    }
    /* LD_BLIT_DUMP=<dir>: while the CSV gate is open, read back the first four
     * blits' destination surfaces right after the copy (one frame's worth). */
    { static const char* dir = (const char*)-1; static u32 n;
      if (dir == (const char*)-1) dir = getenv("LD_BLIT_DUMP");
      if (!(s_csv_file && s_csv_gate_open)) n = 0;
      else if (dir && r == 1 && n < 4) {
          u32 dx, dy;
          const int di = surface_containing(dst_loc, dst_abs, dst_pitch, w, h, &dx, &dy);
          if (di >= 0) {
              char path[MAX_PATH * 2];
              snprintf(path, sizeof(path), "%s\\blit_f%06u_%u_dst%08X.ppm", dir, g_ld_frames, n,
                       g.surfaces[di].offset);
              ld_dump_surface_ppm(path, &g.surfaces[di]);
          }
          n++;
      } }
    return r;
}

/* A SCALED NV3089 blit out of a live surface cannot be a CopyTextureRegion.
 * The one that matters is a supersampled scene resolved into a display buffer
 * (Tornado Outbreak renders 2x2 and downsamples into its scanout in 512x512
 * tiles): the guest-memory copy moves black and the flip presented whatever
 * surface happened to be current. When such a blit lands on a display buffer's
 * origin, remember the source surface; the flip presents it (at the host
 * surface's size) when the buffer has no live surface of its own.
 * ponytail: scanout alias, not a real scaled copy -- a title that samples the
 * resolved buffer as a texture still sees black; add a draw-based scaled blit
 * then. */
/* The copy half of a scaled resolve, done on the GPU. The source is a live
 * surface the host renders at display size although the guest laid it out
 * supersampled (Tornado Outbreak: pitch 10240 = 2560 guest pixels, host
 * surface 1280x720), so a ds x dt guest downsample is a 1:1 copy in host
 * pixels. Each tile lands in a live surface at the display buffer it writes
 * into, so the flip presents a snapshot taken at resolve time -- presenting
 * the source live (the alias below) showed whatever the title rendered into
 * it next, and the menu flickered to black on two frames in three.
 * Returns 1 when copied. */
int rsx_live_draw_resolve_blit(u32 src_loc, u32 src_abs, u32 src_pitch, u32 ds, u32 dt,
                               u32 dst_loc, u32 dst_abs, u32 dst_pitch,
                               u32 out_x, u32 out_y, u32 out_w, u32 out_h)
{
    if (!g.ready || !src_pitch || !dst_pitch || ds != dt || ds < 0x100000u) return 0;
    /* Source surface and the rect's guest position inside it. */
    /* The CLOSEST base at or below the address: surfaces' guest ranges
     * overlap (a stale surface at 0x0 "contains" 0x12804 too), and taking the
     * first match copied every 0x10000 tile out of that black surface. */
    int si = -1; u32 gx = 0, gy = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* s = &g.surfaces[i];
        if (!s->tex || s->location != src_loc || src_abs < s->offset) continue;
        const u32 d = src_abs - s->offset;
        if (d / src_pitch >= 2u * s->h) continue;
        if (si >= 0 && g.surfaces[si].offset > s->offset) continue;
        si = (int)i; gx = (d % src_pitch) / 4u; gy = d / src_pitch;
    }
    if (si < 0) return 0;
    /* Destination: the display buffer this tile writes into. */
    int bi = -1;
    for (u32 b = 0; b < 8; b++) {
        const display_buffer_t* d = &g.display_buffers[b];
        if (d->valid && d->location == dst_loc && dst_abs >= d->offset &&
            dst_abs < d->offset + d->pitch * d->height) { bi = (int)b; break; }
    }
    if (bi < 0) return 0;
    const display_buffer_t* db = &g.display_buffers[bi];
    const u32 dx = (dst_abs - db->offset) % db->pitch / 4u + out_x;
    const u32 dy = (dst_abs - db->offset) / db->pitch + out_y;
    const u32 slot = surface_get(db->location, db->offset, db->width, db->height, g.surfaces[si].fmt);
    if (slot == LD_INVALID_SURFACE || (int)slot == si) return 0;
    surface_t* dst = &g.surfaces[slot];
    if (dst->fmt != g.surfaces[si].fmt) return 0;
    /* Host source position: guest position over the downsample factor. */
    const u32 k = ds >> 20;                       /* 20.12 fixed -> integer factor */
    u32 sx = gx / (k ? k : 1), sy = gy / (k ? k : 1);
    u32 w = out_w, h = out_h;
    if (dx >= dst->w || dy >= dst->h || sx >= g.surfaces[si].w || sy >= g.surfaces[si].h) return 0;
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (sx + w > g.surfaces[si].w) w = g.surfaces[si].w - sx;
    if (sy + h > g.surfaces[si].h) h = g.surfaces[si].h - sy;
    ID3D12Resource* s = g.surfaces[si].tex;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int q = 0; q < 2; q++) {
        b[q].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[q].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[q].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    b[0].Transition.pResource = s;        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst->tex; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    D3D12_TEXTURE_COPY_LOCATION sl = {0}, dl = {0};
    sl.pResource = s;        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.pResource = dst->tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    /* Both at one render scale (a resolve into a display buffer is between
     * canvas-sized surfaces); anything else would need a scaled draw. */
    const surface_t* resolveSource = &g.surfaces[si];
    if (!ld_surface_same_scale(resolveSource, dst)) return 0;
    D3D12_BOX box = { ld_surface_x(resolveSource, sx), ld_surface_scaled(resolveSource, sy), 0,
                      ld_surface_x(resolveSource, sx + w), ld_surface_scaled(resolveSource, sy + h), 1 };
    g.list->lpVtbl->CopyTextureRegion(g.list, &dl, ld_surface_x(dst, dx), ld_surface_scaled(dst, dy),
                                      0, &sl, &box);
    for (int q = 0; q < 2; q++) {
        b[q].Transition.StateBefore = b[q].Transition.StateAfter;
        b[q].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    ld_surface_note_write(slot, LD_SURFACE_WRITE_COPY);
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#resolve,%u,%u:0x%X,%u:0x%X,%u,%u,%ux%u\n", g_ld_frames,
                src_loc, src_abs, dst_loc, dst_abs, sx, sy, w, h);
        fflush(s_csv_file);
    }
    return 1;
}

void rsx_live_draw_note_resolve(u32 src_loc, u32 src_abs, u32 dst_loc, u32 dst_abs)
{
    if (!g.ready) return;
    for (u32 b = 0; b < 8; b++) {
        display_buffer_t* d = &g.display_buffers[b];
        if (!d->valid || d->location != dst_loc || d->offset != dst_abs) continue;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const surface_t* s = &g.surfaces[i];
            if (s->tex && s->location == src_loc && s->offset == src_abs) {
                if (d->resolve_src != (int)i + 1)
                    fprintf(stderr, "[live-draw] display buffer %u resolves from surface %u (0x%X)\n",
                            b, i, s->offset);
                d->resolve_src = (int)i + 1;
                return;
            }
        }
    }
}

static int ld_blit_gpu(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h)
{
    if (!g.ready || !src_pitch || !dst_pitch || !w || !h) return 0;
    u32 sx, sy, dx = 0, dy = 0;
    const int si = surface_containing(src_loc, src_abs, src_pitch, w, h, &sx, &sy);
    if (si < 0) return -1;            /* source is not a live surface */
    int di = surface_containing(dst_loc, dst_abs, dst_pitch, w, h, &dx, &dy);
    if (di < 0) {
        const u32 slot = surface_get(dst_loc, dst_abs, dst_pitch / 4u, h,
                                     g.surfaces[si].fmt);
        if (slot == LD_INVALID_SURFACE) return -2;
        di = (int)slot;
        if (w > g.surfaces[di].w || h > g.surfaces[di].h) return -3;
    }
    if (di == si) return -4;
    if (g.surfaces[di].fmt != g.surfaces[si].fmt) return -5;
    if (!ld_surface_same_scale(&g.surfaces[di], &g.surfaces[si])) {
        static u32 scale_mismatch_logs = 0;
        if (scale_mismatch_logs++ < 8)
            fprintf(stderr, "[live-draw] blit between surfaces at %ux and %ux render scale "
                            "(0x%X %ux%u -> 0x%X %ux%u) not copied\n",
                    g.surfaces[si].scale, g.surfaces[di].scale,
                    g.surfaces[si].offset, g.surfaces[si].w, g.surfaces[si].h,
                    g.surfaces[di].offset, g.surfaces[di].w, g.surfaces[di].h);
        return -6;
    }
    ID3D12Resource* src = g.surfaces[si].tex;
    ID3D12Resource* dst = g.surfaces[di].tex;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    b[0].Transition.pResource = src; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    D3D12_TEXTURE_COPY_LOCATION s = {0}, d = {0};
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const surface_t* blitSurface = &g.surfaces[si];
    D3D12_BOX box = { ld_surface_x(blitSurface, sx), ld_surface_scaled(blitSurface, sy), 0,
                      ld_surface_x(blitSurface, sx + w), ld_surface_scaled(blitSurface, sy + h), 1 };
    g.list->lpVtbl->CopyTextureRegion(g.list, &d, ld_surface_x(blitSurface, dx),
                                      ld_surface_scaled(blitSurface, dy), 0, &s, &box);
    for (int k = 0; k < 2; k++) {
        b[k].Transition.StateBefore = b[k].Transition.StateAfter;
        b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    ld_surface_note_write((u32)di, LD_SURFACE_WRITE_COPY);
    return 1;
}

static u32 current_surface(void)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    return surface_get(sf.color_location[0], sf.color_offset[0],
                       sf.clip_w, sf.clip_h,
                       sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16
                           ? DXGI_FORMAT_R16G16B16A16_FLOAT
                           : DXGI_FORMAT_R8G8B8A8_UNORM);
}

/* On RSX each zeta (depth) address is distinct memory. The offline renderer's
 * s31 fix proved that sharing one D3D depth resource across all of this game's
 * shadow, scene and post-processing passes cross-contaminates later depth
 * tests. Keep the legacy shared target only as a bounded allocation fallback. */
static int honor_zeta_track(void)
{
    static int initialized = 0;
    static int enabled = 1;
    if (!initialized) {
        initialized = 1;
        enabled = getenv("RSX_NO_ZETA_TRACK") ? 0 : 1;
        fprintf(stderr, "[zetatrack] live per-zeta depth %s\n",
                enabled ? "ON" : "OFF (legacy shared target)");
    }
    return enabled;
}

static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &h);
    h.ptr += (size_t)slot * g.dsv_step;
    return h;
}

/* The R32F snapshot is sized from the clip of the first pass that bound the
 * zeta. A title that first binds its full-screen zeta with a smaller clip
 * (a 512x512 pass into a 1280x720 depth buffer) kept a 512x512 snapshot, and
 * every later full-size read of that depth saw 0 outside the top-left
 * corner. Grow the snapshot to the largest clip bound since, within the
 * depth resource. */
static void zdepth_grow_snapshot(u32 i, u32 rt_w, u32 rt_h)
{
    zdepth_t* z = &g.zdepths[i];
    if (!z->snapshot) return;
    u32 nw = rt_w < z->w ? rt_w : z->w, nh = rt_h < z->h ? rt_h : z->h;
    if (nw < z->snapshot_w) nw = z->snapshot_w;
    if (nh < z->snapshot_h) nh = z->snapshot_h;
    if (nw == z->snapshot_w && nh == z->snapshot_h) return;
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = ld_scaled_x(nw);
    rd.Height = ld_scaled(nh);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* snap = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void**)&snap))) {
        fprintf(stderr, "[zetatrack] snapshot grow failed %u:0x%X %ux%u\n",
                z->location, z->offset, nw, nh);
        return;
    }
    fprintf(stderr, "[zetatrack] snapshot %u:0x%X grown %ux%u -> %ux%u\n",
            z->location, z->offset, z->snapshot_w, z->snapshot_h, nw, nh);
    /* The open command list may still read the old one. */
    retire_texture(z->snapshot);
    z->snapshot = snap;
    z->snapshot_w = nw;
    z->snapshot_h = nh;
    z->snapshot_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    srv_write_zdepth(SRV_ZDEPTH_BASE + i, z->snapshot);
    uav_write_zdepth(UAV_ZDEPTH_BASE + i, z->snapshot);
    /* The depth resource still holds what the old snapshot held (a clear
     * would have invalidated both): resolve it again at the next read. */
    if (z->snapshot_valid) z->had_write = 1;
    z->snapshot_valid = 0;
}

/* Returns DSV slot 1+i. Slot 0 is the legacy shared depth fallback. */
static u32 zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    if (!honor_zeta_track()) return 0;
    /* The DSV must cover the complete live framebuffer/viewport it is bound
     * with.  Some early passes declare a smaller clip than the active canvas;
     * allocating only that clip causes the D3D device to reject later
     * surface/PSO creation.  Sampling a declared-size window from this padded
     * backing remains a separate SRV concern. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        zdepth_t* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) {
                zdepth_grow_snapshot(i, rt_w, rt_h);
                return 1 + i;
            }
            slot = i;
            break;
        }
    }
    if (slot == MAX_SURFACES) {
        if (g.n_zdepths >= MAX_SURFACES) {
            fprintf(stderr, "[zetatrack] live cache full; shared fallback\n");
            return 0;
        }
        slot = g.n_zdepths;
    } else {
        zdepth_t* old = &g.zdepths[slot];
        if (want_w < old->w) want_w = old->w;
        if (want_h < old->h) want_h = old->h;
        fprintf(stderr,
                "[zetatrack] live zeta %u:0x%X outgrown %ux%u -> %ux%u\n",
                location, offset, old->w, old->h, want_w, want_h);
        if (old->tex) {
            /* Draws already recorded in the open command list may still
             * reference this DSV.  D3D12 command lists do not retain resource
             * lifetimes for the application, so release it only after the
             * next submit/fence completes. */
            retire_texture(old->tex);
            old->tex = NULL;
        }
        if (old->snapshot) {
            retire_texture(old->snapshot);
            old->snapshot = NULL;
        }
    }

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = ld_scaled_x(want_w);
    rd.Height = ld_scaled(want_h);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    /* Optimized clear values must use the typed DSV format even though the
     * resource itself is typeless so it can also be exposed through an SRV. */
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 1.0f;
    zdepth_t* z = &g.zdepths[slot];
    HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&z->tex);
    if (FAILED(create_hr)) {
        fprintf(stderr,
                "[zetatrack] live create failed %u:0x%X hr=0x%08lX; shared fallback\n",
                location, offset, (unsigned long)create_hr);
        z->tex = NULL;
        return 0;
    }
    z->location = location;
    z->offset = offset;
    z->w = want_w;
    z->h = want_h;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->snapshot_w = rt_w ? rt_w : want_w;
    z->snapshot_h = rt_h ? rt_h : want_h;
    if (z->snapshot_w > want_w) z->snapshot_w = want_w;
    if (z->snapshot_h > want_h) z->snapshot_h = want_h;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {0};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateDepthStencilView(
        g.dev, z->tex, &dsvd, dsv_handle(1 + slot));

    /* D3D12 forbids a partial CopyTextureRegion from a depth/stencil
     * resource.  Keep the replay-proven logical dimensions and resolve the
     * depth plane with a compute shader instead; that is a normal SRV read,
     * not a depth-resource copy operation. */
    D3D12_RESOURCE_DESC snapshot_rd = {0};
    snapshot_rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    snapshot_rd.Width = ld_scaled_x(z->snapshot_w);
    snapshot_rd.Height = ld_scaled(z->snapshot_h);
    snapshot_rd.DepthOrArraySize = 1;
    snapshot_rd.MipLevels = 1;
    snapshot_rd.Format = DXGI_FORMAT_R32_FLOAT;
    snapshot_rd.SampleDesc.Count = 1;
    snapshot_rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT snapshot_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &snapshot_rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void**)&z->snapshot);
    if (FAILED(snapshot_hr)) {
        fprintf(stderr,
                "[zetatrack] live snapshot create failed %u:0x%X "
                "%ux%u hr=0x%08lX; guest fallback\n",
                location, offset, z->snapshot_w, z->snapshot_h,
                (unsigned long)snapshot_hr);
        z->snapshot = NULL;
    } else {
        z->snapshot_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        srv_write_depth_source(SRV_DEPTH_SOURCE_BASE + slot, z->tex);
        srv_write_zdepth(SRV_ZDEPTH_BASE + slot, z->snapshot);
        uav_write_zdepth(UAV_ZDEPTH_BASE + slot, z->snapshot);
    }
    g.list->lpVtbl->ClearDepthStencilView(
        g.list, dsv_handle(1 + slot),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);
    z->cleared = 1;
    fprintf(stderr,
            "[zetatrack] live new #%u %u:0x%X backing=%ux%u "
            "clip=%ux%u canvas=%ux%u\n",
            slot, location, offset, want_w, want_h,
            rt_w, rt_h, g.width, g.height);
    if (slot == g.n_zdepths) g.n_zdepths++;
    return 1 + slot;
}

/* Preserve the last completed depth-writing pass before the guest clears or
 * reuses its live zeta.  The replay renderer gets these bytes from its
 * captured cross-frame memory image; live rendering must explicitly retain
 * the equivalent GPU result because the guest never reads it back to CPU
 * VRAM. */
static int zdepth_snapshot(u32 slot)
{
    if (!slot) return 0;
    zdepth_t* z = &g.zdepths[slot - 1];
    if (!z->snapshot || !g.depth_snapshot_rootsig ||
        !g.depth_snapshot_pso)
        return 0;
    if (!z->had_write) return z->snapshot_valid;

    D3D12_GPU_DESCRIPTOR_HANDLE source_table, destination_table;
    if (!depth_snapshot_descriptors(
            slot - 1u, &source_table, &destination_table)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot descriptor ring exhausted "
                "at frame %u\n",
                g_ld_frames);
        return z->snapshot_valid;
    }

    D3D12_RESOURCE_BARRIER bars[2] = {0};
    u32 nbar = 0;
    bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[nbar].Transition.pResource = z->tex;
    bars[nbar].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bars[nbar].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    bars[nbar].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    nbar++;
    if (z->snapshot_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[nbar].Transition.pResource = z->snapshot;
        bars[nbar].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[nbar].Transition.StateBefore = z->snapshot_state;
        bars[nbar].Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        nbar++;
    }
    g.list->lpVtbl->ResourceBarrier(g.list, nbar, bars);

    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetPipelineState(g.list, g.depth_snapshot_pso);
    g.list->lpVtbl->SetComputeRootSignature(
        g.list, g.depth_snapshot_rootsig);
    g.list->lpVtbl->SetComputeRootDescriptorTable(
        g.list, 0, source_table);
    g.list->lpVtbl->SetComputeRootDescriptorTable(
        g.list, 1, destination_table);
    g.list->lpVtbl->Dispatch(
        g.list, (ld_scaled_x(z->snapshot_w) + 7u) / 8u,
        (ld_scaled(z->snapshot_h) + 7u) / 8u, 1);

    D3D12_RESOURCE_BARRIER uav_done = {0};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = z->snapshot;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &uav_done);

    D3D12_RESOURCE_BARRIER done[2] = {0};
    done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    done[0].Transition.pResource = z->tex;
    done[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    done[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    done[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    done[1].Transition.pResource = z->snapshot;
    done[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    done[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    done[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, done);
    z->snapshot_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    z->snapshot_valid = 1;
    z->had_write = 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * PSO cache (VP+FP+render-state keyed)
 * -----------------------------------------------------------------------*/
static u64 fnv1a(const void* data, u32 n, u64 h)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int sampler_table_needs_flush(const u32 slots[SMP_TABLE_SIZE])
{
    if (g.smp_ring_used < SMP_RING_TABLES) return 0;
    for (u32 i = 0; i < g.smp_ring_used; i++)
        if (memcmp(g.smp_ring_slots[i], slots,
                   sizeof(g.smp_ring_slots[i])) == 0)
            return 0;
    return sampler_table_free_slot() >= SMP_RING_TABLES;
}

static u32 shader_disk_cache_stage_index(u32 stage)
{
    return stage == 'V' ? 0u : 1u;
}

static void shader_disk_cache_progress(u64 value)
{
    if (value > 4u && (value & (value - 1u)) != 0u)
        return;
    fprintf(stderr,
            "[shader-disk-cache] hits{vs=%llu ps=%llu} "
            "misses{vs=%llu ps=%llu} writes{vs=%llu ps=%llu} "
            "rejects=%llu\n",
            (unsigned long long)g_ld_shader_disk_hits[0],
            (unsigned long long)g_ld_shader_disk_hits[1],
            (unsigned long long)g_ld_shader_disk_misses[0],
            (unsigned long long)g_ld_shader_disk_misses[1],
            (unsigned long long)g_ld_shader_disk_writes[0],
            (unsigned long long)g_ld_shader_disk_writes[1],
            (unsigned long long)g_ld_shader_disk_rejects);
}

static int shader_disk_cache_prepare(void)
{
    if (g_ld_shader_disk_ready >= 0)
        return g_ld_shader_disk_ready;
    g_ld_shader_disk_ready = 0;
    if (getenv("RSX_NO_SHADER_DISK_CACHE")) {
        fprintf(stderr, "[shader-disk-cache] disabled\n");
        return 0;
    }

    const char* override_dir = getenv("RSX_SHADER_CACHE_DIR");
    if (override_dir && override_dir[0]) {
        if (snprintf(g_ld_shader_disk_dir, sizeof(g_ld_shader_disk_dir),
                     "%s", override_dir) < 0 ||
            strlen(g_ld_shader_disk_dir) >= sizeof(g_ld_shader_disk_dir) - 1u)
            return 0;
    } else {
        CreateDirectoryA("scratch", NULL);
        snprintf(g_ld_shader_disk_dir, sizeof(g_ld_shader_disk_dir),
                 "scratch\\rsx_shader_cache_v%u",
                 SHADER_DISK_CACHE_VERSION);
    }

    if (!CreateDirectoryA(g_ld_shader_disk_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr,
                "[shader-disk-cache] cannot create '%s' error=%lu\n",
                g_ld_shader_disk_dir, (unsigned long)GetLastError());
        return 0;
    }
    const DWORD attributes = GetFileAttributesA(g_ld_shader_disk_dir);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;

    g_ld_shader_disk_ready = 1;
    fprintf(stderr, "[shader-disk-cache] enabled dir='%s' version=%u\n",
            g_ld_shader_disk_dir, SHADER_DISK_CACHE_VERSION);
    return 1;
}

static int shader_disk_cache_path(
    char path[MAX_PATH], u32 stage, u64 hash, u32 source_length)
{
    if (!shader_disk_cache_prepare())
        return 0;
    const int length = snprintf(
        path, MAX_PATH, "%s\\%c_%016llX_%08X.bin",
        g_ld_shader_disk_dir, (char)stage,
        (unsigned long long)hash, source_length);
    return length > 0 && length < MAX_PATH;
}

static ID3DBlob* shader_disk_cache_load(
    u32 stage, const char* source, u32 source_length, u64 hash)
{
    char path[MAX_PATH];
    const u32 stage_index = shader_disk_cache_stage_index(stage);
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return NULL;

    FILE* file = fopen(path, "rb");
    if (!file) {
        const u64 value = ++g_ld_shader_disk_misses[stage_index];
        shader_disk_cache_progress(value);
        return NULL;
    }

    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    int valid = fread(&header, sizeof(header), 1, file) == 1 &&
        header.magic == SHADER_DISK_CACHE_MAGIC &&
        header.version == SHADER_DISK_CACHE_VERSION &&
        header.stage == stage && header.source_hash == hash &&
        header.source_length == source_length && source_length <= 256u * 1024u &&
        header.blob_length > 0 &&
        header.blob_length <= SHADER_DISK_CACHE_MAX_BLOB;
    char* stored_source = NULL;
    ID3DBlob* blob = NULL;
    if (valid) {
        stored_source = (char*)malloc(source_length ? source_length : 1u);
        valid = stored_source != NULL &&
            fread(stored_source, 1, source_length, file) == source_length &&
            memcmp(stored_source, source, source_length) == 0;
    }
    if (valid) {
        valid = SUCCEEDED(D3DCreateBlob(header.blob_length, &blob)) && blob &&
            fread(blob->lpVtbl->GetBufferPointer(blob), 1,
                  header.blob_length, file) == header.blob_length;
    }
    free(stored_source);
    fclose(file);

    if (!valid) {
        if (blob)
            blob->lpVtbl->Release(blob);
        DeleteFileA(path);
        g_ld_shader_disk_rejects++;
        const u64 value = ++g_ld_shader_disk_misses[stage_index];
        shader_disk_cache_progress(value);
        return NULL;
    }

    const u64 value = ++g_ld_shader_disk_hits[stage_index];
    shader_disk_cache_progress(value);
    return blob;
}

static void shader_disk_cache_store(
    u32 stage, const char* source, u32 source_length, u64 hash,
    ID3DBlob* blob)
{
    if (!blob || source_length > 256u * 1024u)
        return;
    const size_t blob_length = blob->lpVtbl->GetBufferSize(blob);
    if (!blob_length || blob_length > SHADER_DISK_CACHE_MAX_BLOB)
        return;

    char path[MAX_PATH], temporary[MAX_PATH];
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return;
    const int temporary_length = snprintf(
        temporary, sizeof(temporary), "%s.tmp.%lu", path,
        (unsigned long)GetCurrentProcessId());
    if (temporary_length <= 0 || temporary_length >= sizeof(temporary))
        return;

    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    header.magic = SHADER_DISK_CACHE_MAGIC;
    header.version = SHADER_DISK_CACHE_VERSION;
    header.stage = stage;
    header.source_length = source_length;
    header.blob_length = (u32)blob_length;
    header.source_hash = hash;

    FILE* file = fopen(temporary, "wb");
    if (!file)
        return;
    const int wrote = fwrite(&header, sizeof(header), 1, file) == 1 &&
        fwrite(source, 1, source_length, file) == source_length &&
        fwrite(blob->lpVtbl->GetBufferPointer(blob), 1,
               blob_length, file) == blob_length &&
        fflush(file) == 0;
    fclose(file);
    if (!wrote || !MoveFileExA(
            temporary, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary);
        return;
    }

    const u32 stage_index = shader_disk_cache_stage_index(stage);
    const u64 value = ++g_ld_shader_disk_writes[stage_index];
    shader_disk_cache_progress(value);
}

static u64 ld_hash_structural_render_state(
    const render_state_t* state, u64 hash)
{
#define LD_HASH_RENDER_FIELD(name) \
    hash = fnv1a(&state->name, sizeof(state->name), hash)
    /* Alpha payload is deliberately absent. Enable and compare mode still
     * select distinct shader/PSO behavior. Every other D3D12 render-state
     * field is listed explicitly so struct padding can never become identity. */
    LD_HASH_RENDER_FIELD(alpha_test_enable);
    LD_HASH_RENDER_FIELD(alpha_func);
    LD_HASH_RENDER_FIELD(blend_enable);
    LD_HASH_RENDER_FIELD(sf_rgb);
    LD_HASH_RENDER_FIELD(df_rgb);
    LD_HASH_RENDER_FIELD(sf_a);
    LD_HASH_RENDER_FIELD(df_a);
    LD_HASH_RENDER_FIELD(eq_rgb);
    LD_HASH_RENDER_FIELD(eq_a);
    LD_HASH_RENDER_FIELD(depth_test);
    LD_HASH_RENDER_FIELD(depth_write);
    LD_HASH_RENDER_FIELD(depth_func);
    LD_HASH_RENDER_FIELD(cull_enable);
    LD_HASH_RENDER_FIELD(cull_face);
    LD_HASH_RENDER_FIELD(front_face);
    LD_HASH_RENDER_FIELD(color_mask);
    LD_HASH_RENDER_FIELD(rt_fp16);
    LD_HASH_RENDER_FIELD(rt_count);
    LD_HASH_RENDER_FIELD(stencil_enable);
    LD_HASH_RENDER_FIELD(stencil_two_sided);
    LD_HASH_RENDER_FIELD(s_func);
    LD_HASH_RENDER_FIELD(s_func_mask);
    LD_HASH_RENDER_FIELD(s_write_mask);
    LD_HASH_RENDER_FIELD(s_fail);
    LD_HASH_RENDER_FIELD(s_zfail);
    LD_HASH_RENDER_FIELD(s_zpass);
    LD_HASH_RENDER_FIELD(bs_func);
    LD_HASH_RENDER_FIELD(bs_fail);
    LD_HASH_RENDER_FIELD(bs_zfail);
    LD_HASH_RENDER_FIELD(bs_zpass);
    LD_HASH_RENDER_FIELD(poly_offset_fill);
    LD_HASH_RENDER_FIELD(poly_offset_scale);
    LD_HASH_RENDER_FIELD(poly_offset_bias);
#undef LD_HASH_RENDER_FIELD
    return hash;
}

/*
 * PSO identity includes render state, but D3DCompile only consumes the final
 * generated shader text.  Keep those cache boundaries separate: one exact
 * HLSL program is compiled once and its bytecode is reused by every PSO state
 * variant that references it.  The source bytes are retained for collision-
 * safe equality; the 64-bit hash is only an index accelerator.
 */
static ID3DBlob* shader_blob_cache_find(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, int* post_boundary_hit, int* hash_seen)
{
    if (post_boundary_hit)
        *post_boundary_hit = 0;
    if (hash_seen)
        *hash_seen = 0;
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->hash != hash)
            continue;
        if (hash_seen)
            *hash_seen = 1;
        if (
            entry->source_length != source_length ||
            memcmp(entry->source, source, source_length) != 0)
            continue;
        if (post_boundary_hit && i >= FORMER_MAX_SHADER_BLOBS)
            *post_boundary_hit = 1;
        entry->blob->lpVtbl->AddRef(entry->blob);
        return entry->blob;
    }
    return NULL;
}

typedef enum {
    SHADER_BLOB_INSERT_SKIPPED = 0,
    SHADER_BLOB_INSERTED,
    SHADER_BLOB_INSERT_FULL
} shader_blob_insert_result;

static shader_blob_insert_result shader_blob_cache_insert(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, ID3DBlob* blob)
{
    if (!blob)
        return SHADER_BLOB_INSERT_SKIPPED;
    if (cache->count >= MAX_SHADER_BLOBS)
        return SHADER_BLOB_INSERT_FULL;
    char* source_copy = (char*)malloc((size_t)source_length + 1u);
    if (!source_copy)
        return SHADER_BLOB_INSERT_SKIPPED;
    memcpy(source_copy, source, source_length);
    source_copy[source_length] = '\0';
    shader_blob_t* entry = &cache->entries[cache->count++];
    entry->hash = hash;
    entry->source_length = source_length;
    entry->source = source_copy;
    entry->blob = blob;
    blob->lpVtbl->AddRef(blob);
    cache->retained_source_bytes += source_length;
    cache->retained_blob_bytes += blob->lpVtbl->GetBufferSize(blob);
    return SHADER_BLOB_INSERTED;
}

static void shader_blob_cache_insert_accounted(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, ID3DBlob* blob, u32 stage, int hash_seen)
{
    const u32 count_before = cache->count;
    const shader_blob_insert_result result = shader_blob_cache_insert(
        cache, source, source_length, hash, blob);
#if defined(YZ_PERF_PROFILE)
    if (stage == 'V') {
        if (result == SHADER_BLOB_INSERTED) {
            g_ld_profile.total.vs_blob_inserts++;
            if (count_before >= FORMER_MAX_SHADER_BLOBS && !hash_seen)
                g_ld_profile.total.vs_post_boundary_distinct++;
        } else if (result == SHADER_BLOB_INSERT_FULL) {
            g_ld_profile.total.vs_blob_full_rejects++;
        }
    } else {
        if (result == SHADER_BLOB_INSERTED) {
            g_ld_profile.total.ps_blob_inserts++;
            if (count_before >= FORMER_MAX_SHADER_BLOBS && !hash_seen)
                g_ld_profile.total.ps_post_boundary_distinct++;
        } else if (result == SHADER_BLOB_INSERT_FULL) {
            g_ld_profile.total.ps_blob_full_rejects++;
        }
    }
#else
    (void)count_before;
    (void)result;
    (void)stage;
    (void)hash_seen;
#endif
}

static void shader_blob_cache_release(shader_blob_cache_t* cache)
{
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->blob)
            entry->blob->lpVtbl->Release(entry->blob);
        free(entry->source);
    }
    memset(cache, 0, sizeof(*cache));
}

#if defined(YZ_PERF_PROFILE)
static void ld_profile_note_hlsl(
    const char* hlsl, u64 hashes[MAX_SHADER_BLOBS], u32* count, u64* unique)
{
    const size_t length = strlen(hlsl);
    const u64 hash = fnv1a(
        hlsl, (u32)length, 1469598103934665603ull);
    for (u32 i = 0; i < *count; i++)
        if (hashes[i] == hash)
            return;
    if (*count < MAX_SHADER_BLOBS)
        hashes[(*count)++] = hash;
    (*unique)++;
}

static int ld_profile_note_hash(
    u64 hash, u64 hashes[LD_VARIANT_SET_CAPACITY], u32* count)
{
    if (!hash)
        hash = 1;
    u32 slot = (u32)hash & (LD_VARIANT_SET_CAPACITY - 1u);
    for (u32 probe = 0; probe < LD_VARIANT_SET_CAPACITY; ++probe) {
        if (hashes[slot] == hash)
            return 0;
        if (hashes[slot] == 0) {
            hashes[slot] = hash;
            (*count)++;
            return 1;
        }
        slot = (slot + 1u) & (LD_VARIANT_SET_CAPACITY - 1u);
    }
    g_ld_profile.ps_variant_tracking_overflow++;
    return 0;
}

static void ld_profile_note_fp_variants(
    const u8* fp_uc, u32 fp_size, u32 fp_ctrl_key, u32 cube_mask,
    const render_state_t* state)
{
    const u64 seed = 1469598103934665603ull;
    u64 raw_constant = fnv1a(fp_uc, fp_size, seed);
    u64 raw_exact = raw_constant;
    u64 canonical = rsx_fp_structural_hash(fp_uc, fp_size, seed);
    if (!canonical)
        return;

    const u32 alpha_enable = state->alpha_test_enable ? 1u : 0u;
    const u32 alpha_func = alpha_enable ? state->alpha_func : 0u;
#define LD_HASH_FP_SOURCE_FIELD(value) do { \
    const u32 _field = (value); \
    raw_exact = fnv1a(&_field, sizeof(_field), raw_exact); \
    raw_constant = fnv1a( \
        &_field, sizeof(_field), raw_constant); \
    canonical = fnv1a(&_field, sizeof(_field), canonical); \
} while (0)
    LD_HASH_FP_SOURCE_FIELD(fp_ctrl_key);
    LD_HASH_FP_SOURCE_FIELD(cube_mask);
    LD_HASH_FP_SOURCE_FIELD(alpha_enable);
    LD_HASH_FP_SOURCE_FIELD(alpha_func);
#undef LD_HASH_FP_SOURCE_FIELD

    float alpha_ref = 0.0f;
    if (alpha_enable && alpha_func != 0x0207u) {
        alpha_ref = rsx_fp_alpha_ref(
            state->alpha_ref_raw, state->alpha_ref_format);
        raw_exact = fnv1a(
            &alpha_ref, sizeof(alpha_ref), raw_exact);
    }

    const int new_raw_constant = ld_profile_note_hash(
        raw_constant, g_ld_profile.ps_raw_constant_hashes,
        &g_ld_profile.n_ps_raw_constant_hashes);
    const int new_raw_exact = ld_profile_note_hash(
        raw_exact, g_ld_profile.ps_raw_exact_hashes,
        &g_ld_profile.n_ps_raw_exact_hashes);
    if (new_raw_constant || new_raw_exact) {
        u64 literal = rsx_fp_literal_source_hash(
            fp_uc, fp_size, seed);
        literal = fnv1a(
            &fp_ctrl_key, sizeof(fp_ctrl_key), literal);
        literal = fnv1a(
            &cube_mask, sizeof(cube_mask), literal);
        literal = fnv1a(
            &alpha_enable, sizeof(alpha_enable), literal);
        literal = fnv1a(
            &alpha_func, sizeof(alpha_func), literal);
        if (new_raw_constant)
            ld_profile_note_hash(
                literal,
                g_ld_profile.ps_constant_specialized_hashes,
                &g_ld_profile.n_ps_constant_specialized_hashes);
        if (new_raw_exact) {
            u64 exact_source = literal;
            if (alpha_enable && alpha_func != 0x0207u) {
                char alpha_literal[64];
                const int length = snprintf(
                    alpha_literal, sizeof(alpha_literal), "%.9g",
                    (double)alpha_ref);
                if (length > 0 &&
                    (u32)length < sizeof(alpha_literal))
                    exact_source = fnv1a(
                        alpha_literal, (u32)length, exact_source);
            }
            ld_profile_note_hash(
                exact_source, g_ld_profile.ps_exact_source_hashes,
                &g_ld_profile.n_ps_exact_source_hashes);
        }
    }
    ld_profile_note_hash(
        canonical, g_ld_profile.ps_canonical_hashes,
        &g_ld_profile.n_ps_canonical_hashes);
}
#endif

static u32 vp_txl_unit_mask(const u8* ucode, u32 instrs)
{
    u32 mask = 0;
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16;
        const u32 d1 = (u32)p[4] | ((u32)p[5] << 8) |
                       ((u32)p[6] << 16) | ((u32)p[7] << 24);
        if (((d1 >> 22) & 0x1Fu) != 0x19u) continue;
        const u32 d2 = (u32)p[8] | ((u32)p[9] << 8) |
                       ((u32)p[10] << 16) | ((u32)p[11] << 24);
        mask |= 1u << ((d2 >> 8) & 3u);
    }
    return mask;
}

/* IEEE half -> float (blend colour of an FP16 target). */
static float ld_half_to_float(u16 half)
{
    const u32 sign = (u32)(half >> 15) << 31;
    const u32 exponent = (half >> 10) & 0x1Fu;
    const u32 mantissa = half & 0x3FFu;
    u32 bits;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            bits = sign;
        }
        else
        {
            float value = (float)mantissa / 16777216.0f;       /* 2^-24 */
            return (half >> 15) ? -value : value;
        }
    }
    else if (exponent == 31u)
    {
        bits = sign | 0x7F800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

static int ld_is_constant_blend_factor(u32 factor)
{
    return factor >= 0x8001u && factor <= 0x8004u;
}

/* BLEND_COLOR, for the CONSTANT_COLOR / CONSTANT_ALPHA blend factors (the
 * PSO maps them to D3D12_BLEND_BLEND_FACTOR). It was never set, so every
 * draw blending with the constant used D3D12's default of zero and drew
 * nothing: an effect faded with the constant colour vanished. ARGB8 for an
 * 8-bit target; for FP16, BLEND_COLOR holds R|G and BLEND_COLOR2 B|A as
 * halves. D3D12 has no separate constant alpha for the colour channels, so
 * a draw that blends colour by CONSTANT_ALPHA gets the alpha in all four. */
static void ld_set_blend_factor(void)
{
    if (!(rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1u))
    {
        return;
    }
    const u32 sourceFactors = rsx_dsp_reg(&g.rsx, M_BLEND_SFACTOR);
    const u32 destinationFactors = rsx_dsp_reg(&g.rsx, M_BLEND_DFACTOR);
    const u32 sourceColor = sourceFactors & 0xFFFFu;
    const u32 sourceAlpha = sourceFactors >> 16;
    const u32 destinationColor = destinationFactors & 0xFFFFu;
    const u32 destinationAlpha = destinationFactors >> 16;
    if (!ld_is_constant_blend_factor(sourceColor) && !ld_is_constant_blend_factor(sourceAlpha) &&
        !ld_is_constant_blend_factor(destinationColor) && !ld_is_constant_blend_factor(destinationAlpha))
    {
        return;
    }
    const u32 blendColor = rsx_dsp_reg(&g.rsx, M_BLEND_COLOR);
    float factor[4];
    rsx_dsp_surface surface;
    rsx_dsp_get_surface(&g.rsx, &surface);
    if (surface.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16)
    {
        const u32 blendColorHigh = rsx_dsp_reg(&g.rsx, M_BLEND_COLOR2);
        factor[0] = ld_half_to_float((u16)(blendColor & 0xFFFFu));
        factor[1] = ld_half_to_float((u16)(blendColor >> 16));
        factor[2] = ld_half_to_float((u16)(blendColorHigh & 0xFFFFu));
        factor[3] = ld_half_to_float((u16)(blendColorHigh >> 16));
    }
    else
    {
        factor[0] = (float)((blendColor >> 16) & 0xFFu) / 255.0f;
        factor[1] = (float)((blendColor >> 8) & 0xFFu) / 255.0f;
        factor[2] = (float)(blendColor & 0xFFu) / 255.0f;
        factor[3] = (float)(blendColor >> 24) / 255.0f;
    }
    const int colorUsesConstantAlpha =
        sourceColor == 0x8003u || sourceColor == 0x8004u ||
        destinationColor == 0x8003u || destinationColor == 0x8004u;
    const int colorUsesConstantColor =
        sourceColor == 0x8001u || sourceColor == 0x8002u ||
        destinationColor == 0x8001u || destinationColor == 0x8002u;
    if (colorUsesConstantAlpha && !colorUsesConstantColor)
    {
        factor[0] = factor[3];
        factor[1] = factor[3];
        factor[2] = factor[3];
    }
    g.list->lpVtbl->OMSetBlendFactor(g.list, factor);
}

static ID3D12PipelineState* build_pso(
    const char* vs_hlsl, const char* ps_hlsl, const render_state_t* rs,
    const rsx_vertex_layout_plan* masked_layout, int packed_offsets)
{
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    static u32 compile_fail_logs = 0;
    static u32 create_fail_logs = 0;
    const u32 vs_length = (u32)strlen(vs_hlsl);
    const u32 ps_length = (u32)strlen(ps_hlsl);
    const u64 vs_hash = fnv1a(
        vs_hlsl, vs_length, 1469598103934665603ull);
    const u64 ps_hash = fnv1a(
        ps_hlsl, ps_length, 1469598103934665603ull);
#if defined(YZ_PERF_PROFILE)
    ld_profile_note_hlsl(
        vs_hlsl, g_ld_profile.vs_hashes, &g_ld_profile.n_vs_hashes,
        &g_ld_profile.total.vs_unique);
    ld_profile_note_hlsl(
        ps_hlsl, g_ld_profile.ps_hashes, &g_ld_profile.n_ps_hashes,
        &g_ld_profile.total.ps_unique);
    g_ld_profile.total.vs_blob_lookups++;
    const LONGLONG vs_lookup_begin = ld_profile_qpc();
#endif
    int vs_post_boundary_hit = 0;
    int vs_hash_seen = 0;
    /* build_pso also runs on the background compile workers (ld_pso_job):
     * the blob caches and the disk cache are shared, the compiler is not. */
    AcquireSRWLockExclusive(&g_ld_shader_cache_lock);
    vs = shader_blob_cache_find(
        &g.vs_blobs, vs_hlsl, vs_length, vs_hash,
        &vs_post_boundary_hit, &vs_hash_seen);
    ReleaseSRWLockExclusive(&g_ld_shader_cache_lock);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.vs_blob_lookup_qpc +=
        (u64)(ld_profile_qpc() - vs_lookup_begin);
#endif
    HRESULT vs_hr = S_OK;
    if (vs) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.vs_blob_hits++;
        if (vs_post_boundary_hit)
            g_ld_profile.total.vs_post_boundary_repeats++;
#endif
    } else {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.vs_blob_misses++;
#endif
        AcquireSRWLockExclusive(&g_ld_shader_cache_lock);
        vs = shader_disk_cache_load('V', vs_hlsl, vs_length, vs_hash);
        if (vs) {
            shader_blob_cache_insert_accounted(
                &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs,
                'V', vs_hash_seen);
        }
        ReleaseSRWLockExclusive(&g_ld_shader_cache_lock);
        if (!vs) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.vs_compile_calls++;
            const LONGLONG compile_begin = ld_profile_qpc();
#endif
            vs_hr = D3DCompile(
                vs_hlsl, vs_length, "xvs", NULL, NULL, "main",
                "vs_5_0", 0, 0, &vs, &err);
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.vs_compile_qpc +=
                (u64)(ld_profile_qpc() - compile_begin);
#endif
            if (SUCCEEDED(vs_hr)) {
                AcquireSRWLockExclusive(&g_ld_shader_cache_lock);
                shader_disk_cache_store(
                    'V', vs_hlsl, vs_length, vs_hash, vs);
                shader_blob_cache_insert_accounted(
                    &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs,
                    'V', vs_hash_seen);
                ReleaseSRWLockExclusive(&g_ld_shader_cache_lock);
            }
        }
    }
    if (FAILED(vs_hr)) {
        if (compile_fail_logs++ < 32) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] vertex compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_blob_lookups++;
    const LONGLONG ps_lookup_begin = ld_profile_qpc();
#endif
    int ps_post_boundary_hit = 0;
    int ps_hash_seen = 0;
    AcquireSRWLockExclusive(&g_ld_shader_cache_lock);
    ps = shader_blob_cache_find(
        &g.ps_blobs, ps_hlsl, ps_length, ps_hash,
        &ps_post_boundary_hit, &ps_hash_seen);
    ReleaseSRWLockExclusive(&g_ld_shader_cache_lock);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_blob_lookup_qpc +=
        (u64)(ld_profile_qpc() - ps_lookup_begin);
#endif
    HRESULT ps_hr = S_OK;
    if (ps) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_blob_hits++;
        if (ps_post_boundary_hit)
            g_ld_profile.total.ps_post_boundary_repeats++;
#endif
    } else {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_blob_misses++;
#endif
        AcquireSRWLockExclusive(&g_ld_shader_cache_lock);
        ps = shader_disk_cache_load('P', ps_hlsl, ps_length, ps_hash);
        if (ps) {
            shader_blob_cache_insert_accounted(
                &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps,
                'P', ps_hash_seen);
        }
        ReleaseSRWLockExclusive(&g_ld_shader_cache_lock);
        if (!ps) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_compile_calls++;
            const LONGLONG compile_begin = ld_profile_qpc();
#endif
            ps_hr = D3DCompile(
                ps_hlsl, ps_length, "xps", NULL, NULL, "main",
                "ps_5_0", 0, 0, &ps, &err);
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_compile_qpc +=
                (u64)(ld_profile_qpc() - compile_begin);
#endif
            if (SUCCEEDED(ps_hr)) {
                AcquireSRWLockExclusive(&g_ld_shader_cache_lock);
                shader_disk_cache_store(
                    'P', ps_hlsl, ps_length, ps_hash, ps);
                shader_blob_cache_insert_accounted(
                    &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps,
                    'P', ps_hash_seen);
                ReleaseSRWLockExclusive(&g_ld_shader_cache_lock);
            }
        }
    }
    if (FAILED(ps_hr)) {
        if (compile_fail_logs++ < 32) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] pixel compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err);
        vs->lpVtbl->Release(vs); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
    D3D12_INPUT_ELEMENT_DESC il[16];
    const u32 input_count =
        masked_layout ? masked_layout->count : RSX_DSP_NUM_VERTEX_ATTR;
    for (u32 slot = 0; slot < input_count; slot++) {
        const u32 attr =
            masked_layout ? masked_layout->attrs[slot] : slot;
        D3D12_INPUT_ELEMENT_DESC e = {
            "ATTR", attr, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
            masked_layout && !packed_offsets ? attr * 16u : slot * 16u,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        };
        il[slot] = e;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g.rootsig_x;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = input_count ? il : NULL;
    pd.InputLayout.NumElements = input_count;
    apply_render_state(&pd, rs);
    pd.SampleMask = 0xFFFFFFFFu;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = rs->rt_count ? rs->rt_count : 1;
    for (u32 r = 0; r < pd.NumRenderTargets; r++)
        pd.RTVFormats[r] = rs->rt_fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                       : DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.create_pso_calls++;
    const LONGLONG create_begin = ld_profile_qpc();
#endif
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(
        g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.create_pso_qpc +=
        (u64)(ld_profile_qpc() - create_begin);
#endif
    if (FAILED(hr)) {
        const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
        if (create_fail_logs++ < 32)
            fprintf(stderr,
                    "[pso-fail] create hr=0x%08lX removed=0x%08lX "
                    "depth{test=%u write=%u func=%u} blend=%u "
                    "cull{enable=%u face=%u}\n",
                    (unsigned long)hr, (unsigned long)removed,
                    rs->depth_test, rs->depth_write, rs->depth_func,
                    rs->blend_enable, rs->cull_enable, rs->cull_face);
        if (removed != S_OK)
            ld_dump_dred("CreateGraphicsPipelineState", hr);
    }
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps);
    return SUCCEEDED(hr) ? pso : NULL;
}

static int ld_current_vertex_layout(rsx_vertex_layout_plan* layout)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) {
        ld_layout_plan_get(0xFFFFu, layout);
        return 0;
    }
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    const u32 vp_instrs = rsx_vp_program_size_instrs(
        vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    rsx_vp_input_analysis analysis = {0xFFFFu, 0};
    if (!vp_instrs ||
        rsx_vp_analyze_inputs(
            vp_uc, vp_instrs * 16, &analysis) != (int)vp_instrs ||
        !analysis.exact)
        analysis.input_mask = 0xFFFFu;
    ld_layout_plan_get(analysis.input_mask, layout);
    return vp_instrs != 0;
}

/* LD_PSO_DBG=1: name the first bail-out of each kind in get_pso(). Every exit
 * there returns NULL silently and the caller only counts drop{pso=N}, so a
 * title whose draws ALL die in this function has no way to say which gate
 * rejected them. One line per distinct reason, so a stuck title prints a
 * handful of lines rather than one per draw. */
static void ld_pso_bail(int* said, const char* reason)
{
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("LD_PSO_DBG") ? 1 : 0;
    if (!dbg || *said) return;
    *said = 1;
    fprintf(stderr, "[pso-bail] %s\n", reason);
    fflush(stderr);
}
#define LD_PSO_BAIL(reason) \
    do { static int _said_ = 0; ld_pso_bail(&_said_, (reason)); } while (0)

/* PSO index (psos[] by key). Only the present thread touches it. */
static u32 ld_pso_index_slot(u64 key)
{
    return (u32)(key ^ (key >> 31) ^ (key >> 47)) & (PSO_INDEX_SLOTS - 1u);
}

static u32 ld_pso_index_find(u64 key)
{
    u32 slot = ld_pso_index_slot(key);
    for (u32 probe = 0; probe < PSO_INDEX_SLOTS; probe++)
    {
        const u32 stored = g.pso_index[slot];
        if (!stored)
        {
            return UINT32_MAX;
        }
        if (g.psos[stored - 1u].key == key)
        {
            return stored - 1u;
        }
        slot = (slot + 1u) & (PSO_INDEX_SLOTS - 1u);
    }
    return UINT32_MAX;
}

static void ld_pso_index_insert(u64 key, u32 entryIndex)
{
    u32 slot = ld_pso_index_slot(key);
    while (g.pso_index[slot])
    {
        slot = (slot + 1u) & (PSO_INDEX_SLOTS - 1u);
    }
    g.pso_index[slot] = entryIndex + 1u;
}

/* Background compile of one PSO: the shaders through D3DCompile (or the
 * disk cache) and CreateGraphicsPipelineState, off the present thread. A
 * new effect used to stall the frame it first appeared in for as long as
 * fxc took over its shaders -- the hitch the first time each effect played.
 * Until the job finishes, draws needing the PSO are skipped (the effect
 * appears a few frames late, once). */
typedef struct ld_pso_job {
    u32 entryIndex;
    char* vertexSource;
    char* pixelSource;
    render_state_t renderState;
    rsx_vertex_layout_plan layout;
    int hasLayout;
    int packedPayload;
} ld_pso_job;

static ID3D12PipelineState* build_pso(
    const char* vs_hlsl, const char* ps_hlsl, const render_state_t* rs,
    const rsx_vertex_layout_plan* masked_layout, int packed_offsets);

static void ld_pso_job_finish(ld_pso_job* job)
{
    ID3D12PipelineState* pipeline = build_pso(
        job->vertexSource, job->pixelSource, &job->renderState,
        job->hasLayout ? &job->layout : NULL, job->packedPayload);
    InterlockedExchangePointer((PVOID volatile*)&g.psos[job->entryIndex].pso, pipeline);
    InterlockedExchange(&g.psos[job->entryIndex].pending, 0);
    InterlockedDecrement(&g_ld_pso_jobs_pending);
    InterlockedIncrement(&g_ld_pso_jobs_done);
    free(job->vertexSource);
    free(job->pixelSource);
    free(job);
}

static VOID CALLBACK ld_pso_job_run(PTP_CALLBACK_INSTANCE instance, PVOID context)
{
    (void)instance;
    ld_pso_job_finish((ld_pso_job*)context);
}

/* Queue entry `entryIndex` for a background build. Returns 1 when queued
 * (the entry is marked pending), 0 when the caller must build it now. */
static int ld_pso_job_submit(u32 entryIndex, const char* vertexSource, const char* pixelSource,
                             const render_state_t* renderState,
                             const rsx_vertex_layout_plan* layout, int packedPayload)
{
    if (!InterlockedCompareExchange(&g_ld_async_shaders, 0, 0))
    {
        return 0;
    }
    ld_pso_job* job = (ld_pso_job*)calloc(1, sizeof(ld_pso_job));
    if (!job)
    {
        return 0;
    }
    const size_t vertexLength = strlen(vertexSource) + 1u;
    const size_t pixelLength = strlen(pixelSource) + 1u;
    job->vertexSource = (char*)malloc(vertexLength);
    job->pixelSource = (char*)malloc(pixelLength);
    if (!job->vertexSource || !job->pixelSource)
    {
        free(job->vertexSource);
        free(job->pixelSource);
        free(job);
        return 0;
    }
    memcpy(job->vertexSource, vertexSource, vertexLength);
    memcpy(job->pixelSource, pixelSource, pixelLength);
    job->entryIndex = entryIndex;
    job->renderState = *renderState;
    job->hasLayout = layout != NULL;
    if (layout)
    {
        job->layout = *layout;
    }
    job->packedPayload = packedPayload;
    InterlockedExchange(&g.psos[entryIndex].pending, 1);
    InterlockedIncrement(&g_ld_pso_jobs_pending);
    if (!TrySubmitThreadpoolCallback(ld_pso_job_run, job, NULL))
    {
        ld_pso_job_finish(job);          /* no pool: build it here after all */
    }
    return 1;
}

/* Shutdown: let queued compiles finish before the device goes. */
static void ld_pso_jobs_wait(void)
{
    for (u32 waited = 0; waited < 5000u; waited++)
    {
        if (InterlockedCompareExchange(&g_ld_pso_jobs_pending, 0, 0) <= 0)
        {
            return;
        }
        Sleep(1);
    }
}

/* WPOS is the fragment's window position; at a render scale above 1 the
 * host pixel grid is finer than the guest's, so bring it back to guest
 * pixels before a shader uses it (screen-space effects, texel lookups). */
static int ld_scale_wpos(char* hlsl, u32 capacity, u32 drawScale, float drawScaleX)
{
    if (drawScale <= 1u && drawScaleX <= 1.0f)
    {
        return 0;
    }
    static const char needle[] = "input.position";
    char replacement[96];
    const float inverseScale = 1.0f / (float)(drawScale ? drawScale : 1u);
    const float inverseScaleX = 1.0f / (drawScaleX > 0.0f ? drawScaleX : 1.0f);
    snprintf(replacement, sizeof replacement,
             "(input.position * float4(%.9f, %.9f, 1.0, 1.0))", inverseScaleX, inverseScale);
    const size_t needleLength = sizeof(needle) - 1u;
    const size_t replacementLength = strlen(replacement);
    int replaced = 0;
    char* cursor = strstr(hlsl, needle);
    while (cursor)
    {
        const size_t used = strlen(hlsl);
        if (used - needleLength + replacementLength + 1u > capacity)
        {
            return -1;
        }
        memmove(cursor + replacementLength, cursor + needleLength,
                used - (size_t)(cursor - hlsl) - needleLength + 1u);
        memcpy(cursor, replacement, replacementLength);
        replaced++;
        cursor = strstr(cursor + replacementLength, needle);
    }
    return replaced;
}

/* ---------------------------------------------------------------------------
 * Draw probe (diagnostics).
 *
 * LD_PROBE_PSO=<key>[,<key>...] (keys as the pso_key column of draws.csv):
 * while a render capture is open, the first draws with those keys write
 * their state to <LD_DRAW_DUMP>/probe.txt -- every non-zero vertex constant,
 * the texture units and a summary of unit 0's texels -- so a draw that
 * executes but shows nothing can be followed offline.
 *
 * LD_PROBE_AB=<prim>: during a capture, matching draws of that primitive
 * type render as an A/B over consecutive frames (frame % 4): 0 as the game asks, 1 without the
 * depth test, 2 without the alpha test, 3 with texture unit 0 white. A
 * capture's flip images then show which change makes the draw appear.
 * -----------------------------------------------------------------------*/
/* Per-draw shader identity, memoised.
 *
 * Every draw used to rescan its vertex program for the END bit and FNV-hash
 * all of its bytes (twice: the PSO key and the metadata hash) and scan it
 * again for TXL -- several microseconds a draw, which is milliseconds a
 * frame where Folklore issues 2,000 small particle draws. The program only
 * changes when the guest uploads transform-program words, which bump
 * rsx.vp_generation, so a small table keyed by start slot and generation
 * returns the same answers without touching the bytes. */
typedef struct
{
    u32 valid;
    u32 start;
    u32 generation;
    u32 instructionCount;
    u32 txlMask;
    u64 programHash;
} ld_vp_identity;

static ld_vp_identity g_ld_vp_identities[16];
static u64 g_ld_vp_identity_hits = 0;
static u64 g_ld_vp_identity_misses = 0;

static int ld_vp_identity_get(u32 start, ld_vp_identity* out)
{
    const u32 generation = g.rsx.vp_generation;
    ld_vp_identity* entry = &g_ld_vp_identities[start & 15u];
    if (entry->valid && entry->start == start && entry->generation == generation)
    {
        g_ld_vp_identity_hits++;
        *out = *entry;
        return 1;
    }
    g_ld_vp_identity_misses++;
    const u8* programCode = (const u8*)(g.rsx.vp + start * 4);
    const u32 instructionCount = rsx_vp_program_size_instrs(programCode, (RSX_DSP_VP_INSTR - start) * 16);
    if (!instructionCount)
    {
        return 0;
    }
    entry->valid = 1;
    entry->start = start;
    entry->generation = generation;
    entry->instructionCount = instructionCount;
    entry->txlMask = vp_txl_unit_mask(programCode, instructionCount);
    entry->programHash = fnv1a(programCode, instructionCount * 16, 1469598103934665603ull);
    *out = *entry;
    return 1;
}

/* A texture that reads a depth buffer through A8R8G8B8 -- the zeta's own
 * address, a colour format, and a zeta the host has drawn into. Returns that
 * zeta's index in g.zdepths, or -1. The host depth never reaches guest VRAM,
 * so such a read must take the depth snapshot, rebuilt into the Z24S8 bytes
 * by rsx_fp_apply_zeta_alias; from guest VRAM it read stale bytes, which
 * depth-faded effects turned into speckle or faded out entirely.
 * LD_NO_ZETA_ALIAS=1 restores the old guest-VRAM read. */
static int ld_zeta_alias_index(const rsx_dsp_texture* texture)
{
    static int disabled = -1;
    if (disabled < 0)
    {
        const char* text = getenv("LD_NO_ZETA_ALIAS");
        disabled = text && text[0] && text[0] != '0' ? 1 : 0;
    }
    if (disabled || !texture->enabled)
    {
        return -1;
    }
    const u32 baseFormat = texture->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    if (baseFormat != TEX_FMT_A8R8G8B8 || texture->remap != 0x0000AAE4u)
    {
        return -1;
    }
    for (u32 zetaIndex = 0; zetaIndex < g.n_zdepths; zetaIndex++)
    {
        const zdepth_t* zeta = &g.zdepths[zetaIndex];
        if (zeta->location == texture->location && zeta->offset == texture->offset &&
            (zeta->had_write || zeta->snapshot_valid))
        {
            return (int)zetaIndex;
        }
    }
    return -1;
}

static u64 g_ld_probe_keys[8];
static u32 g_ld_probe_key_count = 0;
static int g_ld_probe_ab_prim = -1;
static int g_ld_probe_variant = 0;      /* set per draw before get_pso */

static void ld_probe_init(void)
{
    static int initialized = 0;
    if (initialized)
    {
        return;
    }
    initialized = 1;
    const char* keys = getenv("LD_PROBE_PSO");
    while (keys && *keys && g_ld_probe_key_count < 8)
    {
        char* end = NULL;
        const u64 key = _strtoui64(keys, &end, 16);
        if (end == keys)
        {
            break;
        }
        g_ld_probe_keys[g_ld_probe_key_count++] = key;
        keys = end;
        while (*keys == ',' || *keys == ' ')
        {
            keys++;
        }
    }
    const char* abText = getenv("LD_PROBE_AB");
    if (abText && *abText)
    {
        g_ld_probe_ab_prim = atoi(abText);
    }
    if (g_ld_probe_key_count)
    {
        fprintf(stderr, "[probe] %u key(s), A/B on prim %d\n", g_ld_probe_key_count, g_ld_probe_ab_prim);
    }
}

static int ld_probe_matches(u64 key)
{
    for (u32 keyIndex = 0; keyIndex < g_ld_probe_key_count; keyIndex++)
    {
        if (g_ld_probe_keys[keyIndex] == key)
        {
            return (int)keyIndex + 1;
        }
    }
    return 0;
}

static float ld_probe_half(u16 bits)
{
    const u32 sign = (u32)(bits >> 15) << 31;
    const u32 exponent = (bits >> 10) & 0x1Fu;
    const u32 mantissa = bits & 0x3FFu;
    u32 out;
    if (exponent == 0)
    {
        out = sign;
    }
    else if (exponent == 31)
    {
        out = sign | 0x7F800000u | (mantissa << 13);
    }
    else
    {
        out = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &out, sizeof value);
    return value;
}

/* Texel summary of a texture's first level from guest memory. */
static void ld_probe_texture_summary(FILE* file, const rsx_dsp_texture* texture)
{
    const u32 baseFormat = texture->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const u32 width = texture->width;
    const u32 height = texture->height;
    if (!width || !height || width > 4096 || height > 4096)
    {
        return;
    }
    if (baseFormat == TEX_FMT_DXT1 || baseFormat == TEX_FMT_DXT23 || baseFormat == TEX_FMT_DXT45)
    {
        const u32 blockBytes = baseFormat == TEX_FMT_DXT1 ? 8u : 16u;
        const u32 blocksWide = (width + 3u) / 4u;
        const u32 blocksHigh = (height + 3u) / 4u;
        const u8* source = guest_ptr(texture->location, texture->offset, blocksWide * blocksHigh * blockBytes);
        if (!source)
        {
            fprintf(file, "    texels: unreadable\n");
            return;
        }
        u32 alphaMin = 255;
        u32 alphaMax = 0;
        unsigned long long alphaSum = 0;
        u32 colorMax = 0;
        u32 zeroBlocks = 0;
        const u32 blockCount = blocksWide * blocksHigh;
        for (u32 blockIndex = 0; blockIndex < blockCount; blockIndex++)
        {
            const u8* block = source + (size_t)blockIndex * blockBytes;
            int allZero = 1;
            for (u32 byteIndex = 0; byteIndex < blockBytes; byteIndex++)
            {
                if (block[byteIndex])
                {
                    allZero = 0;
                    break;
                }
            }
            if (allZero)
            {
                zeroBlocks++;
            }
            u32 alphaHigh = 255;
            u32 alphaLow = 255;
            const u8* colorBlock = block;
            if (baseFormat == TEX_FMT_DXT45)
            {
                alphaHigh = block[0] > block[1] ? block[0] : block[1];
                alphaLow = block[0] < block[1] ? block[0] : block[1];
                colorBlock = block + 8;
            }
            else if (baseFormat == TEX_FMT_DXT23)
            {
                alphaHigh = 0;
                alphaLow = 255;
                for (u32 nibbleIndex = 0; nibbleIndex < 16; nibbleIndex++)
                {
                    const u32 nibble = (block[nibbleIndex / 2] >> ((nibbleIndex & 1u) * 4u)) & 0xFu;
                    const u32 alpha = nibble * 17u;
                    alphaHigh = alpha > alphaHigh ? alpha : alphaHigh;
                    alphaLow = alpha < alphaLow ? alpha : alphaLow;
                }
                colorBlock = block + 8;
            }
            alphaMin = alphaLow < alphaMin ? alphaLow : alphaMin;
            alphaMax = alphaHigh > alphaMax ? alphaHigh : alphaMax;
            alphaSum += alphaHigh;
            /* Endpoints as stored (byte order as in guest memory). */
            const u32 endpointA = (u32)colorBlock[0] | ((u32)colorBlock[1] << 8);
            const u32 endpointB = (u32)colorBlock[2] | ((u32)colorBlock[3] << 8);
            colorMax = endpointA > colorMax ? endpointA : colorMax;
            colorMax = endpointB > colorMax ? endpointB : colorMax;
        }
        fprintf(file, "    texels: %u blocks, %u all-zero, block alpha min %u max %u mean-of-max %.1f, "
                      "max colour endpoint 0x%04X\n",
                blockCount, zeroBlocks, alphaMin, alphaMax,
                blockCount ? (double)alphaSum / (double)blockCount : 0.0, colorMax);
        fprintf(file, "    first block:");
        for (u32 byteIndex = 0; byteIndex < blockBytes; byteIndex++)
        {
            fprintf(file, " %02X", source[byteIndex]);
        }
        fprintf(file, "\n");
        return;
    }
    if (baseFormat == TEX_FMT_A8R8G8B8)
    {
        const u8* source = guest_ptr(texture->location, texture->offset, width * height * 4u);
        if (!source)
        {
            fprintf(file, "    texels: unreadable\n");
            return;
        }
        u32 channelMax[4] = {0, 0, 0, 0};
        for (u32 texelIndex = 0; texelIndex < width * height; texelIndex++)
        {
            for (u32 channel = 0; channel < 4; channel++)
            {
                const u32 value = source[(size_t)texelIndex * 4u + channel];
                channelMax[channel] = value > channelMax[channel] ? value : channelMax[channel];
            }
        }
        fprintf(file, "    texels: byte maxima %u %u %u %u\n",
                channelMax[0], channelMax[1], channelMax[2], channelMax[3]);
    }
}

static void ld_probe_draw(u64 key, u32 prim)
{
    if (!g_ld_probe_key_count || !s_csv_file || !s_csv_gate_open)
    {
        return;
    }
    const int match = ld_probe_matches(key);
    if (!match)
    {
        return;
    }
    /* Six draws per key and primitive type per capture, so a key's rarer
     * primitive (the tether's tri-strips among its quads) is reached too. */
    static u32 written[8][16];
    static u32 session = 0;
    if (session != s_csv_gate_session)
    {
        session = s_csv_gate_session;
        memset(written, 0, sizeof written);
    }
    const u32 primSlot = prim & 15u;
    if (written[match - 1][primSlot] >= 6u)
    {
        return;
    }
    written[match - 1][primSlot]++;
    static FILE* file = NULL;
    if (!file)
    {
        const char* directory = getenv("LD_DRAW_DUMP");
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\probe.txt", directory && *directory ? directory : ".");
        file = fopen(path, "a");
        if (!file)
        {
            return;
        }
    }
    render_state_t state;
    decode_render_state(&state);
    rsx_dsp_surface surface;
    rsx_dsp_get_surface(&g.rsx, &surface);
    fprintf(file, "== capture %u frame %u key %016llx prim %u variant %d\n", s_csv_gate_session, g_ld_frames,
            (unsigned long long)key, prim, g_ld_probe_variant);
    fprintf(file, "  alpha test %u func 0x%X ref 0x%X (fmt %u -> %.4f), depth test %u write %u func 0x%X, "
                  "blend %u sf 0x%X df 0x%X, cull %u, zeta 0x%X, clip %ux%u, vp_start %u\n",
            state.alpha_test_enable, state.alpha_func, state.alpha_ref_raw, state.alpha_ref_format,
            rsx_fp_alpha_ref(state.alpha_ref_raw, state.alpha_ref_format), state.depth_test,
            state.depth_write, state.depth_func, state.blend_enable, state.sf_rgb, state.df_rgb,
            state.cull_enable, surface.zeta_offset, surface.clip_w, surface.clip_h, rsx_dsp_vp_start(&g.rsx));
    for (u32 constantIndex = 0; constantIndex < 512; constantIndex++)
    {
        const u32* words = g.rsx.constants[constantIndex];
        if (!(words[0] | words[1] | words[2] | words[3]))
        {
            continue;
        }
        float values[4];
        memcpy(values, words, sizeof values);
        fprintf(file, "  c%u = %.6g %.6g %.6g %.6g\n", constantIndex, values[0], values[1], values[2], values[3]);
    }
    for (u32 unit = 0; unit < 4; unit++)
    {
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx, unit, &texture);
        if (!texture.enabled)
        {
            continue;
        }
        fprintf(file, "  tex%u %u:0x%08X fmt 0x%02X %ux%u mips %u pitch %u wrap 0x%08X remap 0x%08X "
                      "filter 0x%08X control0 0x%08X border 0x%08X\n",
                unit, texture.location, texture.offset, texture.format, texture.width, texture.height,
                texture.mipmaps, texture.pitch, texture.wrap, texture.remap, texture.filter, texture.control0,
                texture.border_color);
        if (unit == 0)
        {
            ld_probe_texture_summary(file, &texture);
        }
    }
    (void)ld_probe_half;
    fflush(file);
}

/* The A/B variant this draw renders with (0 = as asked). */
static int ld_probe_choose_variant(u64 key, u32 prim)
{
    if (g_ld_probe_ab_prim < 0 || (int)prim != g_ld_probe_ab_prim || !ld_probe_matches(key) ||
        !s_csv_file || !s_csv_gate_open)
    {
        return 0;
    }
    return (int)(g_ld_frames & 3u);
}

static ID3D12PipelineState* get_pso(
    const rsx_vertex_layout_plan* masked_layout, int packed_payload)
{
    memset(&g_ld_current_pso, 0, sizeof(g_ld_current_pso));
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) { LD_PSO_BAIL("no vertex program uploaded (vp start out of range)"); return NULL; }
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    ld_vp_identity vpIdentity;
    if (!ld_vp_identity_get(start, &vpIdentity)) { LD_PSO_BAIL("vertex program has zero instructions"); return NULL; }
    const u32 vp_instrs = vpIdentity.instructionCount;

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = guest_ptr(fp_loc, fp_off, 16);
    if (!fp_uc) { LD_PSO_BAIL("fragment program address does not resolve to guest memory"); return NULL; }
    const u32 fp_size = rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size) {
        /* No END-bit instruction in 64 KB: the bytes at (location, offset) are
         * not a fragment program. Almost always the LOCATION is wrong, not the
         * offset -- the same class of bug caner hit in Yakuza with textures --
         * so report the register the guest actually wrote and the head of what
         * we read, which is what says local-vs-main. */
        static int said = 0;
        if (!said && getenv("LD_PSO_DBG")) {
            said = 1;
            fprintf(stderr,
                    "[pso-bail] fragment program size reads as zero: "
                    "reg=0x%08X -> loc=%u off=0x%08X, first words "
                    "%08X %08X %08X %08X\n",
                    rsx_dsp_reg(&g.rsx, 0x08E4 /* M_FP_ACTIVE_PROGRAM */), fp_loc, fp_off,
                    rsx_fp_read_word(fp_uc + 0), rsx_fp_read_word(fp_uc + 4),
                    rsx_fp_read_word(fp_uc + 8), rsx_fp_read_word(fp_uc + 12));
            /* All-zero here has two very different causes: the offset resolved
             * into the wrong memory, or nothing ever wrote the buffer. Scan
             * forward for the first non-zero byte -- a hit a few hundred bytes
             * on means the region IS populated and the offset is wrong; no hit
             * across 64 KB means the producer never ran. */
            {
                const u8* q = guest_ptr(fp_loc, fp_off, 0x10000);
                u32 i = 0;
                if (q) { while (i < 0x10000u && !q[i]) i++; }
                if (!q)
                    fprintf(stderr, "[pso-bail]   (64 KB window not mapped)\n");
                else if (i == 0x10000u)
                    fprintf(stderr, "[pso-bail]   64 KB from here is entirely "
                                    "zero -- nothing ever wrote this buffer\n");
                else
                    fprintf(stderr, "[pso-bail]   first non-zero byte at "
                                    "+0x%X -- the region is populated, the "
                                    "offset is wrong\n", i);
            }
            fflush(stderr);
        }
        return NULL;
    }
    /* re-resolve with the true size to validate the whole program is mapped */
    fp_uc = guest_ptr(fp_loc, fp_off, fp_size);
    if (!fp_uc) { LD_PSO_BAIL("fragment program is not fully mapped in guest memory"); return NULL; }

    /* Fragment output register mode (fp16 h0 vs fp32 r0) is driven by the
     * SHADER_CONTROL word bit 0x40 (same fix as the replay harness — the
     * AUTO heuristic returned stale fp32 scratch for h0-writing materials);
     * fold the deciding bit into the cache key so a program reused under a
     * different export mode gets its own PSO. Kill-switch RSX_FP_CTRL_AUTO=1
     * restores the old heuristic for the A/B. */
    static int ctrl_auto = -1;
    if (ctrl_auto < 0) ctrl_auto = getenv("RSX_FP_CTRL_AUTO") ? 1 : 0;
    const u32 fp_ctrl = ctrl_auto ? RSX_FP_CTRL_AUTO : rsx_dsp_shader_control(&g.rsx);
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < RSX_DSP_NUM_TEXTURES; unit++) {
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx, unit, &texture);
        if (texture.enabled && texture.cubemap)
            cube_mask |= 1u << unit;
    }
    const u32 vtex_mask = vertex_texture_mask();
    const u32 txl_mask = vpIdentity.txlMask;
    if (txl_mask && !(txl_mask & vtex_mask)) {
        g_ld_vtex_missing_for_txl++;
        static u32 warned = 0;
        if (warned++ < 32 && vertex_texture_trace_enabled()) {
            fprintf(stderr,
                    "[vtex] TXL shader has no supported binding txl=0x%X "
                    "bound=0x%X start=%u instrs=%u\n",
                    txl_mask, vtex_mask, start, vp_instrs);
            for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
                if (!((txl_mask >> u) & 1u)) continue;
                const u32 base = 0x0900 + u * 0x20;
                fprintf(stderr,
                        "[vtex] raw u%u off=%08X fmt=%08X wrap=%08X "
                        "ctl0=%08X ctl3=%08X filter=%08X rect=%08X\n",
                        u, rsx_dsp_reg(&g.rsx, base),
                        rsx_dsp_reg(&g.rsx, base + 4),
                        rsx_dsp_reg(&g.rsx, base + 8),
                        rsx_dsp_reg(&g.rsx, base + 12),
                        rsx_dsp_reg(&g.rsx, base + 16),
                        rsx_dsp_reg(&g.rsx, base + 20),
                        rsx_dsp_reg(&g.rsx, base + 24));
            }
        }
    }

    render_state_t rs;
    decode_render_state(&rs);
    if (g_ld_probe_variant == 1)
    {
        rs.depth_test = 0;
    }
    else if (g_ld_probe_variant == 2)
    {
        rs.alpha_test_enable = 0;
    }
    if (rsx_fp_collect_constants(
            fp_uc, fp_size, &g.fp_constants) < 0) {
        LD_PSO_BAIL("fragment program constant collection failed");
        return NULL;
    }
    g.fp_alpha_ref = rsx_fp_alpha_ref(
        rs.alpha_ref_raw, rs.alpha_ref_format);

    u64 key = vpIdentity.programHash;
    if (ld_fp_constants_buffered())
        key = rsx_fp_structural_hash(fp_uc, fp_size, key);
    else
        key = fnv1a(fp_uc, fp_size, key);
    if (!key) {
        LD_PSO_BAIL("shader hash collapsed to zero");
        return NULL;
    }
    const u32 fp_ctrl_key = fp_ctrl & 0x4Eu;   /* 32-bit exports, depth export */
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    key = fnv1a(&cube_mask, sizeof(cube_mask), key);
    key = fnv1a(&vtex_mask, sizeof(vtex_mask), key);
    /* Texel-addressed (RSX_TEX_FMT_UNNORM) units need their coordinates divided
     * by the texture size in the shader -- D3D12 has no unnormalised addressing
     * mode. The divisor is baked into the HLSL, so the sizes are part of the
     * PSO identity and must be in the key. */
    u32 unnorm_mask = 0;
    u32 unnorm_dim[16][2] = {{0}};
    for (u32 uu = 0; uu < 16; uu++) {
        rsx_dsp_texture ut; rsx_dsp_get_texture(&g.rsx, uu, &ut);
        if (!ut.enabled || !(ut.format & TEX_FMT_UNNORM)) continue;
        if (!ut.width || !ut.height) continue;
        unnorm_mask |= 1u << uu;
        unnorm_dim[uu][0] = ut.width;
        unnorm_dim[uu][1] = ut.height;
        /* A depth texture bound to a zeta's snapshot reads that host
         * resource, which is as large as the largest pass that used the zeta
         * (bounded by the zeta), not as large as the texture. Texel
         * coordinates address the resource, so divide by its size: a texture
         * smaller than the zeta then reads its top-left corner, as it does in
         * VRAM, instead of a stretched copy of the whole depth buffer. */
        if ((ut.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM) ==
                TEX_FMT_DEPTH24_D8) {
            for (u32 zi = 0; zi < g.n_zdepths; zi++) {
                const zdepth_t* z = &g.zdepths[zi];
                if (z->location != ut.location || z->offset != ut.offset)
                    continue;
                /* Only when the bind will take the snapshot (it has, or
                 * will copy, a depth-writing pass); a zeta that was only
                 * cleared is read from guest memory at texture size. */
                if (z->snapshot && (z->had_write || z->snapshot_valid) &&
                    z->snapshot_w >= ut.width &&
                    z->snapshot_h >= ut.height) {
                    unnorm_dim[uu][0] = z->snapshot_w;
                    unnorm_dim[uu][1] = z->snapshot_h;
                }
                break;
            }
        }
    }
    key = fnv1a(&unnorm_mask, sizeof(unnorm_mask), key);
    if (unnorm_mask) key = fnv1a(unnorm_dim, sizeof(unnorm_dim), key);
    /* Units filtered through the RSX's convolution kernel
     * (rsx_fp_apply_convolution): the kernel is applied in the shader, so
     * which units and which kernel are part of the PSO identity. */
    u32 convolution_mask = 0;
    u8 convolution_kernels[16] = {0};
    for (u32 cu = 0; cu < 16; cu++)
    {
        rsx_dsp_texture ct; rsx_dsp_get_texture(&g.rsx, cu, &ct);
        const u32 minFilter = (ct.filter >> 16) & 0x7u;
        const u32 magFilter = (ct.filter >> 24) & 0x7u;
        if (!ct.enabled || (minFilter != 7u && magFilter != 4u))
        {
            continue;
        }
        convolution_mask |= 1u << cu;
        convolution_kernels[cu] = (u8)((ct.filter >> 13) & 0x7u);
    }
    if (ld_convolution_enabled())
    {
        key = fnv1a(&convolution_mask, sizeof(convolution_mask), key);
        if (convolution_mask) key = fnv1a(convolution_kernels, sizeof(convolution_kernels), key);
    }
    else
    {
        convolution_mask = 0;
    }
    /* Units that read a zeta through A8R8G8B8 (ld_zeta_alias_index): their
     * Sample() calls are rewritten, so the set is part of the identity. */
    u32 zetaAliasMask = 0;
    for (u32 aliasUnit = 0; aliasUnit < 16; aliasUnit++)
    {
        rsx_dsp_texture aliasTexture;
        rsx_dsp_get_texture(&g.rsx, aliasUnit, &aliasTexture);
        if (ld_zeta_alias_index(&aliasTexture) >= 0)
        {
            zetaAliasMask |= 1u << aliasUnit;
        }
    }
    zetaAliasMask &= ~cube_mask;
    convolution_mask &= ~zetaAliasMask;
    if (zetaAliasMask)
    {
        static const u32 zetaAliasTag = 0x415A4C41u; /* "ALZA" */
        key = fnv1a(&zetaAliasTag, sizeof(zetaAliasTag), key);
        key = fnv1a(&zetaAliasMask, sizeof(zetaAliasMask), key);
    }
    if (masked_layout) {
        static const u32 masked_layout_tag = 0x314B534Du; /* "MSK1" */
        const u32 payload_stride =
            packed_payload ? masked_layout->stride : VERT_STRIDE;
        key = fnv1a(
            &masked_layout_tag, sizeof(masked_layout_tag), key);
        key = fnv1a(
            &masked_layout->mask, sizeof(masked_layout->mask), key);
        key = fnv1a(
            &payload_stride, sizeof(payload_stride), key);
        key = fnv1a(
            &packed_payload, sizeof(packed_payload), key);
    }
    if (ld_fp_constants_buffered())
        key = ld_hash_structural_render_state(&rs, key);
    else
        key = fnv1a(&rs, sizeof(rs), key);
    key = fnv1a(&rs.draw_scale, sizeof(rs.draw_scale), key);
    key = fnv1a(&rs.draw_scale_x_bits, sizeof(rs.draw_scale_x_bits), key);
#if defined(YZ_PERF_PROFILE)
    ld_profile_note_fp_variants(
        fp_uc, fp_size, fp_ctrl_key, cube_mask, &rs);
#endif
    g_ld_current_pso.valid = 1;
    g_ld_current_pso.key = key;
    g_ld_current_pso.vp_hash = vpIdentity.programHash;
    g_ld_current_pso.vp_start = start;
    g_ld_current_pso.vp_instrs = vp_instrs;
    g_ld_current_pso.fp_location = fp_loc;
    g_ld_current_pso.fp_offset = fp_off;
    g_ld_current_pso.fp_size = fp_size;
    g_ld_current_pso.fp_control = fp_ctrl;
    g_ld_current_pso.cube_mask = cube_mask;
    g_ld_current_pso.vtex_mask = vtex_mask;
    g_ld_current_pso.txl_mask = txl_mask;
    g_ld_current_pso.input_mask =
        masked_layout ? masked_layout->mask : 0xFFFFu;
    g_ld_current_pso.input_stride =
        masked_layout && packed_payload
            ? masked_layout->stride : VERT_STRIDE;
    g_ld_current_pso.packed_offsets =
        masked_layout && packed_payload;

#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.pso_lookups++;
#endif
    {
        const u32 found = ld_pso_index_find(key);
        if (found != UINT32_MAX) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.pso_hits++;
#endif
            g_ld_current_pso.ps_unit_mask = g.psos[found].ps_unit_mask;
            if (InterlockedCompareExchange(&g.psos[found].pending, 0, 0)) {
                InterlockedIncrement(&g_ld_pso_draws_skipped);
                return NULL;             /* still compiling (ld_pso_job) */
            }
            return g.psos[found].pso;
        }
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.pso_misses++;
#endif
    if (g.n_psos >= MAX_PSOS) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.pso_full++;
#endif
        LD_PSO_BAIL("PSO cache full (MAX_PSOS)");
        return NULL;
    }

    static char vs_hlsl[256 * 1024];
    static char ps_hlsl[256 * 1024];
    ID3D12PipelineState* pso = NULL;
#if defined(YZ_PERF_PROFILE)
    const LONGLONG decompile_begin = ld_profile_qpc();
#endif
    rsx_vp_set_branch_base(rsx_dsp_vp_start(&g.rsx));
    const int vi = masked_layout
        ? rsx_vp_decompile_compact_ex(
            vp_uc, vp_instrs * 16, vtex_mask, masked_layout->mask,
            vs_hlsl, sizeof(vs_hlsl))
        : rsx_vp_decompile_ex(
            vp_uc, vp_instrs * 16, vtex_mask, vs_hlsl, sizeof(vs_hlsl));
    u32 decompiled_constant_count = 0;
    int fi;
    if (ld_fp_constants_buffered()) {
        fi = rsx_fp_decompile_buffered_ex(
            fp_uc, fp_size, fp_ctrl, cube_mask, ps_hlsl,
            sizeof(ps_hlsl), &decompiled_constant_count);
        if (fi > 0 &&
            decompiled_constant_count != g.fp_constants.count)
            fi = -1;
        if (fi > 0 && rs.alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(
                ps_hlsl, sizeof(ps_hlsl), rs.alpha_func) < 0)
            fi = -1;
        if (fi > 0) {
            const int np = rsx_fp_apply_unnorm_scale(
                ps_hlsl, sizeof(ps_hlsl), unnorm_mask, unnorm_dim, cube_mask);
            { static int n = 0;
              if (n++ < 8)
                  fprintf(stderr, "[unnorm] mask=0x%X cube=0x%X dim0=%ux%u"
                                  " patched=%d\n",
                          unnorm_mask, cube_mask, unnorm_dim[0][0],
                          unnorm_dim[0][1], np); }
            if (np < 0) fi = -1;
        }
        if (fi > 0 && rsx_fp_apply_convolution(
                ps_hlsl, sizeof(ps_hlsl), convolution_mask,
                convolution_kernels, cube_mask) < 0)
            fi = -1;
    } else {
        fi = rsx_fp_decompile_ex(
            fp_uc, fp_size, fp_ctrl, cube_mask,
            ps_hlsl, sizeof(ps_hlsl));
        if (fi > 0 && rs.alpha_test_enable &&
            rsx_fp_apply_alpha_test(
                ps_hlsl, sizeof(ps_hlsl), rs.alpha_func,
                g.fp_alpha_ref) < 0)
            fi = -1;
        if (fi > 0 && rsx_fp_apply_unnorm_scale(
                ps_hlsl, sizeof(ps_hlsl), unnorm_mask,
                unnorm_dim, cube_mask) < 0)
            fi = -1;
        if (fi > 0 && rsx_fp_apply_convolution(
                ps_hlsl, sizeof(ps_hlsl), convolution_mask,
                convolution_kernels, cube_mask) < 0)
            fi = -1;
    }
    if (fi > 0 && zetaAliasMask)
    {
        const int aliasSites = rsx_fp_apply_zeta_alias(ps_hlsl, sizeof(ps_hlsl), zetaAliasMask, cube_mask);
        if (aliasSites < 0)
        {
            fi = -1;
        }
        else
        {
            static int aliasLogged = 0;
            if (aliasSites > 0 && aliasLogged < 8)
            {
                aliasLogged++;
                fprintf(stderr, "[zeta-alias] pso %016llx reads its depth through ARGB8 on units 0x%X (%d sites)\n",
                        (unsigned long long)live_legacy_pso_key(), zetaAliasMask, aliasSites);
            }
        }
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.decompile_qpc +=
        (u64)(ld_profile_qpc() - decompile_begin);
#endif
    /* LD_FORCE_GREEN=1: make every fragment program return solid green.
     *
     * The same trick as LD_CLEAR_TEST, applied one stage later. Clears are
     * visible on these surfaces and draws are not, while the engine reports
     * 18,452 groups executed with zero drops and the GPU state cannot discard
     * anything (viewport 0,0 720x512, scissor full, depth test off, colour mask
     * all channels, blend off, cull off). Two possibilities remain and they need
     * opposite fixes: the draws reach the GPU and the SAMPLED DATA is black, or
     * they never reach it and the accounting is wrong. Green separates them. */
    { static int fg = -1;
      /* LD_FORCE_GREEN=<pso key hex> limits it to one PSO. */
      static u64 gk = 0;
      if (fg < 0) { const char* e = getenv("LD_FORCE_GREEN"); fg = e ? 1 : 0;
                    gk = e ? _strtoui64(e, 0, 16) : 0; if (gk == 1) gk = 0; }
      if (fg && fi > 0 && (!gk || gk == live_legacy_pso_key())) {
          /* The emulated alpha test discards before the return, so replacing
           * only the return still showed nothing for a failing alpha. */
          for (char* d = strstr(ps_hlsl, "discard;"); d; d = strstr(d, "discard;"))
              memcpy(d, "        ", 8);
          char* r = strstr(ps_hlsl, "    return ");
          if (r) {
              char* semi = strchr(r, ';');
              if (semi) {
                  const char rep[] = "    return float4(0,1,0,1);";
                  const size_t rl = sizeof(rep) - 1;
                  const size_t old = (size_t)(semi + 1 - r);
                  const size_t used = strlen(ps_hlsl);
                  if (used - old + rl + 1 <= sizeof(ps_hlsl)) {
                      memmove(r + rl, semi + 1, used - (size_t)(semi + 1 - ps_hlsl) + 1);
                      memcpy(r, rep, rl);
                  }
              }
          }
      } }
    /* LD_FORCE_COL0=1: every vertex program outputs white COLOR0 -- separates
     * "the VS computes a wrong colour" from everything downstream of it. */
    /* LD_FORCE_COL0=<pso key hex> limits it to one PSO. */
    { static int fc = -1; static u64 fk = 0;
      if (fc < 0) { const char* e = getenv("LD_FORCE_COL0"); fc = e ? 1 : 0;
                    fk = e ? _strtoui64(e, 0, 16) : 0; if (fk == 1) fk = 0; }
      char* c0 = fc && vi > 0 && (!fk || fk == live_legacy_pso_key())
                 ? strstr(vs_hlsl, "Out.col0 = o[1];") : NULL;
      if (c0) memcpy(c0, "Out.col0=1+0*_p;", 16); }   /* same length, in place */
    /* LD_HLSL_DUMP=<dir>: files named by the PSO key the draw CSV prints. */
    { static const char* hd = (const char*)-1;
      if (hd == (const char*)-1) hd = getenv("LD_HLSL_DUMP");
      if (hd) {
        char fn[MAX_PATH]; FILE* f;
        snprintf(fn, sizeof fn, "%s/hlsl_%016llx.vs.txt", hd, (unsigned long long)live_legacy_pso_key());
        f = fopen(fn, "wb"); if (f) { fputs(vi > 0 ? vs_hlsl : "<vp fail>", f); fclose(f); }
        snprintf(fn, sizeof fn, "%s/hlsl_%016llx.ps.txt", hd, (unsigned long long)live_legacy_pso_key());
        f = fopen(fn, "wb"); if (f) { fputs(fi > 0 ? ps_hlsl : "<fp fail>", f); fclose(f); }
      } }
    /* After the dump, so the dump shows the text replacements are keyed by. */
    if (fi > 0)
    {
        ld_apply_ps_replacement(ps_hlsl, sizeof(ps_hlsl));
    }
    float drawScaleX = 1.0f;
    memcpy(&drawScaleX, &rs.draw_scale_x_bits, sizeof drawScaleX);
    if (fi > 0 && ld_scale_wpos(ps_hlsl, sizeof(ps_hlsl), rs.draw_scale, drawScaleX) < 0)
        fi = -1;
    const u32 entryIndex = g.n_psos;
    g.psos[entryIndex].key = key;
    g.psos[entryIndex].pso = NULL;
    g.psos[entryIndex].pending = 0;
    g.psos[entryIndex].ps_unit_mask = rsx_fp_texture_units(fp_uc, fp_size);
    g_ld_current_pso.ps_unit_mask = g.psos[entryIndex].ps_unit_mask;
    if (vi > 0 && fi > 0) {
        if (ld_pso_job_submit(entryIndex, vs_hlsl, ps_hlsl, &rs, masked_layout,
                              packed_payload)) {
            InterlockedIncrement(&g_ld_pso_draws_skipped);
        } else {
            pso = build_pso(
                vs_hlsl, ps_hlsl, &rs, masked_layout, packed_payload);
            g.psos[entryIndex].pso = pso;
        }
    }
    g.n_psos++;
    ld_pso_index_insert(key, entryIndex);
    if (InterlockedCompareExchange(&g.psos[entryIndex].pending, 0, 0))
        return NULL;
    return g.psos[entryIndex].pso;
}

static int ld_upload_pixel_constants(u32* out_offset)
{
    if (out_offset)
        *out_offset = 0;
    if (!ld_fp_constants_buffered())
        return 1;
    if (!g.ps_cb || !g.ps_cb_mapped)
        return 0;

    /* HLSL uses a one-element placeholder array for programs with no inline
     * constants so fp_alpha retains a valid 16-byte slot. */
    const u32 value_slots =
        g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 data_bytes = (value_slots + 1u) * 16u;
    u32 offset = 0;
    u32 allocation_bytes = 0;
    int plan = rsx_fp_constant_ring_plan(
        g.ps_cb_used, ld_ps_cb_end(), data_bytes,
        &offset, &allocation_bytes);
    if (plan < 0) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_constant_capacity_failures++;
#endif
        return 0;
    }
    if (plan == 0) {
        ld_flush(LD_FLUSH_PIXEL_CONSTANT_RING);
        if (!g.ready)
            return 0;
        g.ps_cb_used = ld_ps_cb_base();
        plan = rsx_fp_constant_ring_plan(
            g.ps_cb_used, ld_ps_cb_end(), data_bytes,
            &offset, &allocation_bytes);
        if (plan != 1) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_constant_capacity_failures++;
#endif
            return 0;
        }
    }

#if defined(YZ_PERF_PROFILE)
    const LONGLONG upload_begin = ld_profile_qpc();
#endif
    u8* destination = g.ps_cb_mapped + offset;
    memset(destination, 0, data_bytes);
    if (g.fp_constants.count)
        memcpy(
            destination, g.fp_constants.values,
            (size_t)g.fp_constants.count * 16u);
    memcpy(
        destination + value_slots * 16u,
        &g.fp_alpha_ref, sizeof(g.fp_alpha_ref));
    g.ps_cb_used += allocation_bytes;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_constant_allocations++;
    g_ld_profile.total.ps_constant_upload_bytes += data_bytes;
    g_ld_profile.total.ps_constant_upload_qpc +=
        (u64)(ld_profile_qpc() - upload_begin);
#endif
    if (out_offset)
        *out_offset = offset;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Draw accumulation sink (mirrors the harness sink)
 * -----------------------------------------------------------------------*/
typedef struct { float a[16][4]; } vtx_t;
typedef struct { u32 first, count; } batch_t;
#define LD_MAX_BATCHES 4096u

typedef struct {
    /* INLINE_ARRAY source for this primitive, or NULL. The stream is the
     * vertices themselves, in guest byte order, so the fetch path reads it
     * with the same big-endian decoders it uses for a vertex array -- only
     * the base pointer differs. inl_off[i] is attribute i's byte offset
     * within one inline vertex; the attributes are packed in ascending
     * index order, which is what makes the offsets derivable at all (an
     * inline stream carries no per-array offset registers). */
    const u8* inl; u32 inl_bytes, inl_stride; u32 inl_off[16];
    /* One BEGIN/END can carry far more than 256 packets: each DRAW_ARRAYS /
     * DRAW_INDEX_ARRAY word covers at most 256 vertices, so a 30k-triangle
     * mesh arrives as ~350 of them. Contiguous packets are merged on arrival
     * (dc_queue_batch), and the queue is sized for meshes that are not. */
    batch_t arr[LD_MAX_BATCHES]; u32 n_arr;
    batch_t idx[LD_MAX_BATCHES]; u32 n_idx;
    u32     n_packets;
    vtx_t*  verts; u32 n_verts, cap_verts;
    rsx_vertex_ref* refs; u32 n_refs, cap_refs;
    u32     n_source_refs;
    int     refs_remapped;
    rsx_vertex_remap ref_remap;
    u8* compact_verts; u64 compact_capacity;
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int     fetch_ok;
    /* Primitive-restart cut points (s25 port of the replay-harness fix):
     * a guest index equal to the RSX restart sentinel is a cut marker, not
     * a vertex reference (RPCS3 RSXThread.cpp:398); fetch_batches records
     * the n_verts position at each one and the STRIP/FAN expansion must
     * not bridge across a cut. */
    u32*    cuts; u32 n_cuts, cap_cuts;
} draw_ctx;

static draw_ctx dc;

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_verts = dc.n_refs = 0;
    dc.n_source_refs = 0;
    dc.refs_remapped = 0;
    dc.n_packets = 0;
    dc.n_cuts = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL; dc.inl_bytes = dc.inl_stride = 0;
}
static void push_vert(const vtx_t* v)
{
    if (dc.n_verts >= dc.cap_verts) {
        u32 nc = dc.cap_verts ? dc.cap_verts * 2 : 4096;
        vtx_t* nv = (vtx_t*)realloc(dc.verts, (size_t)nc * sizeof(vtx_t));
        if (!nv) { dc.fetch_ok = 0; return; }
        dc.verts = nv; dc.cap_verts = nc;
    }
    dc.verts[dc.n_verts++] = *v;
}

static void push_compact_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next_capacity = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* refs = (rsx_vertex_ref*)realloc(
            dc.refs, (size_t)next_capacity * sizeof(*refs));
        if (!refs) {
            dc.fetch_ok = 0;
            return;
        }
        dc.refs = refs;
        dc.cap_refs = next_capacity;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
}

static int reserve_compact_vertices(u64 bytes)
{
    if (bytes <= dc.compact_capacity)
        return 1;
    if (bytes > SIZE_MAX)
        return 0;
    u64 next_capacity = dc.compact_capacity ? dc.compact_capacity : 1u << 20;
    while (next_capacity < bytes) {
        if (next_capacity > SIZE_MAX / 2u)
            return 0;
        next_capacity *= 2u;
    }
    u8* vertices = (u8*)realloc(
        dc.compact_verts, (size_t)next_capacity);
    if (!vertices)
        return 0;
    dc.compact_verts = vertices;
    dc.compact_capacity = next_capacity;
    return 1;
}

static int reserve_legacy_vertices(u32 count)
{
    if (count <= dc.cap_verts)
        return 1;
    u32 next_capacity = dc.cap_verts ? dc.cap_verts : 4096u;
    while (next_capacity < count) {
        if (next_capacity > UINT32_MAX / 2u)
            return 0;
        next_capacity *= 2u;
    }
    vtx_t* vertices = (vtx_t*)realloc(
        dc.verts, (size_t)next_capacity * sizeof(*vertices));
    if (!vertices)
        return 0;
    dc.verts = vertices;
    dc.cap_verts = next_capacity;
    return 1;
}

static float ld_be_f32(const u8* p)
{
    const u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float ld_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static int fetch_attr(u32 i, u32 base, u32 vertex_id, u32 base_index,
                      float out[4])
{
    rsx_dsp_vertex_attr a;
    rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
    if (!a.type || !a.size) return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const u32 elem_size = rsx_vertex_attrib_size(a.type, a.size);
    const u32 stride = a.stride ? a.stride : elem_size;
    const u32 divider_mask = rsx_dsp_reg(&g.rsx, 0x1FC0);
    if (a.frequency >= 2)
        g_ld_divider_fetches++;
    const u32 source_element = rsx_vertex_element_index(
        vertex_id, base_index, a.frequency,
        (divider_mask >> i) & 1u);
    const u8* p;
    if (dc.inl) {
        /* Inline vertices ignore the array offset and the data base: the
         * stream IS the array, packed at the declared stride. */
        const u64 at = (u64)source_element * dc.inl_stride + dc.inl_off[i];
        if (at + elem_size > dc.inl_bytes) return 0;
        p = dc.inl + at;
    } else {
        p = guest_ptr(
            a.location, base + a.offset + source_element * stride, elem_size);
    }
    if (!p) return 0;
    for (u32 c = 0; c < a.size && c < 4; c++) {
        switch (a.type) {
        case RSX_VTX_TYPE_FLOAT:
            out[c] = ld_be_f32(p + c * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[c] = ld_be_f16(p + c * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[c] = p[c] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[c] = (float)p[c];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = v / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = (float)v;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                          ((u32)p[2] << 8) | (u32)p[3];
            s32 x = (s32)(w & 0x7FF);          if (x & 0x400) x -= 0x800;
            s32 y = (s32)((w >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
            s32 z = (s32)((w >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

static void fetch_one(u32 base, u32 vertex_id, u32 base_index)
{
    vtx_t v;
    for (u32 i = 0; i < 16; i++) {
        rsx_dsp_vertex_attr a;
        rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
        if (a.type && a.size &&
            fetch_attr(i, base, vertex_id, base_index, v.a[i])) continue;
        if (i == 0) { dc.fetch_ok = 0; return; }
        rsx_dsp_vertex_default(&g.rsx, i, v.a[i]);
        if (i == 3 && v.a[3][0] == 0 && v.a[3][1] == 0 && v.a[3][2] == 0 && v.a[3][3] == 1)
            v.a[3][0] = v.a[3][1] = v.a[3][2] = 1.0f;
    }
    push_vert(&v);
}

static void fetch_batches(void)
{
    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    for (u32 r = 0; r < dc.n_arr && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.arr[r].count && dc.fetch_ok; i++)
            fetch_one(base, dc.arr[r].first + i, 0);
    if (!dc.n_idx) return;
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia; rsx_dsp_get_index_array(&g.rsx, &ia);
    /* Restart sentinel handling, same rule as the replay harness (RPCS3
     * RSXThread.cpp:398 "if (value == restart) continue" + rsx_methods.h
     * restart_index_enabled()/restart_index()): record a cut, never fetch
     * the phantom vertex. Kill-switch RSX_NO_RESTART shared with the
     * harness for byte-exact A/B. */
    static int s_no_restart = -1;
    if (s_no_restart < 0) s_no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
    const int restart_en = !s_no_restart && rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
    const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
    for (u32 r = 0; r < dc.n_idx && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.idx[r].count && dc.fetch_ok; i++) {
            const u32 esz = ia.is_u32 ? 4 : 2;
            const u8* ip = guest_ptr(ia.location, ia.offset + (dc.idx[r].first + i) * esz, esz);
            if (!ip) { dc.fetch_ok = 0; return; }
            const u32 index = ia.is_u32
                ? (((u32)ip[0] << 24) | ((u32)ip[1] << 16) | ((u32)ip[2] << 8) | ip[3])
                : (u32)((ip[0] << 8) | ip[1]);
            if (restart_en && index == restart_val) {
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                          dc.n_verts)) {
                    dc.fetch_ok = 0;
                    return;
                }
                continue;
            }
            fetch_one(base, index, base_index);
        }
}

/* ---------------------------------------------------------------------------
 * Geometry cache (rsx_geometry_cache.h): converted draws kept across frames.
 *
 * fetch_batches_hoisted reads a draw's indices, de-duplicates them and
 * decodes every referenced vertex -- for every draw, every frame. At the pub
 * that was most of the present thread's 22 ms a frame. The cache key covers
 * what decides the output apart from the vertex bytes (layout, attribute
 * formats and addresses, defaults, draw ranges, index format, address and
 * contents); an entry is used only if the vertex bytes it was decoded from
 * still hash the same. LD_GEOM_CACHE=0 turns it off.
 * -----------------------------------------------------------------------*/
static u64 g_ld_geom_hits = 0, g_ld_geom_misses = 0, g_ld_geom_stale = 0, g_ld_geom_uncacheable = 0;
#define LD_GEOM_KEY_SEED 0x6765306D2D6B6579ull
#define LD_GEOM_CHECK_SEED 0x636865636B2D3033ull
#define LD_GEOM_SPAN_SEED 0x7370616E2D686173ull

static int ld_geom_cache_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char* text = getenv("LD_GEOM_CACHE");
        enabled = (text && *text == '0') ? 0 : 1;
    }
    return enabled;
}

static u64 ld_geom_mix(u64 hash, u64 value)
{
    return rsx_geom_hash_u64(hash, value);
}

/* The draw's key and check hash. 0 when it cannot be cached: an inline
 * draw, or an index range that is not one readable span. */
static int ld_geom_key(const rsx_vertex_layout_plan* layout, u64* outKey, u64* outCheck)
{
    if (dc.inl || !layout->stride || !g.guest_ptr)
    {
        return 0;
    }
    rsx_vertex_fetch_plan plan;
    rsx_vertex_fetch_plan_init(&plan, &g.rsx, layout,
                               (rsx_vertex_guest_ptr_fn)g.guest_ptr, g.guest_user);
    u64 describe = LD_GEOM_KEY_SEED;
    describe = ld_geom_mix(describe, ((u64)layout->mask << 32) | layout->stride);
    describe = ld_geom_mix(describe, layout->count);
    describe = ld_geom_mix(describe, ((u64)plan.base_offset << 32) | plan.divider_mask);
    describe = ld_geom_mix(describe, rsx_dsp_vertex_data_base_index(&g.rsx));
    for (u32 slot = 0; slot < layout->count; slot++)
    {
        const u32 attr = layout->attrs[slot];
        const rsx_dsp_vertex_attr* desc = &plan.attr[attr].desc;
        describe = ld_geom_mix(describe, ((u64)attr << 48) | ((u64)desc->type << 32) |
                                         ((u64)desc->size << 16) | desc->frequency);
        describe = ld_geom_mix(describe, ((u64)desc->stride << 32) | desc->location);
        describe = ld_geom_mix(describe, desc->offset);
        if (!desc->type || !desc->size)
        {
            u64 defaults[2];
            memcpy(defaults, plan.attr[attr].default_value, 16);
            describe = ld_geom_mix(describe, defaults[0]);
            describe = ld_geom_mix(describe, defaults[1]);
        }
    }
    describe = ld_geom_mix(describe, ((u64)dc.n_arr << 32) | dc.n_idx);
    for (u32 batch = 0; batch < dc.n_arr; batch++)
    {
        describe = ld_geom_mix(describe, ((u64)dc.arr[batch].first << 32) | dc.arr[batch].count);
    }
    u64 contents = 0;
    if (dc.n_idx)
    {
        rsx_dsp_index_array index_array;
        rsx_dsp_get_index_array(&g.rsx, &index_array);
        const u32 element_size = index_array.is_u32 ? 4u : 2u;
        static int no_restart = -1;
        if (no_restart < 0)
        {
            no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
        }
        const int restart_enabled =
            !no_restart && rsx_dsp_restart_index_enabled(&g.rsx, index_array.is_u32);
        describe = ld_geom_mix(describe, ((u64)index_array.offset << 32) |
                                         ((u64)index_array.location << 8) | index_array.is_u32);
        describe = ld_geom_mix(describe, ((u64)restart_enabled << 32) | rsx_dsp_restart_index(&g.rsx));
        for (u32 batch = 0; batch < dc.n_idx; batch++)
        {
            const u32 first = dc.idx[batch].first;
            const u32 count = dc.idx[batch].count;
            describe = ld_geom_mix(describe, ((u64)first << 32) | count);
            if (!count)
            {
                continue;
            }
            const u64 start = (u64)index_array.offset + (u64)first * element_size;
            const u64 bytes = (u64)count * element_size;
            if (start + bytes > 0x100000000ull)
            {
                return 0;
            }
            const u8* indices = g.guest_ptr(g.guest_user, index_array.location, (u32)start, (u32)bytes);
            if (!indices)
            {
                return 0;
            }
            contents = rsx_geom_hash(indices, (size_t)bytes, contents ^ LD_GEOM_KEY_SEED);
        }
    }
    *outKey = ld_geom_mix(describe, contents);
    *outCheck = ld_geom_mix(ld_geom_mix(LD_GEOM_CHECK_SEED, describe ^ 0x5A5A5A5A5A5A5A5Aull), contents);
    return 1;
}

/* Restore a cached draw into dc. 1 on a hit. */
static int ld_geom_restore(const rsx_vertex_layout_plan* layout, u64 key, u64 check)
{
    rsx_geom_entry* entry = rsx_geom_cache_find(key, check, g_ld_frames);
    if (!entry)
    {
        return 0;
    }
    if (entry->stride != layout->stride)
    {
        rsx_geom_cache_drop(entry);
        return 0;
    }
    for (u32 span = 0; span < entry->n_spans; span++)
    {
        const rsx_geom_span* source = &entry->spans[span];
        const u8* bytes = g.guest_ptr(g.guest_user, source->location, source->offset, source->bytes);
        if (!bytes || rsx_geom_hash(bytes, source->bytes, LD_GEOM_SPAN_SEED) != source->hash)
        {
            g_ld_geom_stale++;
            rsx_geom_cache_drop(entry);
            return 0;
        }
    }

    if (entry->n_refs > dc.cap_refs)
    {
        rsx_vertex_ref* refs = (rsx_vertex_ref*)realloc(dc.refs, (size_t)entry->n_refs * sizeof(*refs));
        if (!refs)
        {
            return 0;
        }
        dc.refs = refs;
        dc.cap_refs = entry->n_refs;
    }
    if (entry->refs_remapped && entry->n_source_refs > dc.ref_remap.occurrence_capacity)
    {
        u32* occurrences = (u32*)realloc(dc.ref_remap.occurrence_to_unique,
                                         (size_t)entry->n_source_refs * sizeof(u32));
        if (!occurrences)
        {
            return 0;
        }
        dc.ref_remap.occurrence_to_unique = occurrences;
        dc.ref_remap.occurrence_capacity = entry->n_source_refs;
    }
    if (entry->n_cuts > dc.cap_cuts)
    {
        u32* cuts = (u32*)realloc(dc.cuts, (size_t)entry->n_cuts * sizeof(u32));
        if (!cuts)
        {
            return 0;
        }
        dc.cuts = cuts;
        dc.cap_cuts = entry->n_cuts;
    }
    if (!reserve_compact_vertices((u64)entry->n_refs * entry->stride))
    {
        return 0;
    }

    memcpy(dc.refs, entry->refs, (size_t)entry->n_refs * sizeof(*dc.refs));
    dc.n_refs = entry->n_refs;
    dc.n_source_refs = entry->n_source_refs;
    dc.refs_remapped = entry->refs_remapped;
    if (entry->refs_remapped)
    {
        memcpy(dc.ref_remap.occurrence_to_unique, entry->occurrence,
               (size_t)entry->n_source_refs * sizeof(u32));
    }
    if (entry->n_cuts)
    {
        memcpy(dc.cuts, entry->cuts, (size_t)entry->n_cuts * sizeof(u32));
    }
    dc.n_cuts = entry->n_cuts;
    memcpy(dc.compact_verts, entry->vertices, (size_t)entry->n_refs * entry->stride);
    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout,
                               (rsx_vertex_guest_ptr_fn)g.guest_ptr, g.guest_user);
    dc.n_verts = dc.n_refs;
    return 1;
}

/* After a successful conversion: remember it, with the vertex spans it was
 * decoded from. */
static void ld_geom_store(const rsx_vertex_layout_plan* layout, u64 key, u64 check)
{
    rsx_geom_span spans[RSX_GEOM_MAX_SPANS];
    u32 n_spans = 0;
    for (u32 slot = 0; slot < layout->count; slot++)
    {
        const u32 attr = layout->attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &dc.fetch_plan.attr[attr];
        if (!fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
        {
            continue;
        }
        if (!fetch->stable_base)
        {
            /* Read vertex by vertex: no single span to check. */
            g_ld_geom_uncacheable++;
            return;
        }
        const u64 start = (u64)dc.fetch_plan.base_offset + fetch->desc.offset +
                          (u64)fetch->stable_first * fetch->stride;
        const u64 end = (u64)dc.fetch_plan.base_offset + fetch->desc.offset +
                        (u64)fetch->stable_last * fetch->stride + fetch->elem_size;
        if (end <= start || end > 0x100000000ull)
        {
            g_ld_geom_uncacheable++;
            return;
        }
        /* Interleaved attributes share one buffer: merge overlapping or
         * touching ranges in the same location. */
        int merged = 0;
        for (u32 index = 0; index < n_spans && !merged; index++)
        {
            rsx_geom_span* span = &spans[index];
            const u64 spanStart = span->offset;
            const u64 spanEnd = spanStart + span->bytes;
            if (span->location == fetch->desc.location && start <= spanEnd && end >= spanStart)
            {
                const u64 unionStart = start < spanStart ? start : spanStart;
                const u64 unionEnd = end > spanEnd ? end : spanEnd;
                span->offset = (u32)unionStart;
                span->bytes = (u32)(unionEnd - unionStart);
                merged = 1;
            }
        }
        if (!merged)
        {
            if (n_spans >= RSX_GEOM_MAX_SPANS)
            {
                g_ld_geom_uncacheable++;
                return;
            }
            spans[n_spans].location = fetch->desc.location;
            spans[n_spans].offset = (u32)start;
            spans[n_spans].bytes = (u32)(end - start);
            n_spans++;
        }
    }
    for (u32 index = 0; index < n_spans; index++)
    {
        const u8* bytes = g.guest_ptr(g.guest_user, spans[index].location, spans[index].offset, spans[index].bytes);
        if (!bytes)
        {
            g_ld_geom_uncacheable++;
            return;
        }
        spans[index].hash = rsx_geom_hash(bytes, spans[index].bytes, LD_GEOM_SPAN_SEED);
    }
    rsx_geom_cache_store(key, check, g_ld_frames, spans, n_spans,
                         dc.refs, dc.n_refs, dc.n_source_refs, dc.refs_remapped,
                         dc.ref_remap.occurrence_to_unique, dc.cuts, dc.n_cuts,
                         dc.compact_verts, layout->stride);
}

static void fetch_batches_hoisted(
    const rsx_vertex_layout_plan* layout, int packed_payload)
{
    u64 geomKey = 0, geomCheck = 0;
    int geomKeyed = 0;
    if (packed_payload && ld_geom_cache_enabled())
    {
        geomKeyed = ld_geom_key(layout, &geomKey, &geomCheck);
        if (!geomKeyed)
        {
            g_ld_geom_uncacheable++;
        }
        else if (ld_geom_restore(layout, geomKey, geomCheck))
        {
            g_ld_geom_hits++;
            return;
        }
        else
        {
            g_ld_geom_misses++;
        }
    }
    for (u32 batch = 0; batch < dc.n_arr && dc.fetch_ok; batch++)
        for (u32 i = 0; i < dc.arr[batch].count && dc.fetch_ok; i++)
            push_compact_ref(dc.arr[batch].first + i, 0);

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array index_array;
        rsx_dsp_get_index_array(&g.rsx, &index_array);
        const u32 element_size = index_array.is_u32 ? 4u : 2u;
        static int no_restart = -1;
        if (no_restart < 0)
            no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
        const int restart_enabled =
            !no_restart &&
            rsx_dsp_restart_index_enabled(&g.rsx, index_array.is_u32);
        const u32 restart_value = rsx_dsp_restart_index(&g.rsx);

        for (u32 batch = 0; batch < dc.n_idx && dc.fetch_ok; batch++) {
            const u32 first = dc.idx[batch].first;
            const u32 count = dc.idx[batch].count;
            const u64 start64 =
                (u64)index_array.offset + (u64)first * element_size;
            const u64 bytes64 = (u64)count * element_size;
            const u8* contiguous = NULL;
            if (count && start64 <= UINT32_MAX &&
                bytes64 <= UINT32_MAX &&
                start64 + bytes64 <= 0x100000000ull) {
                contiguous = guest_ptr(
                    index_array.location, (u32)start64, (u32)bytes64);
            }
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u32 source_offset =
                    index_array.offset + (first + i) * element_size;
                const u8* source = contiguous
                    ? contiguous + (size_t)i * element_size
                    : guest_ptr(
                        index_array.location, source_offset, element_size);
                if (!source) {
                    dc.fetch_ok = 0;
                    break;
                }
                const u32 index = index_array.is_u32
                    ? (((u32)source[0] << 24) |
                       ((u32)source[1] << 16) |
                       ((u32)source[2] << 8) | source[3])
                    : (u32)((source[0] << 8) | source[1]);
                if (restart_enabled && index == restart_value) {
                    if (!rsx_restart_cut_push(
                            &dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                            dc.n_refs)) {
                        dc.fetch_ok = 0;
                        break;
                    }
                    continue;
                }
                push_compact_ref(index, base_index);
            }
        }
    }

    if (!dc.fetch_ok)
        return;
    dc.n_source_refs = dc.n_refs;
    if (packed_payload && dc.n_refs > 1) {
        u32 unique_refs = dc.n_refs;
        if (rsx_vertex_remap_build(
                &dc.ref_remap, dc.refs, dc.n_refs, &unique_refs)) {
            dc.refs_remapped = unique_refs < dc.n_refs;
            dc.n_refs = unique_refs;
        }
    }
    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(
        &dc.fetch_plan, &g.rsx, layout,
        (rsx_vertex_guest_ptr_fn)g.guest_ptr, g.guest_user);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    for (u32 slot = 0; slot < layout->count; slot++) {
        const u32 attr = layout->attrs[slot];
        if (dc.fetch_plan.attr[attr].desc.frequency >= 2)
            g_ld_divider_fetches += dc.n_refs;
    }
    if (packed_payload) {
        const u64 required = (u64)dc.n_refs * layout->stride;
        if (!reserve_compact_vertices(required)) {
            dc.fetch_ok = 0;
            return;
        }
        if (!rsx_vertex_fetch_many(
                &dc.fetch_plan, dc.refs, dc.n_refs,
                layout->stride ? dc.compact_verts : NULL, layout->stride)) {
            dc.fetch_ok = 0;
            return;
        }
    } else {
        /*
         * H and M deliberately retain the complete legacy payload.  With an
         * all-attribute layout, slot order is register order, so the helper
         * writes exactly the original attribute*16 byte offsets.
         */
        if (!reserve_legacy_vertices(dc.n_refs)) {
            dc.fetch_ok = 0;
            return;
        }
        /* Legacy treats an absent ATTR0 as a failed group, even though other
         * absent attributes use their RSX current/default value. */
        if (dc.n_refs &&
            (!dc.fetch_plan.attr[0].desc.type ||
             !dc.fetch_plan.attr[0].desc.size)) {
            dc.fetch_ok = 0;
            return;
        }
        if (dc.n_refs &&
            !rsx_vertex_fetch_many(
                &dc.fetch_plan, dc.refs, dc.n_refs,
                (u8*)&dc.verts[0], (u32)sizeof(dc.verts[0]))) {
            dc.fetch_ok = 0;
            return;
        }
    }
    dc.n_verts = dc.n_refs;
    if (geomKeyed && dc.fetch_ok && dc.n_refs)
    {
        ld_geom_store(layout, geomKey, geomCheck);
    }
}

static const float* dc_vertex_attr(
    u32 vertex, u32 attr, float fallback[4])
{
    if (!ld_vertex_compact_payload())
        return dc.verts[vertex].a[attr];
    const s8 slot = dc.layout.slot_by_attr[attr];
    if (slot >= 0 && vertex < dc.n_verts)
        return (const float*)(
            dc.compact_verts + (u64)vertex * dc.layout.stride +
            (u32)slot * 16u);
    memcpy(fallback, dc.fetch_plan.attr[attr].default_value, 16);
    return fallback;
}

/* Diagnostic-only counterpart to dc_vertex_attr().  Compact rendering omits
 * attributes the active vertex program does not read, but the legacy replay
 * fingerprint contains all sixteen RSX attributes.  Re-fetch an omitted,
 * enabled attribute from the original guest reference so a CSV hash compares
 * the producer bytes rather than the two renderers' different upload layouts. */
static const float* dc_vertex_attr_canonical(
    u32 vertex, u32 attr, float fallback[4])
{
    if (!ld_vertex_compact_payload())
        return dc.verts[vertex].a[attr];
    const s8 slot = dc.layout.slot_by_attr[attr];
    if (slot >= 0 && vertex < dc.n_verts)
        return (const float*)(
            dc.compact_verts + (u64)vertex * dc.layout.stride +
            (u32)slot * 16u);

    const rsx_vertex_fetch_attr* fetch = &dc.fetch_plan.attr[attr];
    if (vertex < dc.n_verts && fetch->desc.type && fetch->desc.size &&
        fetch->elem_size) {
        const rsx_vertex_ref* ref = &dc.refs[vertex];
        const u32 element = rsx_vertex_element_index(
            ref->vertex_id, ref->base_index, fetch->desc.frequency,
            (dc.fetch_plan.divider_mask >> attr) & 1u);
        const u64 offset =
            (u64)dc.fetch_plan.base_offset + fetch->desc.offset +
            (u64)element * fetch->stride;
        if (offset <= UINT32_MAX) {
            const u8* source = guest_ptr(
                fetch->desc.location, (u32)offset, fetch->elem_size);
            if (source && rsx_vertex_decode_element(
                    fetch->desc.type, fetch->desc.size,
                    source, fallback))
                return fallback;
        }
    }
    memcpy(fallback, fetch->default_value, 16);
    return fallback;
}

/* primitive ids = raw NV4097 VERTEX_BEGIN_END arg (rsx_dispatch stores it raw;
 * matches the replay harness). These were off by one (4/5/6/7), which dropped
 * EVERY quad/triangle draw through the switch's default: return -> black. */
#define PRIM_TRIANGLES       5
#define PRIM_TRIANGLE_STRIP  6
#define PRIM_TRIANGLE_FAN    7
#define PRIM_QUADS           8

/* Live-draw activity counters (verification: is real geometry flowing, or only
 * clears?). Reported per presented frame in rsx_live_draw_present. */
/* DRAW_ARRAYS/DRAW_INDEX_ARRAY writes are packets. Multiple packets between a
 * single BEGIN/END are deliberately coalesced into one D3D12 DrawInstanced,
 * so comparing packet count directly with executed D3D draws was invalid.
 * Keep two independently balanced ledgers instead:
 *
 *   packets_seen = packets_queued + packets_movie + packets_queue_full
 *   groups_seen  = groups_executed + every group_drop_* outcome
 *
 * An END with no accepted packet is counted separately as groups_empty and is
 * not a render attempt. This makes every early return explicit. */
typedef struct ld_stats {
    unsigned long long packets_seen;
    unsigned long long packets_queued;
    unsigned long long packets_movie;
    unsigned long long packets_queue_full;
    unsigned long long groups_seen;
    unsigned long long groups_empty;
    unsigned long long groups_executed;
    unsigned long long group_drop_fetch;
    unsigned long long group_drop_degenerate;
    unsigned long long group_drop_primitive;
    unsigned long long group_drop_alloc;
    unsigned long long group_drop_pso;
    unsigned long long group_drop_ring;
    unsigned long long group_drop_surface;
    unsigned long long clears;
    unsigned long long clear_drop_surface;
    unsigned long long implicit_depth_clears;
} ld_stats;

static ld_stats g_ld_stats;

void rsx_live_draw_a010_probe_begin(void)
{
    if ((!getenv("RSX_A010_PROBE") &&
         !getenv("RSX_A010_SURFACE_DUMP")) || !g.ready)
        return;
    /*
     * Targeted surface readbacks submit and fence the open D3D12 list. When
     * surface capture alone is requested, defer that cost until AUTH confirms
     * that both the character palette and verified camera are synchronized.
     * Explicit RSX_A010_PROBE retains its scene-open diagnostic behavior.
     */
    if (!getenv("RSX_A010_PROBE") &&
        InterlockedCompareExchange(
            &g_yz_a010_reference_camera_active, 0, 0) == 0)
        return;
    if (g_yz_runtime_config.a010_start_reference) {
        /* Deterministic visual-verification camera derived from the shipped
         * cam_Came_000_002.cmt frame 612. Install it on the RSX thread as soon
         * as the a010 probe opens so a later blocked PPU scene update cannot
         * prevent the reference matrix from reaching the first world draws. */
        static const float reference_matrix_612[16] = {
             3.18890834f,   -0.000270011135f,  0.08452246f,   5.81511511f,
            -0.0102274104f,  5.65674844f,      0.403935942f, -18.5085361f,
             0.0264371686f,  0.0712562195f,   -0.997207931f,  9.99292802f,
             0.0264345249f,  0.0712490938f,   -0.99710821f,  10.0919287f,
        };
        static const float reference_matrix_874[16] = {
             6.80647601f,   -0.00327668415f,  0.611324803f, -36.9135075f,
             0.00523179535f, 12.1491076f,     0.00686819648f, -38.8146474f,
             0.0894642733f,  0.000524588748f,-0.996090306f, -11.7467763f,
             0.0894553268f,  0.000524536289f,-0.995990697f, -11.6456016f,
        };
        const float* reference_matrix =
            g_yz_runtime_config.a010_reference_874
                ? reference_matrix_874 : reference_matrix_612;
        rsx_live_draw_set_a010_camera_matrix(reference_matrix);
    }
    CreateDirectoryA("scratch\\a010_probe", NULL);
    g_ld_a010_probe_start_frame = g_ld_frames;
    g_ld_a010_probe_sample = 0;
    g_ld_a010_probe_touched = 0;
    InterlockedExchange(&g_ld_a010_world_ready, 0);
    InterlockedExchange(&g_ld_a010_probe_active, 1);
    fprintf(stderr,
            "[a010-probe] BEGIN live_frame=%u surfaces=%u packets=%llu groups=%llu\n",
            g_ld_frames, g.n_surfaces, g_ld_stats.packets_seen,
            g_ld_stats.groups_seen);
    fflush(stderr);
}

int rsx_live_draw_a010_probe_active(void)
{
    return InterlockedCompareExchange(&g_ld_a010_probe_active, 0, 0) != 0;
}

int rsx_live_draw_a010_world_ready(void)
{
    return InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0;
}

static unsigned long long ld_groups_accounted(void)
{
    return g_ld_stats.groups_executed + g_ld_stats.group_drop_fetch +
           g_ld_stats.group_drop_degenerate + g_ld_stats.group_drop_primitive +
           g_ld_stats.group_drop_alloc + g_ld_stats.group_drop_pso +
           g_ld_stats.group_drop_ring + g_ld_stats.group_drop_surface;
}

#if defined(YZ_PERF_PROFILE)
static double ld_profile_ticks_ms(u64 ticks)
{
    return g_ld_profile.qpc_frequency
        ? (double)ticks * 1000.0 / (double)g_ld_profile.qpc_frequency
        : 0.0;
}

static void ld_profile_present(u32 frame)
{
    const LONGLONG now = ld_profile_qpc();
    const double frame_ms = ld_profile_ticks_ms(
        (u64)(now - g_ld_profile.previous_present_qpc));
    const ld_profile_counts* total = &g_ld_profile.total;
    const ld_profile_counts* previous = &g_ld_profile.previous;
#define LD_PROFILE_DELTA(field) (total->field - previous->field)
    const u64 packets =
        g_ld_stats.packets_seen - g_ld_profile.previous_packets;
    const u64 groups =
        g_ld_stats.groups_seen - g_ld_profile.previous_groups;
    const u64 executed =
        g_ld_stats.groups_executed - g_ld_profile.previous_executed;
    const u64 evictions =
        g_ld_texture_cache_evictions - g_ld_profile.previous_evictions;
    const u64 refreshes =
        g_ld_vtex_refreshes - g_ld_profile.previous_refreshes;
    const u64 used_attribute_draws =
        LD_PROFILE_DELTA(used_attribute_draws);
    const double used_attribute_average = used_attribute_draws
        ? (double)LD_PROFILE_DELTA(used_attribute_sum) /
              (double)used_attribute_draws
        : 0.0;
    const int compile_free =
        LD_PROFILE_DELTA(vs_compile_calls) == 0 &&
        LD_PROFILE_DELTA(ps_compile_calls) == 0;
    g_ld_profile.measured_frames++;
    g_ld_profile.total_frame_ms += frame_ms;
    if (compile_free) {
        g_ld_profile.compile_free_frames++;
        g_ld_profile.compile_free_frame_ms += frame_ms;
    }
    /*
     * The two matched visual-validation windows are deliberately small.
     * Emit one aggregate line per presented frame there so an improvement
     * below the generic 250 ms slow-frame threshold remains measurable.
     * Scene identity is still established by the run driver's asset marker
     * and screenshots; these values are only run-local capture locators.
     */
    const int matched_window =
        (frame >= 2240u && frame <= 2300u) ||
        (frame >= 4800u && frame <= 4900u);
    const int emit =
        frame <= 16u || matched_window || (frame & 63u) == 0u ||
        frame_ms >= 250.0 ||
        LD_PROFILE_DELTA(pso_misses) != 0 ||
        LD_PROFILE_DELTA(pso_full) != 0 ||
        LD_PROFILE_DELTA(flush_reason[LD_FLUSH_VERTEX_RING]) != 0 ||
        LD_PROFILE_DELTA(flush_reason[LD_FLUSH_RETIRE_QUEUE]) != 0 ||
        LD_PROFILE_DELTA(
            flush_reason[LD_FLUSH_PIXEL_CONSTANT_RING]) != 0 ||
        LD_PROFILE_DELTA(ps_constant_capacity_failures) != 0;

    if (emit) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory = {0};
        if (g_ld_profile.adapter)
            g_ld_profile.adapter->lpVtbl->QueryVideoMemoryInfo(
                g_ld_profile.adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &memory);
        fprintf(stderr,
                "[rsx-perf] frame=%u dt_ms=%.3f "
                "pso{look=%llu hit=%llu miss=%llu miss_total=%llu "
                "probes=%llu full=%llu cached=%u capacity=%u mem_kb=%.1f} "
                "reject{requests=%llu unique=%u "
                "world=%llu/%u character=%llu/%u "
                "ui=%llu/%u other=%llu/%u} "
                "hlsl{vs_lookup=%llu vs_hit=%llu vs_compile=%llu "
                "vs_new=%llu vs_unique=%u ps_lookup=%llu ps_hit=%llu "
                "ps_compile=%llu ps_new=%llu ps_unique=%u} "
                "time_ms{decompile=%.3f vs_compile=%.3f ps_compile=%.3f "
                "create_pso=%.3f flush=%.3f fence=%.3f draw_cpu=%.3f} "
                "vertex{path=%s used_avg=%.3f used_draws=%llu used_max=%u "
                "legacy_mb=%.3f actual_mb=%.3f fetch_pack_ms=%.3f} "
                "vb_sync{flushes=%llu flush_ms=%.3f wait_ms=%.3f} "
                "frame{compile_free=%u} "
                "draw{packets=%llu groups=%llu exec=%llu "
                "input_v=%llu expanded_v=%llu vb_mb=%.3f} "
                "flush{present=%llu ref=%llu vb=%llu retire=%llu "
                "movie=%llu movie_present=%llu readback=%llu} "
                "tex{cached=%u evict=%llu refresh=%llu upload_mb=%.3f "
                "decode=%llu decode_ms=%.3f upload_hi_mb=%.3f "
                "retired_hi=%u} "
                "rings{vb_hi_mb=%.3f upload_total_hi_mb=%.3f "
                "vb_total_hi_mb=%.3f retired_total_hi=%u} "
                "vram{usage_mb=%.1f budget_mb=%.1f reservation_mb=%.1f}\n",
                frame, frame_ms,
                (unsigned long long)LD_PROFILE_DELTA(pso_lookups),
                (unsigned long long)LD_PROFILE_DELTA(pso_hits),
                (unsigned long long)LD_PROFILE_DELTA(pso_misses),
                (unsigned long long)total->pso_misses,
                (unsigned long long)LD_PROFILE_DELTA(pso_probes),
                (unsigned long long)LD_PROFILE_DELTA(pso_full),
                g.n_psos,
                MAX_PSOS,
                (double)sizeof(g.psos) / 1024.0,
                (unsigned long long)total->pso_full,
                g_ld_profile.n_rejected_pso_keys,
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_WORLD],
                g_ld_profile.rejected_pso_unique[LD_REJECT_WORLD],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_CHARACTER],
                g_ld_profile.rejected_pso_unique[LD_REJECT_CHARACTER],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_UI],
                g_ld_profile.rejected_pso_unique[LD_REJECT_UI],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_OTHER],
                g_ld_profile.rejected_pso_unique[LD_REJECT_OTHER],
                (unsigned long long)LD_PROFILE_DELTA(vs_blob_lookups),
                (unsigned long long)LD_PROFILE_DELTA(vs_blob_hits),
                (unsigned long long)LD_PROFILE_DELTA(vs_compile_calls),
                (unsigned long long)LD_PROFILE_DELTA(vs_unique),
                g_ld_profile.n_vs_hashes,
                (unsigned long long)LD_PROFILE_DELTA(ps_blob_lookups),
                (unsigned long long)LD_PROFILE_DELTA(ps_blob_hits),
                (unsigned long long)LD_PROFILE_DELTA(ps_compile_calls),
                (unsigned long long)LD_PROFILE_DELTA(ps_unique),
                g_ld_profile.n_ps_hashes,
                ld_profile_ticks_ms(LD_PROFILE_DELTA(decompile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(vs_compile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(ps_compile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(create_pso_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(flush_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(fence_wait_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(sink_end_qpc)),
                ld_vertex_mode_name(),
                used_attribute_average,
                (unsigned long long)used_attribute_draws,
                g_ld_profile.frame_used_attribute_max,
                (double)LD_PROFILE_DELTA(legacy_vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                (double)LD_PROFILE_DELTA(vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(vertex_fetch_pack_qpc)),
                (unsigned long long)
                    LD_PROFILE_DELTA(
                        flush_reason[LD_FLUSH_VERTEX_RING]),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(
                        flush_reason_qpc[LD_FLUSH_VERTEX_RING])),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(
                        fence_reason_qpc[LD_FLUSH_VERTEX_RING])),
                compile_free ? 1u : 0u,
                (unsigned long long)packets,
                (unsigned long long)groups,
                (unsigned long long)executed,
                (unsigned long long)LD_PROFILE_DELTA(input_vertices),
                (unsigned long long)LD_PROFILE_DELTA(expanded_vertices),
                (double)LD_PROFILE_DELTA(vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_PRESENT]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_GUEST_REFERENCE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_VERTEX_RING]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_RETIRE_QUEUE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_MOVIE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_MOVIE_PRESENT]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_READBACK]),
                g.n_textures,
                (unsigned long long)evictions,
                (unsigned long long)refreshes,
                (double)LD_PROFILE_DELTA(texture_upload_bytes) /
                    (1024.0 * 1024.0),
                (unsigned long long)
                    LD_PROFILE_DELTA(texture_decode_calls),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(texture_decode_qpc)),
                (double)g_ld_profile.frame_upload_high /
                    (1024.0 * 1024.0),
                g_ld_profile.frame_retired_high,
                (double)g_ld_profile.frame_vb_high /
                    (1024.0 * 1024.0),
                (double)g_ld_profile.total_upload_high /
                    (1024.0 * 1024.0),
                (double)g_ld_profile.total_vb_high /
                    (1024.0 * 1024.0),
                g_ld_profile.total_retired_high,
                (double)memory.CurrentUsage / (1024.0 * 1024.0),
                (double)memory.Budget / (1024.0 * 1024.0),
                (double)memory.CurrentReservation / (1024.0 * 1024.0));
        fprintf(stderr,
                "[shader-cache-perf] frame=%u "
                "vs{count=%u capacity=%u lookups=%llu hits=%llu "
                "misses=%llu inserts=%llu full_rejects=%llu "
                "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
                "post2048_distinct=%llu post2048_repeats=%llu "
                "source_bytes=%llu blob_bytes=%llu} "
                "ps{count=%u capacity=%u lookups=%llu hits=%llu "
                "misses=%llu inserts=%llu full_rejects=%llu "
                "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
                "post2048_distinct=%llu post2048_repeats=%llu "
                "source_bytes=%llu blob_bytes=%llu}\n",
                frame,
                g.vs_blobs.count, MAX_SHADER_BLOBS,
                (unsigned long long)total->vs_blob_lookups,
                (unsigned long long)total->vs_blob_hits,
                (unsigned long long)total->vs_blob_misses,
                (unsigned long long)total->vs_blob_inserts,
                (unsigned long long)total->vs_blob_full_rejects,
                (unsigned long long)total->vs_compile_calls,
                ld_profile_ticks_ms(total->vs_blob_lookup_qpc),
                ld_profile_ticks_ms(total->vs_compile_qpc),
                (unsigned long long)total->vs_post_boundary_distinct,
                (unsigned long long)total->vs_post_boundary_repeats,
                (unsigned long long)g.vs_blobs.retained_source_bytes,
                (unsigned long long)g.vs_blobs.retained_blob_bytes,
                g.ps_blobs.count, MAX_SHADER_BLOBS,
                (unsigned long long)total->ps_blob_lookups,
                (unsigned long long)total->ps_blob_hits,
                (unsigned long long)total->ps_blob_misses,
                (unsigned long long)total->ps_blob_inserts,
                (unsigned long long)total->ps_blob_full_rejects,
                (unsigned long long)total->ps_compile_calls,
                ld_profile_ticks_ms(total->ps_blob_lookup_qpc),
                ld_profile_ticks_ms(total->ps_compile_qpc),
                (unsigned long long)total->ps_post_boundary_distinct,
                (unsigned long long)total->ps_post_boundary_repeats,
                 (unsigned long long)g.ps_blobs.retained_source_bytes,
                 (unsigned long long)g.ps_blobs.retained_blob_bytes);
        const u32 collapsed_by_constants =
            g_ld_profile.n_ps_constant_specialized_hashes >=
                    g_ld_profile.n_ps_canonical_hashes
                ? g_ld_profile.n_ps_constant_specialized_hashes -
                      g_ld_profile.n_ps_canonical_hashes
                : 0;
        const u32 collapsed_by_alpha =
            g_ld_profile.n_ps_exact_source_hashes >=
                    g_ld_profile.n_ps_constant_specialized_hashes
                ? g_ld_profile.n_ps_exact_source_hashes -
                      g_ld_profile.n_ps_constant_specialized_hashes
                : 0;
        fprintf(
            stderr,
            "[fp-constant-perf] frame=%u mode=%s "
            "variants{exact_source=%u constant_specialized=%u "
            "canonical=%u collapsed_constants=%u collapsed_alpha=%u "
            "tracking_overflow=%llu} "
            "cb{allocations=%llu upload_bytes=%llu upload_ms=%.3f "
            "ring_used=%u ring_capacity=%u capacity_failures=%llu "
            "flushes=%llu flush_ms=%.3f wait_ms=%.3f}\n",
            frame, ld_fp_constant_mode_name(),
            g_ld_profile.n_ps_exact_source_hashes,
            g_ld_profile.n_ps_constant_specialized_hashes,
            g_ld_profile.n_ps_canonical_hashes,
            collapsed_by_constants, collapsed_by_alpha,
            (unsigned long long)
                g_ld_profile.ps_variant_tracking_overflow,
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_allocations),
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_upload_bytes),
            ld_profile_ticks_ms(
                LD_PROFILE_DELTA(ps_constant_upload_qpc)),
            g.ps_cb_used, PS_CB_RING_BYTES,
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_capacity_failures),
            (unsigned long long)LD_PROFILE_DELTA(
                flush_reason[LD_FLUSH_PIXEL_CONSTANT_RING]),
            ld_profile_ticks_ms(LD_PROFILE_DELTA(
                flush_reason_qpc[LD_FLUSH_PIXEL_CONSTANT_RING])),
            ld_profile_ticks_ms(LD_PROFILE_DELTA(
                fence_reason_qpc[LD_FLUSH_PIXEL_CONSTANT_RING])));
        fflush(stderr);
    }

    g_ld_profile.previous = g_ld_profile.total;
    g_ld_profile.previous_packets = g_ld_stats.packets_seen;
    g_ld_profile.previous_groups = g_ld_stats.groups_seen;
    g_ld_profile.previous_executed = g_ld_stats.groups_executed;
    g_ld_profile.previous_evictions = g_ld_texture_cache_evictions;
    g_ld_profile.previous_refreshes = g_ld_vtex_refreshes;
    g_ld_profile.previous_present_qpc = now;
    g_ld_profile.frame_upload_high = 0;
    g_ld_profile.frame_vb_high = 0;
    g_ld_profile.frame_retired_high = 0;
    g_ld_profile.frame_used_attribute_max = 0;
#undef LD_PROFILE_DELTA
}
#endif

static int ld_target_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RSX_TARGET_TRACE") ? 1 : 0;
    return enabled;
}

static void ld_trace_target(const char* event, u32 target, u32 mask)
{
    if (!ld_target_trace_enabled()) return;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    fprintf(stderr,
            "[rsx-target] frame=%u event=%s target=%u loc=%u off=0x%08X "
            "fmt=0x%X pitch=%u clip=%u,%u+%ux%u mask=0x%02X\n",
            g_ld_frames, event, target, sf.color_location[0], sf.color_offset[0],
            sf.color_format, sf.color_pitch[0], sf.clip_x, sf.clip_y,
            sf.clip_w, sf.clip_h, mask);
}

/* Movie mode: while a host-decoded movie owns the window
 * (rsx_live_draw_present_rgba), do not make the default boot process the
 * guest's otherwise invisible RSX stream.  That experiment materially changes
 * CRI/movie handoff scheduling and regressed the proven title/menu path.
 * RSX_MOVIE_TRACK keeps the state-tracking experiment available for a
 * focused post-movie A/B once the transition itself is deterministic. */
static volatile int g_ld_movie_mode = 0;
static int g_ld_movie_track_rsx = -1;
static int g_ld_movie_composite_ui = -1;

#if defined(YZ_PERF_PROFILE)
static int ld_vp_has_indexed_constants(const u8* ucode, u32 instrs)
{
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16u + 12u;
        const u32 d3 = (u32)p[0] | ((u32)p[1] << 8) |
                       ((u32)p[2] << 16) | ((u32)p[3] << 24);
        if ((d3 >> 1) & 1u)
            return 1;
    }
    return 0;
}

static ld_reject_class ld_classify_rejected_pso(u32 target)
{
    const u32 start = g_ld_current_pso.vp_start;
    const u8* vp_uc = start < RSX_DSP_VP_INSTR
        ? (const u8*)(g.rsx.vp + start * 4)
        : NULL;
    if (vp_uc && ld_vp_has_indexed_constants(
            vp_uc, g_ld_current_pso.vp_instrs))
        return LD_REJECT_CHARACTER;

    render_state_t rs;
    decode_render_state(&rs);
    if (target < g.n_surfaces) {
        const surface_t* surface = &g.surfaces[target];
        int scanout = 0;
        for (u32 i = 0; i < 8; i++) {
            const display_buffer_t* display = &g.display_buffers[i];
            if (display->valid &&
                display->location == surface->location &&
                display->offset == surface->offset) {
                scanout = 1;
                break;
            }
        }
        if (scanout && !rs.depth_test && rs.blend_enable)
            return LD_REJECT_UI;
        if (rs.depth_test ||
            surface->offset == 0x00E40000u ||
            surface->offset == 0x01800000u)
            return LD_REJECT_WORLD;
    }
    return LD_REJECT_OTHER;
}

static void ld_profile_note_rejected_pso(void)
{
    if (!g_ld_current_pso.valid || g.n_psos < MAX_PSOS)
        return;

    rsx_dsp_surface rsx_surface;
    rsx_dsp_get_surface(&g.rsx, &rsx_surface);
    u32 target = LD_INVALID_SURFACE;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].location == rsx_surface.color_location[0] &&
            g.surfaces[i].offset == rsx_surface.color_offset[0]) {
            target = i;
            break;
        }
    }
    const ld_reject_class draw_class = ld_classify_rejected_pso(target);
    const u8 class_bit = (u8)(1u << (u32)draw_class);
    g_ld_profile.rejected_pso_requests[draw_class]++;

    u32 key_index = g_ld_profile.n_rejected_pso_keys;
    for (u32 i = 0; i < g_ld_profile.n_rejected_pso_keys; i++) {
        if (g_ld_profile.rejected_pso_keys[i] ==
            g_ld_current_pso.key) {
            key_index = i;
            break;
        }
    }
    if (key_index == g_ld_profile.n_rejected_pso_keys) {
        if (key_index >= MAX_REJECTED_PSO_KEYS)
            return;
        g_ld_profile.rejected_pso_keys[key_index] =
            g_ld_current_pso.key;
        g_ld_profile.rejected_pso_class_masks[key_index] = 0;
        g_ld_profile.n_rejected_pso_keys++;
    }
    if (!(g_ld_profile.rejected_pso_class_masks[key_index] & class_bit)) {
        g_ld_profile.rejected_pso_class_masks[key_index] |= class_bit;
        g_ld_profile.rejected_pso_unique[draw_class]++;
    }

    if (!g_ld_profile.first_pso_full_frame) {
        g_ld_profile.first_pso_full_frame = g_ld_frames;
        const u32 surface_offset = target < g.n_surfaces
            ? g.surfaces[target].offset
            : 0;
        fprintf(stderr,
                "[pso-capacity] reached count=%u capacity=%u "
                "movie=%d orphanage_world_ready=%d "
                "first_rejected_key=%016llx class=%u "
                "surface=0x%08X frame_locator=%u\n",
                g.n_psos, MAX_PSOS, g_ld_movie_mode,
                InterlockedCompareExchange(
                    &g_ld_a010_world_ready, 0, 0) != 0,
                (unsigned long long)g_ld_current_pso.key,
                (u32)draw_class, surface_offset, g_ld_frames);
        fflush(stderr);
    }
}
#endif

static int ld_movie_composite_ui_enabled(void)
{
    if (g_ld_movie_composite_ui < 0)
        g_ld_movie_composite_ui = getenv("RSX_MOVIE_COMPOSITE_UI") ? 1 : 0;
    return g_ld_movie_composite_ui;
}

static void ld_movie_reset_rings(void)
{
    ld_rings_reset();
    g.depth_cleared = 0;
}

static int ld_movie_overlay_ensure(void)
{
    if (g.movie_upload && g.movie_upload_mapped &&
        g.movie_overlay_readback && g.movie_overlay_rgba &&
        g.movie_overlay_mask)
        return 1;

    /* The readback holds the surface at its host (render-scaled) size. */
    g.movie_overlay_pitch = (ld_scaled_x(g.width) * 4 + 255) & ~255u;
    const UINT64 rb_size = (UINT64)g.movie_overlay_pitch * ld_scaled(g.height);
    D3D12_HEAP_PROPERTIES hp = {0};
    D3D12_RESOURCE_DESC bd = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = rb_size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_overlay_readback))) {
        fprintf(stderr, "[movie-ui] readback allocation failed\n");
        return 0;
    }
    g.movie_overlay_rgba = (u8*)malloc((size_t)g.width * g.height * 4);
    g.movie_overlay_mask = (u8*)malloc((size_t)g.width * g.height);
    if (!g.movie_overlay_rgba || !g.movie_overlay_mask) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] CPU overlay allocation failed\n");
        return 0;
    }

    /* Do not reuse the general guest upload ring for host frames. Guest draw
     * commands recorded between flips can still reference that memory; a
     * dedicated upload buffer lets host presentation append to the same
     * command list without a second fence/wait on every movie frame. */
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_upload))) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload allocation failed\n");
        return 0;
    }
    D3D12_RANGE no_read = {0, 0};
    if (FAILED(g.movie_upload->lpVtbl->Map(
            g.movie_upload, 0, &no_read, (void**)&g.movie_upload_mapped))) {
        g.movie_upload->lpVtbl->Release(g.movie_upload);
        g.movie_upload = NULL;
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload map failed\n");
        return 0;
    }
    return 1;
}

/* Start movies from a known transparent guest surface. This removes the stale
 * pre-movie frame before the game begins drawing captions, while leaving the
 * swap chain entirely owned by the 30 Hz host presenter. */
static void ld_movie_overlay_begin(void)
{
    g.movie_overlay_valid = 0;
    g.movie_overlay_frames = 0;
    if (!ld_movie_overlay_ensure()) return;

    ld_flush(LD_FLUSH_MOVIE);
    const float transparent[4] = {0, 0, 0, 0};
    for (u32 i = 0; i < g.n_surfaces; i++) {
        g.list->lpVtbl->ClearRenderTargetView(
            g.list, rtv_handle(LD_SWAP_BUFFERS + i), transparent, 0, NULL);
        ld_surface_note_write(i, LD_SURFACE_WRITE_OTHER);
    }
    if (g.n_surfaces) ld_flush(LD_FLUSH_MOVIE);
    ld_movie_reset_rings();
    fprintf(stderr, "[movie-ui] compositor armed (%ux%u, %u guest surfaces)\n",
            g.width, g.height, g.n_surfaces);
    fflush(stderr);
}

/* Capture the guest's latest offscreen result without presenting it. The
 * guest target is not a true transparent overlay: depending on the auth
 * sequence it can contain black, a fade, or a complete rendered scene.
 * Extract only low-saturation bright glyphs from the subtitle-safe lower band
 * and synthesize a small dark outline. The decoded movie therefore remains the
 * background even when the guest rendered an opaque full-screen image. */
static void ld_movie_capture_overlay(void)
{
    if (!ld_movie_overlay_ensure()) return;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        g.movie_overlay_valid = 0;
        return;
    }
    if (!ld_surface_is_canvas_scale(&g.surfaces[target])) {
        /* A post-effect-sized target (kept at 1x): not a subtitle overlay. */
        g.movie_overlay_valid = 0;
        return;
    }
    ID3D12Resource* rt = g.surfaces[target].tex;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g.movie_overlay_readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = ld_scaled_x(g.width);
    dst.PlacedFootprint.Footprint.Height = ld_scaled(g.height);
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g.movie_overlay_pitch;
    {
        const u32 boxWidth = ld_scaled_x(g.surfaces[target].w < g.width ? g.surfaces[target].w : g.width);
        const u32 boxHeight = ld_scaled(g.surfaces[target].h < g.height ? g.surfaces[target].h : g.height);
        D3D12_BOX overlayBox = {0, 0, 0, boxWidth, boxHeight, 1};
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, &overlayBox);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    ld_flush(LD_FLUSH_MOVIE);

    const SIZE_T rb_size = (SIZE_T)g.movie_overlay_pitch * ld_scaled(g.height);
    u8* mapped = NULL;
    D3D12_RANGE rr = {0, rb_size};
    if (FAILED(g.movie_overlay_readback->lpVtbl->Map(
            g.movie_overlay_readback, 0, &rr, (void**)&mapped))) {
        g.movie_overlay_valid = 0;
        return;
    }
    if (g.scale <= 1u && ld_canvas_scale_x() <= 1.0f)
    {
        for (u32 y = 0; y < g.height; y++)
            memcpy(g.movie_overlay_rgba + (size_t)y * g.width * 4,
                   mapped + (size_t)y * g.movie_overlay_pitch,
                   (size_t)g.width * 4);
    }
    else
    {
        /* Back to guest pixels: one host pixel per guest pixel. */
        for (u32 row = 0; row < g.height; row++)
        {
            const u8* sourceRow = mapped + (size_t)ld_scaled(row) * g.movie_overlay_pitch;
            u8* destinationRow = g.movie_overlay_rgba + (size_t)row * g.width * 4;
            for (u32 column = 0; column < g.width; column++)
            {
                memcpy(destinationRow + (size_t)column * 4,
                       sourceRow + (size_t)ld_scaled_x(column) * 4, 4);
            }
        }
    }
    D3D12_RANGE wr = {0, 0};
    g.movie_overlay_readback->lpVtbl->Unmap(
        g.movie_overlay_readback, 0, &wr);

    const u64 total = (u64)g.width * g.height;
    const u32 band_y0 = g.height * 52 / 100;
    const u32 band_y1 = g.height * 96 / 100;
    const u32 band_x0 = g.width * 5 / 100;
    const u32 band_x1 = g.width * 95 / 100;
    const u64 band_pixels =
        (u64)(band_y1 - band_y0) * (band_x1 - band_x0);
    u64 glyph_pixels = 0;
    memset(g.movie_overlay_mask, 0, (size_t)total);

    for (u32 y = band_y0; y < band_y1; y++) {
        for (u32 x = band_x0; x < band_x1; x++) {
            const u64 i = (u64)y * g.width + x;
            const u8* p = g.movie_overlay_rgba + i * 4;
            const int hi = p[0] > p[1]
                ? (p[0] > p[2] ? p[0] : p[2])
                : (p[1] > p[2] ? p[1] : p[2]);
            const int lo = p[0] < p[1]
                ? (p[0] < p[2] ? p[0] : p[2])
                : (p[1] < p[2] ? p[1] : p[2]);
            if (hi < 145 || hi - lo > 52)
                continue;
            int coverage = (hi - 96) * 255 / 159;
            if (coverage < 64) coverage = 64;
            if (coverage > 255) coverage = 255;
            g.movie_overlay_mask[i] = (u8)coverage;
            glyph_pixels++;
        }
    }

    /* A value of 1 denotes the synthetic outline; 64..255 are glyph
     * coverage. Work from a copy condition (>=64) so dilation does not grow
     * recursively. */
    if (glyph_pixels >= 8 && glyph_pixels < band_pixels / 6) {
        for (u32 y = band_y0; y < band_y1; y++) {
            for (u32 x = band_x0; x < band_x1; x++) {
                const u64 i = (u64)y * g.width + x;
                if (g.movie_overlay_mask[i] < 64)
                    continue;
                const u32 ya = y > 1 ? y - 2 : 0;
                const u32 yb = y + 2 < g.height ? y + 2 : g.height - 1;
                const u32 xa = x > 1 ? x - 2 : 0;
                const u32 xb = x + 2 < g.width ? x + 2 : g.width - 1;
                for (u32 oy = ya; oy <= yb; oy++)
                    for (u32 ox = xa; ox <= xb; ox++) {
                        u8* m = &g.movie_overlay_mask[(u64)oy * g.width + ox];
                        if (!*m) *m = 1;
                    }
            }
        }
    }
    g.movie_overlay_valid =
        glyph_pixels >= 8 && glyph_pixels < band_pixels / 6;
    g.movie_overlay_frames++;
    if (g.movie_overlay_frames <= 16 ||
        (g.movie_overlay_frames & 63) == 0 ||
        (!g.movie_overlay_valid && glyph_pixels)) {
        fprintf(stderr,
                "[movie-ui] overlay=%llu glyphs=%llu/%llu %s\n",
                (unsigned long long)g.movie_overlay_frames,
                (unsigned long long)glyph_pixels,
                (unsigned long long)band_pixels,
                g.movie_overlay_valid ? "accepted" : "rejected");
        fflush(stderr);
    }

    /* A guest flip still marks a new texture-generation boundary even though
     * the guest surface is not sent to the swap chain. */
    g_ld_frames++;
    ld_movie_reset_rings();
}

static void sink_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; (void)prim; dc_reset(); }

/* Queue one (first, count) packet. A packet that continues the previous one
 * (first == previous first + count) extends it instead: the primitive stream
 * is the concatenation of the packets either way, so this is exact, and it
 * keeps a large mesh -- hundreds of 256-vertex packets -- in one batch. Only a
 * full queue of non-contiguous packets drops one (counted as queue_full). */
static int dc_queue_batch(batch_t* batches, u32* batchCount, u32 first, u32 count)
{
    if (*batchCount > 0)
    {
        batch_t* previous = &batches[*batchCount - 1];
        if (previous->first + previous->count == first &&
            previous->count + count >= previous->count)
        {
            previous->count += count;
            return 1;
        }
    }
    if (*batchCount >= LD_MAX_BATCHES)
    {
        static int warned = 0;
        if (!warned)
        {
            warned = 1;
            fprintf(stderr, "[live-draw] more than %u separate draw packets in one primitive: "
                    "the rest are dropped\n", LD_MAX_BATCHES);
        }
        return 0;
    }
    batches[*batchCount].first = first;
    batches[*batchCount].count = count;
    (*batchCount)++;
    return 1;
}
static void sink_draw_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (!dc_queue_batch(dc.arr, &dc.n_arr, first, count)) { g_ld_stats.packets_queue_full++; return; }
    g_ld_stats.packets_queued++;
}
/* INLINE_ARRAY arrives as one finished stream instead of a (first, count)
 * range, so plan the layout here -- offsets and stride come from the VTXFMT
 * declaration -- and queue it as an ordinary consecutive batch. Everything
 * downstream (topology, textures, PSO, upload) is then identical to a draw
 * out of a vertex array, because by then it IS one. */
static void sink_inline_array(void* u, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)u; (void)r;
    g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }

    const u32 stride = rsx_vertex_inline_layout(&g.rsx, dc.inl_off);
    if (!stride || bytes < stride) { g_ld_stats.packets_queue_full++; return; }

    dc.inl = data; dc.inl_bytes = bytes; dc.inl_stride = stride;
    dc.n_packets++;
    if (dc.n_arr >= LD_MAX_BATCHES) { g_ld_stats.packets_queue_full++; return; }
    dc.arr[dc.n_arr].first = 0; dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
    g_ld_stats.packets_queued++;
}

static void sink_draw_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (!dc_queue_batch(dc.idx, &dc.n_idx, first, count)) { g_ld_stats.packets_queue_full++; return; }
    g_ld_stats.packets_queued++;
}

void rsx_live_draw_set_a010_camera_matrix(const float* matrix16)
{
    if (!matrix16) {
        InterlockedExchange(&g_ld_a010_camera_ready, 0);
        return;
    }
    memcpy(g_ld_a010_camera_bits, matrix16, sizeof(g_ld_a010_camera_bits));
    MemoryBarrier();
    InterlockedExchange(&g_ld_a010_camera_ready, 1);
}

/* Hash the same pre-cubemap/pre-VTF PSO inputs used by the reference replay's
 * RSX_DRAW_CSV.  Keeping this legacy key lets a live draw be matched directly
 * to the known-good a010 capture even though the current live PSO key contains
 * additional feature masks. */
static u64 live_legacy_pso_key(void)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR)
        return 0;
    ld_vp_identity vpIdentity;
    if (!ld_vp_identity_get(start, &vpIdentity))
    {
        return 0;
    }

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = guest_ptr(fp_loc, fp_off, 16);
    if (!fp_uc)
        return 0;
    const u32 fp_size = rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size)
        return 0;
    fp_uc = guest_ptr(fp_loc, fp_off, fp_size);
    if (!fp_uc)
        return 0;

    u64 key = vpIdentity.programHash;
    key = fnv1a(fp_uc, fp_size, key);
    const u32 fp_ctrl_key = rsx_dsp_shader_control(&g.rsx) & 0x4Eu;
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    render_state_t rs;
    decode_render_state(&rs);
    return fnv1a(&rs, sizeof(rs), key);
}

/* Hash the canonical sixteen-attribute vertex stream in source occurrence
 * order.  Compact mode may deduplicate the uploaded payload, so hashing its
 * packed bytes directly is not comparable with the legacy replay renderer.
 * Re-expanding through the remap and applying the RSX defaults produces the
 * same vtx_t byte stream on both paths and places the producer/consumer
 * boundary immediately after guest-memory vertex decoding. */
static u64 live_decoded_vertex_hash(u64 attr_hash[16])
{
    u64 hash = 1469598103934665603ull;
    for (u32 attr = 0; attr < 16; attr++)
        attr_hash[attr] = 1469598103934665603ull;
    const u32 count = dc.n_source_refs ? dc.n_source_refs : dc.n_verts;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const u32 vertex = dc.refs_remapped
            ? rsx_vertex_remap_index(&dc.ref_remap, occurrence)
            : occurrence;
        for (u32 attr = 0; attr < 16; attr++) {
            float fallback[4];
            const float* value =
                dc_vertex_attr_canonical(vertex, attr, fallback);
            hash = fnv1a(value, 16u, hash);
            attr_hash[attr] = fnv1a(value, 16u, attr_hash[attr]);
        }
    }
    return hash;
}

/* RSX_DRAW_CSV=path: uncapped per-draw fingerprints for direct comparison
 * with the working RPCS3 .rxs replay.  Default-off and renderer-neutral. */
/* The open RSX_DRAW_CSV and its gate, for marker rows from outside a draw
 * (rsx_live_draw_blit notes each 2D copy where it falls in the draw stream). */
static FILE* s_csv_file;
static int   s_csv_gate_open = 1;
/* Counts gate openings, so LD_DRAW_DUMP and the flip dumps re-arm for each
 * capture instead of only the first one of the run. */
static u32   s_csv_gate_session;

/* Capture-only geometry diagnostics for one draw, appended to its CSV row:
 *   ar=first+count,...      the DRAW_ARRAYS batches (first four)
 *   ix=loc:offset:16|32/n   the index array when the draw is indexed
 *   el=min-max              the vertex element range the draw reads
 *   pos=x0/x1/y0/y1/z0/z1 bad=N   attribute 0 bounding box as decoded, and
 *                           how many vertices are non-finite or beyond 1e5
 * With LD_VB_DUMP=<dir>, the raw guest bytes behind attribute 0 (the whole
 * interleaved record range) and the index array are also written once per
 * capture session, so a mesh that draws wrong can be inspected offline. */
static void live_draw_csv_geometry(FILE* file, u32 session)
{
    const u32 baseIndex = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array indexArray;
    rsx_dsp_get_index_array(&g.rsx, &indexArray);
    const u32 elementSize = indexArray.is_u32 ? 4u : 2u;
    const int restartEnabled = rsx_dsp_restart_index_enabled(&g.rsx, indexArray.is_u32);
    const u32 restartValue = rsx_dsp_restart_index(&g.rsx);
    u32 minElement = 0xFFFFFFFFu;
    u32 maxElement = 0;
    u32 indexCount = 0;
    u32 indexFirst = 0xFFFFFFFFu;

    if (dc.n_arr)
    {
        fprintf(file, "ar=");
        for (u32 batch = 0; batch < dc.n_arr; batch++)
        {
            if (batch < 4)
            {
                fprintf(file, "%u+%u%s", dc.arr[batch].first, dc.arr[batch].count,
                        batch + 1 < dc.n_arr && batch < 3 ? "," : "");
            }
            if (dc.arr[batch].count)
            {
                const u32 batchFirst = dc.arr[batch].first;
                const u32 batchLast = batchFirst + dc.arr[batch].count - 1;
                if (batchFirst < minElement)
                {
                    minElement = batchFirst;
                }
                if (batchLast > maxElement)
                {
                    maxElement = batchLast;
                }
            }
        }
        fprintf(file, "%s ", dc.n_arr > 4 ? ",..." : "");
    }
    for (u32 batch = 0; batch < dc.n_idx; batch++)
    {
        const u32 first = dc.idx[batch].first;
        const u32 count = dc.idx[batch].count;
        if (first < indexFirst)
        {
            indexFirst = first;
        }
        indexCount += count;
        for (u32 position = 0; position < count; position++)
        {
            const u8* source = guest_ptr(indexArray.location,
                indexArray.offset + (first + position) * elementSize, elementSize);
            if (!source)
            {
                break;
            }
            const u32 index = indexArray.is_u32
                ? (((u32)source[0] << 24) | ((u32)source[1] << 16) |
                   ((u32)source[2] << 8) | source[3])
                : (u32)((source[0] << 8) | source[1]);
            if (restartEnabled && index == restartValue)
            {
                continue;
            }
            const u32 element = index + baseIndex;
            if (element < minElement)
            {
                minElement = element;
            }
            if (element > maxElement)
            {
                maxElement = element;
            }
        }
    }
    if (dc.n_idx)
    {
        fprintf(file, "ix=%u:%X:%u/%u ", indexArray.location, indexArray.offset,
                elementSize * 8u, indexCount);
    }
    if (minElement != 0xFFFFFFFFu)
    {
        fprintf(file, "el=%u-%u ", minElement, maxElement);
    }

    const u32 vertexCount = dc.n_source_refs ? dc.n_source_refs : dc.n_verts;
    if (vertexCount && dc.fetch_plan.attr[0].desc.type)
    {
        float lowest[3] = { 1e30f, 1e30f, 1e30f };
        float highest[3] = { -1e30f, -1e30f, -1e30f };
        u32 badCount = 0;
        for (u32 occurrence = 0; occurrence < vertexCount; occurrence++)
        {
            const u32 vertex = dc.refs_remapped
                ? rsx_vertex_remap_index(&dc.ref_remap, occurrence)
                : occurrence;
            float fallback[4];
            const float* value = dc_vertex_attr_canonical(vertex, 0, fallback);
            int vertexBad = 0;
            for (u32 axis = 0; axis < 3; axis++)
            {
                const float component = value[axis];
                if (!(component == component) || component > 1e5f || component < -1e5f)
                {
                    vertexBad = 1;
                    continue;
                }
                if (component < lowest[axis])
                {
                    lowest[axis] = component;
                }
                if (component > highest[axis])
                {
                    highest[axis] = component;
                }
            }
            badCount += (u32)vertexBad;
        }
        fprintf(file, "pos=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f bad=%u ",
                lowest[0], highest[0], lowest[1], highest[1], lowest[2], highest[2], badCount);
    }

    static const char* dumpDir = (const char*)-1;
    if (dumpDir == (const char*)-1)
    {
        dumpDir = getenv("LD_VB_DUMP");
    }
    if (!dumpDir || !dumpDir[0])
    {
        return;
    }
    static u32 seenSession = ~0u;
    static u64 seenKeys[8192];
    static u32 seenCount = 0;
    static u64 dumpedBytes = 0;
    if (seenSession != session)
    {
        seenSession = session;
        seenCount = 0;
        dumpedBytes = 0;
    }
    const u64 dumpLimit = 512ull * 1024ull * 1024ull;
    rsx_dsp_vertex_attr position;
    rsx_dsp_get_vertex_attr(&g.rsx, 0, &position);
    struct
    {
        u32 location;
        u64 start;
        u64 bytes;
        const char* kind;
        u32 stride;
    } regions[2];
    u32 regionCount = 0;
    if (position.type && position.size && position.stride && minElement != 0xFFFFFFFFu &&
        maxElement >= minElement && maxElement - minElement < 0x100000u)
    {
        regions[regionCount].location = position.location;
        regions[regionCount].start = (u64)rsx_dsp_vertex_data_base_offset(&g.rsx) +
            position.offset + (u64)minElement * position.stride;
        regions[regionCount].bytes = (u64)(maxElement - minElement + 1) * position.stride;
        regions[regionCount].kind = "vb";
        regions[regionCount].stride = position.stride;
        regionCount++;
    }
    if (dc.n_idx && indexCount && indexFirst != 0xFFFFFFFFu)
    {
        regions[regionCount].location = indexArray.location;
        regions[regionCount].start = (u64)indexArray.offset + (u64)indexFirst * elementSize;
        regions[regionCount].bytes = (u64)indexCount * elementSize;
        regions[regionCount].kind = "ib";
        regions[regionCount].stride = elementSize;
        regionCount++;
    }
    for (u32 regionIndex = 0; regionIndex < regionCount; regionIndex++)
    {
        const u64 start = regions[regionIndex].start;
        const u64 bytes = regions[regionIndex].bytes;
        if (start > UINT32_MAX || bytes == 0 || bytes > 16u * 1024u * 1024u ||
            dumpedBytes + bytes > dumpLimit || seenCount >= 8192)
        {
            continue;
        }
        const u64 key = ((u64)regions[regionIndex].location << 60) ^ (start << 24) ^ bytes;
        int already = 0;
        for (u32 seenIndex = 0; seenIndex < seenCount; seenIndex++)
        {
            if (seenKeys[seenIndex] == key)
            {
                already = 1;
                break;
            }
        }
        if (already)
        {
            continue;
        }
        seenKeys[seenCount++] = key;
        const u8* source = guest_ptr(regions[regionIndex].location, (u32)start, (u32)bytes);
        if (!source)
        {
            continue;
        }
        char dumpPath[MAX_PATH * 2];
        snprintf(dumpPath, sizeof(dumpPath), "%s\\%s_c%u_f%06u_L%u_%08X_s%u_n%u.bin", dumpDir,
                 regions[regionIndex].kind, session, g_ld_frames, regions[regionIndex].location,
                 (u32)start, regions[regionIndex].stride, (u32)bytes);
        FILE* dumpFile = fopen(dumpPath, "wb");
        if (dumpFile)
        {
            fwrite(source, 1, (size_t)bytes, dumpFile);
            fclose(dumpFile);
            dumpedBytes += bytes;
        }
    }
}

static void live_draw_csv_emit(u32 prim, u32 n_tri, const char* outcome)
{
#if defined(YZ_PERF_CLEAN) && !defined(YZ_PERF_PROFILE)
    (void)prim;
    (void)n_tri;
    (void)outcome;
#else
    static int inited = 0;
    static FILE* file = NULL;
    static u64 draw = 0;
    /* RSX_DRAW_CSV_FROM=<frame>: skip rows before that frame. Logging every
     * draw from boot slows a 10-minute run to an hour. */
    { static long from = -1;
      if (from < 0) { const char* e = getenv("RSX_DRAW_CSV_FROM"); from = e ? atol(e) : 0; }
      if ((long)g_ld_frames < from) return; }
    /* RSX_DRAW_CSV_GATE=<file>: rows only while <file> exists (polled every
     * 512 draws), so a capture can be switched on when the scene of interest
     * is on screen instead of guessing its frame number. */
    { static const char* gate = (const char*)-1; static int open_gate = 0; static unsigned poll;
      if (gate == (const char*)-1) gate = getenv("RSX_DRAW_CSV_GATE");
      if (gate) {
          if ((poll++ & 511u) == 0) {
              struct stat st;
              const int nowOpen = stat(gate, &st) == 0;
              if (nowOpen && !open_gate) {
                  s_csv_gate_session++;
                  fprintf(stderr, "[live-diff] capture %u opened at frame %u\n",
                          s_csv_gate_session, g_ld_frames);
              }
              open_gate = nowOpen;
          }
          s_csv_gate_open = open_gate;
          if (!open_gate) return;
      } }
    static int a010_only = -1;
    if (a010_only < 0) a010_only = getenv("RSX_DRAW_CSV_A010_ONLY") != NULL;
    if (a010_only) {
        if (InterlockedCompareExchange(
                &g_yz_a010_root_active, 0, 0) == 0)
            return;
    }
    if (!inited) {
        inited = 1;
        const char* path = getenv("RSX_DRAW_CSV");
        if (path && path[0]) {
            file = fopen(path, "w");
            s_csv_file = file;
            if (file) {
                fprintf(file,
                    "draw,frame,outcome,surf,prim,verts,source_verts,decoded_hash,"
                    "pso_key,regs_hash,vp_hash,"
                    "const_hash,blend,dtest,dwrite,dfunc,cull,cullface,"
                    "frontface,cmask,vpx,vpy,vpw,vph,sclx,scly,sclz,"
                    "trnx,trny,trnz,clipw,cliph,zeta_off,zeta_pitch,zeta_loc,"
                    "seen_vtex,seen_vtxfmt,seen_freqdiv,"
                    "attr0_hash,attr1_hash,attr2_hash,attr3_hash,"
                    "attr4_hash,attr5_hash,attr6_hash,attr7_hash,"
                    "attr8_hash,attr9_hash,attr10_hash,attr11_hash,"
                    "attr12_hash,attr13_hash,attr14_hash,attr15_hash,"
                    "active_attr_mask,used_attr_mask,tex\n");
                fprintf(stderr, "[live-diff] RSX_DRAW_CSV armed: %s\n", path);
            } else {
                fprintf(stderr, "[live-diff] cannot open RSX_DRAW_CSV: %s\n",
                        path);
            }
            fflush(stderr);
        }
    }
    if (!file)
        return;

    const u32 target = current_surface();
    const u32 surf = target < g.n_surfaces ? g.surfaces[target].offset : 0;
    /* LD_DRAW_DUMP=<dir> [LD_DRAW_DUMP_EVERY=k, default 10]: in the first
     * gated frame, read back each k-th draw's target BEFORE it is recorded,
     * named by frame-relative index -- bisects which draw changes a surface. */
    { static const char* dir = (const char*)-1; static u32 every, frame = ~0u, idx, dumped_frame = ~0u;
      static u32 dumpedSession = 0;
      if (dir == (const char*)-1) { dir = getenv("LD_DRAW_DUMP");
          const char* e = getenv("LD_DRAW_DUMP_EVERY"); every = e ? (u32)atoi(e) : 10; if (!every) every = 10; }
      if (dumpedSession != s_csv_gate_session) {
          dumpedSession = s_csv_gate_session;
          dumped_frame = ~0u;
      }
      if (frame != g_ld_frames) { frame = g_ld_frames; idx = 0; }
      if (dir && target < g.n_surfaces && (dumped_frame == ~0u || dumped_frame == frame) &&
          (idx % every) == 0) {
          dumped_frame = frame;
          char path[MAX_PATH * 2];
          snprintf(path, sizeof(path), "%s\\draw_c%u_f%06u_i%04u_%08X.ppm", dir, s_csv_gate_session, frame, idx, surf);
          ld_dump_surface_ppm(path, &g.surfaces[target]);
      }
      idx++; }
    const u64 regs_hash = fnv1a(
        g.rsx.regs, RSX_DSP_NUM_REGS * sizeof(g.rsx.regs[0]),
        1469598103934665603ull);
    const u64 vp_hash = fnv1a(
        g.rsx.vp, sizeof(g.rsx.vp), 1469598103934665603ull);
    const u64 const_hash = fnv1a(
        g.rsx.constants, sizeof(g.rsx.constants), 1469598103934665603ull);
    render_state_t rs;
    decode_render_state(&rs);
    rsx_dsp_viewport vp;
    rsx_dsp_get_viewport(&g.rsx, &vp);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);

    u32 seen_vtex = 0, seen_vtxfmt = 0;
    for (u32 i = 0; i < 32; i++)
        seen_vtex += g.rsx.seen[(0x0900u >> 2) + i];
    for (u32 i = 0; i < 16; i++)
        seen_vtxfmt += g.rsx.seen[(0x1740u >> 2) + i];
    const u32 seen_freqdiv = g.rsx.seen[0x1FC0u >> 2];
    u64 attr_hash[16];
    const u64 decoded_hash = live_decoded_vertex_hash(attr_hash);
    u32 active_attr_mask = 0;
    for (u32 attr = 0; attr < 16; attr++)
        if (dc.fetch_plan.attr[attr].desc.type &&
            dc.fetch_plan.attr[attr].desc.size)
            active_attr_mask |= 1u << attr;

    fprintf(file,
        "%llu,%u,%s,0x%X,%u,%u,%u,%016llx,"
        "%016llx,%016llx,%016llx,%016llx,"
        "%u,%u,%u,0x%X,%u,0x%X,0x%X,0x%08X,"
        "%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
        "%u,%u,0x%X,%u,%u,%u,%u,%u",
        (unsigned long long)draw++, g_ld_frames, outcome, surf, prim, n_tri,
        dc.n_source_refs ? dc.n_source_refs : dc.n_verts,
        (unsigned long long)decoded_hash,
        (unsigned long long)live_legacy_pso_key(),
        (unsigned long long)regs_hash, (unsigned long long)vp_hash,
        (unsigned long long)const_hash,
        rs.blend_enable, rs.depth_test, rs.depth_write, rs.depth_func,
        rs.cull_enable, rs.cull_face, rs.front_face, rs.color_mask,
        vp.x, vp.y, vp.w, vp.h,
        vp.scale[0], vp.scale[1], vp.scale[2],
        vp.translate[0], vp.translate[1], vp.translate[2],
        sf.clip_w, sf.clip_h, sf.zeta_offset, sf.zeta_pitch, sf.zeta_location,
        seen_vtex, seen_vtxfmt, seen_freqdiv);
    for (u32 attr = 0; attr < 16; attr++)
        fprintf(file, ",%016llx", (unsigned long long)attr_hash[attr]);
    fprintf(file, ",0x%04X,0x%04X,",
            active_attr_mask, dc.layout.mask & 0xFFFFu);
    /* Enabled fragment texture units, so a pass that samples a surface (or an
     * empty copy of one) is visible in the row: unit:loc:offset:fmt:WxH. */
    for (u32 u = 0; u < 16; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled)
            fprintf(file, "%u:%u:%X:%02X:%ux%u ", u, t.location, t.offset,
                    t.format & 0xFF, t.width, t.height);
    }
    /* Vertex attribute layout: index:type x size /stride @location:offset
     * (type 1 s16n 2 f32 3 f16 4 u8n 5 s16 6 cmp 7 u8). A mesh that draws
     * stray polygons can be told apart by its layout and its data address. */
    fprintf(file, "vb=%X ", rsx_dsp_vertex_data_base_offset(&g.rsx));
    for (u32 attr = 0; attr < 16; attr++) {
        rsx_dsp_vertex_attr layout;
        rsx_dsp_get_vertex_attr(&g.rsx, attr, &layout);
        if (layout.type && layout.size) {
            fprintf(file, "v%u:%ux%u/%u@%u:%X ", attr, layout.type, layout.size,
                    layout.stride, layout.location, layout.offset);
        }
    }
    live_draw_csv_geometry(file, s_csv_gate_session);
    if (rs.blend_enable)
        fprintf(file, "bf=%X/%X/%X/%X eq=%X/%X ", rs.sf_rgb, rs.df_rgb, rs.sf_a, rs.df_a,
                rs.eq_rgb, rs.eq_a);
    fprintf(file, "at=%u/0x%X/0x%X aac=%X ", rs.alpha_test_enable, rs.alpha_func, rs.alpha_ref_raw,
            rsx_dsp_reg(&g.rsx, M_ANTI_ALIASING_CONTROL));
    fprintf(file, "sc=%08X/%08X ", rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL));
    if (rs.stencil_enable) {
        fprintf(file, "st=%X/%X/%X/%X/%X/%X ref=%X ", rs.s_func, rs.s_func_mask, rs.s_write_mask, rs.s_fail, rs.s_zfail, rs.s_zpass, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC + 4));
        /* Two-sided stencil: back-face func/ops (shadow volumes need them). */
        fprintf(file, "bst=%u/%X/%X/%X/%X ", rs.stencil_two_sided, rs.bs_func, rs.bs_fail, rs.bs_zfail, rs.bs_zpass);
    }
    /* Inline draws (HUD sprites): the largest vertex-colour alpha byte, attr 3
     * as UB4 -- an element faded to 0 is drawn but invisible. */
    if (dc.inl && dc.inl_stride) {
        rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, 3, &va);
        if (va.type == RSX_VTX_TYPE_UNORM8 && va.size == 4) {
            u32 amax = 0;
            for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                if (dc.inl[o] > amax) amax = dc.inl[o];
            fprintf(file, "va=%u ", amax);
        }
        /* ...and the xy bounding box of attr 0 when it is float (screen-space HUD). */
        rsx_dsp_vertex_attr p0; rsx_dsp_get_vertex_attr(&g.rsx, 0, &p0);
        if (p0.type == RSX_VTX_TYPE_FLOAT && p0.size >= 2) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (u32 o = dc.inl_off[0]; o + 8 <= dc.inl_bytes; o += dc.inl_stride) {
                u32 xb = (dc.inl[o] << 24) | (dc.inl[o+1] << 16) | (dc.inl[o+2] << 8) | dc.inl[o+3];
                u32 yb = (dc.inl[o+4] << 24) | (dc.inl[o+5] << 16) | (dc.inl[o+6] << 8) | dc.inl[o+7];
                float x, y; memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
                if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
            fprintf(file, "bb=%.0f,%.0f-%.0f,%.0f ", x0, y0, x1, y1);
        }
    }
    fprintf(file, "A=%u:%X/%u fmt=%u rt=%u aa=%u ", sf.color_location[0], sf.color_offset[0], sf.color_pitch[0], sf.color_format, sf.raster_type, sf.antialias);
    fprintf(file, "slot=%u:%ux%u ", target, target < g.n_surfaces ? g.surfaces[target].w : 0u,
            target < g.n_surfaces ? g.surfaces[target].h : 0u);
    fprintf(file, "ct=0x%X B=%u:%X C=%u:%X D=%u:%X ", sf.color_target,
            sf.color_location[1], sf.color_offset[1], sf.color_location[2], sf.color_offset[2],
            sf.color_location[3], sf.color_offset[3]);
    fputc('\n', file);
    fflush(file);
#endif
}

/* RSX_A010_GEOM: trace one stable orphanage mesh.  This is draw 520 from
 * the healthy a010 reference: a 270-vertex triangle strip on the main world
 * surface with PSO 7d9f....  The model-initialization repair now reproduces
 * this exact tuple in the live stream.  Keeping the tuple exact makes the gate
 * quiet outside a010 while allowing capture/playback only after known-good
 * world geometry has actually reached the backend. */
static void live_a010_geom_trace(u32 prim, u32 n_tri)
{
    static int enabled = -1;
    static u32 logged = 0;
    static u32 reference_logged = 0;
    if (enabled < 0)
        enabled = getenv("RSX_A010_GEOM") ? 1 : 0;
    if (prim != PRIM_TRIANGLE_STRIP || n_tri != 270)
        return;

    const u32 target = current_surface();
    const u32 surf =
        target < g.n_surfaces ? g.surfaces[target].offset : 0;
    const u64 key = live_legacy_pso_key();
    if (surf != 0x01800000u || key != 0x7d9f528329f20c52ull)
        return;
    /*
     * This topology/PSO/surface tuple is the stable Kiryu mesh measured in
     * the healthy a010 capture. Reaching this point means vertex fetch has
     * completed and the scene has produced real world geometry, rather than
     * only clears and fullscreen post-processing draws.
     */
    if (InterlockedCompareExchange(&g_ld_a010_world_ready, 1, 0) == 0) {
        fprintf(stderr,
                "[a010-world-ready] frame=%u surface=0x%X "
                "pso=%016llx vertices=%u\n",
                g_ld_frames, surf, (unsigned long long)key, n_tri);
        fflush(stderr);
    }
    if (!enabled)
        return;
    if (InterlockedCompareExchange(
            &g_yz_a010_reference_camera_active, 0, 0) != 0) {
        if (reference_logged >= 8)
            return;
        reference_logged++;
    } else {
        if (logged >= 8)
            return;
        logged++;
    }

    float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f};
    float mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
    u32 nan_count = 0;
    for (u32 vi = 0; vi < dc.n_verts; vi++) {
        float fallback[4];
        const float* p = dc_vertex_attr(vi, 0, fallback);
        for (u32 k = 0; k < 4; k++) {
            if (p[k] != p[k]) {
                nan_count++;
                continue;
            }
            if (p[k] < mn[k]) mn[k] = p[k];
            if (p[k] > mx[k]) mx[k] = p[k];
        }
    }

    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(&g.rsx, &ia);
    rsx_dsp_vertex_attr a0, a7;
    rsx_dsp_get_vertex_attr(&g.rsx, 0, &a0);
    rsx_dsp_get_vertex_attr(&g.rsx, 7, &a7);

    fprintf(stderr,
        "[a010-geom] match=%u frame=%u fetched=%u expanded=%u cuts=%u "
        "pos=[%.6g %.6g %.6g %.6g]-[%.6g %.6g %.6g %.6g] nan=%u "
        "decoded_hash=%016llx\n",
        logged, g_ld_frames, dc.n_verts, n_tri, dc.n_cuts,
        mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3],
        nan_count,
        (unsigned long long)fnv1a(
            ld_vertex_compact_payload()
                ? (const void*)dc.compact_verts
                : (const void*)dc.verts,
            ld_vertex_compact_payload()
                ? (u32)((u64)dc.n_verts * dc.layout.stride)
                : (u32)((u64)dc.n_verts * sizeof(vtx_t)),
            1469598103934665603ull));
    fprintf(stderr,
        "[a010-geom] base=0x%X base_index=%u "
        "idx(loc=%u off=0x%X u32=%u first=%u count=%u) "
        "a0(type=%u size=%u stride=%u loc=%u off=0x%X freq=%u) "
        "a7(type=%u size=%u stride=%u loc=%u off=0x%X freq=%u)\n",
        base, base_index,
        ia.location, ia.offset, ia.is_u32,
        dc.n_idx ? dc.idx[0].first : 0,
        dc.n_idx ? dc.idx[0].count : 0,
        a0.type, a0.size, a0.stride, a0.location, a0.offset, a0.frequency,
        a7.type, a7.size, a7.stride, a7.location, a7.offset, a7.frequency);

    for (u32 slot = 108; slot <= 115; slot++) {
        const u32* c = rsx_dsp_constant(&g.rsx, slot);
        union { u32 u; float f; } v[4];
        for (u32 k = 0; k < 4; k++) v[k].u = c[k];
        fprintf(stderr,
            "[a010-geom] c%u=(%.6g %.6g %.6g %.6g) "
            "raw=%08X/%08X/%08X/%08X\n",
            slot, v[0].f, v[1].f, v[2].f, v[3].f,
            v[0].u, v[1].u, v[2].u, v[3].u);
    }
    {
        const u32* c = rsx_dsp_constant(&g.rsx, 467);
        union { u32 u; float f; } v[4];
        for (u32 k = 0; k < 4; k++) v[k].u = c[k];
        fprintf(stderr,
            "[a010-geom] c467=(%.6g %.6g %.6g %.6g)\n",
            v[0].f, v[1].f, v[2].f, v[3].f);
    }
    {
        float camera[4][4];
        float c467[4];
        for (u32 row = 0; row < 4; row++)
            memcpy(camera[row], rsx_dsp_constant(&g.rsx, 108u + row),
                   sizeof(camera[row]));
        memcpy(c467, rsx_dsp_constant(&g.rsx, 467), sizeof(c467));
        rsx_dsp_viewport vp;
        rsx_dsp_get_viewport(&g.rsx, &vp);
        float sx_min = 1e30f, sy_min = 1e30f;
        float sx_max = -1e30f, sy_max = -1e30f;
        float w_min = 1e30f, w_max = -1e30f;
        u32 finite = 0, in_front = 0, in_view = 0;
        for (u32 vi = 0; vi < dc.n_verts; vi++) {
            float p_fallback[4], weight_fallback[4], bone_fallback[4];
            const float* p = dc_vertex_attr(vi, 0, p_fallback);
            const float* weight =
                dc_vertex_attr(vi, 1, weight_fallback);
            const float* bone =
                dc_vertex_attr(vi, 7, bone_fallback);
            const float pw = p[3] != 0.0f ? p[3] : 1.0f;
            /*
             * Exact transform used by healthy PSO 7d9f...: ATTR7 selects
             * four 3-row bone matrices at c112+, ATTR1.yzw supplies weights
             * 1..3, and weight 0 is implicit.  The old probe incorrectly
             * treated c112..c115 as one fixed 4x4 matrix.
             */
            const float influence[4] = {
                c467[1] - weight[1] - weight[2] - weight[3],
                weight[1], weight[2], weight[3]
            };
            float skin[3][4] = {{0}};
            for (u32 inf = 0; inf < 4; inf++) {
                const u32 base_slot =
                    (u32)(bone[inf] * c467[0]);
                for (u32 row = 0; row < 3; row++) {
                    const float* palette = (const float*)
                        rsx_dsp_constant(
                            &g.rsx, (112u + base_slot + row) & 511u);
                    for (u32 lane = 0; lane < 4; lane++)
                        skin[row][lane] +=
                            influence[inf] * palette[lane];
                }
            }
            const float r7x =
                p[0]*skin[0][0] + p[1]*skin[0][1] +
                p[2]*skin[0][2] + pw*skin[0][3];
            const float r7y =
                p[0]*skin[1][0] + p[1]*skin[1][1] +
                p[2]*skin[1][2] + pw*skin[1][3];
            const float r7z =
                p[0]*skin[2][0] + p[1]*skin[2][1] +
                p[2]*skin[2][2] + pw*skin[2][3];
            const float r4w =
                c467[2] * (p[0] + p[1] + p[2]) + pw;
            const float cx =
                r7x*camera[0][0] + r7y*camera[0][1] +
                r7z*camera[0][2] + r4w*camera[0][3];
            const float cy =
                r7x*camera[1][0] + r7y*camera[1][1] +
                r7z*camera[1][2] + r4w*camera[1][3];
            const float cw =
                r7x*camera[3][0] + r7y*camera[3][1] +
                r7z*camera[3][2] + r4w*camera[3][3];
            if (cx != cx || cy != cy || cw != cw || cw == 0.0f)
                continue;
            finite++;
            if (cw > 0.0f) in_front++;
            if (cw < w_min) w_min = cw;
            if (cw > w_max) w_max = cw;
            const float sx = (cx / cw) * vp.scale[0] + vp.translate[0];
            const float sy = (cy / cw) * vp.scale[1] + vp.translate[1];
            if (sx < sx_min) sx_min = sx;
            if (sx > sx_max) sx_max = sx;
            if (sy < sy_min) sy_min = sy;
            if (sy > sy_max) sy_max = sy;
            if (cw > 0.0f && sx >= 0.0f && sx < (float)vp.w &&
                sy >= 0.0f && sy < (float)vp.h)
                in_view++;
        }
        fprintf(stderr,
                "[a010-clip] frame=%u finite=%u/%u front=%u view=%u "
                "screen=[%.3f %.3f]-[%.3f %.3f] w=[%.6g %.6g] "
                "viewport=%ux%u\n",
                g_ld_frames, finite, dc.n_verts, in_front, in_view,
                sx_min, sy_min, sx_max, sy_max, w_min, w_max,
                vp.w, vp.h);
        if (InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0) != 0) {
            static LONG acceptance_written = 0;
            if (InterlockedCompareExchange(
                    &acceptance_written, 1, 0) == 0) {
                FILE* acceptance =
                    fopen("scratch\\a010_acceptance.txt", "a");
                if (acceptance) {
                    fprintf(acceptance,
                            "reference-mesh frame=%u finite=%u/%u "
                            "front=%u view=%u screen=%.3f,%.3f,"
                            "%.3f,%.3f viewport=%u,%u\n",
                            g_ld_frames, finite, dc.n_verts,
                            in_front, in_view, sx_min, sy_min,
                            sx_max, sy_max, vp.w, vp.h);
                    fclose(acceptance);
                }
            }
        }
    }
    fflush(stderr);
}

typedef enum {
    LD_COMPACT_EXPAND_OK = 1,
    LD_COMPACT_EXPAND_DEGENERATE = 0,
    LD_COMPACT_EXPAND_ALLOC = -1,
    LD_COMPACT_EXPAND_PRIMITIVE = -2
} ld_compact_expand_result;

static void compact_vertex_copy(
    u8* destination, u32 destination_vertex, u32 source_vertex)
{
    if (!dc.layout.stride)
        return;
    memcpy(
        destination + (u64)destination_vertex * dc.layout.stride,
        dc.compact_verts + (u64)source_vertex * dc.layout.stride,
        dc.layout.stride);
}

static ld_compact_expand_result expand_compact_topology(
    u32 prim, u8** expanded, u32* expanded_vertices, int* owned)
{
    *expanded = dc.compact_verts;
    *expanded_vertices = 0;
    *owned = 0;
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES:
        *expanded_vertices = dc.n_verts - dc.n_verts % 3;
        break;
    case PRIM_TRIANGLE_STRIP: {
        if (dc.n_verts < 3)
            return LD_COMPACT_EXPAND_DEGENERATE;
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count >= 3) total += (count - 2) * 3;
        }
        if (!total)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = total;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc((size_t)total * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 0; i + 2 < count; i++) {
                if (i & 1) {
                    compact_vertex_copy(*expanded, write++, begin + i + 1);
                    compact_vertex_copy(*expanded, write++, begin + i);
                    compact_vertex_copy(*expanded, write++, begin + i + 2);
                } else {
                    compact_vertex_copy(*expanded, write++, begin + i);
                    compact_vertex_copy(*expanded, write++, begin + i + 1);
                    compact_vertex_copy(*expanded, write++, begin + i + 2);
                }
            }
        }
        break;
    }
    case PRIM_TRIANGLE_FAN: {
        if (dc.n_verts < 3)
            return LD_COMPACT_EXPAND_DEGENERATE;
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count >= 3) total += (count - 2) * 3;
        }
        if (!total)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = total;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc((size_t)total * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                compact_vertex_copy(*expanded, write++, begin);
                compact_vertex_copy(*expanded, write++, begin + i);
                compact_vertex_copy(*expanded, write++, begin + i + 1);
            }
        }
        break;
    }
    case PRIM_QUADS: {
        const u32 quads = dc.n_verts / 4;
        if (!quads)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = quads * 6;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc(
            (size_t)*expanded_vertices * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 quad = 0; quad < quads; quad++) {
            const u32 base = quad * 4;
            compact_vertex_copy(*expanded, write++, base);
            compact_vertex_copy(*expanded, write++, base + 1);
            compact_vertex_copy(*expanded, write++, base + 2);
            compact_vertex_copy(*expanded, write++, base + 2);
            compact_vertex_copy(*expanded, write++, base + 3);
            compact_vertex_copy(*expanded, write++, base);
        }
        break;
    }
    default:
        return LD_COMPACT_EXPAND_PRIMITIVE;
    }
    return *expanded_vertices
        ? LD_COMPACT_EXPAND_OK : LD_COMPACT_EXPAND_DEGENERATE;
}

static u32 topology_index_count(u32 prim)
{
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES:
        return dc.n_source_refs - dc.n_source_refs % 3u;
    case PRIM_TRIANGLE_STRIP:
    case PRIM_TRIANGLE_FAN: {
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case PRIM_QUADS:
        return (dc.n_source_refs / 4) * 6;
    default:
        return 0;
    }
}

static u32 topology_vertex_index(u32 occurrence)
{
    return dc.refs_remapped
        ? rsx_vertex_remap_index(&dc.ref_remap, occurrence)
        : occurrence;
}

static void write_topology_indices(u32 prim, u32* indices)
{
    u32 write = 0;
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES: {
        const u32 count =
            dc.n_source_refs - dc.n_source_refs % 3u;
        for (u32 occurrence = 0; occurrence < count; occurrence++)
            indices[write++] = topology_vertex_index(occurrence);
        break;
    }
    case PRIM_TRIANGLE_STRIP:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = topology_vertex_index(
                    begin + i + (i & 1u));
                indices[write++] = topology_vertex_index(
                    begin + i + 1u - (i & 1u));
                indices[write++] = topology_vertex_index(begin + i + 2u);
            }
        }
        break;
    case PRIM_TRIANGLE_FAN:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = topology_vertex_index(begin);
                indices[write++] = topology_vertex_index(begin + i);
                indices[write++] = topology_vertex_index(begin + i + 1u);
            }
        }
        break;
    case PRIM_QUADS:
        for (u32 quad = 0; quad < dc.n_source_refs / 4; quad++) {
            const u32 base = quad * 4;
            indices[write++] = topology_vertex_index(base);
            indices[write++] = topology_vertex_index(base + 1u);
            indices[write++] = topology_vertex_index(base + 2u);
            indices[write++] = topology_vertex_index(base + 2u);
            indices[write++] = topology_vertex_index(base + 3u);
            indices[write++] = topology_vertex_index(base);
        }
        break;
    }
}

/* Where the present thread's per-draw time goes, for the [fps] report:
 * every draw's sink_end, split into vertex fetch, shader lookup, texture
 * binding and the rest (constants, state and the draw call). */
typedef struct
{
    u64 draws;
    u64 totalTicks;
    u64 fetchTicks;
    u64 shaderTicks;
    u64 bindTicks;
} ld_draw_perf;

static ld_draw_perf g_ld_draw_perf;

static u64 ld_perf_ticks(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (u64)value.QuadPart;
}

static double ld_perf_ticks_to_ms(u64 ticks)
{
    static double millisecondsPerTick = 0.0;
    if (millisecondsPerTick == 0.0)
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        millisecondsPerTick = 1000.0 / (double)frequency.QuadPart;
    }
    return (double)ticks * millisecondsPerTick;
}

static void sink_end_impl(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    if (!dc.n_packets) { g_ld_stats.groups_empty++; return; }
    g_ld_stats.groups_seen++;
    /*
     * a010's native per-mesh constant builder can overwrite the otherwise
     * valid reconstructed camera with NaNs after the ordinary camera upload.
     * Repair at the last measured boundary, immediately before the draw
     * consumes the constants. Once the synchronized a010 reference is armed,
     * replace finite-but-wrong native uploads too: the measured failure
     * matrix is finite, yet projects the model thousands of pixels offscreen.
     */
    if (InterlockedCompareExchange(&g_ld_a010_camera_ready, 0, 0) != 0) {
        int camera_has_nan = 0;
        for (u32 slot = 108; slot <= 111 && !camera_has_nan; slot++)
            for (u32 lane = 0; lane < 4; lane++) {
                const u32 bits = g.rsx.constants[slot][lane];
                if ((bits & 0x7F800000u) == 0x7F800000u &&
                    (bits & 0x007FFFFFu) != 0) {
                    camera_has_nan = 1;
                    break;
                }
            }
        if (camera_has_nan || g_yz_runtime_config.a010_start_reference ||
            InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0) != 0) {
            for (u32 i = 0; i < 16; i++)
                g.rsx.constants[108 + i / 4][i % 4] =
                    g_ld_a010_camera_bits[i];
            static u64 repairs = 0;
            repairs++;
            if (repairs <= 16 || (repairs & (repairs - 1)) == 0) {
                fprintf(stderr,
                        "[a010-rsx-camera-repair] n=%llu frame=%u "
                        "c108=%08X/%08X/%08X/%08X\n",
                        (unsigned long long)repairs, g_ld_frames,
                        g.rsx.constants[108][0], g.rsx.constants[108][1],
                        g.rsx.constants[108][2], g.rsx.constants[108][3]);
                fflush(stderr);
            }
        }
    }
    const u32 prim = g.rsx.current_primitive;
    rsx_vertex_layout_plan used_layout;
    ld_current_vertex_layout(&used_layout);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.used_attribute_draws++;
    g_ld_profile.total.used_attribute_sum += used_layout.count;
    if (used_layout.count > g_ld_profile.frame_used_attribute_max)
        g_ld_profile.frame_used_attribute_max = used_layout.count;
    if (used_layout.count > g_ld_profile.total_used_attribute_max)
        g_ld_profile.total_used_attribute_max = used_layout.count;
    const LONGLONG fetch_pack_begin = ld_profile_qpc();
#endif
    const u64 fetchBegin = ld_perf_ticks();
    if (ld_vertex_compact_payload()) {
        fetch_batches_hoisted(&used_layout, 1);
    } else if (ld_vertex_hoist_fetch()) {
        rsx_vertex_layout_plan legacy_layout;
        ld_layout_plan_get(0xFFFFu, &legacy_layout);
        fetch_batches_hoisted(&legacy_layout, 0);
    } else {
        fetch_batches();
    }
    if (!dc.n_source_refs)
        dc.n_source_refs = dc.n_verts;
    g_ld_draw_perf.fetchTicks += ld_perf_ticks() - fetchBegin;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.vertex_fetch_pack_qpc +=
        (u64)(ld_profile_qpc() - fetch_pack_begin);
#endif
    if (!dc.n_verts || !dc.fetch_ok) { g_ld_stats.group_drop_fetch++; return; }

    /* Preserve validated occurrence order while allowing repeated compact
     * vertex references to share one fetched and uploaded payload. */
    int indexed = 0;
    if (!rsx_vertex_topology_plan(prim, dc.refs_remapped, &indexed)) {
        g_ld_stats.group_drop_primitive++;
        return;
    }
    u32 n_tri = indexed
        ? topology_index_count(prim)
        : dc.n_source_refs - dc.n_source_refs % 3;

    if (!n_tri) {
        g_ld_stats.group_drop_degenerate++;
        return;
    }

    live_a010_geom_trace(prim, n_tri);

    const u32 vertex_stride =
        ld_vertex_compact_payload() ? used_layout.stride : VERT_STRIDE;
    const u32 uploaded_vertices = indexed ? dc.n_verts : n_tri;
    const void* vertex_data = ld_vertex_compact_payload()
        ? (const void*)dc.compact_verts : (const void*)dc.verts;
    const u64 draw_vb_bytes = (u64)uploaded_vertices * vertex_stride;
    const u64 draw_ib_bytes = indexed ? (u64)n_tri * sizeof(u32) : 0;
    const u64 vb_capacity = VERT_BANK_SIZE;
    if (draw_vb_bytes > vb_capacity || draw_ib_bytes > INDEX_BANK_SIZE) {
        live_draw_csv_emit(prim, n_tri, "drop_vbring_oversize");
        g_ld_stats.group_drop_ring++;
        return;
    }
    if ((u64)g.vb_used + draw_vb_bytes > ld_vb_end() ||
        (u64)g.ib_used + draw_ib_bytes > ld_ib_end()) {
        /*
         * The replay-proven renderer recycles this upload ring mid-frame.
         * Live used to drop every remaining draw instead, which discarded
         * 181/184 sampled a010 groups after the orphanage workload filled the
         * 256K-vertex ring.  We are still before this draw's memcpy, so it is
         * safe to submit/wait and reuse every transient draw ring here.
         */
        ld_flush(LD_FLUSH_VERTEX_RING);
        ld_rings_reset();
    }
    if (draw_vb_bytes)
        memcpy(
            g.vb_mapped + g.vb_used, vertex_data,
            (size_t)draw_vb_bytes);
    if (indexed)
        write_topology_indices(
            prim, (u32*)((u8*)g.ib_mapped + g.ib_used));

    ld_probe_init();
    g_ld_probe_variant = 0;
    if (g_ld_probe_key_count)
    {
        const u64 probeKey = live_legacy_pso_key();
        g_ld_probe_variant = ld_probe_choose_variant(probeKey, prim);
        ld_probe_draw(probeKey, prim);
    }
    const u64 shaderBegin = ld_perf_ticks();
    ID3D12PipelineState* pso = get_pso(
        ld_vertex_mask_signature() ? &used_layout : NULL,
        ld_vertex_compact_payload());
    g_ld_draw_perf.shaderTicks += ld_perf_ticks() - shaderBegin;
    if (!pso) {
#if defined(YZ_PERF_PROFILE)
        ld_profile_note_rejected_pso();
#endif
        live_draw_csv_emit(prim, n_tri, "drop_pso");
        g_ld_stats.group_drop_pso++;
        return;   /* no fallback in live path */
    }
    u32 vertex_cb_offset = 0;
    int vertex_cb_plan = rsx_vertex_constant_ring_plan(
        g.cb_used, ld_cb_end(), CB_BLOCK_ALIGNED, &vertex_cb_offset);
    if (vertex_cb_plan == 0) {
#if !defined(YZ_PERF_CLEAN)
        g_ld_vertex_constant_ring_recycles++;
        g_ld_frame_vertex_constant_ring_recycles++;
        yz_frontier_trace_emit(
            YZ_FT_PARITY_CB_RECYCLE, g_ld_frames, g_ld_last_present_target,
            g.cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED,
            (u32)g_ld_stats.groups_executed,
            g_ld_fifo_get, g_ld_fifo_put);
#endif
        ld_flush(LD_FLUSH_VERTEX_CONSTANT_RING);
        if (!g.ready)
            return;
        g.cb_used = ld_cb_base();
        vertex_cb_plan = rsx_vertex_constant_ring_plan(
            g.cb_used, ld_cb_end(), CB_BLOCK_ALIGNED,
            &vertex_cb_offset);
    }
    if (vertex_cb_plan != 1) {
        live_draw_csv_emit(prim, n_tri, "drop_cbring_invalid");
        g_ld_stats.group_drop_ring++;
        return;
    }
    g.cb_used = vertex_cb_offset;
    u32 ps_cb_offset = 0;
    if (!ld_upload_pixel_constants(&ps_cb_offset)) {
        live_draw_csv_emit(prim, n_tri, "drop_ps_cbring");
        g_ld_stats.group_drop_ring++;
        return;
    }

    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        live_draw_csv_emit(prim, n_tri, "drop_surface");
        g_ld_stats.group_drop_surface++;
        return;
    }
    /* LD_SKIP_SAMPLING=<hex offset>: drop draws that sample a texture at that
     * offset. Debug only -- removing a post-process pass shows whether the
     * scene under it has any colour at all. */
    { static int64_t skip = -2;
      if (skip == -2) { const char* e = getenv("LD_SKIP_SAMPLING"); skip = e ? (int64_t)strtoul(e, 0, 16) : -1; }
      if (skip >= 0)
          for (u32 u = 0; u < 16; u++) {
              rsx_dsp_texture st; rsx_dsp_get_texture(&g.rsx, u, &st);
              if (st.enabled && st.offset == (u32)skip) return;
          } }
    /* LD_TRACE_PSO=<hex key>: raw vertex attributes + unit-0 texels of the
     * first three draws with that CSV pso_key. */
    { static u64 tk = 1; static int tn;
      if (tk == 1) { const char* e = getenv("LD_TRACE_PSO"); tk = e ? _strtoui64(e, 0, 16) : 0; }
      if (tk && live_legacy_pso_key() == tk && tn >= 3) {
          static u32 every;
          if (dc.inl && (++every % 60) == 0)
              fprintf(stderr, "[pso-trace] frame=%u col=%02X%02X%02X%02X\n", g_ld_frames,
                      dc.inl[16], dc.inl[17], dc.inl[18], dc.inl[19]);
      }
      /* LD_TRACE_PSO_FROM=<frame>: the first three draws at or after that
       * frame -- a PSO's first uses can predate the data it later draws. */
      static long tfrom = -1;
      if (tfrom < 0) { const char* e = getenv("LD_TRACE_PSO_FROM"); tfrom = e ? atol(e) : 0; }
      if (tk && tn < 3 && live_legacy_pso_key() == tk && (long)g_ld_frames >= tfrom) {
          tn++;
          if (dc.inl) {
              fprintf(stderr, "[pso-trace] inline stride=%u bytes=%u:", dc.inl_stride, dc.inl_bytes);
              for (u32 k = 0; k < dc.inl_bytes && k < 6 * dc.inl_stride; k++)
                  fprintf(stderr, "%s%02X", (k % dc.inl_stride) ? "" : " |", dc.inl[k]);
              fprintf(stderr, "\n");
          }
          for (u32 ai = 0; ai < 16; ai++) {
              rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, ai, &va);
              if (!va.type) continue;
              fprintf(stderr, "[pso-trace] attr%u type=%u size=%u stride=%u loc=%u off=0x%X:",
                      ai, va.type, va.size, va.stride, va.location, va.offset);
              for (u32 vi = 0; vi < 6; vi++) {
                  const u8* p = guest_ptr(va.location, va.offset + vi * va.stride, 16);
                  if (!p) break;
                  fprintf(stderr, " |");
                  for (u32 k = 0; k < (va.type == RSX_VTX_TYPE_FLOAT ? 4u * va.size : 4u) && k < 16; k++)
                      fprintf(stderr, "%02X", p[k]);
              }
              fprintf(stderr, "\n");
          }
          /* The raw array registers and base (the attrN lines above print the
           * decoded offset only), and the constant colour a disabled attr3
           * feeds -- the two things a draw that vanishes is usually missing. */
          fprintf(stderr, "[pso-trace] vdata base_offset=0x%X base_index=%u raw attr0 off=0x%08X fmt=0x%08X\n",
                  rsx_dsp_vertex_data_base_offset(&g.rsx), rsx_dsp_vertex_data_base_index(&g.rsx),
                  rsx_dsp_reg(&g.rsx, 0x1680), rsx_dsp_reg(&g.rsx, 0x1740));
          { float d3[4]; rsx_dsp_vertex_default(&g.rsx, 3, d3);
            fprintf(stderr, "[pso-trace] attr3 const=%g %g %g %g\n", d3[0], d3[1], d3[2], d3[3]); }
          /* Every non-zero transform constant (raw words), so the VS can be
           * evaluated offline against exactly what this draw saw. */
          for (u32 q = 0; q < 512; q++) {
              const u32* w = g.rsx.constants[q];
              if (!(w[0] | w[1] | w[2] | w[3])) continue;
              fprintf(stderr, "[pso-const] %u %08X %08X %08X %08X\n", q, w[0], w[1], w[2], w[3]);
          }
          for (u32 ai = 0; ai < 16; ai++) {   /* the constant value each attribute falls back to */
              float d[4]; rsx_dsp_vertex_default(&g.rsx, ai, d);
              fprintf(stderr, "[pso-vdef] %u %g %g %g %g\n", ai, d[0], d[1], d[2], d[3]);
          }
          { u32 fl = 0; const u32 fo = rsx_dsp_fragment_program(&g.rsx, &fl);
            const u8* fp = guest_ptr(fl, fo, 16 * 16);
            if (fp) { fprintf(stderr, "[pso-trace] fp %u:0x%X:", fl, fo);
                      for (u32 k = 0; k < 16 * 16; k++) fprintf(stderr, "%s%02X", (k & 15) ? "" : " ", fp[k]);
                      fprintf(stderr, "\n"); } }
          rsx_dsp_texture t0; rsx_dsp_get_texture(&g.rsx, 0, &t0);
          const u8* tp = guest_ptr(t0.location, t0.offset, 32);
          if (tp) { { rsx_dsp_texture t1; rsx_dsp_get_texture(&g.rsx, 1, &t1); fprintf(stderr, "[pso-trace] outmask=0x%X inmask=0x%X tex0 fmt=0x%X remap=0x%X wrap=0x%X border=0x%08X filter=0x%X tex1 remap=0x%X wrap=0x%X:", rsx_dsp_reg(&g.rsx, 0x1FF4), rsx_dsp_reg(&g.rsx, 0x1FF0), t0.format, t0.remap, t0.wrap, rsx_dsp_reg(&g.rsx, 0x1A1C), t0.filter, t1.remap, t1.wrap); }
                    for (u32 k = 0; k < 32; k++) fprintf(stderr, "%02X", tp[k]);
                    fprintf(stderr, "\n"); }
      } }
    /* LD_FORCE_VA=1: debug -- inline draws whose UB4 vertex colour (attr 3)
     * has alpha 0 everywhere get alpha 255, to see what a faded-out element is. */
    { static int fva = -1; if (fva < 0) fva = getenv("LD_FORCE_VA") ? 1 : 0;
      if (fva && dc.inl && dc.inl_stride) {
          rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, 3, &va);
          if (va.type == RSX_VTX_TYPE_UNORM8 && va.size == 4) {
              u32 amax = 0;
              for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                  if (dc.inl[o] > amax) amax = dc.inl[o];
              if (!amax)
                  for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                      ((u8*)dc.inl)[o] = 255;
          } } }
    /* LD_SKIP_PSO=<hex key>: drop draws with that CSV pso_key. Debug only. */
    { static u64 skipk = 1;
      if (skipk == 1) { const char* e = getenv("LD_SKIP_PSO"); skipk = e ? _strtoui64(e, 0, 16) : 0; }
      if (skipk && live_legacy_pso_key() == skipk) return; }
    /* MRT: colour targets B..D, resolved (and created) here, before any of
     * this draw's commands are recorded. They share A's format and clip. */
    u32 mrt[4] = { target, 0, 0, 0 };
    u32 n_mrt = 1;
    {
        rsx_dsp_surface msf;
        rsx_dsp_get_surface(&g.rsx, &msf);
        const u32 want = ld_rt_count(msf.color_target);
        for (u32 r = 1; r < want; r++) {
            const u32 s = surface_get(msf.color_location[r], msf.color_offset[r],
                                      msf.clip_w, msf.clip_h,
                                      msf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16
                                          ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                          : DXGI_FORMAT_R8G8B8A8_UNORM);
            if (s == LD_INVALID_SURFACE || s == target) break;
            mrt[n_mrt++] = s;
        }
    }
    live_draw_csv_emit(prim, n_tri, "execute");
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    { static u32 last_target = LD_INVALID_SURFACE;
      if (target != last_target) { ld_trace_target("draw", target, 0); last_target = target; } }
    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 current_zslot = g.depth
        ? zdepth_get(sf.zeta_location, sf.zeta_offset, sf.clip_w, sf.clip_h)
        : 0;
    const u64 bindBegin = ld_perf_ticks();
    u32 slots[SRV_TABLE_SIZE], smp_slots[SMP_TABLE_SIZE];
    u32 surf_used[SRV_TABLE_SIZE], n_surf_used = 0;
    u32 zdepth_used[SRV_TABLE_SIZE], n_zdepth_used = 0;
    u32 texture_mask = 0;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) slots[u] = SRV_WHITE;
    for (u32 u = 0; u < SMP_TABLE_SIZE; u++) smp_slots[u] = SMP_DEFAULT;
    /* A draw that samples its own zeta while not writing depth (deferred
     * lighting reading the scene depth it also depth-tests against, soft
     * particles) reads the snapshot: the zeta does not change during such a
     * draw, so that is what RSX returns. Before, it fell back to guest VRAM,
     * which the host-side depth never reaches (depth 0). A draw that writes
     * the zeta it reads keeps that fallback. */
    const int self_zeta_read = current_zslot &&
        !((rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u) &&
          (rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1u));
    /* LD_UNITS=1: every enabled texture unit of every draw that samples PS1
     * VRAM, once per distinct combination.
     *
     * ps1_netemu's display composite is a Cg program picked from seven
     * (CG_fp_gradient / orientation / mofix / smart / upscale / upscale_smart /
     * sharpen) and its uniforms are texture0, texture1 and hwidth -- TWO
     * textures. Every probe here so far has looked at unit 0 only, so "the
     * renderer reads the right bytes" was never actually tested for the second
     * one. This lists them. */
    { static int lu = -1; static u32 seen[8]; static int ns;
      if (lu < 0) lu = getenv("LD_UNITS") ? 1 : 0;
      if (lu) {
          u32 key = 0; int ps1 = 0;
          for (u32 q = 0; q < SRV_TABLE_SIZE; q++) {
              rsx_dsp_texture tq; rsx_dsp_get_texture(&g.rsx, q, &tq);
              if (!tq.enabled) continue;
              key = key * 31u + (tq.offset ^ (tq.format << 8) ^ (q << 24));
              if (tq.location == 1u && tq.offset >= 0x400000u &&
                  tq.offset < 0x480000u) ps1 = 1;
          }
          if (ps1) {
              int fresh = 1;
              for (int z = 0; z < ns; z++) if (seen[z] == key) fresh = 0;
              if (fresh && ns < 8) {
                  seen[ns++] = key;
                  fprintf(stderr, "[units] draw sampling PS1 VRAM:");
                  for (u32 q = 0; q < SRV_TABLE_SIZE; q++) {
                      rsx_dsp_texture tq; rsx_dsp_get_texture(&g.rsx, q, &tq);
                      if (!tq.enabled) continue;
                      fprintf(stderr, " u%u[%u:0x%08X fmt=0x%02X %ux%u p=%u]",
                              q, tq.location, tq.offset, tq.format,
                              tq.width, tq.height, tq.pitch);
                  }
                  fprintf(stderr, "\n");
              }
          }
      } }
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        /* LD_PS1_BUF0=1 -- DIAGNOSTIC, not a fix. Force a 24-bit PS1 VRAM
         * bind at buffer 1 (offset 0x4003C0) to sample buffer 0 (0x400000).
         *
         * After the intro FMV both the driven and undriven paths end on a blank
         * screen, and during it every bind is 0x4003C0 while the content that
         * exists -- the decoded FMV frame -- sits in buffer 0. That is the shape
         * of a display flip parked on the buffer the game is not drawing into.
         * If forcing buffer 0 puts a picture back on screen the flip is the bug;
         * if the screen stays blank it is not. Nothing else distinguishes those
         * two, and the override is one line. */
        { static int fb0 = -1;
          if (fb0 < 0) fb0 = getenv("LD_PS1_BUF0") ? 1 : 0;
          if (fb0 && t.location == 1u && t.offset == 0x4003C0u)
              t.offset = 0x400000u; }
        if (!t.enabled) continue;
        /* A unit the fragment program never samples keeps its white
         * placeholder: games leave stale textures enabled on every unit
         * (Folklore: eight per draw, one or two sampled), and resolving each
         * one -- cache lookups, content hashes, surface searches and the
         * barriers a stale render target needs -- was most of the per-draw
         * cost of its particle-heavy scenes. LD_ALL_TEXTURE_UNITS=1 binds
         * every enabled unit as before. */
        { static int allUnits = -1;
          if (allUnits < 0)
          {
              const char* text = getenv("LD_ALL_TEXTURE_UNITS");
              allUnits = text && text[0] && text[0] != '0' ? 1 : 0;
          }
          if (!allUnits && !((g_ld_current_pso.ps_unit_mask >> u) & 1u))
          {
              continue;
          } }
        texture_mask |= 1u << u;
        smp_slots[u] = sampler_slot(&t, sampler_key(&t));
        /* Pick WHICH surface at this offset to sample.
         *
         * Matching on (location, offset) alone was fine when only one surface
         * could exist per offset. Now that surfaces are keyed on size too --
         * they have to be, or the two passes destroy each other's contents --
         * several can, and taking the first match by offset takes an arbitrary
         * one. ps1_netemu composites the PS1 framebuffer into a 720x512 surface
         * at offset 0x0 and then upscales into a 1280x720 output; the upscale
         * pass samples offset 0x0 and was getting the EMPTY 1280x720 surface
         * that happens to sit earlier in the table.
         *
         * Prefer the surface whose dimensions match what the texture
         * descriptor actually declares; failing that, the most recently drawn
         * one, which is the only other defensible answer. */
        int sampled = -1;
        /* An address can be both a colour target and a zeta (a shadow map
         * pass that binds the same memory as both, colour writes masked).
         * A depth-format read of an address with a written host zeta is a
         * depth read: try the zeta first. Before, the colour surface at the
         * same address was taken (zeros). */
        int zeta_pref = 0;
        const int zetaAliasIndex = ((g_ld_current_pso.cube_mask >> u) & 1u) ? -1 : ld_zeta_alias_index(&t);
        if (zetaAliasIndex >= 0)
        {
            zeta_pref = 1;
        }
        if ((t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM) == TEX_FMT_DEPTH24_D8)
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset &&
                    (g.zdepths[i].had_write || g.zdepths[i].snapshot_valid)) {
                    zeta_pref = 1;
                    break;
                }
        if (!zeta_pref) { u32 best_gen = 0;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              const surface_t* sf_i = &g.surfaces[i];
              if (sf_i->location != t.location || sf_i->offset != t.offset ||
                  i == target)
                  continue;
              if (sf_i->w == t.width && sf_i->h == t.height) { sampled = (int)i; break; }
              if (sampled < 0 || sf_i->last_draw_generation > best_gen) {
                  sampled = (int)i;
                  best_gen = sf_i->last_draw_generation;
              }
          } }
        /* LD_UVDBG=1: the texture coordinates of the draw that samples the PS1
         * framebuffer. Placed here rather than in fetch_batches because that
         * function is not on this draw's path -- probing it printed nothing at
         * all, which is how that was established.
         *
         * This is the last unmeasured link: the PS3 side binds all 1024x512 of
         * PS1 VRAM as ONE texture, so which part reaches the screen is decided
         * entirely by these UVs, and every pixel the PS1 draws lives at
         * x >= 640 while columns 0..639 are black. */
        { static int uvd = -1;
          if (uvd < 0) uvd = getenv("LD_UVDBG") ? 1 : 0;
          if (uvd && t.location == 1u && t.offset == 0x400000u) {
              static unsigned long n = 0;
              if (n++ < 4) {
                  const u32 vbase = rsx_dsp_vertex_data_base_offset(&g.rsx);
                  fprintf(stderr, "[uvdbg] unit=%u ps1fb %ux%u pitch=%u fmt=0x%02X"
                                  " base=0x%X target=%u\n",
                          u, t.width, t.height, t.pitch, t.format,
                          vbase, target);
                  /* The GPU state that can silently discard a draw. Clears are
                   * visible on these surfaces and the draws are not, so one of
                   * these is throwing the fragments away. */
                  { const u32 sch = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
                    const u32 scv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
                    fprintf(stderr, "[uvdbg]   vp=(%u,%u %ux%u) scale=(%.2f %.2f)"
                                    " xlate=(%.2f %.2f) scissor=(%u+%u, %u+%u)"
                                    " depth_test=%u zwrite=%u zfunc=0x%X"
                                    " colmask=0x%08X blend=%u cull=%u/%u"
                                    " surf=%ux%u\n",
                            vp.x, vp.y, vp.w, vp.h,
                            vp.scale[0], vp.scale[1],
                            vp.translate[0], vp.translate[1],
                            sch & 0xFFFFu, sch >> 16, scv & 0xFFFFu, scv >> 16,
                            rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x0A78) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x0A7C),
                            rsx_dsp_reg(&g.rsx, M_COLOR_MASK),
                            rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x1918) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x090C),
                            sf.clip_w, sf.clip_h);
                    /* The PS1 framebuffer's fragment program unpacks the
                     * sampled value arithmetically (NV40 has no integer ops):
                     *
                     *   r0 = sample(tex0, tc0 * (1/1024, 1/512))
                     *   r0 = r0 * fp_constants[0].x + fp_constants[0].y
                     *   r0 = floor(r0) / 8.0
                     *   r0 = sign-preserving truncate
                     *   r0 = r0 * fp_constants[1].x + fp_constants[1].y
                     *
                     * So the output depends entirely on those two constants.
                     * Wrong or absent, it produces exactly the repeating stripe
                     * pattern the composite currently shows. */
                    fprintf(stderr, "[uvdbg]   fp_const count=%u mode=%c"
                                    " c0=(%.6f %.6f %.6f %.6f)"
                                    " c1=(%.6f %.6f %.6f %.6f)\n",
                            g.fp_constants.count, g.fp_constant_mode,
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][0] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][1] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][2] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][3] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][0] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][1] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][2] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][3] : 0u));
                  }
                  for (u32 vi = 0; vi < 4; vi++) {
                      float pos[4] = {0,0,0,1}, tc[4] = {0,0,0,1};
                      const int okp = fetch_attr(0, vbase, vi, 0, pos);
                      const int okt = fetch_attr(8, vbase, vi, 0, tc);
                      fprintf(stderr, "[uvdbg]   v%u pos%s(%.1f %.1f) tc%s(%.4f %.4f)\n",
                              vi, okp ? "" : "!", pos[0], pos[1],
                              okt ? "" : "!", tc[0], tc[1]);
                  }
              }
          } }
        if (sampled < 0 && LD_DIAG_ENABLED("LD_ALIAS_DBG")) {
            static u32 n_dbg = 0;
            if (n_dbg++ < 24) {
                fprintf(stderr, "[alias-miss] tex %u:0x%08X fmt=0x%02X %ux%u target=%u surf:",
                        t.location, t.offset, t.format, t.width, t.height, target);
                for (u32 i = 0; i < g.n_surfaces; i++)
                    fprintf(stderr, " [%u]%u:0x%08X %ux%u", i, g.surfaces[i].location,
                            g.surfaces[i].offset, g.surfaces[i].w, g.surfaces[i].h);
                fprintf(stderr, "\n");
            }
        }
        if (sampled < 0 && !zeta_pref && g.surfaces[target].location == t.location &&
            g.surfaces[target].offset == t.offset) {
            /* Reads its own render target: sample a copy of it. */
            slots[u] = surface_feedback_srv(target);
            g_ld_bind_surf++;
        } else if (sampled >= 0) {
            /* LD_ALIAS_DBG also reports the HIT. A texture that aliases a render
             * surface when it should not is exactly as interesting as one that
             * fails to alias when it should, and only the miss was visible --
             * so a title whose every bind resolves to a surface (binds[real=0
             * surf=N] in the frame stats) had no way to show WHY. Guitar Hero
             * III does that: every bind lands on a surface and not one real
             * texture is ever created, so the screen stays black. */
            static int alias_dbg = -1;
            if (alias_dbg < 0) alias_dbg = getenv("LD_ALIAS_DBG") != NULL;
            if (alias_dbg) {
                static u32 n_hit = 0;
                if (n_hit++ < 24)
                    fprintf(stderr, "[alias-hit] tex %u:0x%08X fmt=0x%02X %ux%u "
                            "-> surface[%d] %u:0x%08X %ux%u" "\n",
                            t.location, t.offset, t.format, t.width, t.height,
                            sampled, g.surfaces[sampled].location,
                            g.surfaces[sampled].offset,
                            g.surfaces[sampled].w, g.surfaces[sampled].h);
            }
            slots[u] = SRV_SURFACE_BASE + sampled;
            g_ld_bind_surf++;
            int seen = 0;
            for (u32 k = 0; k < n_surf_used; k++) if (surf_used[k] == (u32)sampled) seen = 1;
            if (!seen && n_surf_used < SRV_TABLE_SIZE) surf_used[n_surf_used++] = (u32)sampled;
        } else {
            int sampled_depth = -1;
            const u32 texture_base_fmt =
                t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
            /* Mirror the replay-proven depth-RT contract: address and format
             * must match, and the pass must have executed a depth-writing draw.
             * Clear-only zetas intentionally fall through to guest VRAM. */
            if (texture_base_fmt == TEX_FMT_DEPTH24_D8 || zetaAliasIndex >= 0)
                for (u32 i = 0; i < g.n_zdepths; i++)
                    if (g.zdepths[i].location == t.location &&
                        g.zdepths[i].offset == t.offset &&
                        (current_zslot != 1 + i || self_zeta_read)) {
                        if (zdepth_snapshot(1 + i))
                            sampled_depth = (int)i;
                        else {
                            g_ld_zdepth_srv_reject_no_write++;
                            if (!g.zdepths[i].reject_logged &&
                                rsx_live_draw_a010_probe_active()) {
                                g.zdepths[i].reject_logged = 1;
                                fprintf(stderr,
                                    "[zetatrack] a010 reject #%u %u:0x%X "
                                    "draws=%llu dtest=%llu dwrite=%llu "
                                    "both=%llu snapshot=%d\n",
                                    i, g.zdepths[i].location,
                                    g.zdepths[i].offset,
                                    (unsigned long long)g.zdepths[i].draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_test_draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_write_draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_both_draws,
                                    g.zdepths[i].snapshot_valid);
                            }
                        }
                        break;
                    }
            if (sampled_depth >= 0) {
                slots[u] = SRV_ZDEPTH_BASE + sampled_depth;
                g_ld_zdepth_srv_binds++;
                int seen = 0;
                for (u32 k = 0; k < n_zdepth_used; k++)
                    if (zdepth_used[k] == (u32)sampled_depth) seen = 1;
                if (!seen && n_zdepth_used < SRV_TABLE_SIZE)
                    zdepth_used[n_zdepth_used++] = (u32)sampled_depth;
            } else {
                slots[u] = (g_ld_probe_variant == 3 && u == 0) ? SRV_WHITE : texture_srv_slot(&t);
                if (slots[u] == SRV_WHITE) g_ld_bind_white++; else g_ld_bind_real++;
            }
        }
    }

    const u32 vtex_mask = vertex_texture_mask();
    u32 vtex_slots[SRV_TABLE_SIZE];
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++)
        vtex_slots[u] = SRV_WHITE;
    /* Resolve and upload every vertex texture before recording this draw's
     * graphics state.  Cache refresh can retire a texture and flush the open
     * command list; doing that after SetPipelineState/root bindings silently
     * loses those bindings on the replacement list. */
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        vtex_slots[u] = vertex_texture_srv_slot(&vt);
        if (vtex_slots[u] != SRV_WHITE)
            g_ld_vtex_binds++;
        else
            g_ld_vtex_unsupported++;
    }
    const u32 required_srv_tables = 1u + (vtex_mask != 0);
    if (g.srv_ring_used + required_srv_tables > ld_srv_end() ||
        sampler_table_needs_flush(smp_slots)) {
        /* Every descriptor referenced by the old command list is consumed
         * before recycling its shader-visible heap entries. Texture upload
         * commands recorded while resolving slots are also completed here;
         * no render-state command for this draw has been recorded yet. */
        ld_flush(LD_FLUSH_DESCRIPTOR_RING);
        if (!g.ready) return;
        g.srv_ring_used = ld_srv_base();
    }
    g_ld_draw_perf.bindTicks += ld_perf_ticks() - bindBegin;

    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv; int have_dsv = 0;
    if (g.depth) {
        dsv = dsv_handle(current_zslot);
        have_dsv = 1;
        if (current_zslot) {
            zdepth_t* z = &g.zdepths[current_zslot - 1];
            if (!z->cleared) {
                g.list->lpVtbl->ClearDepthStencilView(
                    g.list, dsv,
                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    1.0f, 0, 0, NULL);
                z->cleared = 1;
                z->had_write = 0;
                g_ld_stats.implicit_depth_clears++;
            }
        } else if (!g.depth_cleared) {
            g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
            g.depth_cleared = 1;
            g_ld_stats.implicit_depth_clears++;
        }
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
        rtvs[0] = rtv;
        for (u32 r = 1; r < n_mrt; r++)
            rtvs[r] = rtv_handle(LD_SWAP_BUFFERS + mrt[r]);
        g.list->lpVtbl->OMSetRenderTargets(g.list, n_mrt, rtvs, FALSE,
                                           have_dsv ? &dsv : NULL);
    }
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    const D3D12_GPU_DESCRIPTOR_HANDLE stbl = sampler_table(smp_slots);
    D3D12_GPU_DESCRIPTOR_HANDLE vtex_table = {0};
    if (vtex_mask)
        vtex_table = srv_table(vtex_slots);
    u64 descriptor_signature =
        fnv1a(slots, sizeof(slots), 1469598103934665603ull);
    descriptor_signature =
        fnv1a(smp_slots, sizeof(smp_slots), descriptor_signature);
    if (vtex_mask)
        descriptor_signature =
            fnv1a(vtex_slots, sizeof(vtex_slots), descriptor_signature);

    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    /* The guest viewport is in pixels; an antialiased surface's clip is in
     * samples (aa_sx x aa_sy per pixel), so map NDC over the pixel extent. */
    const float Wv = W / (float)sf.aa_sx;
    const float Hv = H / (float)sf.aa_sy;
    float xf[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    u8* cbdst = g.cb_mapped + g.cb_used;
    memcpy(cbdst, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16);
    memcpy(cbdst + 512 * 16, xf, sizeof(xf));

    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.rootsig_x);
    g.list->lpVtbl->SetGraphicsRootConstantBufferView(
        g.list, 0, g.cb->lpVtbl->GetGPUVirtualAddress(g.cb) + g.cb_used);
    if (ld_fp_constants_buffered())
        g.list->lpVtbl->SetGraphicsRootConstantBufferView(
            g.list, 4,
            g.ps_cb->lpVtbl->GetGPUVirtualAddress(g.ps_cb) +
                ps_cb_offset);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 2, stbl);
    if (vtex_mask)
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(
            g.list, 3, vtex_table);
    g.cb_used += CB_BLOCK_ALIGNED;

    /* Guest pixels; scaled to the host surface just before they are set. */
    const surface_t* drawSurface = &g.surfaces[target];
    const float renderScale = (float)(drawSurface->scale ? drawSurface->scale : 1u);
    const float renderScaleX = drawSurface->scale_x > 0.0f ? drawSurface->scale_x : 1.0f;
    /* A 1x post-effect target with a canvas-sized (scaled) zeta bound: fine
     * while the pass ignores depth, which is what such passes do. Say so if
     * one does not -- its depth test would read the wrong part of the zeta. */
    if (current_zslot && !ld_surface_is_canvas_scale(drawSurface) &&
        ((rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u) || (rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1u)))
    {
        static u32 depthScaleLogs = 0;
        if (depthScaleLogs++ < 8)
        {
            fprintf(stderr, "[live-draw] depth-tested draw into 1x surface 0x%X %ux%u with a %ux zeta\n",
                    drawSurface->offset, drawSurface->w, drawSurface->h, g.scale);
        }
    }
    D3D12_VIEWPORT dvp = {0, 0, W * renderScaleX, H * renderScale, 0.0f, 1.0f};
    D3D12_RECT sc = {
        0, 0,
        (LONG)(g.surfaces[target].w ? g.surfaces[target].w : g.width),
        (LONG)(g.surfaces[target].h ? g.surfaces[target].h : g.height)
    };
    /* Guest scissor, intersected with the surface rect. The nv40 reset value
     * is a full-window 4096x4096; never-written regs read 0 here, so an
     * unwritten scissor means "no scissor". A WRITTEN zero width or height is
     * a real, empty scissor and must clip everything: GH3's gameplay venue
     * sits under a ViewportElement whose clip window is off-screen (scissor
     * x=1039 w=0), and drawing it full-screen painted its never-rendered
     * texture over the whole scene. */
    {
        const u32 sh = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 sv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        /* The scissor is in pixels too: scale it to the sample grid. */
        const LONG ax = (LONG)sf.aa_sx, ay = (LONG)sf.aa_sy;
        const LONG sx = (LONG)(sh & 0xFFFFu) * ax, sw = (LONG)(sh >> 16) * ax;
        const LONG sy = (LONG)(sv & 0xFFFFu) * ay, svh = (LONG)(sv >> 16) * ay;
        const int written = g.rsx.seen[M_SCISSOR_HORIZONTAL >> 2] != 0 ||
                            g.rsx.seen[M_SCISSOR_VERTICAL >> 2] != 0;
        if (written) {
            if (sx > sc.left)            sc.left   = sx;
            if (sy > sc.top)             sc.top    = sy;
            if (sx + sw  < sc.right)     sc.right  = sx + sw;
            if (sy + svh < sc.bottom)    sc.bottom = sy + svh;
            if (sc.right  < sc.left)     sc.right  = sc.left;
            if (sc.bottom < sc.top)      sc.bottom = sc.top;
        }
    }
    sc.left = (LONG)ld_surface_x(drawSurface, (u32)sc.left);
    sc.top = (LONG)ld_surface_scaled(drawSurface, (u32)sc.top);
    sc.right = (LONG)ld_surface_x(drawSurface, (u32)sc.right);
    sc.bottom = (LONG)ld_surface_scaled(drawSurface, (u32)sc.bottom);
    g.list->lpVtbl->RSSetViewports(g.list, 1, &dvp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    /* Dynamic stencil reference (kept out of the PSO key on purpose). */
    g.list->lpVtbl->OMSetStencilRef(
        g.list, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);
    ld_set_blend_factor();

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = g.vb->lpVtbl->GetGPUVirtualAddress(g.vb) + g.vb_used;
    vbv.StrideInBytes = vertex_stride;
    vbv.SizeInBytes = (u32)draw_vb_bytes;
    if (vertex_stride)
        g.list->lpVtbl->IASetVertexBuffers(g.list, 0, 1, &vbv);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    {
        const u64 serial = ++g_ld_recent_draw_total;
        ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        memset(draw, 0, sizeof(*draw));
        draw->serial = serial;
        draw->frame = g_ld_frames;
        draw->pso_key = g_ld_current_pso.key;
        draw->descriptor_signature = descriptor_signature;
        draw->vp_start = g_ld_current_pso.vp_start;
        draw->vp_instrs = g_ld_current_pso.vp_instrs;
        draw->fp_location = g_ld_current_pso.fp_location;
        draw->fp_offset = g_ld_current_pso.fp_offset;
        draw->fp_size = g_ld_current_pso.fp_size;
        draw->fp_control = g_ld_current_pso.fp_control;
        draw->cube_mask = g_ld_current_pso.cube_mask;
        draw->vtex_mask = g_ld_current_pso.vtex_mask;
        draw->txl_mask = g_ld_current_pso.txl_mask;
        draw->texture_mask = texture_mask;
        draw->primitive = prim;
        draw->vertices = n_tri;
        draw->target = target;
        draw->zslot = current_zslot;
        draw->clip_x = sf.clip_x;
        draw->clip_y = sf.clip_y;
        draw->clip_w = sf.clip_w;
        draw->clip_h = sf.clip_h;
        draw->srv_ring_used = g.srv_ring_used;
        draw->sampler_ring_used = g.smp_ring_used;
        draw->cb_used = g.cb_used;
        draw->vb_used = g.vb_used;
    }
    if (indexed) {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation =
            g.ib->lpVtbl->GetGPUVirtualAddress(g.ib) + g.ib_used;
        ibv.SizeInBytes = (u32)draw_ib_bytes;
        ibv.Format = DXGI_FORMAT_R32_UINT;
        g.list->lpVtbl->IASetIndexBuffer(g.list, &ibv);
        g.list->lpVtbl->DrawIndexedInstanced(
            g.list, n_tri, 1, 0, 0, 0);
    } else {
        g.list->lpVtbl->DrawInstanced(g.list, n_tri, 1, 0, 0);
    }
    ld_surface_note_write(target, LD_SURFACE_WRITE_DRAW);
    for (u32 r = 1; r < n_mrt; r++)
        ld_surface_note_write(mrt[r], LD_SURFACE_WRITE_DRAW);
    if (current_zslot) {
        render_state_t depth_state;
        decode_render_state(&depth_state);
        zdepth_t* z = &g.zdepths[current_zslot - 1];
        z->draws++;
        if (depth_state.depth_test) z->depth_test_draws++;
        if (depth_state.depth_write) z->depth_write_draws++;
        if (depth_state.depth_test && depth_state.depth_write)
            z->depth_both_draws++;
        /* A write-enable bit alone does not prove that this draw produced a
         * usable depth map.  With depth testing disabled RSX does not execute
         * the depth pass represented by this tracked zeta. */
        if (depth_state.depth_test && depth_state.depth_write)
            z->had_write = 1;
    }
    g_ld_stats.groups_executed++;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.input_vertices += dc.n_source_refs;
    g_ld_profile.total.expanded_vertices += n_tri;
    g_ld_profile.total.legacy_vertex_upload_bytes +=
        (u64)(prim == PRIM_TRIANGLES ? n_tri : dc.n_source_refs) *
        VERT_STRIDE;
    g_ld_profile.total.vertex_upload_bytes += draw_vb_bytes;
#endif

    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    /* A guest-side failure can occur before the next flip even though useful
     * orphanage draws have already reached internal world targets.  Preserve
     * a few progressively later surface snapshots directly from the draw
     * stream so presentation is not a prerequisite for visual diagnosis. */
    if (LD_DIAG_ENABLED("RSX_A010_EAGER_DUMP") &&
        InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0) {
        static u64 eager_origin = 0;
        static u32 eager_sample = 0;
        if (!eager_origin)
            eager_origin = g_ld_stats.groups_executed;
        if (eager_sample < 4u &&
            g_ld_stats.groups_executed >=
                eager_origin + (u64)eager_sample * 32u) {
            CreateDirectoryA("scratch\\a010_probe", NULL);
            for (u32 i = 0; i < g.n_surfaces; i++) {
                const u32 off = g.surfaces[i].offset;
                if (off != 0x00E40000u && off != 0x01800000u &&
                    off != 0x02D10000u && off != 0x02710000u &&
                    off != 0x01440000u)
                    continue;
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch\\a010_probe\\eager_%03u_group_%08llu_"
                         "surface_%02u_off_%08X.ppm",
                         eager_sample,
                         (unsigned long long)g_ld_stats.groups_executed,
                         i, off);
                ld_dump_surface_ppm(path, &g.surfaces[i]);
            }
            fprintf(stderr,
                    "[a010-eager-dump] sample=%u groups=%llu surfaces=%u\n",
                    eager_sample,
                    (unsigned long long)g_ld_stats.groups_executed,
                    g.n_surfaces);
            eager_sample++;
            fflush(stderr);
        }
    }
    g.vb_used += (u32)draw_vb_bytes;
    g.ib_used += (u32)draw_ib_bytes;
    ld_profile_note_ring_highwater();
}

static void sink_end(void* user, const rsx_dispatch* r)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin = ld_profile_qpc();
#endif
    const u64 drawBegin = ld_perf_ticks();
    sink_end_impl(user, r);
    g_ld_draw_perf.totalTicks += ld_perf_ticks() - drawBegin;
    g_ld_draw_perf.draws++;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.sink_end_qpc += (u64)(ld_profile_qpc() - begin);
#endif
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    g_ld_stats.clears++;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) { g_ld_stats.clear_drop_surface++; return; }
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    if (ld_target_trace_enabled()) {
        static u32 last_clear_target = LD_INVALID_SURFACE;
        const int changed = target != last_clear_target;
        if (changed) {
            ld_trace_target("clear-target", target, mask);
            last_clear_target = target;
        }
        /* A broken scene can issue tens of thousands of clears. Preserve the
         * opening sequence and a periodic heartbeat without turning tracing
         * itself into a scheduler perturbation. Target changes are always
         * emitted above. */
        if (g_ld_stats.clears <= 256 || (g_ld_stats.clears & 1023) == 0) {
            const u32 z = rsx_dsp_clear_zstencil(&g.rsx);
            fprintf(stderr,
                    "[rsx-clear] frame=%u n=%llu target=%u mask=0x%02X "
                    "argb=0x%08X z24=0x%06X stencil=0x%02X\n",
                    g_ld_frames, g_ld_stats.clears, target, mask,
                    rsx_dsp_clear_color(&g.rsx), z >> 8, z & 0xFF);
        }
    }
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#clear,%u,0x%X,mask=0x%02X,scissor=0x%08X/0x%08X,cmask=0x%08X,argb=0x%08X\n",
                g_ld_frames, g.surfaces[target].offset, mask,
                rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL),
                rsx_dsp_reg(&g.rsx, M_COLOR_MASK), rsx_dsp_clear_color(&g.rsx));
        fflush(s_csv_file);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G | RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                         (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f };
        /* LD_CLEAR_TEST=1: clear to magenta instead of the guest's colour.
         *
         * This validates the MEASUREMENT, not the renderer. LD_SURF_DUMP
         * reports nonblack=0 for every surface, and every conclusion drawn from
         * that depends on the readback actually being able to see a non-black
         * pixel. If magenta clears still read back as 0, the readback is broken
         * and those conclusions are void. Establish that X would be visible
         * before concluding X never happens. */
        { static int ct = -1;
          if (ct < 0) ct = getenv("LD_CLEAR_TEST") ? 1 : 0;
          if (ct) { col[0] = 1.0f; col[1] = 0.0f; col[2] = 1.0f; col[3] = 1.0f; } }
        g.list->lpVtbl->ClearRenderTargetView(g.list, rtv, col, 0, NULL);
        ld_surface_note_write(target, LD_SURFACE_WRITE_CLEAR);
    }
    if ((mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) && g.depth) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(zslot);
        D3D12_CLEAR_FLAGS clear_flags = (D3D12_CLEAR_FLAGS)0;
        if (mask & RSX_CLEAR_DEPTH)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_DEPTH);
        if (mask & RSX_CLEAR_STENCIL)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_STENCIL);
        /* ZSTENCIL_CLEAR_VALUE is laid out as the zeta is: depth24 << 8 |
         * stencil8 for Z24S8, a bare depth16 for Z16 (SURFACE_FORMAT depth
         * field 1). Read as Z24S8, a Z16 surface's 0xFFFF -- its far plane --
         * would clear it to 255/16777215, nearly the near plane, and every
         * LESS or LEQUAL draw into it would then fail. The nv40 reset
         * 0xFFFFFF00 seeded by rsx_dispatch_init clears to 1.0 / 0. Same
         * decode as the draw engine's clear. */
        {
            const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
            const int z16 = sf.depth_format == 1u;
            const float cd = !zs ? 1.0f
                           : z16 ? (float)(zs & 0xFFFFu) / 65535.0f
                                 : (float)(zs >> 8) / 16777215.0f;
            g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
                clear_flags, cd, z16 ? (UINT8)0 : (UINT8)(zs & 0xFFu), 0, NULL);
        }
        if (zslot) {
            g.zdepths[zslot - 1].cleared = 1;
            if (mask & RSX_CLEAR_DEPTH) {
                g.zdepths[zslot - 1].had_write = 0;
                /* Each guest zeta owns a distinct live resource.  A clear
                 * invalidates its older published image; do not resolve it
                 * speculatively.  The first later texture consumer resolves
                 * the newly written pass exactly once. */
                g.zdepths[zslot - 1].snapshot_valid = 0;
            }
        } else {
            if (mask & RSX_CLEAR_DEPTH)
                g.depth_cleared = 1;
        }
    }
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    const u32 buffer_id = arg & 7u;
    g_ld_flip_requested++;
    g_ld_last_requested_buffer = buffer_id;
    if (g_ld_movie_mode) {
        if (ld_movie_composite_ui_enabled()) ld_movie_capture_overlay();
        g_ld_flip_consumed++;
        g_ld_last_consumed_buffer = buffer_id;
        return;
    }
    rsx_live_draw_present(buffer_id);
    g_ld_flip_consumed++;
    g_ld_last_consumed_buffer = buffer_id;
}

/* ---------------------------------------------------------------------------
 * device / resource setup
 * -----------------------------------------------------------------------*/
/* ---------------------------------------------------------------------------
 * Present pass: the presented image drawn into the swap chain's back buffer
 * with a full-screen triangle instead of CopyResource, so the back buffer can
 * be any size (the window's client area) and the image any size (the render
 * scale). Three pixel shaders: plain bilinear, FXAA, and a box filter for
 * downsampling a supersampled image (render scale above the window size).
 * -----------------------------------------------------------------------*/
static const char k_ld_present_hlsl[] =
    "Texture2D<float4> sourceImage : register(t0);\n"
    "SamplerState linearClamp : register(s0);\n"
    "cbuffer PresentConstants : register(b0)\n"
    "{\n"
    "    float4 sourceSize;   /* w, h, 1/w, 1/h of the image */\n"
    "    float4 targetSize;   /* w, h, 1/w, 1/h of the back buffer */\n"
    "    float4 presentOptions;\n"
    "};\n"
    "struct PresentVertex\n"
    "{\n"
    "    float4 position : SV_Position;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "PresentVertex presentVs(uint vertexId : SV_VertexID)\n"
    "{\n"
    "    PresentVertex result;\n"
    "    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);\n"
    "    result.uv = corner;\n"
    "    result.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    return result;\n"
    "}\n"
    "float3 fetchColor(float2 uv)\n"
    "{\n"
    "    return saturate(sourceImage.SampleLevel(linearClamp, uv, 0.0).rgb);\n"
    "}\n"
    "float lumaAt(float2 uv)\n"
    "{\n"
    "    return dot(fetchColor(uv), float3(0.299, 0.587, 0.114));\n"
    "}\n"
    "/* Contrast-adaptive sharpening (after AMD FidelityFX CAS): a cross of\n"
    " * neighbours one pixel away, weighted down where the local contrast is\n"
    " * already high so edges do not ring. presentOptions.y is 0-1. */\n"
    "float3 sharpenColor(float2 uv, float3 center, float2 pixelStep)\n"
    "{\n"
    "    float strength = presentOptions.y;\n"
    "    [branch] if (strength <= 0.0)\n"
    "    {\n"
    "        return center;\n"
    "    }\n"
    "    float3 north = fetchColor(uv - float2(0.0, pixelStep.y));\n"
    "    float3 south = fetchColor(uv + float2(0.0, pixelStep.y));\n"
    "    float3 west = fetchColor(uv - float2(pixelStep.x, 0.0));\n"
    "    float3 east = fetchColor(uv + float2(pixelStep.x, 0.0));\n"
    "    float3 minimum = min(center, min(min(north, south), min(west, east)));\n"
    "    float3 maximum = max(center, max(max(north, south), max(west, east)));\n"
    "    float3 amplitude = sqrt(saturate(min(minimum, 1.0 - maximum) / max(maximum, 1e-5)));\n"
    "    float peak = -1.0 / lerp(8.0, 5.0, strength);\n"
    "    float3 weight = amplitude * peak;\n"
    "    return saturate((center + (north + south + west + east) * weight) / (1.0 + 4.0 * weight));\n"
    "}\n"
    "float4 presentScalePs(PresentVertex input) : SV_Target\n"
    "{\n"
    "    return float4(sharpenColor(input.uv, fetchColor(input.uv), sourceSize.zw), 1.0);\n"
    "}\n"
    "float4 presentBoxPs(PresentVertex input) : SV_Target\n"
    "{\n"
    "    float2 footprint = sourceSize.xy * targetSize.zw;\n"
    "    int tapCount = clamp((int)ceil(max(footprint.x, footprint.y) - 0.01), 1, 4);\n"
    "    float3 total = 0.0;\n"
    "    [loop] for (int tapY = 0; tapY < tapCount; tapY++)\n"
    "    {\n"
    "        [loop] for (int tapX = 0; tapX < tapCount; tapX++)\n"
    "        {\n"
    "            float2 tapPosition = (float2(tapX, tapY) + 0.5) / tapCount - 0.5;\n"
    "            total += fetchColor(input.uv + tapPosition * footprint * sourceSize.zw);\n"
    "        }\n"
    "    }\n"
    "    return float4(sharpenColor(input.uv, total / (tapCount * tapCount), targetSize.zw), 1.0);\n"
    "}\n"
    "float4 presentFxaaPs(PresentVertex input) : SV_Target\n"
    "{\n"
    "    float2 uv = input.uv;\n"
    "    float2 texel = sourceSize.zw;\n"
    "    float3 colorCenter = fetchColor(uv);\n"
    "    float lumaCenter = dot(colorCenter, float3(0.299, 0.587, 0.114));\n"
    "    float lumaDown = lumaAt(uv + float2(0.0, texel.y));\n"
    "    float lumaUp = lumaAt(uv - float2(0.0, texel.y));\n"
    "    float lumaLeft = lumaAt(uv - float2(texel.x, 0.0));\n"
    "    float lumaRight = lumaAt(uv + float2(texel.x, 0.0));\n"
    "    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));\n"
    "    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));\n"
    "    float lumaRange = lumaMax - lumaMin;\n"
    "    if (lumaRange < max(0.0312, lumaMax * 0.125))\n"
    "    {\n"
    "        return float4(sharpenColor(uv, colorCenter, texel), 1.0);\n"
    "    }\n"
    "    float lumaDownLeft = lumaAt(uv + float2(-texel.x, texel.y));\n"
    "    float lumaUpRight = lumaAt(uv + float2(texel.x, -texel.y));\n"
    "    float lumaUpLeft = lumaAt(uv - texel);\n"
    "    float lumaDownRight = lumaAt(uv + texel);\n"
    "    float lumaDownUp = lumaDown + lumaUp;\n"
    "    float lumaLeftRight = lumaLeft + lumaRight;\n"
    "    float lumaLeftCorners = lumaDownLeft + lumaUpLeft;\n"
    "    float lumaDownCorners = lumaDownLeft + lumaDownRight;\n"
    "    float lumaRightCorners = lumaDownRight + lumaUpRight;\n"
    "    float lumaUpCorners = lumaUpRight + lumaUpLeft;\n"
    "    float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) +\n"
    "                           abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 +\n"
    "                           abs(-2.0 * lumaRight + lumaRightCorners);\n"
    "    float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) +\n"
    "                         abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 +\n"
    "                         abs(-2.0 * lumaDown + lumaDownCorners);\n"
    "    bool isHorizontal = edgeHorizontal >= edgeVertical;\n"
    "    float lumaNegative = isHorizontal ? lumaUp : lumaLeft;\n"
    "    float lumaPositive = isHorizontal ? lumaDown : lumaRight;\n"
    "    float gradientNegative = lumaNegative - lumaCenter;\n"
    "    float gradientPositive = lumaPositive - lumaCenter;\n"
    "    bool negativeSteepest = abs(gradientNegative) >= abs(gradientPositive);\n"
    "    float gradientScaled = 0.25 * max(abs(gradientNegative), abs(gradientPositive));\n"
    "    float stepLength = isHorizontal ? texel.y : texel.x;\n"
    "    float lumaLocalAverage = 0.0;\n"
    "    if (negativeSteepest)\n"
    "    {\n"
    "        stepLength = -stepLength;\n"
    "        lumaLocalAverage = 0.5 * (lumaNegative + lumaCenter);\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        lumaLocalAverage = 0.5 * (lumaPositive + lumaCenter);\n"
    "    }\n"
    "    float2 edgeUv = uv;\n"
    "    if (isHorizontal)\n"
    "    {\n"
    "        edgeUv.y += stepLength * 0.5;\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        edgeUv.x += stepLength * 0.5;\n"
    "    }\n"
    "    float2 edgeStep = isHorizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);\n"
    "    float2 uvNegative = edgeUv - edgeStep;\n"
    "    float2 uvPositive = edgeUv + edgeStep;\n"
    "    float lumaEndNegative = lumaAt(uvNegative) - lumaLocalAverage;\n"
    "    float lumaEndPositive = lumaAt(uvPositive) - lumaLocalAverage;\n"
    "    bool reachedNegative = abs(lumaEndNegative) >= gradientScaled;\n"
    "    bool reachedPositive = abs(lumaEndPositive) >= gradientScaled;\n"
    "    static const float stepScale[10] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
    "    [loop] for (int stepIndex = 0; stepIndex < 10; stepIndex++)\n"
    "    {\n"
    "        if (reachedNegative && reachedPositive)\n"
    "        {\n"
    "            break;\n"
    "        }\n"
    "        if (!reachedNegative)\n"
    "        {\n"
    "            uvNegative -= edgeStep * stepScale[stepIndex];\n"
    "            lumaEndNegative = lumaAt(uvNegative) - lumaLocalAverage;\n"
    "            reachedNegative = abs(lumaEndNegative) >= gradientScaled;\n"
    "        }\n"
    "        if (!reachedPositive)\n"
    "        {\n"
    "            uvPositive += edgeStep * stepScale[stepIndex];\n"
    "            lumaEndPositive = lumaAt(uvPositive) - lumaLocalAverage;\n"
    "            reachedPositive = abs(lumaEndPositive) >= gradientScaled;\n"
    "        }\n"
    "    }\n"
    "    float distanceNegative = isHorizontal ? (uv.x - uvNegative.x) : (uv.y - uvNegative.y);\n"
    "    float distancePositive = isHorizontal ? (uvPositive.x - uv.x) : (uvPositive.y - uv.y);\n"
    "    bool negativeCloser = distanceNegative < distancePositive;\n"
    "    float distanceFinal = min(distanceNegative, distancePositive);\n"
    "    float edgeLength = max(distanceNegative + distancePositive, 1e-6);\n"
    "    float pixelOffset = -distanceFinal / edgeLength + 0.5;\n"
    "    bool centerSmaller = lumaCenter < lumaLocalAverage;\n"
    "    bool correctVariation = ((negativeCloser ? lumaEndNegative : lumaEndPositive) < 0.0) != centerSmaller;\n"
    "    float finalOffset = correctVariation ? pixelOffset : 0.0;\n"
    "    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);\n"
    "    float subPixelFirst = saturate(abs(lumaAverage - lumaCenter) / lumaRange);\n"
    "    float subPixelSecond = (-2.0 * subPixelFirst + 3.0) * subPixelFirst * subPixelFirst;\n"
    "    finalOffset = max(finalOffset, subPixelSecond * subPixelSecond * 0.75);\n"
    "    float2 finalUv = uv;\n"
    "    if (isHorizontal)\n"
    "    {\n"
    "        finalUv.y += finalOffset * stepLength;\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        finalUv.x += finalOffset * stepLength;\n"
    "    }\n"
    "    return float4(fetchColor(finalUv), 1.0);\n"
    "}\n";

#define LD_PRESENT_SCALE 0
#define LD_PRESENT_FXAA  1
#define LD_PRESENT_BOX   2

static ID3DBlob* ld_present_compile(const char* entryPoint, const char* profile)
{
    ID3DBlob* shader = NULL;
    ID3DBlob* errors = NULL;
    const HRESULT result = D3DCompile(
        k_ld_present_hlsl, sizeof(k_ld_present_hlsl) - 1u, "present_pass", NULL, NULL,
        entryPoint, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (FAILED(result))
    {
        fprintf(stderr, "[present] %s compile failed hr=0x%08lX: %s\n", entryPoint,
                (unsigned long)result,
                errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors) : "<no diagnostics>");
        if (errors)
        {
            errors->lpVtbl->Release(errors);
        }
        return NULL;
    }
    if (errors)
    {
        errors->lpVtbl->Release(errors);
    }
    return shader;
}

/* Root signature: b0 = 12 root constants, t0 = one-SRV table, s0 = static
 * linear clamp sampler. Failure leaves the old fixed-size CopyResource
 * present in place. */
static int make_present_pipeline(void)
{
    D3D12_DESCRIPTOR_RANGE sourceRange = {0};
    sourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sourceRange.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2] = {0};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 12;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &sourceRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC linearSampler = {0};
    linearSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.MaxLOD = D3D12_FLOAT32_MAX;
    linearSampler.ShaderRegister = 0;
    linearSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description = {0};
    description.NumParameters = 2;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &linearSampler;
    ID3DBlob* serialized = NULL;
    ID3DBlob* errors = NULL;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized, &errors)))
    {
        if (errors)
        {
            errors->lpVtbl->Release(errors);
        }
        fprintf(stderr, "[present] root signature failed; fixed-size present\n");
        return -1;
    }
    if (errors)
    {
        errors->lpVtbl->Release(errors);
    }
    const HRESULT rootResult = g.dev->lpVtbl->CreateRootSignature(
        g.dev, 0, serialized->lpVtbl->GetBufferPointer(serialized),
        serialized->lpVtbl->GetBufferSize(serialized),
        &IID_ID3D12RootSignature, (void**)&g.present_rootsig);
    serialized->lpVtbl->Release(serialized);
    if (FAILED(rootResult))
    {
        g.present_rootsig = NULL;
        return -1;
    }
    ID3DBlob* vertexShader = ld_present_compile("presentVs", "vs_5_0");
    if (!vertexShader)
    {
        return -1;
    }
    static const char* const pixelEntries[3] = { "presentScalePs", "presentFxaaPs", "presentBoxPs" };
    int created = 0;
    for (u32 variant = 0; variant < 3; variant++)
    {
        ID3DBlob* pixelShader = ld_present_compile(pixelEntries[variant], "ps_5_0");
        if (!pixelShader)
        {
            continue;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {0};
        pipeline.pRootSignature = g.present_rootsig;
        pipeline.VS.pShaderBytecode = vertexShader->lpVtbl->GetBufferPointer(vertexShader);
        pipeline.VS.BytecodeLength = vertexShader->lpVtbl->GetBufferSize(vertexShader);
        pipeline.PS.pShaderBytecode = pixelShader->lpVtbl->GetBufferPointer(pixelShader);
        pipeline.PS.BytecodeLength = pixelShader->lpVtbl->GetBufferSize(pixelShader);
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = 0xFFFFFFFFu;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        const HRESULT pipelineResult = g.dev->lpVtbl->CreateGraphicsPipelineState(
            g.dev, &pipeline, &IID_ID3D12PipelineState, (void**)&g.present_pso[variant]);
        if (FAILED(pipelineResult))
        {
            g.present_pso[variant] = NULL;
            fprintf(stderr, "[present] %s pipeline failed hr=0x%08lX\n", pixelEntries[variant],
                    (unsigned long)pipelineResult);
        }
        else
        {
            created++;
        }
        pixelShader->lpVtbl->Release(pixelShader);
    }
    vertexShader->lpVtbl->Release(vertexShader);
    if (!g.present_pso[LD_PRESENT_SCALE])
    {
        return -1;
    }
    fprintf(stderr, "[present] present pass ready (%d of 3 filters)\n", created);
    return 0;
}

/* The swap chain follows the window's client area, so a maximised or
 * fullscreen window gets a back buffer at its own resolution instead of
 * DXGI stretching the 1280x720 one. Only with the present pass: the old
 * CopyResource present needs the back buffer at the image's size. */
static void ld_sync_swapchain_size(void)
{
    if (!g.hwnd || !g.present_pso[LD_PRESENT_SCALE])
    {
        return;
    }
    RECT client;
    if (!GetClientRect(g.hwnd, &client))
    {
        return;
    }
    const u32 clientWidth = (u32)(client.right - client.left);
    const u32 clientHeight = (u32)(client.bottom - client.top);
    if (!clientWidth || !clientHeight || clientWidth > 16384u || clientHeight > 16384u)
    {
        return;                          /* minimised */
    }
    if (clientWidth == g.bb_w && clientHeight == g.bb_h)
    {
        return;
    }
    /* No submission may still reference the old back buffers. */
    ld_flush(LD_FLUSH_PRESENT);
    if (!g.ready)
    {
        return;
    }
    for (u32 bufferIndex = 0; bufferIndex < LD_SWAP_BUFFERS; bufferIndex++)
    {
        if (g.backbuf[bufferIndex])
        {
            g.backbuf[bufferIndex]->lpVtbl->Release(g.backbuf[bufferIndex]);
            g.backbuf[bufferIndex] = NULL;
        }
    }
    const HRESULT resizeResult = g.swap->lpVtbl->ResizeBuffers(
        g.swap, LD_SWAP_BUFFERS, clientWidth, clientHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (SUCCEEDED(resizeResult))
    {
        g.bb_w = clientWidth;
        g.bb_h = clientHeight;
    }
    else
    {
        fprintf(stderr, "[present] ResizeBuffers %ux%u failed hr=0x%08lX; keeping %ux%u\n",
                clientWidth, clientHeight, (unsigned long)resizeResult, g.bb_w, g.bb_h);
        /* Stop trying: the next present would only fail the same way. */
        g.hwnd = NULL;
    }
    for (u32 bufferIndex = 0; bufferIndex < LD_SWAP_BUFFERS; bufferIndex++)
    {
        g.swap->lpVtbl->GetBuffer(g.swap, bufferIndex, &IID_ID3D12Resource,
                                  (void**)&g.backbuf[bufferIndex]);
        g.dev->lpVtbl->CreateRenderTargetView(g.dev, g.backbuf[bufferIndex], NULL,
                                              rtv_handle(bufferIndex));
    }
    fprintf(stderr, "[present] back buffer now %ux%u\n", g.bb_w, g.bb_h);
}

/* Draw the image in SRV slot `sourceSlot` (sourceWidth x sourceHeight host
 * pixels, already in PIXEL_SHADER_RESOURCE) into back buffer `bufferIndex`,
 * which is in PRESENT and is left in RENDER_TARGET. */
/* gameImage: 1 for the game's frame (sharpening applies), 0 for a movie. */
static int ld_present_pass(u32 sourceSlot, u32 sourceWidth, u32 sourceHeight,
                           ID3D12Resource* backBuffer, u32 bufferIndex, int gameImage)
{
    int mode = LD_PRESENT_SCALE;
    if (InterlockedCompareExchange(&g_ld_post_aa, 0, 0) == 1 && g.present_pso[LD_PRESENT_FXAA])
    {
        mode = LD_PRESENT_FXAA;
    }
    if ((sourceWidth > g.bb_w + 1u || sourceHeight > g.bb_h + 1u) && g.present_pso[LD_PRESENT_BOX])
    {
        mode = LD_PRESENT_BOX;           /* supersampled: already antialiased */
    }
    u32 slots[SRV_TABLE_SIZE];
    for (u32 slotIndex = 0; slotIndex < SRV_TABLE_SIZE; slotIndex++)
    {
        slots[slotIndex] = SRV_WHITE;
    }
    slots[0] = sourceSlot;
    if (g.srv_ring_used >= ld_srv_end())
    {
        ld_flush(LD_FLUSH_DESCRIPTOR_RING);
        if (!g.ready)
        {
            return 0;
        }
        g.srv_ring_used = ld_srv_base();
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    if (!table.ptr)
    {
        return 0;
    }
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &barrier);

    const D3D12_CPU_DESCRIPTOR_HANDLE target = rtv_handle(bufferIndex);
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &target, FALSE, NULL);
    /* Keep the image's shape: a window of another aspect ratio gets black
     * bars rather than a stretched picture. */
    float imageWidth = (float)g.bb_w;
    float imageHeight = (float)g.bb_h;
    const float sourceAspect = (float)sourceWidth / (float)sourceHeight;
    if (imageWidth / imageHeight > sourceAspect + 0.01f)
    {
        imageWidth = imageHeight * sourceAspect;
    }
    else if (imageWidth / imageHeight < sourceAspect - 0.01f)
    {
        imageHeight = imageWidth / sourceAspect;
    }
    imageWidth = (float)(u32)(imageWidth + 0.5f);
    imageHeight = (float)(u32)(imageHeight + 0.5f);
    const float imageLeft = (float)(u32)(((float)g.bb_w - imageWidth) * 0.5f);
    const float imageTop = (float)(u32)(((float)g.bb_h - imageHeight) * 0.5f);
    if (imageWidth < (float)g.bb_w || imageHeight < (float)g.bb_h)
    {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        g.list->lpVtbl->ClearRenderTargetView(g.list, target, black, 0, NULL);
    }
    const D3D12_VIEWPORT viewport = {imageLeft, imageTop, imageWidth, imageHeight, 0.0f, 1.0f};
    const D3D12_RECT scissor = {0, 0, (LONG)g.bb_w, (LONG)g.bb_h};
    g.list->lpVtbl->RSSetViewports(g.list, 1, &viewport);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.present_rootsig);
    g.list->lpVtbl->SetPipelineState(g.list, g.present_pso[mode]);
    const float constants[12] = {
        (float)sourceWidth, (float)sourceHeight, 1.0f / (float)sourceWidth, 1.0f / (float)sourceHeight,
        imageWidth, imageHeight, 1.0f / imageWidth, 1.0f / imageHeight,
        (float)mode,
        gameImage ? (float)InterlockedCompareExchange(&g_ld_sharpen_percent, 0, 0) / 100.0f : 0.0f,
        0.0f, 0.0f
    };
    g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 12, constants, 0);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
    return 1;
}

static int make_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE xrange = {0};
    xrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    xrange.NumDescriptors = SRV_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE srange = {0};
    srange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    srange.NumDescriptors = SMP_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE vrange = {0};
    vrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    vrange.NumDescriptors = RSX_DSP_NUM_VERTEX_TEXTURES;
    vrange.BaseShaderRegister = 16;
    D3D12_ROOT_PARAMETER xp[5] = {0};
    xp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    xp[0].Descriptor.ShaderRegister = 0;
    xp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    xp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[1].DescriptorTable.NumDescriptorRanges = 1;
    xp[1].DescriptorTable.pDescriptorRanges = &xrange;
    xp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[2].DescriptorTable.NumDescriptorRanges = 1;
    xp[2].DescriptorTable.pDescriptorRanges = &srange;
    xp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[3].DescriptorTable.NumDescriptorRanges = 1;
    xp[3].DescriptorTable.pDescriptorRanges = &vrange;
    xp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    xp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    xp[4].Descriptor.ShaderRegister = 1;
    xp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC vsmp[RSX_DSP_NUM_VERTEX_TEXTURES] = {0};
    for (u32 i = 0; i < RSX_DSP_NUM_VERTEX_TEXTURES; i++) {
        vsmp[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        vsmp[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].MaxLOD = D3D12_FLOAT32_MAX;
        vsmp[i].ShaderRegister = i;
        vsmp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    rsd.NumParameters = 5; rsd.pParameters = xp;
    rsd.NumStaticSamplers = RSX_DSP_NUM_VERTEX_TEXTURES;
    rsd.pStaticSamplers = vsmp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) err->lpVtbl->Release(err); return -1;
    }
    HRESULT hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                                                    sig->lpVtbl->GetBufferSize(sig),
                                                    &IID_ID3D12RootSignature, (void**)&g.rootsig_x);
    sig->lpVtbl->Release(sig);
    return SUCCEEDED(hr) ? 0 : -1;
}

static int make_depth_snapshot_pipeline(void)
{
    static const char source[] =
        "Texture2D<float> depth_source : register(t0);\n"
        "RWTexture2D<float> depth_destination : register(u0);\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    uint width, height;\n"
        "    depth_destination.GetDimensions(width, height);\n"
        "    if (id.x < width && id.y < height)\n"
        "        depth_destination[id.xy] = "
        "depth_source.Load(int3(id.xy, 0));\n"
        "}\n";
    ID3DBlob* shader = NULL;
    ID3DBlob* errors = NULL;
    HRESULT hr = D3DCompile(
        source, sizeof(source) - 1u, "depth_snapshot_compute", NULL, NULL,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader, &errors);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot shader compile failed "
                "hr=0x%08lX: %s\n",
                (unsigned long)hr,
                errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors)
                       : "<no diagnostics>");
        if (errors) errors->lpVtbl->Release(errors);
        return -1;
    }
    if (errors) errors->lpVtbl->Release(errors);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {0};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameters[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        parameters[i].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC description = {0};
    description.NumParameters = 2;
    description.pParameters = parameters;
    ID3DBlob* serialized = NULL;
    ID3DBlob* signature_errors = NULL;
    hr = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &signature_errors);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot root signature failed "
                "hr=0x%08lX: %s\n",
                (unsigned long)hr,
                signature_errors
                    ? (const char*)signature_errors->lpVtbl->GetBufferPointer(
                          signature_errors)
                    : "<no diagnostics>");
        if (signature_errors)
            signature_errors->lpVtbl->Release(signature_errors);
        shader->lpVtbl->Release(shader);
        return -1;
    }
    if (signature_errors)
        signature_errors->lpVtbl->Release(signature_errors);
    hr = g.dev->lpVtbl->CreateRootSignature(
        g.dev, 0, serialized->lpVtbl->GetBufferPointer(serialized),
        serialized->lpVtbl->GetBufferSize(serialized),
        &IID_ID3D12RootSignature, (void**)&g.depth_snapshot_rootsig);
    serialized->lpVtbl->Release(serialized);
    if (FAILED(hr)) {
        shader->lpVtbl->Release(shader);
        return -1;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {0};
    pipeline.pRootSignature = g.depth_snapshot_rootsig;
    pipeline.CS.pShaderBytecode = shader->lpVtbl->GetBufferPointer(shader);
    pipeline.CS.BytecodeLength = shader->lpVtbl->GetBufferSize(shader);
    hr = g.dev->lpVtbl->CreateComputePipelineState(
        g.dev, &pipeline, &IID_ID3D12PipelineState,
        (void**)&g.depth_snapshot_pso);
    shader->lpVtbl->Release(shader);
    return SUCCEEDED(hr) ? 0 : -1;
}

int rsx_live_draw_init(void* hwnd, u32 width, u32 height,
                       rsx_live_guest_ptr_fn guest_fn, void* guest_user)
{
    if (!rsx_live_draw_enabled()) return 0;
    if (g.ready) return 0;
    ld_present_measure_init();
#if defined(YZ_PERF_PROFILE)
    memset(&g_ld_profile, 0, sizeof(g_ld_profile));
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_ld_profile.qpc_frequency = frequency.QuadPart;
        g_ld_profile.previous_present_qpc = ld_profile_qpc();
    }
#endif
    g.width = width; g.height = height;
    g.bb_w = width; g.bb_h = height;
    g.hwnd = (HWND)hwnd;
    g.scale = g_ld_requested_scale;
    {
        const char* scaleOverride = getenv("LD_RENDER_SCALE");
        if (scaleOverride && scaleOverride[0])
        {
            g.scale = (u32)strtoul(scaleOverride, NULL, 10);
        }
        if (g.scale < 1u || g.scale > 4u)
        {
            g.scale = 1u;
        }
    }
    g.widen = 1.0f;
    {
        float aspect = g_ld_requested_aspect;
        const char* aspectOverride = getenv("LD_ASPECT");
        if (aspectOverride && aspectOverride[0])
        {
            aspect = (float)atof(aspectOverride);
        }
        const float baseAspect = (float)width / (float)(height ? height : 1u);
        if (aspect > baseAspect + 0.01f)
        {
            g.widen = aspect / baseAspect;
            if (g.widen > 4.0f)
            {
                g.widen = 4.0f;
            }
        }
    }
    g.pipelined = getenv("LD_NO_PIPELINE") ? 0 : 1;
    fprintf(stderr, "[live-draw] render scale %ux (%ux%u), aspect %.3f, frame pipelining %s\n",
            g.scale, ld_scaled_x(width), height * g.scale,
            (float)width / (float)(height ? height : 1u) * g.widen, g.pipelined ? "on" : "off");
    if (g.widen > 1.0f && g.hwnd)
    {
        /* Open the window at the wider shape (the user can still resize it). */
        RECT clientRect = {0, 0, (LONG)((float)width * g.widen + 0.5f), (LONG)height};
        const DWORD windowStyle = (DWORD)GetWindowLongPtrA(g.hwnd, GWL_STYLE);
        const DWORD windowExStyle = (DWORD)GetWindowLongPtrA(g.hwnd, GWL_EXSTYLE);
        AdjustWindowRectEx(&clientRect, windowStyle, FALSE, windowExStyle);
        SetWindowPos(g.hwnd, NULL, 0, 0, clientRect.right - clientRect.left,
                     clientRect.bottom - clientRect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g.guest_ptr = guest_fn; g.guest_user = guest_user;
    {
        const char* requested = getenv("RSX_FP_CONSTANT_MODE");
        char mode = 'B';
        if (requested && requested[0]) {
            if ((requested[0] == 'L' || requested[0] == 'l') &&
                (!requested[1] ||
                 _stricmp(requested, "literal") == 0))
                mode = 'L';
            else if ((requested[0] == 'B' || requested[0] == 'b') &&
                     (!requested[1] ||
                      _stricmp(requested, "buffered") == 0))
                mode = 'B';
            else
                fprintf(
                    stderr,
                    "[rsx-fp-constants] unknown "
                    "RSX_FP_CONSTANT_MODE='%s'; using buffered\n",
                    requested);
        }
        g.fp_constant_mode = mode;
        fprintf(
            stderr,
            "[rsx-fp-constants] mode=%s (startup-selected) "
            "d3dcompile_flags1=0x0 d3dcompile_flags2=0x0\n",
            ld_fp_constant_mode_name());
    }
    {
        const char* requested = getenv("RSX_VERTEX_MODE");
        const char* old_path = getenv("RSX_VERTEX_PATH");
        char mode = 'C';
        if (requested && requested[0] && !requested[1]) {
            mode = (char)toupper((unsigned char)requested[0]);
        } else if (!requested && old_path) {
            mode = strcmp(old_path, "compact") == 0 ? 'C' : 'L';
        }
        switch (mode) {
        case 'L':
            g.vertex_features = 0;
            break;
        case 'H':
            g.vertex_features = LD_VERTEX_HOIST_FETCH;
            break;
        case 'M':
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE;
            break;
        case 'C':
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE |
                LD_VERTEX_COMPACT_PAYLOAD;
            break;
        default:
            fprintf(stderr,
                    "[rsx-vertex] unknown RSX_VERTEX_MODE='%s'; "
                    "using C\n",
                    requested ? requested : "");
            mode = 'C';
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE |
                LD_VERTEX_COMPACT_PAYLOAD;
            break;
        }
        g.vertex_mode = mode;
#if !defined(YZ_PERF_CLEAN)
        const char* diag_dir = getenv("RSX_VERTEX_DIAG_DIR");
        if (diag_dir && diag_dir[0]) {
            strncpy(
                g.vertex_diag_dir, diag_dir,
                sizeof(g.vertex_diag_dir) - 1u);
            g.vertex_diag_dir[sizeof(g.vertex_diag_dir) - 1u] = '\0';
        }
#endif
        fprintf(stderr,
                "[rsx-vertex] mode=%s features=0x%X "
                "(startup-selected)\n",
                ld_vertex_mode_name(), g.vertex_features);
    }
    g_ld_flip_requested = 0;
    g_ld_flip_consumed = 0;
    g_ld_last_requested_buffer = UINT32_MAX;
    g_ld_last_consumed_buffer = UINT32_MAX;
    g_ld_last_present_target = UINT32_MAX;
#if !defined(YZ_PERF_CLEAN)
    g_ld_surface_generation = 0;
    g_ld_surface_resource_serial = 0;
    g_ld_guest_blit_generation = 0;
    g_ld_present_copy_generation = 0;
    g_ld_vertex_constant_ring_recycles = 0;
    g_ld_frame_vertex_constant_ring_recycles = 0;
    g_ld_fifo_get = 0;
    g_ld_fifo_put = 0;
    InterlockedExchange(&g_ld_diag_post_movie_pending, 0);
    InterlockedExchange(&g_ld_diag_post_movie_presents, 0);
#endif
    g_ld_last_dump_fingerprint = 0;
    memset(g_ld_layout_cache, 0, sizeof(g_ld_layout_cache));
    g_ld_layout_cache_count = 0;

    ld_enable_debug_layer();
    ld_enable_dred();
    IDXGIFactory4* factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory))) return -1;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g.dev))) {
        factory->lpVtbl->Release(factory); return -1;
    }
#if defined(YZ_PERF_PROFILE)
    {
        LUID luid = {0};
        g.dev->lpVtbl->GetAdapterLuid(g.dev, &luid);
        factory->lpVtbl->EnumAdapterByLuid(
            factory, luid, &IID_IDXGIAdapter3,
            (void**)&g_ld_profile.adapter);
    }
#endif
    ld_open_info_queue();
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    g.dev->lpVtbl->CreateCommandQueue(g.dev, &qd, &IID_ID3D12CommandQueue, (void**)&g.queue);
    for (u32 bankIndex = 0; bankIndex < LD_BANKS; bankIndex++)
    {
        g.dev->lpVtbl->CreateCommandAllocator(g.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              &IID_ID3D12CommandAllocator,
                                              (void**)&g.allocs[bankIndex]);
    }
    g.bank = 0;
    g.alloc = g.allocs[0];
    g.dev->lpVtbl->CreateCommandList(g.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, NULL,
                                     &IID_ID3D12GraphicsCommandList, (void**)&g.list);
    if (g.queue) g.queue->lpVtbl->SetName(g.queue, L"rsx-live-queue");
    if (g.list) g.list->lpVtbl->SetName(g.list, L"rsx-live-list");
    g.dev->lpVtbl->CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g.fence);
    g.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* swap chain bound to the runtime's HWND */
    DXGI_SWAP_CHAIN_DESC1 scd = {0};
    scd.Width = width; scd.Height = height; scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = LD_SWAP_BUFFERS; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = NULL;
    if (FAILED(factory->lpVtbl->CreateSwapChainForHwnd(factory, (IUnknown*)g.queue,
            (HWND)hwnd, &scd, NULL, NULL, &sc1))) {
        factory->lpVtbl->Release(factory); return -1;
    }
    sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g.swap);
    sc1->lpVtbl->Release(sc1);
    factory->lpVtbl->Release(factory);

    /* RTV heap: [0..1] backbuffers, [2..] surface cache */
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = LD_SWAP_BUFFERS + MAX_SURFACES;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.rtv_heap);
    g.rtv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (u32 i = 0; i < LD_SWAP_BUFFERS; i++) {
        g.swap->lpVtbl->GetBuffer(g.swap, i, &IID_ID3D12Resource, (void**)&g.backbuf[i]);
        g.dev->lpVtbl->CreateRenderTargetView(g.dev, g.backbuf[i], NULL, rtv_handle(i));
    }

    /* upload arena */
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UPLOAD_SIZE;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.upload);
    D3D12_RANGE rr = {0, 0};
    g.upload->lpVtbl->Map(g.upload, 0, &rr, (void**)&g.upload_mapped);

    bd.Width = VERT_BUFFER_SIZE;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.vb);
    g.vb->lpVtbl->Map(g.vb, 0, &rr, (void**)&g.vb_mapped);

    bd.Width = INDEX_BUFFER_SIZE;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.ib);
    g.ib->lpVtbl->Map(g.ib, 0, &rr, (void**)&g.ib_mapped);

    bd.Width = CB_RING_BYTES;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.cb);
    g.cb->lpVtbl->Map(g.cb, 0, &rr, (void**)&g.cb_mapped);

    bd.Width = PS_CB_RING_BYTES;
    g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
        &IID_ID3D12Resource, (void**)&g.ps_cb);
    if (g.ps_cb)
        g.ps_cb->lpVtbl->Map(
            g.ps_cb, 0, &rr, (void**)&g.ps_cb_mapped);

    /* SRV heaps */
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = SRV_CPU_HEAP_TOTAL; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_cpu_heap);
    hd.NumDescriptors = SRV_RING_TABLES * SRV_TABLE_SIZE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_heap);
    g.srv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* sampler heaps */
    D3D12_DESCRIPTOR_HEAP_DESC shd = {0};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    shd.NumDescriptors = SMP_CACHE_SLOTS + 1; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_cpu_heap);
    shd.NumDescriptors = SMP_RING_TABLES * SMP_TABLE_SIZE;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_heap);
    g.smp_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    /* Fail init (fall back to null present) rather than crash later if any
     * shader-visible/CPU descriptor heap didn't create -- e.g. an over-limit
     * sampler heap would return NULL here and fault in sampler_table. */
    if (!g.srv_cpu_heap || !g.srv_heap || !g.smp_cpu_heap || !g.smp_heap) {
        fprintf(stderr, "[live-draw] descriptor heap alloc failed "
                "(srv_cpu=%p srv=%p smp_cpu=%p smp=%p)\n",
                (void*)g.srv_cpu_heap, (void*)g.srv_heap,
                (void*)g.smp_cpu_heap, (void*)g.smp_heap);
        return -1;
    }
    {
        D3D12_SAMPLER_DESC def = {0};
        def.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        def.AddressU = def.AddressV = def.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def.MaxLOD = D3D12_FLOAT32_MAX; def.MaxAnisotropy = 1;
        g.dev->lpVtbl->CreateSampler(g.dev, &def, smp_cpu(SMP_DEFAULT));
    }

    /* depth */
    D3D12_DESCRIPTOR_HEAP_DESC dhd = {0};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dhd.NumDescriptors = 1 + MAX_SURFACES;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &dhd, &IID_ID3D12DescriptorHeap, (void**)&g.dsv_heap);
    g.dsv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(
        g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    {
        D3D12_HEAP_PROPERTIES dhp = {0}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC drd = {0};
        drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        drd.Width = ld_scaled_x(width); drd.Height = ld_scaled(height); drd.DepthOrArraySize = 1;
        drd.MipLevels = 1; drd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; drd.SampleDesc.Count = 1;
        drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv = {0}; dcv.Format = drd.Format; dcv.DepthStencil.Depth = 1.0f;
        if (SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &dhp, D3D12_HEAP_FLAG_NONE, &drd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, &IID_ID3D12Resource, (void**)&g.depth))) {
            D3D12_CPU_DESCRIPTOR_HANDLE dh;
            g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &dh);
            g.dev->lpVtbl->CreateDepthStencilView(g.dev, g.depth, NULL, dh);
        } else {
            g.depth = NULL;
        }
    }

    if (make_root_signature() != 0 ||
        make_depth_snapshot_pipeline() != 0)
        return -1;
    if (make_present_pipeline() != 0)
    {
        /* Fixed-size present: the back buffer must match the image. */
        g.hwnd = NULL;
        if (g.scale != 1u || g.widen > 1.0f)
        {
            fprintf(stderr, "[live-draw] no present pass; render scale back to 1x, aspect 16:9\n");
            g.scale = 1u;
            g.widen = 1.0f;
        }
    }
    g.upload_used = ld_upload_base();
    ld_rings_reset();

    static const u8 white[4] = {255, 255, 255, 255};
    g.white_tex = create_texture_rgba(white, 1, 1);
    if (g.white_tex) srv_write(SRV_WHITE, g.white_tex);

    /* dispatcher + sink */
    rsx_dispatch_sink sink = {0};
    sink.user = &g;
    sink.clear = sink_clear;
    sink.begin = sink_begin;
    sink.end = sink_end;
    sink.draw_arrays = sink_draw_arrays;
    sink.inline_array = sink_inline_array;
    sink.draw_index_array = sink_draw_index;
    sink.flip = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);

    g.ready = 1;
    g.enabled = 1;
    return 0;
}

void rsx_live_draw_seed_registers(const u32* regs, u32 count)
{
    if (g.ready) rsx_dispatch_seed_registers(&g.rsx, regs, count);
}
void rsx_live_draw_seed_transform_program(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_program(&g.rsx, words, count);
}
void rsx_live_draw_seed_transform_constants(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_constants(&g.rsx, words, count);
}

void rsx_live_draw_set_display_buffer(
    u32 buffer_id, u32 location, u32 offset, u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    display_buffer_t* display = &g.display_buffers[buffer_id];
    display->location = location;
    display->offset = offset;
    display->pitch = pitch;
    display->width = width;
    display->height = height;
    display->valid = width && height;
    display->resolve_src = 0;
    fprintf(stderr,
            "[live-draw] display buffer %u = loc%u:0x%08X pitch=%u %ux%u\n",
            buffer_id, location, offset, pitch, width, height);
}

void rsx_live_draw_method(u32 method, u32 arg)
{
    /* Bounded ingress trace for the a010 live-vs-capture comparison.  Keep the
     * raw method (including its FIFO subchannel bits) visible: if an SPU-built
     * command list binds NV4097 on a different subchannel, feeding the raw
     * 0x2xxx-shifted method into the canonical dispatcher silently stores the
     * state in the wrong register bank. */
    static int state_trace = -1;
    static u32 state_trace_lines = 0;
    if (state_trace < 0)
        state_trace = getenv("RSX_VERTEX_STATE_TRACE") ? 1 : 0;
    if (state_trace && state_trace_lines < 512) {
        const u32 canonical = method & 0x1FFCu;
        const int vertex_texture =
            canonical >= 0x0900u && canonical < 0x0980u;
        const int vertex_format =
            canonical >= 0x1740u && canonical < 0x1780u;
        const int frequency_divider = canonical == 0x1FC0u;
        if (vertex_texture || vertex_format || frequency_divider) {
            fprintf(stderr,
                    "[rsx-vstate] frame=%u raw=0x%04X sub=%u canonical=0x%04X "
                    "arg=0x%08X\n",
                    g_ld_frames, method & 0xFFFFu, (method >> 13) & 7u,
                    canonical, arg);
            fflush(stderr);
            state_trace_lines++;
        }
    }

    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (serialized) {
        for (;;) {
            if (g_ld_movie_mode || g_ld_host_waiting) {
                while (g_ld_host_waiting)
                    SwitchToThread();
                AcquireSRWLockExclusive(&g_ld_access_lock);
                if (!g.ready) {
                    ReleaseSRWLockExclusive(&g_ld_access_lock);
                    return;
                }
                if (g_ld_movie_mode && !composite) {
                    if (g_ld_movie_track_rsx < 0)
                        g_ld_movie_track_rsx =
                            getenv("RSX_MOVIE_TRACK") ? 1 : 0;
                    if (!g_ld_movie_track_rsx) {
                        ReleaseSRWLockExclusive(&g_ld_access_lock);
                        return;
                    }
                }
                rsx_dispatch_method(&g.rsx, method, arg);
                ReleaseSRWLockExclusive(&g_ld_access_lock);
                return;
            }
            InterlockedIncrement(&g_ld_guest_active);
            MemoryBarrier();
            if (!g_ld_movie_mode && !g_ld_host_waiting) {
                if (g.ready)
                    rsx_dispatch_method(&g.rsx, method, arg);
                InterlockedDecrement(&g_ld_guest_active);
                return;
            }
            InterlockedDecrement(&g_ld_guest_active);
        }
    }

    if (!g.ready) return;
    if (g_ld_movie_mode) {
        if (g_ld_movie_track_rsx < 0)
            g_ld_movie_track_rsx = getenv("RSX_MOVIE_TRACK") ? 1 : 0;
        if (!g_ld_movie_track_rsx) return;
    }
    rsx_dispatch_method(&g.rsx, method, arg);
}

void rsx_live_draw_set_movie_mode(int on)
{
    static unsigned long long suppressed_at_start = 0;
    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (on && rsx_live_draw_a010_probe_active()) {
        fprintf(stderr,
                "[a010-probe] END movie-mode live_frame=%u elapsed_frames=%u "
                "surfaces=%u samples=%u touched=0x%016llX\n",
                g_ld_frames, g_ld_frames - g_ld_a010_probe_start_frame,
                g.n_surfaces, g_ld_a010_probe_sample,
                (unsigned long long)g_ld_a010_probe_touched);
        fflush(stderr);
        InterlockedExchange(&g_ld_a010_probe_active, 0);
    }
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        if (on) InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 1);
        while (InterlockedCompareExchange(&g_ld_guest_active, 0, 0) != 0)
            SwitchToThread();
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (on) {
        suppressed_at_start = g_ld_stats.packets_movie;
        if (composite) ld_movie_overlay_begin();
        else g_ld_movie_mode = 1;
    } else {
        if (composite) {
            g.movie_overlay_valid = 0;
            /* Do not expose a partially rendered auth/fade surface between
             * the last host movie frame and the next clean guest scene. */
            ld_flush(LD_FLUSH_MOVIE);
            const float black[4] = {0, 0, 0, 1};
            for (u32 i = 0; i < g.n_surfaces; i++) {
                g.list->lpVtbl->ClearRenderTargetView(
                    g.list, rtv_handle(LD_SWAP_BUFFERS + i), black, 0, NULL);
                ld_surface_note_write(i, LD_SURFACE_WRITE_OTHER);
            }
            if (g.n_surfaces) ld_flush(LD_FLUSH_MOVIE);
            ld_movie_reset_rings();
            InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 0);
            fprintf(stderr, "[movie-ui] compositor disarmed after %llu guest overlays\n",
                    (unsigned long long)g.movie_overlay_frames);
            fflush(stderr);
        } else {
            g_ld_movie_mode = 0;
        }
        if (!composite && g_ld_movie_track_rsx > 0) {
            fprintf(stderr,
                    "[live-draw] movie handoff: tracked RSX state, suppressed %llu guest draw packets\n",
                    g_ld_stats.packets_movie - suppressed_at_start);
            fflush(stderr);
        }
#if !defined(YZ_PERF_CLEAN)
        if (g.vertex_diag_dir[0]) {
            InterlockedExchange(&g_ld_diag_post_movie_presents, 0);
            InterlockedExchange(&g_ld_diag_post_movie_pending, 1);
        }
#endif
    }
    ld_vertex_diag_emit(on ? "movie-begin" : "movie-end", 0);
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

u32 rsx_live_draw_get_frames(void) { return g_ld_frames; }

void rsx_live_draw_set_fifo_position(u32 get, u32 put)
{
#if defined(YZ_PERF_CLEAN)
    (void)get;
    (void)put;
#else
    g_ld_fifo_get = get;
    g_ld_fifo_put = put;
#endif
}

void rsx_live_draw_note_inline_transfer(u32 dma, u32 offset, u32 value)
{
#if defined(YZ_PERF_CLEAN)
    (void)dma;
    (void)offset;
    (void)value;
#else
    if (!g.ready || !yz_frontier_trace_is_armed())
        return;
    const u32 location = dma == 0xFEED0000u ? 0u :
                         dma == 0xFEED0001u ? 1u : UINT32_MAX;
    if (location == UINT32_MAX)
        return;
    for (u32 i = 0; i < g.n_surfaces; ++i) {
        surface_t* surface = &g.surfaces[i];
        const u64 begin = surface->offset;
        const u64 end = begin + (u64)surface->w * surface->h * 4u;
        if (surface->location != location || offset < begin || offset >= end)
            continue;
        surface->last_guest_blit_generation = ++g_ld_guest_blit_generation;
        yz_frontier_trace_emit(
            YZ_FT_PARITY_GUEST_BLIT, i, location,
            surface->last_guest_blit_generation, offset, value,
            g_ld_fifo_get, g_ld_fifo_put, surface->last_write_generation);
    }
#endif
}

void* rsx_live_draw_get_present_thread_handle(void)
{
#if defined(YZ_PPU_SAMPLE)
    return (void*)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_ld_present_thread_handle, NULL, NULL);
#else
    return NULL;
#endif
}

static u32 g_ld_last_frame_draws = 0;
/* Draws in the last COMPLETED frame (title-bar telemetry: distinguishes
 * "presenting fresh content" from "flipping a static image" -- the dead
 * journal-consumer limp state renders ~0 draws/frame while flips tick). */
u32 rsx_live_draw_get_last_draws(void) { return g_ld_last_frame_draws; }

double rsx_live_draw_get_present_fps(void)
{
    const u64 total = g_ld_present_total;
    if (total < 2u || g_ld_qpc_frequency <= 0)
        return 0.0;

    const u64 available =
        total < LD_PRESENT_RING_CAP ? total : LD_PRESENT_RING_CAP;
    const ld_present_sample* newest =
        &g_ld_present_ring[(total - 1u) & (LD_PRESENT_RING_CAP - 1u)];
    if (newest->present_id != total)
        return 0.0;

    const ld_present_sample* oldest = newest;
    u64 intervals = 0;
    for (u64 back = 1u; back < available; ++back) {
        const u64 present_id = total - back;
        const ld_present_sample* candidate =
            &g_ld_present_ring[(present_id - 1u) &
                               (LD_PRESENT_RING_CAP - 1u)];
        if (candidate->present_id != present_id)
            break;
        oldest = candidate;
        intervals = back;
        const double elapsed =
            (double)(newest->qpc - oldest->qpc) /
            (double)g_ld_qpc_frequency;
        /*
         * Keep at least a 15-second view and about 30 intervals when possible;
         * cap at 30 seconds so sub-1-FPS operation still updates usefully.
         */
        if (elapsed >= 30.0 || (elapsed >= 15.0 && intervals >= 30u))
            break;
    }

    const LONGLONG ticks = newest->qpc - oldest->qpc;
    return intervals != 0u && ticks > 0
        ? (double)intervals * (double)g_ld_qpc_frequency / (double)ticks
        : 0.0;
}

void rsx_live_draw_dump_present_samples(void)
{
    ld_present_measure_dump();
}

/* Present a host-decoded RGBA8 frame to the window: copy it straight into the
 * swap-chain backbuffer (both R8G8B8A8_UNORM at the swap size) and Present.
 * The frame is clamped to the backbuffer size. Call from a single thread with
 * movie mode on (so guest draws don't touch g.list). */
void rsx_live_draw_present_rgba(const uint8_t* rgba, u32 w, u32 h)
{
    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (!g.ready || !rgba) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }
    if (composite && !ld_movie_overlay_ensure()) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
        return;
    }
    /* The upload ring (and with it offset 0) may still be read by a frame in
     * flight; movie frames are few, so simply let the GPU catch up. The
     * window may also have changed size since the last present. */
    ld_wait_idle();
    ld_sync_swapchain_size();
    if (w > g.width)  w = g.width;
    if (h > g.height) h = g.height;
    const u32 pitch = (w * 4 + 255) & ~255u;          /* D3D12 copy pitch align */
    if ((UINT64)pitch * h > UPLOAD_SIZE) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }

    u8* host_upload = composite ? g.movie_upload_mapped : g.upload_mapped;
    for (u32 y = 0; y < h; y++) {
        u8* dstrow = host_upload + (size_t)y * pitch;
        memcpy(dstrow, rgba + (size_t)y * w * 4, (size_t)w * 4);
        if (composite && g.movie_overlay_valid && g.movie_overlay_rgba &&
            g.movie_overlay_mask) {
            const u8* ov = g.movie_overlay_rgba + (size_t)y * g.width * 4;
            const u8* mask =
                g.movie_overlay_mask + (size_t)y * g.width;
            for (u32 x = 0; x < w; x++, ov += 4) {
                const int coverage = mask[x];
                if (!coverage) continue;
                u8* out = dstrow + (size_t)x * 4;
                if (coverage == 1) {
                    out[0] = (u8)((out[0] * 64 + 127) / 255);
                    out[1] = (u8)((out[1] * 64 + 127) / 255);
                    out[2] = (u8)((out[2] * 64 + 127) / 255);
                    continue;
                }
                const int white = ov[0] > ov[1]
                    ? (ov[0] > ov[2] ? ov[0] : ov[2])
                    : (ov[1] > ov[2] ? ov[1] : ov[2]);
                for (int c = 0; c < 3; c++) {
                    const int v =
                        (white * coverage +
                         out[c] * (255 - coverage) + 127) / 255;
                    out[c] = (u8)v;
                }
                out[3] = 255;
            }
        }
    }

    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    src.pResource = composite ? g.movie_upload : g.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;

    /* A frame the size of the back buffer is copied straight in, as before;
     * any other size goes through a texture and the present pass. */
    int scaledMovie = 0;
    if ((w != g.bb_w || h != g.bb_h) && g.present_pso[LD_PRESENT_SCALE])
    {
        if (g.present_movie && (g.present_movie_w != w || g.present_movie_h != h))
        {
            retire_texture(g.present_movie);
            g.present_movie = NULL;
        }
        if (!g.present_movie)
        {
            D3D12_HEAP_PROPERTIES movieHeap = {0};
            movieHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC movieDesc = {0};
            movieDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            movieDesc.Width = w;
            movieDesc.Height = h;
            movieDesc.DepthOrArraySize = 1;
            movieDesc.MipLevels = 1;
            movieDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            movieDesc.SampleDesc.Count = 1;
            if (SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(
                    g.dev, &movieHeap, D3D12_HEAP_FLAG_NONE, &movieDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                    (void**)&g.present_movie)))
            {
                g.present_movie_w = w;
                g.present_movie_h = h;
                srv_write(SRV_PRESENT_MOVIE, g.present_movie);
            }
            else
            {
                g.present_movie = NULL;
            }
        }
        scaledMovie = g.present_movie != NULL;
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (scaledMovie)
    {
        dst.pResource = g.present_movie;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        b.Transition.pResource = g.present_movie;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        const int drawn = ld_present_pass(SRV_PRESENT_MOVIE, w, h, bb, bbi, 0);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        ld_overlay_then_present_state(bb, bbi, drawn ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                     : D3D12_RESOURCE_STATE_PRESENT);
    }
    else
    {
        b.Transition.pResource = bb;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        if (w > g.bb_w) w = g.bb_w;          /* never copy past a smaller back buffer */
        if (h > g.bb_h) h = g.bb_h;
        D3D12_BOX copyBox = {0, 0, 0, w, h, 1};
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, &copyBox);
        ld_overlay_then_present_state(bb, bbi, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    ld_flush(LD_FLUSH_MOVIE_PRESENT);
    {
        const HRESULT present_hr = g.swap->lpVtbl->Present(g.swap, 1, 0);
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames);
        else {
            fprintf(stderr,
                    "[d3d-fail] movie Present hr=0x%08lX frame=%u\n",
                    (unsigned long)present_hr, g_ld_frames);
            ld_dump_dred("MoviePresent", present_hr);
            g.ready = 0;
        }
    }
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

/* Env-gated (RSX_DUMP) framebuffer dump: read the current color surface back
 * and write a binary PPM. Self-contained -- creates + releases its own readback
 * buffer, so no init/struct changes. Uses g.list which ld_flush leaves open. */
/* Readback texel -> 8-bit RGB.  This title renders HDR into FP16 targets, so a
 * dump has to tone it down rather than reinterpret the bytes.  Reinhard keeps
 * the whole range visible without pretending to match the guest tonemapper. */
static void ld_dump_texel_rgb(const u8* p, int fp16, u8* out)
{
    if (!fp16) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return; }
    for (int c = 0; c < 3; c++) {
        const u16 h = (u16)(p[c * 2] | (p[c * 2 + 1] << 8));
        const int ex = (h >> 10) & 0x1F, ma = h & 0x3FF;
        float f;
        if (ex == 0)       f = (float)ma * (1.0f / 16384.0f) * (1.0f / 64.0f);
        else if (ex == 31) f = 65504.0f;
        else { f = 1.0f + (float)ma / 1024.0f;
               int k = ex - 15;
               while (k > 0) { f *= 2.0f; k--; }
               while (k < 0) { f *= 0.5f; k++; } }
        if (h & 0x8000) f = 0.0f;
        f = f / (1.0f + f);                          /* Reinhard */
        out[c] = (u8)(f * 255.0f + 0.5f);
    }
}

/* --record: when set, ld_dump_surface_ppm appends to this ffmpeg pipe instead
 * of opening `path`, and skips its per-dump diagnostics (every frame goes
 * through here while recording). */
static FILE* g_ld_record_to;

static u64 ld_dump_surface_ppm(const char* path, const surface_t* surface)
{
    ID3D12Resource* rt = surface ? surface->tex : NULL;
    if (!rt) return UINT64_MAX;
    const u32 width = ld_surface_x(surface, surface->w);     /* the host resource */
    const u32 height = ld_surface_scaled(surface, surface->h);
    if (!width || !height) return UINT64_MAX;
    const int fp16 = surface->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const u32 texel = fp16 ? 8u : 4u;
    const u32 pitch = (width * texel + 255) & ~255u;             /* 256-align */
    const UINT64 rb_size = (UINT64)pitch * height;

    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = rb_size;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb)))
        return UINT64_MAX;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = surface->fmt;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    ld_flush(LD_FLUSH_READBACK);                   /* copy completes on the GPU */

    u64 nonblack = UINT64_MAX;
    u64 fingerprint = 1469598103934665603ull;
    u8* px = NULL; D3D12_RANGE rr = {0, (SIZE_T)rb_size};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, (void**)&px))) {
        nonblack = 0;
        if (!g_ld_record_to) {   /* raw texels, to tell an FP16 target from 8-bit data read as one */
            const u8* r0 = px + (SIZE_T)(height / 4) * pitch + (width / 4) * texel;
            fprintf(stderr, "[dump-raw] %s fmt=%d texel=%u bytes:", path, (int)surface->fmt, texel);
            for (u32 bi = 0; bi < 16; bi++) fprintf(stderr, " %02X", r0[bi]);
            fprintf(stderr, "\n");
        }
        FILE* f = g_ld_record_to ? g_ld_record_to : fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (u32 y = 0; y < height; y++) {
                const u8* row = px + (SIZE_T)y * pitch;
                for (u32 x = 0; x < width; x++) {
                    const u8* pixel = row + x * texel;
                    u8 rgb[3]; ld_dump_texel_rgb(pixel, fp16, rgb);
                    if (rgb[0] || rgb[1] || rgb[2])
                        nonblack++;
                    fingerprint = fnv1a(rgb, 3, fingerprint);
                    fwrite(rgb, 1, 3, f);  /* RGBA->RGB */
                }
            }
            if (f != g_ld_record_to) {
                fclose(f);
                fprintf(stderr, "[live-draw] wrote %s\n", path);
            }
        } else {
            for (u32 y = 0; y < height; y++) {
                const u8* row = px + (SIZE_T)y * pitch;
                for (u32 x = 0; x < width; x++) {
                    const u8* pixel = row + x * texel;
                    u8 rgb[3]; ld_dump_texel_rgb(pixel, fp16, rgb);
                    if (rgb[0] || rgb[1] || rgb[2])
                        nonblack++;
                    fingerprint = fnv1a(rgb, 3, fingerprint);
                }
            }
        }
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
        g_ld_last_dump_fingerprint = fingerprint;
    }
    rb->lpVtbl->Release(rb);
    return nonblack;
}

#if !defined(YZ_PERF_CLEAN)
static void ld_parity_capture_surface(
    const char* directory, const char* phase, u32 serial,
    u32 buffer_id, u32 target)
{
    if (!directory || !directory[0] || target >= g.n_surfaces)
        return;
    surface_t* surface = &g.surfaces[target];
    char path[MAX_PATH * 2];
    snprintf(
        path, sizeof(path),
        "%s\\%s_%03u_flip_%llu_buf_%u_surface_%u_write_%u_kind_%u.ppm",
        directory, phase, serial,
        (unsigned long long)g_ld_flip_requested,
        buffer_id, target, surface->last_write_generation,
        surface->last_write_kind);
    const u64 nonblack = ld_dump_surface_ppm(path, surface);
    fprintf(
        stderr,
        "[parity-capture] phase=%s serial=%u flip=%llu buffer=%u "
        "surface=%u resource=%u write=%u kind=%u draw=%u clear=%u "
        "copy=%u blit=%u resolve=%u other=%u guest_blit=%u "
        "present_copy=%u get=%08X put=%08X cb=%u vb=%u srv=%u "
        "groups=%llu drop_ring=%llu cb_recycles=%llu frame_cb_recycles=%u "
        "hash=%016llX nonblack=%s%llu path=%s\n",
        phase, serial, (unsigned long long)g_ld_flip_requested,
        buffer_id, target, surface->resource_serial,
        surface->last_write_generation, surface->last_write_kind,
        surface->last_draw_generation, surface->last_clear_generation,
        surface->last_copy_generation, surface->last_blit_generation,
        surface->last_resolve_generation, surface->last_other_generation,
        surface->last_guest_blit_generation,
        surface->last_present_copy_generation,
        g_ld_fifo_get, g_ld_fifo_put, g.cb_used, g.vb_used,
        g.srv_ring_used,
        (unsigned long long)g_ld_stats.groups_executed,
        (unsigned long long)g_ld_stats.group_drop_ring,
        (unsigned long long)g_ld_vertex_constant_ring_recycles,
        g_ld_frame_vertex_constant_ring_recycles,
        (unsigned long long)g_ld_last_dump_fingerprint,
        nonblack == UINT64_MAX ? "unknown:" : "",
        (unsigned long long)(nonblack == UINT64_MAX ? 0 : nonblack),
        path);
    fflush(stderr);
}
#endif

int rsx_live_draw_debug_dump_surface(
    u32 location, u32 offset, const char* path)
{
    if (!g.ready || !path || !path[0]) return -1;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* surface = &g.surfaces[i];
        if (surface->location != location || surface->offset != offset)
            continue;
        const u64 nonblack = ld_dump_surface_ppm(path, surface);
        if (nonblack == UINT64_MAX) return -1;
        fprintf(stderr,
                "[live-replay-boundary] surface=%u:%08X index=%u "
                "size=%ux%u fmt=%d nonblack=%llu path=%s\n",
                location, offset, i, surface->w, surface->h, (int)surface->fmt,
                (unsigned long long)nonblack, path);
        fflush(stderr);
        return 0;
    }
    fprintf(stderr,
            "[live-replay-boundary] missing surface=%u:%08X path=%s\n",
            location, offset, path);
    return 1;
}

static void ld_vertex_diag_emit(const char* reason, int dump_surface)
{
    if (!g.ready || !g.vertex_diag_dir[0])
        return;
    rsx_dsp_surface state_surface;
    rsx_dsp_get_surface(&g.rsx, &state_surface);
    u32 current = LD_INVALID_SURFACE;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].location == state_surface.color_location[0] &&
            g.surfaces[i].offset == state_surface.color_offset[0]) {
            current = i;
            break;
        }
    }
    const u32 presented = g_ld_last_present_target;
    const surface_t* presented_surface =
        presented < g.n_surfaces ? &g.surfaces[presented] : NULL;
    char attrs[80];
    size_t used = 0;
    attrs[0] = '\0';
    for (u32 attr = 0; attr < 16; attr++) {
        if (!(g_ld_current_pso.input_mask & (1u << attr)))
            continue;
        const int written = snprintf(
            attrs + used, sizeof(attrs) - used,
            "%s%u", used ? "," : "", attr);
        if (written < 0 || (size_t)written >= sizeof(attrs) - used)
            break;
        used += (size_t)written;
    }
    const UINT64 d3d_messages = g_ld_info_queue
        ? g_ld_info_queue->lpVtbl->GetNumStoredMessages(g_ld_info_queue)
        : 0;
#if defined(YZ_PERF_PROFILE)
    const u64 pso_lookups = g_ld_profile.total.pso_lookups;
    const u64 pso_hits = g_ld_profile.total.pso_hits;
    const u64 pso_misses = g_ld_profile.total.pso_misses;
    const u64 pso_full = g_ld_profile.total.pso_full;
#else
    const u64 pso_lookups = 0, pso_hits = 0, pso_misses = 0, pso_full = 0;
#endif
    fprintf(stderr,
            "[rsx-vertex-state] reason=%s mode=%s frame=%u "
            "flip{requested=%llu last_req=%u consumed=%llu last_cons=%u} "
            "surface{present=%u off=0x%08X size=%ux%u current=%u} "
            "draw{packets=%llu groups=%llu exec=%llu "
            "drop_fetch=%llu drop_pso=%llu drop_ring=%llu} "
            "pso{look=%llu hit=%llu miss=%llu full=%llu "
            "cached=%u capacity=%u} "
            "vp{hash=%016llX start=%u instrs=%u mask=0x%04X "
            "attrs=[%s] stride=%u packed=%u} "
            "d3d{debug=%d queued_messages=%llu}\n",
            reason ? reason : "unknown", ld_vertex_mode_name(), g_ld_frames,
            (unsigned long long)g_ld_flip_requested,
            g_ld_last_requested_buffer,
            (unsigned long long)g_ld_flip_consumed,
            g_ld_last_consumed_buffer,
            presented,
            presented_surface ? presented_surface->offset : 0,
            presented_surface ? presented_surface->w : 0,
            presented_surface ? presented_surface->h : 0,
            current,
            (unsigned long long)g_ld_stats.packets_seen,
            (unsigned long long)g_ld_stats.groups_seen,
            (unsigned long long)g_ld_stats.groups_executed,
            (unsigned long long)g_ld_stats.group_drop_fetch,
            (unsigned long long)g_ld_stats.group_drop_pso,
            (unsigned long long)g_ld_stats.group_drop_ring,
            (unsigned long long)pso_lookups,
            (unsigned long long)pso_hits,
            (unsigned long long)pso_misses,
            (unsigned long long)pso_full,
            g.n_psos, MAX_PSOS,
            (unsigned long long)g_ld_current_pso.vp_hash,
            g_ld_current_pso.vp_start,
            g_ld_current_pso.vp_instrs,
            g_ld_current_pso.input_mask,
            attrs,
            g_ld_current_pso.input_stride,
            g_ld_current_pso.packed_offsets,
            g_ld_debug_layer_enabled,
            (unsigned long long)d3d_messages);
    if (dump_surface && presented_surface && g.vertex_diag_dir[0]) {
        char path[MAX_PATH * 2];
        snprintf(
            path, sizeof(path), "%s\\last_present_surface.ppm",
            g.vertex_diag_dir);
        const u64 nonblack =
            ld_dump_surface_ppm(path, presented_surface);
        fprintf(stderr,
                "[rsx-vertex-surface] mode=%s target=%u "
                "pixels=%llu nonblack=%s%llu path=%s\n",
                ld_vertex_mode_name(), presented,
                (unsigned long long)presented_surface->w *
                    presented_surface->h,
                nonblack == UINT64_MAX ? "unknown:" : "",
                (unsigned long long)(
                    nonblack == UINT64_MAX ? 0 : nonblack),
                path);
    }
    fflush(stderr);
}

void rsx_live_draw_present(u32 buffer_id)
{
    if (!g.ready) return;

    /* LD_SURF_DUMP=<dir>: dump EVERY live surface once, with its non-black
     * count, at LD_SURF_DUMP_FRAME (default 1200).
     *
     * LD_FRAME_DUMP only reads back the presented surface, which answers "is
     * the screen black" (it is: nonblack=0 on every frame) but not WHERE the
     * pixels stop. ps1_netemu composites PS1 VRAM into a 720x512 surface and
     * then upscales into the presented 1280x720 one, so the interesting
     * question is which of those is already empty. */
    { static int done = 0; const char* sd = getenv("LD_SURF_DUMP");
      u32 at = 1200; { const char* fe = getenv("LD_SURF_DUMP_FRAME");
                       if (fe) at = (u32)strtoul(fe, 0, 0); }
      /* LD_SURF_DUMP_ON_VRAM=1 waits for PS1 VRAM to hold a drawn frame instead
       * of firing at a frame number. Needs PS1_PC=1, which is what samples it. */
      int gate;
      { static int onv = -1;
        if (onv < 0) onv = getenv("LD_SURF_DUMP_ON_VRAM") ? 1 : 0;
        gate = onv ? (g_ld_ps1_vram_ready != 0) : (g_ld_frames >= at);
        /* LD_SURF_DUMP_GATE=<file>: fire when <file> appears instead. */
        static const char* gf = (const char*)-1;
        if (gf == (const char*)-1) gf = getenv("LD_SURF_DUMP_GATE");
        /* Re-arms when the file goes away, so each appearance dumps once. */
        if (gf && sd) { FILE* f = fopen(gf, "rb"); gate = f != NULL; if (f) fclose(f);
                        if (!gate) done = 0; } }
      if (sd && sd[0] && !done && gate) {
          done = 1;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              char path[MAX_PATH * 2];
              snprintf(path, sizeof(path), "%s\\f%06u_surf_%02u_%08X_%ux%u.ppm",
                       sd, g_ld_frames, i, g.surfaces[i].offset,
                       g.surfaces[i].w, g.surfaces[i].h);
              const u64 nb = ld_dump_surface_ppm(path, &g.surfaces[i]);
              fprintf(stderr, "[surf-dump] slot=%u %u:0x%08X %ux%u nonblack=%llu"
                              " draw_gen=%u clear_gen=%u\n",
                      i, g.surfaces[i].location, g.surfaces[i].offset,
                      g.surfaces[i].w, g.surfaces[i].h,
                      (unsigned long long)(nb == UINT64_MAX ? 0 : nb),
                      g.surfaces[i].last_draw_generation,
                      g.surfaces[i].last_clear_generation);
          }
      } }
    /* LD_FRAME_DUMP=<dir> [+ LD_FRAME_DUMP_EVERY=<n>, default 300]: write the
     * PRESENTED surface to a .ppm every n flips. The engine could already dump
     * a surface, but only at shutdown or behind the parity harness -- neither
     * reachable when a title is simply left running, which is exactly when you
     * want to see what is on screen. PrintWindow and CopyFromScreen both come
     * back white for a D3D12 swapchain, so a readback here is the only way to
     * answer "what is it actually drawing?". */
    /* The dump itself runs once the presented surface is known (below): a
     * title that scans out through cellResc never matches a registered display
     * buffer and presents the fallback surface, so matching here dumped nothing. */
    static int dump_every = -1; static const char* dump_dir; static unsigned dump_n;
    if (dump_every < 0) { dump_dir = getenv("LD_FRAME_DUMP");
                          const char* e = getenv("LD_FRAME_DUMP_EVERY");
                          dump_every = (dump_dir && dump_dir[0]) ? (e ? atoi(e) : 300) : 0;
                          if (dump_every <= 0 && dump_dir && dump_dir[0]) dump_every = 300; }
    /* LD_FRAME_DUMP_FROM=<frame>: start dumping there (dense captures of a
     * late scene, e.g. frames for a GIF, without thousands of early files). */
    static long dump_from = -1;
    if (dump_from < 0) { const char* e = getenv("LD_FRAME_DUMP_FROM"); dump_from = e ? atol(e) : 0; }
    const int dump_now = dump_every > 0 && (long)g_ld_frames >= dump_from &&
                         (dump_n++ % (unsigned)dump_every) == 0;
    /* A flip names a registered display buffer. The current color target may
     * be an offscreen shadow/postprocess surface at that instant; copying it
     * caused a010 to present black despite executing the scene's draws. */
    u32 target = LD_INVALID_SURFACE;
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const display_buffer_t* display = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (g.surfaces[i].location == display->location &&
                g.surfaces[i].offset == display->offset) {
                target = i;
                break;
            }
        }
    }
    if (target == LD_INVALID_SURFACE && buffer_id < 8 &&
        g.display_buffers[buffer_id].valid && g.display_buffers[buffer_id].resolve_src > 0 &&
        (u32)g.display_buffers[buffer_id].resolve_src <= g.n_surfaces)
        target = (u32)g.display_buffers[buffer_id].resolve_src - 1;   /* see note_resolve */
    if (target == LD_INVALID_SURFACE) {
        target = current_surface();
        static u32 fallback_logs = 0;
        if (fallback_logs++ < 32)
            fprintf(stderr,
                    "[live-draw] flip %u has no registered/rendered scanout; "
                    "falling back to current surface %u\n",
                    buffer_id, target);
    }
    if (target == LD_INVALID_SURFACE) {
        fprintf(stderr, "[live-draw] frame present skipped: no color surface\n");
        return;
    }
    g_ld_last_present_target = target;
    /* While an RSX_DRAW_CSV capture is open: a #flip row per present, and the
     * first six presented frames of each capture into LD_DRAW_DUMP (flicker
     * shows up as consecutive frames that differ). */
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#flip,%u,buffer=%u,slot=%u,0x%X,%ux%u\n", g_ld_frames,
                buffer_id, target, g.surfaces[target].offset,
                g.surfaces[target].w, g.surfaces[target].h);
        fflush(s_csv_file);
        static const char* flipDir = (const char*)-1;
        static u32 flipSession = 0;
        static u32 flipDumps = 0;
        if (flipDir == (const char*)-1) {
            flipDir = getenv("LD_DRAW_DUMP");
        }
        if (flipSession != s_csv_gate_session) {
            flipSession = s_csv_gate_session;
            flipDumps = 0;
        }
        if (flipDir && flipDir[0] && flipDumps < 6u) {
            char flipPath[MAX_PATH * 2];
            snprintf(flipPath, sizeof(flipPath), "%s\\flip_c%u_f%06u_slot%u.ppm", flipDir,
                     flipSession, g_ld_frames, target);
            ld_dump_surface_ppm(flipPath, &g.surfaces[target]);
            flipDumps++;
        }
    }
    if (dump_now) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%s\\frame_%06u.ppm", dump_dir, dump_n - 1u);
        const u64 nb = ld_dump_surface_ppm(path, &g.surfaces[target]);
        fprintf(stderr, "[frame-dump] %s %ux%u nonblack=%llu\n", path,
                g.surfaces[target].w, g.surfaces[target].h,
                (unsigned long long)(nb == UINT64_MAX ? 0 : nb));
        fflush(stderr);
    }
    /* LD_RECORD=<out.mp4> (boot_main's --record): every presented frame, as
     * PPM, into an ffmpeg pipe -- ffmpeg's image2pipe reads concatenated PPMs
     * as a stream. Fragmented MP4 so a run that is killed rather than exiting
     * still leaves a playable file. Frames are stamped at a fixed 30 fps, so
     * playback speed is only right when the title presents at 30.
     * ponytail: one blocking GPU readback per frame; a ring of readback
     * buffers if recording ever has to run at full speed. */
    { static FILE* rec = (FILE*)-1;
      if (rec == (FILE*)-1) {
          const char* out = getenv("LD_RECORD");
          rec = NULL;
          if (out && out[0]) {
              char cmd[MAX_PATH * 2];
              snprintf(cmd, sizeof(cmd),
                       "ffmpeg -loglevel error -y -f image2pipe -framerate 30 -c:v ppm -i - "
                       "-vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" -pix_fmt yuv420p "
                       "-movflags frag_keyframe+empty_moov \"%s\"", out);
              rec = _popen(cmd, "wb");
              fprintf(stderr, "[record] %s -> %s\n", rec ? "ffmpeg" : "FAILED to start ffmpeg", out);
          }
      }
      if (rec) {
          g_ld_record_to = rec;
          ld_dump_surface_ppm(NULL, &g.surfaces[target]);
          g_ld_record_to = NULL;
          fflush(rec);
      } }
    const u32 current = current_surface();
    surface_t* presented_surface = &g.surfaces[target];
    /* LD_PRESENT_DBG=1: is this surface actually finished? A surface whose last
     * clear is NEWER than its last draw has been wiped and not redrawn -- that
     * presents as a black frame. Counting them separates "we present the wrong
     * buffer" from "we present the right buffer too early". */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("LD_PRESENT_DBG") ? 1 : 0;
      if (dbg) {
          static unsigned long long n = 0, blank = 0;
          n++;
          if (presented_surface->last_draw_generation <
              presented_surface->last_clear_generation) blank++;
          if ((n % 256) == 0)
              /* Also say WHICH surface. Surfaces are now keyed on size as well
               * as offset, so several exist at once and "we present the wrong
               * one" is a live possibility -- the PS1 composite is 720x512 and
               * the PS3 output 1280x720. */
              fprintf(stderr,
                      "[present] %llu/%llu presents showed a surface cleared "
                      "after its last draw (%.1f%%); target=%u %u:0x%X %ux%u"
                      " draw_gen=%u clear_gen=%u\n",
                      (unsigned long long)blank, (unsigned long long)n,
                      100.0 * (double)blank / (double)n,
                      target, presented_surface->location,
                      presented_surface->offset,
                      presented_surface->w, presented_surface->h,
                      presented_surface->last_draw_generation,
                      presented_surface->last_clear_generation);
      } }
#if !defined(YZ_PERF_CLEAN)
    presented_surface->last_present_copy_generation =
        ++g_ld_present_copy_generation;
    yz_frontier_trace_emit(
        YZ_FT_PARITY_FLIP, buffer_id, target,
        (u32)g_ld_flip_requested,
        presented_surface->last_draw_generation,
        presented_surface->last_clear_generation,
        g_ld_fifo_get, g_ld_fifo_put, current);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_SURFACE, target, presented_surface->last_write_kind,
        presented_surface->last_write_generation,
        presented_surface->last_draw_generation,
        presented_surface->last_clear_generation,
        presented_surface->last_copy_generation,
        presented_surface->last_blit_generation,
        presented_surface->last_resolve_generation);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_SURFACE_AUX, target,
        presented_surface->resource_serial,
        presented_surface->last_create_generation,
        presented_surface->last_other_generation,
        presented_surface->last_guest_blit_generation,
        presented_surface->offset, presented_surface->location,
        presented_surface->last_present_copy_generation);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_RENDER, target, current,
        (u32)g_ld_stats.groups_executed,
        (u32)g_ld_stats.group_drop_ring,
        g.cb_used, g.vb_used, g.srv_ring_used,
        presented_surface->last_present_copy_generation);
#endif
    { static u32 last_present_target = LD_INVALID_SURFACE;
      if (target != last_present_target) {
          ld_trace_target("present", target, buffer_id);
          last_present_target = target;
      } }
    ID3D12Resource* srcimg = presented_surface->tex;
    ld_sync_swapchain_size();
    if (!g.ready) return;
    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];
    const u32 sourceWidth = ld_surface_x(presented_surface, presented_surface->w);
    const u32 sourceHeight = ld_surface_scaled(presented_surface, presented_surface->h);

    D3D12_RESOURCE_BARRIER bar[2] = {0};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = srcimg;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[1].Transition.pResource = bb;
    bar[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    /* The present pass when the image and back buffer differ in size or
     * format, or FXAA is on; otherwise the plain copy it always was. */
    const int presentPass = g.present_pso[LD_PRESENT_SCALE] &&
        (sourceWidth != g.bb_w || sourceHeight != g.bb_h ||
         presented_surface->fmt != DXGI_FORMAT_R8G8B8A8_UNORM ||
         InterlockedCompareExchange(&g_ld_post_aa, 0, 0) != 0 ||
         InterlockedCompareExchange(&g_ld_sharpen_percent, 0, 0) != 0);
    if (presentPass)
    {
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, bar);
        const int drawn = ld_present_pass(SRV_SURFACE_BASE + target, sourceWidth,
                                          sourceHeight, bb, bbi, 1);
        bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, bar);
        ld_overlay_then_present_state(bb, bbi, drawn ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                     : D3D12_RESOURCE_STATE_PRESENT);
    }
    else
    {
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);

        g.list->lpVtbl->CopyResource(g.list, bb, srcimg);

        bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, bar);
        ld_overlay_then_present_state(bb, bbi, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    /* Submit without waiting for the GPU (frame pipelining). */
    ld_submit_frame();
    {
        const u64 swapBegin = gcm_perf_now_ns();
        const HRESULT present_hr = g.swap->lpVtbl->Present(g.swap, 1, 0);
        g_gcm_perf_swap_ns += gcm_perf_now_ns() - swapBegin;
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames + 1u);
        else {
            fprintf(stderr,
                    "[d3d-fail] Present hr=0x%08lX frame=%u "
                    "target=%u size=%ux%u window=%ux%u\n",
                    (unsigned long)present_hr, g_ld_frames,
                    target, g.surfaces[target].w, g.surfaces[target].h,
                    g.width, g.height);
            ld_dump_dred("Present", present_hr);
            g.ready = 0;
        }
    }
#if !defined(YZ_PERF_CLEAN)
    if (InterlockedCompareExchange(
            &g_ld_diag_post_movie_pending, 0, 0) != 0) {
        const LONG post_movie_present =
            InterlockedIncrement(&g_ld_diag_post_movie_presents);
        if (post_movie_present == 32 &&
            InterlockedCompareExchange(
                &g_ld_diag_post_movie_pending, 0, 1) == 1) {
            char path[MAX_PATH * 2];
            snprintf(
                path, sizeof(path),
                "%s\\semantic_checkpoint_surface.ppm",
                g.vertex_diag_dir);
            const u64 nonblack =
                ld_dump_surface_ppm(path, &g.surfaces[target]);
            fprintf(stderr,
                    "[rsx-vertex-surface] reason=post-movie-32 "
                    "mode=%s target=%u pixels=%llu "
                    "nonblack=%s%llu path=%s\n",
                    ld_vertex_mode_name(), target,
                    (unsigned long long)g.surfaces[target].w *
                        g.surfaces[target].h,
                    nonblack == UINT64_MAX ? "unknown:" : "",
                    (unsigned long long)(
                        nonblack == UINT64_MAX ? 0 : nonblack),
                    path);
            fflush(stderr);
        }
    }
#endif

    { static unsigned long long packets_at_last_frame = 0;
      g_ld_last_frame_draws = (u32)(g_ld_stats.packets_seen - packets_at_last_frame);
      packets_at_last_frame = g_ld_stats.packets_seen; }
    g_ld_frames++;
    rsx_geom_cache_end_frame(g_ld_frames);
#if defined(YZ_PERF_PROFILE)
    ld_profile_present(g_ld_frames);
#endif
#if !defined(YZ_PERF_CLEAN)
    /* Autonomous parity capture.  Scalar records remain circular and cheap.
     * Readback begins only after two alternating display surfaces stop
     * receiving writes while FIFO progress continues, and again for the first
     * 16 held-movement presentations.  The latter continues counting to 120
     * without readback so validation does not materially alter movement. */
    {
        static int parity_enabled = -1;
        static char parity_dir[MAX_PATH * 2];
        static u32 observed_write[MAX_SURFACES];
        static u8 observed_valid[MAX_SURFACES];
        static u32 prior_target = LD_INVALID_SURFACE;
        static u32 prior_get;
        static u32 prior_put;
        static u32 dialogue_stable;
        static u64 dialogue_targets;
        static int dialogue_armed;
        static u32 dialogue_captures;
        static u32 movement_frames;
        static u32 movement_captures;
        if (parity_enabled < 0) {
            const char* enabled = getenv("RSX_PARITY_DIAG");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            parity_enabled = enabled && *enabled && directory && *directory;
            if (parity_enabled) {
                strncpy(parity_dir, directory, sizeof(parity_dir) - 1u);
                parity_dir[sizeof(parity_dir) - 1u] = '\0';
            }
        }
        if (parity_enabled) {
            const LONG movement_phase = InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0);
            const u64 elapsed = g_yz_auto_start_tick
                ? GetTickCount64() - g_yz_auto_start_tick : 0;
            surface_t* surface = &g.surfaces[target];
            const int fifo_progress =
                g_ld_fifo_get != prior_get || g_ld_fifo_put != prior_put;
            const int alternating =
                prior_target != LD_INVALID_SURFACE && prior_target != target;
            const int unchanged = observed_valid[target] &&
                observed_write[target] == surface->last_write_generation;

            if (!dialogue_armed && !dialogue_captures &&
                movement_phase == 0 &&
                ((g_ld_frame_vertex_constant_ring_recycles != 0 &&
                  elapsed >= 300000ull) || elapsed >= 420000ull)) {
                if (g_ld_frame_vertex_constant_ring_recycles != 0) {
                    dialogue_armed = 1;
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE HIGH-DRAW TRIGGER "
                        "elapsed_ms=%llu frame_recycles=%u total_recycles=%llu "
                        "drop_ring=%llu frame=%u flip=%llu\n",
                        (unsigned long long)elapsed,
                        g_ld_frame_vertex_constant_ring_recycles,
                        (unsigned long long)
                            g_ld_vertex_constant_ring_recycles,
                        (unsigned long long)g_ld_stats.group_drop_ring,
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                } else if (fifo_progress && alternating && unchanged) {
                    dialogue_stable++;
                    if (target < 64u)
                        dialogue_targets |= 1ull << target;
                } else if (fifo_progress) {
                    dialogue_stable = 0;
                    dialogue_targets = target < 64u ? 1ull << target : 0;
                }
                if (dialogue_stable >= 12u &&
                    dialogue_targets &&
                    (dialogue_targets & (dialogue_targets - 1u)) != 0) {
                    dialogue_armed = 1;
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE TRIGGER elapsed_ms=%llu "
                        "stable=%u targets=%016llX drop_ring=%llu "
                        "frame=%u flip=%llu\n",
                        (unsigned long long)elapsed, dialogue_stable,
                        (unsigned long long)dialogue_targets,
                        (unsigned long long)g_ld_stats.group_drop_ring,
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                }
            }
            if (dialogue_armed && dialogue_captures < 16u) {
                const u32 capture = ++dialogue_captures;
                ld_parity_capture_surface(
                    parity_dir, "dialogue", capture, buffer_id, target);
                if (capture == 16u) {
                    dialogue_armed = 0;
                    yz_frontier_trace_dump(5u);
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE COMPLETE captures=16 "
                        "frame=%u flip=%llu\n",
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                }
            }

            if (movement_phase == 4) {
                movement_frames++;
                if (movement_captures < 16u) {
                    const u32 capture = ++movement_captures;
                    ld_parity_capture_surface(
                        parity_dir, "movement", capture, buffer_id, target);
                    if (capture == 16u)
                        yz_frontier_trace_dump(6u);
                }
                if (movement_frames == 120u) {
                    yz_frontier_trace_dump(7u);
                    fprintf(
                        stderr,
                        "[parity-diag] MOVEMENT 120 COMPLETE "
                        "captures=16 frame=%u flip=%llu groups=%llu "
                        "drop_ring=%llu\n",
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested,
                        (unsigned long long)g_ld_stats.groups_executed,
                        (unsigned long long)g_ld_stats.group_drop_ring);
                    fflush(stderr);
                }
            }

            observed_write[target] = surface->last_write_generation;
            observed_valid[target] = 1;
            prior_target = target;
            prior_get = g_ld_fifo_get;
            prior_put = g_ld_fifo_put;
        }
    }
    /* Capture the actual presented surface at acknowledged input boundaries.
     * This is independent of desktop visibility/focus and is inert unless the
     * promotion controller explicitly enables RSX_MOVEMENT_PROOF. */
    {
        static int movement_capture = -1;
        static char movement_dir[MAX_PATH * 2];
        if (movement_capture < 0) {
            const char* enabled = getenv("RSX_MOVEMENT_PROOF");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            movement_capture = enabled && directory && *directory;
            if (movement_capture) {
                strncpy(movement_dir, directory,
                        sizeof(movement_dir) - 1u);
                movement_dir[sizeof(movement_dir) - 1u] = '\0';
            }
        }
        if (movement_capture) {
            const LONG phase = InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0);
            static u64 next_probe_tick;
            static u32 probe_serial;
            static u64 probe_delay_ms;
            static u64 probe_interval_ms;
            static int probe_configured;

            if (!probe_configured) {
                const char* delay = getenv("RSX_MOVEMENT_PROOF_DELAY_MS");
                const char* interval = getenv("RSX_MOVEMENT_PROBE_INTERVAL_MS");
                probe_delay_ms = delay && *delay
                    ? _strtoui64(delay, NULL, 10) : 780000ull;
                probe_interval_ms = interval && *interval
                    ? _strtoui64(interval, NULL, 10) : 0ull;
                if (probe_interval_ms && probe_interval_ms < 30000ull)
                    probe_interval_ms = 30000ull;
                probe_configured = 1;
            }

            /* A time threshold alone cannot distinguish authored camera work
             * from playable Akiyama.  In visual-arm mode, retain occasional
             * renderer-surface probes while dialogue input continues.  The
             * proof is armed only after one of these images visibly shows the
             * gameplay HUD, avoiding timing-dependent false positives. */
            if (phase == 0 && probe_interval_ms && g_yz_auto_start_tick) {
                const u64 now = GetTickCount64();
                const u64 elapsed = now - g_yz_auto_start_tick;
                if (elapsed >= probe_delay_ms &&
                    (!next_probe_tick || now >= next_probe_tick)) {
                    char path[MAX_PATH * 2];
                    const u32 serial = ++probe_serial;
                    snprintf(path, sizeof(path),
                             "%s\\frontier_probe_%03u.ppm",
                             movement_dir, serial);
                    {
                        const u64 nonblack =
                            ld_dump_surface_ppm(path, &g.surfaces[target]);
                        fprintf(stderr,
                                "[movement-proof] frontier-probe serial=%u "
                                "elapsed_ms=%llu frame=%u nonblack=%s%llu "
                                "path=%s\n",
                                serial, (unsigned long long)elapsed,
                                g_ld_frames,
                                nonblack == UINT64_MAX ? "unknown:" : "",
                                (unsigned long long)(
                                    nonblack == UINT64_MAX ? 0 : nonblack),
                                path);
                        fflush(stderr);
                    }
                    next_probe_tick = now + probe_interval_ms;
                }
            }
            const char* name = NULL;
            LONG next_phase = phase;
            if (phase == 1) {
                name = "movement_before.ppm";
                next_phase = 2;
            } else if (phase == 3) {
                name = "camera_after.ppm";
                next_phase = 4;
            } else if (phase == 5) {
                name = "movement_after_60s.ppm";
                next_phase = 6;
            }
            if (name) {
                char path[MAX_PATH * 2];
                snprintf(path, sizeof(path), "%s\\%s", movement_dir, name);
                ld_movement_camera_snapshot(phase);
                {
                    const u64 nonblack =
                        ld_dump_surface_ppm(path, &g.surfaces[target]);
                    fprintf(stderr,
                            "[movement-proof] capture phase=%ld frame=%u "
                            "flip=%llu nonblack=%s%llu path=%s\n",
                            phase, g_ld_frames,
                            (unsigned long long)g_ld_flip_requested,
                            nonblack == UINT64_MAX ? "unknown:" : "",
                            (unsigned long long)(
                                nonblack == UINT64_MAX ? 0 : nonblack),
                            path);
                    fflush(stderr);
                }
                InterlockedCompareExchange(
                    &g_yz_movement_proof_phase, next_phase, phase);
                if (next_phase == 6) {
                    fprintf(stderr,
                            "[movement-proof] COMPLETE sustained_ms=60000 "
                            "camera_ms=5000 frame=%u\n", g_ld_frames);
                    fflush(stderr);
                }
            }
        }
    }
    /* Narrow validation readback for the four user-visible orphanage shots.
     * This captures the renderer's actual presented surface and therefore is
     * independent of desktop focus, window visibility, and screenshot APIs.
     * It is inert unless the validation controller supplies both variables. */
    {
        static int validation_capture = -1;
        static u32 validation_dumped = 0;
        static u64 validation_origin = UINT64_MAX;
        static char validation_dir[MAX_PATH * 2];
        if (validation_capture < 0) {
            const char* enabled = getenv("RSX_VALIDATION_CAPTURE");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            validation_capture = enabled && directory && *directory;
            if (validation_capture) {
                strncpy(validation_dir, directory,
                        sizeof(validation_dir) - 1u);
                validation_dir[sizeof(validation_dir) - 1u] = '\0';
            }
        }
        /* The a010 file-lifetime gate is the authoritative scene boundary in
         * clean production builds.  The older exact-mesh gate was useful for
         * a geometry experiment but is not guaranteed to occur in every
         * authored camera block, so using it for validation can miss the
         * entire scene even while valid draws are being presented. */
        if (validation_capture && validation_origin == UINT64_MAX &&
            InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0) {
            validation_origin = g_ld_flip_requested;
            fprintf(stderr,
                    "[validation-capture] a010 origin flip=%llu frame=%u\n",
                    (unsigned long long)validation_origin, g_ld_frames);
        }
        if (validation_capture && validation_origin != UINT64_MAX) {
            /* Scene-open itself is a 43-flip black/fade interval.  Preserve
             * the original four-shot spacing after that measured lead-in so
             * the promotion run lands on the outdoor ground, children,
             * over-shoulder hair, and face close-up rather than the fade,
             * sink, and pre-pickup phone shots. */
            static const u64 offsets[] = {43, 128, 302, 316};
            static const char* labels[] = {
                "background-ground", "children-eyes", "hair", "face-eyes"
            };
            for (u32 i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
                const u64 requested = validation_origin + offsets[i];
                if (!(validation_dumped & (1u << i)) &&
                    g_ld_flip_requested >= requested) {
                    char path[MAX_PATH * 2];
                    snprintf(path, sizeof(path),
                             "%s\\orphanage_gpu_%s_flip_%llu.ppm",
                             validation_dir, labels[i],
                             (unsigned long long)requested);
                    const u64 nonblack =
                        ld_dump_surface_ppm(path, &g.surfaces[target]);
                    fprintf(stderr,
                            "[validation-capture] label=%s requested_flip=%llu "
                            "actual_flip=%llu frame=%u target=%u "
                            "nonblack=%s%llu path=%s\n",
                            labels[i], (unsigned long long)requested,
                            (unsigned long long)g_ld_flip_requested,
                            g_ld_frames, target,
                            nonblack == UINT64_MAX ? "unknown:" : "",
                            (unsigned long long)(
                                nonblack == UINT64_MAX ? 0 : nonblack),
                            path);
                    validation_dumped |= 1u << i;
                }
            }
        }
    }
#endif
    /* First 32 frames verbatim, then every 32nd: keeps the log bounded while
     * making the TRUE frame count measurable from the log. (The old hard cap
     * at 32 made "stalls at frame ~32" unfalsifiable from the .err alone.) */
    if (g_ld_frames <= 32 || (g_ld_frames & 31) == 0)
        fprintf(stderr,
                "[live-draw] frame %u packets[seen=%llu queued=%llu movie=%llu qfull=%llu] "
                "groups[seen=%llu exec=%llu empty=%llu drop{fetch=%llu degen=%llu prim=%llu "
                "alloc=%llu pso=%llu ring=%llu surface=%llu}] clears[guest=%llu badsurf=%llu implicitZ=%llu] "
                "textures[cached=%u/%u full=%llu decodefail=%llu depthSRV=%llu rejectZ=%llu] "
                "vtex[cached=%u enabled=%llu binds=%llu uploads=%llu refresh=%llu "
                "unsupported=%llu missingTXL=%llu] divider=%llu "
                "balance[p=%s g=%s] binds[white=%llu real=%llu surf=%llu] (cumulative)\n",
                g_ld_frames,
                g_ld_stats.packets_seen, g_ld_stats.packets_queued,
                g_ld_stats.packets_movie, g_ld_stats.packets_queue_full,
                g_ld_stats.groups_seen, g_ld_stats.groups_executed,
                g_ld_stats.groups_empty, g_ld_stats.group_drop_fetch,
                g_ld_stats.group_drop_degenerate, g_ld_stats.group_drop_primitive,
                g_ld_stats.group_drop_alloc, g_ld_stats.group_drop_pso,
                g_ld_stats.group_drop_ring, g_ld_stats.group_drop_surface,
                g_ld_stats.clears, g_ld_stats.clear_drop_surface,
                g_ld_stats.implicit_depth_clears,
                g.n_textures, MAX_TEXTURES,
                (unsigned long long)g_ld_texture_cache_full,
                (unsigned long long)g_ld_texture_decode_fail,
                (unsigned long long)g_ld_zdepth_srv_binds,
                (unsigned long long)g_ld_zdepth_srv_reject_no_write,
                g.n_vtex,
                (unsigned long long)g_ld_vtex_enabled,
                (unsigned long long)g_ld_vtex_binds,
                (unsigned long long)g_ld_vtex_uploads,
                (unsigned long long)g_ld_vtex_refreshes,
                (unsigned long long)g_ld_vtex_unsupported,
                (unsigned long long)g_ld_vtex_missing_for_txl,
                (unsigned long long)g_ld_divider_fetches,
                g_ld_stats.packets_seen == g_ld_stats.packets_queued +
                    g_ld_stats.packets_movie + g_ld_stats.packets_queue_full ? "ok" : "BAD",
                g_ld_stats.groups_seen == ld_groups_accounted() ? "ok" : "BAD",
                g_ld_bind_white, g_ld_bind_real, g_ld_bind_surf);
    /* [hitch]: one line for each frame that took much longer than a vblank,
     * with where that frame's time went. The [fps] lines average over five
     * seconds, so a single 150 ms stall (a dialogue box opening, say) only
     * shows there as 57 fps. PS3_HITCH_MS sets the threshold (default 50,
     * 0 turns it off); at most 400 lines a run. */
    { static int hitchThresholdMs = -1;
      static u64 previousTicks = 0;
      static u32 hitchLines = 0;
      static u64 lastWaitFlip = 0, lastUsleep = 0, lastSyscall = 0, lastHle = 0;
      static u64 lastDrain = 0, lastPresent = 0, lastGpu = 0, lastSwap = 0, lastRingWait = 0;
      static u64 lastFileNs = 0, lastFileBytes = 0, lastUploadBytes = 0;
      static u64 lastCompiles = 0, lastGeomMisses = 0;
      static LONG lastPsoJobs = 0, lastPsoSkipped = 0;
      static u32 lastSurfaces = 0;
      static ld_draw_perf lastPerf;
      if (hitchThresholdMs < 0)
      {
          const char* thresholdText = getenv("PS3_HITCH_MS");
          hitchThresholdMs = thresholdText ? atoi(thresholdText) : 50;
          if (hitchThresholdMs < 0)
          {
              hitchThresholdMs = 0;
          }
      }
      const u64 nowTicks = ld_perf_ticks();
      const u64 fileNs = ps3_fs_read_ns();
      const u64 fileBytes = ps3_fs_read_bytes();
      const u64 compiles = g_ld_shader_disk_misses[0] + g_ld_shader_disk_misses[1];
      const LONG psoJobs = InterlockedCompareExchange(&g_ld_pso_jobs_pending, 0, 0);
      const LONG psoSkipped = InterlockedCompareExchange(&g_ld_pso_draws_skipped, 0, 0);
      const ld_draw_perf nowPerf = g_ld_draw_perf;
      if (hitchThresholdMs > 0 && previousTicks != 0)
      {
          const double frameMs = ld_perf_ticks_to_ms(nowTicks - previousTicks);
          if (frameMs >= (double)hitchThresholdMs && hitchLines < 400u)
          {
              hitchLines++;
              fprintf(stderr,
                      "[hitch] frame %u: %.0f ms. game thread: waitflip %.1f, usleep %.1f, syscalls %.1f, HLE %.1f ms; "
                      "file reads %.1f ms (%llu KB, all threads); present thread: drain %.1f, execute+present %.1f, "
                      "gpu wait %.1f, swap %.1f ms, ring held %.1f ms; renderer %.1f ms (%llu draws: fetch %.1f, "
                      "shaders %.1f, textures %.1f); new shaders compiled %llu, shader jobs queued %ld, draws waiting "
                      "for shaders %ld; textures uploaded %llu KB; new surfaces %u; geometry misses %llu\n",
                      g_ld_frames, frameMs,
                      (double)(g_gcm_perf_waitflip_ns - lastWaitFlip) / 1e6,
                      (double)(g_gcm_perf_main_usleep_ns - lastUsleep) / 1e6,
                      (double)(g_gcm_perf_main_syscall_ns - lastSyscall) / 1e6,
                      (double)(g_gcm_perf_main_hle_ns - lastHle) / 1e6,
                      (double)(fileNs - lastFileNs) / 1e6,
                      (unsigned long long)((fileBytes - lastFileBytes) / 1024u),
                      (double)(g_gcm_perf_drain_ns - lastDrain) / 1e6,
                      (double)(g_gcm_perf_present_ns - lastPresent) / 1e6,
                      (double)(g_gcm_perf_gpu_wait_ns - lastGpu) / 1e6,
                      (double)(g_gcm_perf_swap_ns - lastSwap) / 1e6,
                      (double)(g_gcm_perf_ring_wait_ns - lastRingWait) / 1e6,
                      ld_perf_ticks_to_ms(nowPerf.totalTicks - lastPerf.totalTicks),
                      (unsigned long long)(nowPerf.draws - lastPerf.draws),
                      ld_perf_ticks_to_ms(nowPerf.fetchTicks - lastPerf.fetchTicks),
                      ld_perf_ticks_to_ms(nowPerf.shaderTicks - lastPerf.shaderTicks),
                      ld_perf_ticks_to_ms(nowPerf.bindTicks - lastPerf.bindTicks),
                      (unsigned long long)(compiles - lastCompiles),
                      (long)(psoJobs - lastPsoJobs),
                      (long)(psoSkipped - lastPsoSkipped),
                      (unsigned long long)((g_ld_texture_upload_bytes - lastUploadBytes) / 1024u),
                      g.n_surfaces - lastSurfaces,
                      (unsigned long long)(g_ld_geom_misses - lastGeomMisses));
          }
      }
      previousTicks = nowTicks;
      lastWaitFlip = g_gcm_perf_waitflip_ns; lastUsleep = g_gcm_perf_main_usleep_ns;
      lastSyscall = g_gcm_perf_main_syscall_ns; lastHle = g_gcm_perf_main_hle_ns;
      lastDrain = g_gcm_perf_drain_ns; lastPresent = g_gcm_perf_present_ns;
      lastGpu = g_gcm_perf_gpu_wait_ns; lastSwap = g_gcm_perf_swap_ns;
      lastRingWait = g_gcm_perf_ring_wait_ns;
      lastFileNs = fileNs; lastFileBytes = fileBytes;
      lastUploadBytes = g_ld_texture_upload_bytes;
      lastCompiles = compiles; lastGeomMisses = g_ld_geom_misses;
      lastPsoJobs = psoJobs; lastPsoSkipped = psoSkipped;
      lastSurfaces = g.n_surfaces;
      lastPerf = nowPerf; }
    /* [fps] heartbeat: direct frame-rate logging, one line per ~5s wall.
     * Exists because pace repeatedly had to be inferred from frame-counter
     * arithmetic and log ordering, and two such inferences were wrong in one
     * night (s42). Volume-bounded per LESSONS #6c. */
    { static ULONGLONG fps_t0 = 0; static u32 fps_f0 = 0;
      ULONGLONG now = GetTickCount64();
      if (fps_t0 == 0) { fps_t0 = now; fps_f0 = g_ld_frames; }
      else if (now - fps_t0 >= 5000) {
          /* PS1_PC=1: the R3000 state block (0x76C080) on the same heartbeat.
           *
           * READ +0x124 (instructions retired), NOT +0x108. The interpreter
           * loads PC from +0x108 on entry (lwz r26, 0x108(r23) at 0x001066CC)
           * and writes it back only in its epilogue (stw r26, 0x108(r23) at
           * 0x00106964) -- and it has not returned, because it is entered once
           * and loops internally. So +0x108 is stale from boot: 280,000 reads
           * across 14 samples gave 0xBFC00000 every time while +0x124 climbed
           * past a billion. Reading that as "stuck at the reset vector" would
           * be wrong twice over -- the epilogue does `ori r26, r26, 0x80`
           * first, so a real exit there could not even write 0xBFC00000.
           *
           * +0x124 is updated from inside the loop and is the honest progress
           * signal. Finding the LIVE pc needs the interpreter's register, not
           * this field. */
          { static int pc_on = -1;
            if (pc_on < 0) pc_on = getenv("PS1_PC") ? 1 : 0;
            if (pc_on) {
                extern uint32_t vm_read32(uint64_t);
                /* One read of +0x108 cannot tell a reset loop from a field that
                 * is simply not live between interpreter entries -- both look
                 * like a constant 0xBFC00000. So sample it hard: 20000 reads,
                 * and report how many DISTINCT values appear plus the lowest
                 * and highest. A live PC sweeps; a dead field does not. */
                uint32_t lo = 0xFFFFFFFFu, hi = 0, prev = 0, chg = 0;
                for (int k = 0; k < 20000; k++) {
                    uint32_t v = vm_read32(0x76C080u + 0x108u);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                    if (k && v != prev) chg++;
                    prev = v;
                }
                /* The PS1 GPU command ring. func_0010F658 pushes one 0x100-byte
                 * packet per GP0 batch (header 3 at +0, payload from +0x10) and
                 * advances a write offset; the four GPU SPUs consume it. If this
                 * offset never moves, the R3000 is not producing draw commands
                 * at all, and no amount of SPU-side work will help.
                 *
                 *   ring_off_ptr = *(TOC-0x794C)   (TOC = 0x1C3D30)
                 *   ring_base    = *(TOC-0x7918)
                 * from func_0010F658's first two instructions. */
                uint32_t roff_p = vm_read32(0x1BC3E4u);
                uint32_t rbase  = vm_read32(0x1BC418u);
                uint32_t roff   = roff_p ? vm_read32(roff_p) : 0xFFFFFFFFu;
                /* Each GPU SPU polls its OWN local store at 0x15010 for that
                 * offset (it reports that address to the PPU through its
                 * outbound mailbox during init), and a raw SPU's local store is
                 * aliased into guest memory at 0xE0000000 + n*0x100000 -- so
                 * these four reads see exactly what the SPU sees. If they match
                 * `off`, the kick landed and the SPUs are simply not reacting;
                 * if they do not, the kick is being lost. Opposite problems. */
                /* +0x15010 is the offset the PPU publishes to this SPU;
                 * +0x15020 is how far the SPU has actually CONSUMED (its work
                 * loop at 0x3158 advances it by 0x100 per packet under the same
                 * 0x007FFFFF mask the PPU uses, and stores it back). The pair
                 * separates "the SPUs are not being told" from "the SPUs are
                 * told and not working" from "both are fine and the problem is
                 * downstream in compositing". */
                uint32_t ls[4], cons[4];
                for (int n = 0; n < 4; n++) {
                    uint32_t w = 0xE0000000u + (uint32_t)n * 0x100000u;
                    ls[n]   = vm_read32(w + 0x15010u);
                    cons[n] = vm_read32(w + 0x15020u);
                }
                /* The scheduler's own state. func_00105FA8 hands the
                 * interpreter a budget at +0x120; when that is permanently zero
                 * the interpreter spins without executing. These are the fields
                 * it works from, so a frozen run says WHICH state it froze in
                 * rather than just that it froze. */
                fprintf(stderr, "[ps1] pc=0x%08X(chg=%u) exited=%u total=%u"
                                " cost=%u budget=%d reason=%d evhead=0x%08X"
                                " ring[off=0x%08X] pub[%06X %06X %06X %06X]"
                                " done[%06X %06X %06X %06X]\n",

                        lo, chg,
                        vm_read32(0x76C080u + 0x110u),
                        vm_read32(0x76C080u + 0x124u),
                        vm_read32(0x76C080u + 0x114u),
                        (int32_t)vm_read32(0x76C080u + 0x120u),
                        (int32_t)vm_read32(0x76C080u + 0x138u),
                        vm_read32(0x76C080u + 0x540u),
                        roff, ls[0], ls[1], ls[2], ls[3],
                        cons[0], cons[1], cons[2], cons[3]);
                /* Does PS1 VRAM actually have pixels in it?
                 *
                 * The texture the PS3 side binds for the PS1 framebuffer is
                 * location 1 (RSX local), offset 0x400000, 1024x512 fmt 0xE2 --
                 * exactly PS1 VRAM. Everything upstream of here is now known
                 * good (the four GPU SPUs consume every packet the R3000
                 * produces), so this is the first place the picture can go
                 * missing: either the SPUs are not writing pixels, or they are
                 * and our compositing drops them. Count non-zero words rather
                 * than guessing from a window capture -- PrintWindow on a D3D12
                 * swapchain cannot tell "black" from "capture failed". */
                { const u8* fb = guest_ptr(1u, 0x400000u, 1024u * 512u * 2u);
                  /* The guest EA of PS1 VRAM, once. Watching 0xC0400000 --
                   * derived from cellGcmSys's localAddress -- caught no writes
                   * at all from either the PPU store path or SPU DMA, which is
                   * either a real finding or a wrong address. This says which,
                   * instead of assuming. */
                  { static int shown = 0;
                    extern uint8_t* vm_base;
                    if (!shown && fb && vm_base) { shown = 1;
                        fprintf(stderr, "[ps1] VRAM guest EA = 0x%08X\n",
                                (u32)(size_t)(fb - vm_base)); } }
                  if (!fb) fprintf(stderr, "[ps1] vram: not mapped\n");
                  else {
                      u32 nz = 0, first = 0xFFFFFFFFu, n = 1024u * 512u / 2u;
                      const u32* w32 = (const u32*)fb;
                      for (u32 i = 0; i < n; i++)
                          if (w32[i]) { nz++; if (first == 0xFFFFFFFFu) first = i * 4u; }
                      fprintf(stderr, "[ps1] vram nonzero=%u/%u first=+0x%X\n",
                              nz, n, first == 0xFFFFFFFFu ? 0u : first);
                      /* PS1_EVENTS=1: the three kernel events the BIOS polls.
                       *
                       * The BIOS parks at 0xBFC53840 calling TestEvent
                       * (B-table 0x0B, trampoline 0xBFC58B60) on three
                       * descriptors loaded from PS1 kernel RAM 0x0000B21C,
                       * 0x0000B224 and 0x0000B228. Reading their event CLASS
                       * says what it is waiting for; the PS1 CD class is
                       * 0xF0000003.
                       *
                       * PS1 RAM base is *(TOC-0x79D4) = 0x00770780, and the
                       * interpreter reads PS1 memory with lwbrx -- PS1 RAM is
                       * stored LITTLE-endian in guest memory, so every read
                       * here has to be byte-swapped. Reading it big-endian
                       * would produce plausible-looking nonsense. */
                      { static int ev = -1; static int ev_done = 0;
                        if (ev < 0) ev = getenv("PS1_EVENTS") ? 1 : 0;
                        static int ev_beats = 0;
                        if (ev && !ev_done && ++ev_beats >= 3) {
                            ev_done = 1;
                            const u32 ram = vm_read32(0x001BC35Cu);
                            #define PS1LE(a) __builtin_bswap32(vm_read32((ram) + (u32)(a)))
                            const u32 tot_evcb = PS1LE(0x0120);
                            const u32 tot_size = PS1LE(0x0124);
                            fprintf(stderr, "[ps1ev] ram=0x%08X  EvCB tbl=0x%08X size=%u\n",
                                    ram, tot_evcb, tot_size);
                            /* All FIVE slots CdInit fills, not just the three
                             * the poll loop reads: -0x4de8/-0x4de4/-0x4de0/
                             * -0x4ddc/-0x4dd8 = 0xB218..0xB228. All zero means
                             * CdInit never ran; a mix means it ran partway. */
                            static const u32 slot[5] = { 0xB218u, 0xB21Cu, 0xB220u,
                                                         0xB224u, 0xB228u };
                            for (int k = 0; k < 5; k++) {
                                const u32 h = PS1LE(slot[k]);
                                fprintf(stderr, "[ps1ev]   slot 0x%04X handle=0x%08X",
                                        slot[k], h);
                                if ((h >> 24) == 0xF1u && tot_evcb) {
                                    const u32 idx = h & 0xFFFFu;
                                    const u32 cb  = (tot_evcb & 0x1FFFFFu) + idx * 0x1Cu;
                                    fprintf(stderr, "  idx=%u class=0x%08X status=0x%08X"
                                                    " spec=0x%08X mode=0x%08X",
                                            idx, PS1LE(cb + 0), PS1LE(cb + 4),
                                            PS1LE(cb + 8), PS1LE(cb + 12));
                                }
                                fprintf(stderr, "\n");
                            }
                            /* Dump the whole EvCB table. The table pointer
                             * itself is valid (0xA000E028, 448 bytes = 16
                             * entries of 0x1C), so the PS1 kernel IS up and
                             * events CAN be opened. Whether any ARE distinguishes
                             * "CdInit specifically never ran" from "nothing ever
                             * opens an event", which are very different faults.
                             * EvCB layout: class +0, status +4, spec +8,
                             * mode +0xC, func +0x10. */
                            { const u32 tb = tot_evcb & 0x1FFFFFu;
                              const u32 n = tot_size / 0x1Cu;
                              u32 live = 0;
                              for (u32 i = 0; i < n && i < 32; i++) {
                                  const u32 cb = tb + i * 0x1Cu;
                                  const u32 cls = PS1LE(cb + 0);
                                  const u32 st  = PS1LE(cb + 4);
                                  if (!cls && !st) continue;
                                  live++;
                                  fprintf(stderr, "[ps1ev]   EvCB[%2u] class=0x%08X"
                                                  " status=0x%08X spec=0x%08X"
                                                  " mode=0x%08X func=0x%08X\n",
                                          i, cls, st, PS1LE(cb + 8),
                                          PS1LE(cb + 12), PS1LE(cb + 16));
                              }
                              fprintf(stderr, "[ps1ev]   %u of %u EvCB entries in use\n",
                                      live, n); }
                            #undef PS1LE
                        } }
                      /* PS1_GP0HIST=1: which GP0 PRIMITIVES the ring actually
                       * carries, as a histogram over the last 64 packets.
                       *
                       * "The menu draws but nothing 3D does" is a statement
                       * about primitive TYPES, and no counter here reported
                       * those -- packets, groups and VRAM pixels all count the
                       * same whether the PS1 is drawing a textured rectangle or
                       * a gouraud triangle. GP0 opcodes: 0x20-0x3F polygons,
                       * 0x40-0x5F lines, 0x60-0x7F rectangles, 0x80+ transfers,
                       * 0x00-0x1F misc/state. If the 3D is missing because the
                       * polygons never reach the ring, that is upstream in the
                       * R3000; if they reach it, it is the rasteriser or the
                       * composite. Opposite halves of the pipeline. */
                      { static int gh = -1;
                        if (gh < 0) gh = getenv("PS1_GP0HIST") ? 1 : 0;
                        if (gh && rbase && roff >= 0x4000u) {
                            unsigned cls[8] = {0}; unsigned pk_types[8] = {0};
                            for (u32 k = 1; k <= 64u; k++) {
                                const u32 pk = rbase + ((roff - k * 0x100u) & 0x007FFFFFu);
                                pk_types[vm_read32(pk) & 7u]++;
                                for (u32 q = 0; q < 60u; q++) {
                                    const u32 w = vm_read32(pk + 0x10u + q * 4u);
                                    if (!w) continue;
                                    cls[(w >> 24) >> 5]++;   /* opcode/0x20 */
                                }
                            }
                            /* PS1 VRAM comes out solid 0x83E0 -- R=0, G=31,
                             * B=0, mask bit set -- across the full 640-pixel
                             * display width, fully written (207 DMA writes per
                             * 64-byte block). Something paints the screen
                             * green. GP0 0x02 is Fill Rectangle and its colour
                             * word is right here in the ring, so print the
                             * fills: if the guest asks for green, the bug is
                             * upstream in the R3000; if it asks for something
                             * else, our SPU rasteriser is packing the colour
                             * wrong. Opposite halves of the pipeline. */
                            for (u32 k = 1; k <= 64u; k++) {
                                const u32 pk = rbase + ((roff - k * 0x100u) & 0x007FFFFFu);
                                for (u32 q = 0; q < 58u; q++) {
                                    const u32 w = vm_read32(pk + 0x10u + q * 4u);
                                    if ((w >> 24) != 0x02u) continue;
                                    fprintf(stderr, "[gp0fill] pkt+0x%X"
                                                    " cmd=%08X xy=%08X wh=%08X\n",
                                            0x10u + q * 4u, w,
                                            vm_read32(pk + 0x14u + q * 4u),
                                            vm_read32(pk + 0x18u + q * 4u));
                                    break;
                                }
                            }
                            fprintf(stderr, "[gp0hist] last 64 pkts types[");
                            for (int q = 0; q < 8; q++) fprintf(stderr, "%u ", pk_types[q]);
                            fprintf(stderr, "] words: misc=%u POLY=%u line=%u"
                                            " rect=%u xfer=%u %u %u %u\n",
                                    cls[0], cls[1], cls[2], cls[3],
                                    cls[4], cls[5], cls[6], cls[7]);
                        } }
                      /* PS1_RINGDUMP=1: the last few GP0 command packets.
                       *
                       * func_0010F658 writes one 0x100-byte packet per batch:
                       * a type word at +0 (it writes 3 in one variant and 2 in
                       * another) and the payload from +0x10. The four GPU SPUs
                       * consume every packet and never DMA a pixel to VRAM, so
                       * the question is what these packets actually contain --
                       * PS1 GP0 drawing commands, or only state and sync. */
                      { static int rd2 = -1; static int shown = 0;
                        if (rd2 < 0) rd2 = getenv("PS1_RINGDUMP") ? 1 : 0;
                        if (rd2 && shown < 3 && rbase && roff >= 0x300u) {
                            shown++;
                            for (int k = 3; k >= 1; k--) {
                                const u32 pk = rbase + ((roff - (u32)k * 0x100u) & 0x007FFFFFu);
                                fprintf(stderr, "[ring] pkt@0x%08X type=%u:", pk,
                                        vm_read32(pk));
                                for (u32 q = 0; q < 12; q++)
                                    fprintf(stderr, " %08X", vm_read32(pk + 0x10u + q * 4u));
                                fprintf(stderr, "\n");
                            }
                        } }
                      { static u32 rdy = 0;
                        if (!rdy) { const char* re = getenv("PS1_VRAM_READY");
                                    rdy = re ? (u32)strtoul(re, 0, 0) : 20000u; }
                        if (nz >= rdy) g_ld_ps1_vram_ready = 1; }
                      /* PS1_FBDUMP=<path>: write PS1 VRAM out as a PPM.
                       *
                       * This exists because "the window is black" was an
                       * ASSUMPTION for a long stretch of this port: PrintWindow
                       * on a D3D12 swapchain returns black whether the page is
                       * black or the capture simply failed, so every screenshot
                       * was unfalsifiable. This reads the pixels the PS1 itself
                       * produced, straight out of guest memory, with no D3D and
                       * no window involved. 1024x512, 16-bit 1-5-5-5, pitch
                       * 2048 -- the format the [tex-refresh] line reports. */
                      /* ONE dump, taken once the framebuffer has at least
                       * PS1_FBDUMP_MIN non-zero words (default 20000).
                       *
                       * It used to write a 1.5 MB PPM on every heartbeat, and
                       * that instrumentation was itself most of the
                       * "nondeterminism" this port was chasing: runs carrying
                       * the periodic dump reached 0-7,127 non-zero words, while
                       * clean runs reach 75,527 every time. Measuring the thing
                       * was changing it. One dump, gated on content. */
                      { const char* dp = getenv("PS1_FBDUMP");
                        static int dumped = 0;
                        u32 want = 20000;
                        { const char* mn = getenv("PS1_FBDUMP_MIN");
                          if (mn) want = (u32)strtoul(mn, 0, 0); }
                        if (dp && !dumped && nz >= want) {
                            dumped = 1;
                            char path[512];
                            snprintf(path, sizeof path, "%s", dp);
                            FILE* f = fopen(path, "wb");
                            if (f) {
                                fprintf(f, "P6\n1024 512\n255\n");
                                for (u32 y = 0; y < 512; y++) {
                                    const u8* row = fb + (size_t)y * 2048u;
                                    for (u32 x = 0; x < 1024; x++) {
                                        u16 p = (u16)(row[x * 2] | (row[x * 2 + 1] << 8));
                                        u8 rgb[3];
                                        rgb[0] = (u8)(((p >> 10) & 0x1F) * 255 / 31);
                                        rgb[1] = (u8)(((p >> 5)  & 0x1F) * 255 / 31);
                                        rgb[2] = (u8)(( p        & 0x1F) * 255 / 31);
                                        fwrite(rgb, 1, 3, f);
                                    }
                                }
                                fclose(f);
                            }
                        } }
                  } }
                (void)hi; (void)rbase;
            } }
          fprintf(stderr, "[fps] %.1f (frames %u..%u over %.1fs)\n",
                  (g_ld_frames - fps_f0) * 1000.0 / (double)(now - fps_t0),
                  fps_f0, g_ld_frames, (now - fps_t0) / 1000.0);
          /* Where each frame went, averaged over the same window: the guest
           * blocked in cellGcmSetWaitFlip, the present thread idle (no FIFO
           * work), the CPU waiting on the GPU fence, and DXGI Present. */
          { static u64 lastWaitFlip = 0, lastIdle = 0, lastGpu = 0, lastSwap = 0;
            const u64 waitFlipNs = g_gcm_perf_waitflip_ns;
            const u64 idleNs = g_gcm_perf_present_idle_ns;
            const u64 gpuNs = g_gcm_perf_gpu_wait_ns;
            const u64 swapNs = g_gcm_perf_swap_ns;
            const double frameCount = (g_ld_frames > fps_f0) ? (double)(g_ld_frames - fps_f0) : 1.0;
            fprintf(stderr, "[fps]   per frame: %.1f ms; guest waitflip %.1f ms, present thread idle %.1f ms, gpu wait %.1f ms, swap %.1f ms\n",
                    (double)(now - fps_t0) / frameCount,
                    (double)(waitFlipNs - lastWaitFlip) / 1e6 / frameCount,
                    (double)(idleNs - lastIdle) / 1e6 / frameCount,
                    (double)(gpuNs - lastGpu) / 1e6 / frameCount,
                    (double)(swapNs - lastSwap) / 1e6 / frameCount);
            lastWaitFlip = waitFlipNs; lastIdle = idleNs; lastGpu = gpuNs; lastSwap = swapNs;
            { static u64 lastUsleep = 0, lastSyscall = 0, lastHle = 0;
              const u64 usleepNs = g_gcm_perf_main_usleep_ns;
              const u64 syscallNs = g_gcm_perf_main_syscall_ns;
              const u64 hleNs = g_gcm_perf_main_hle_ns;
              fprintf(stderr, "[fps]   main thread per frame: usleep %.1f ms, other syscalls %.1f ms, HLE calls %.1f ms (waitflip included)\n",
                      (double)(usleepNs - lastUsleep) / 1e6 / frameCount,
                      (double)(syscallNs - lastSyscall) / 1e6 / frameCount,
                      (double)(hleNs - lastHle) / 1e6 / frameCount);
              lastUsleep = usleepNs; lastSyscall = syscallNs; lastHle = hleNs; }
            { static u64 lastRingWait = 0;
              const u64 ringWaitNs = g_gcm_perf_ring_wait_ns;
              if (g_gcm_ring_bytes)
              {
                  fprintf(stderr, "[fps]   command ring: largest recent frame %u KB of %u KB, guest held for the drain %.1f ms per frame\n",
                          g_gcm_ring_frame_peak / 1024u, g_gcm_ring_bytes / 1024u,
                          (double)(ringWaitNs - lastRingWait) / 1e6 / frameCount);
              }
              lastRingWait = ringWaitNs; }
            /* The present thread's own split, and how long a flip takes to
             * get from the game to the drain and from the drain to the
             * screen (the vblank that retires it). */
            { static u64 lastDrain = 0, lastPresent = 0, lastDrainLat = 0, lastShowLat = 0;
              static u32 lastShown = 0, lastTimeouts = 0;
              const u64 drainNs = g_gcm_perf_drain_ns;
              const u64 presentNs = g_gcm_perf_present_ns;
              const u64 drainLatNs = g_gcm_perf_flip_drain_lat_ns;
              const u64 showLatNs = g_gcm_perf_flip_show_lat_ns;
              const u32 shown = g_gcm_perf_flips_shown;
              const u32 timeouts = g_gcm_perf_waitflip_timeouts;
              const double shownCount = (shown > lastShown) ? (double)(shown - lastShown) : 1.0;
              fprintf(stderr, "[fps]   present thread per frame: drain %.1f ms, execute+present %.1f ms; flip latency: game->drain %.1f ms, drain->shown %.1f ms; waitflip timeouts %u\n",
                      (double)(drainNs - lastDrain) / 1e6 / frameCount,
                      (double)(presentNs - lastPresent) / 1e6 / frameCount,
                      (double)(drainLatNs - lastDrainLat) / 1e6 / shownCount,
                      (double)(showLatNs - lastShowLat) / 1e6 / shownCount,
                      timeouts - lastTimeouts);
              lastDrain = drainNs; lastPresent = presentNs; lastDrainLat = drainLatNs;
              lastShowLat = showLatNs; lastShown = shown; lastTimeouts = timeouts; }
            { static u64 lastHits = 0, lastMisses = 0, lastStale = 0, lastUncacheable = 0;
              fprintf(stderr, "[fps]   geometry cache per frame: %.0f hits, %.0f misses (%.0f changed), %.0f not cacheable; %u draws held, %.0f MB\n",
                      (double)(g_ld_geom_hits - lastHits) / frameCount,
                      (double)(g_ld_geom_misses - lastMisses) / frameCount,
                      (double)(g_ld_geom_stale - lastStale) / frameCount,
                      (double)(g_ld_geom_uncacheable - lastUncacheable) / frameCount,
                      rsx_geom_cache_entries(), (double)rsx_geom_cache_bytes() / (1024.0 * 1024.0));
              lastHits = g_ld_geom_hits; lastMisses = g_ld_geom_misses;
              lastStale = g_ld_geom_stale; lastUncacheable = g_ld_geom_uncacheable; }
            { static ld_draw_perf lastPerf;
              static u64 lastIdentityHits = 0;
              static u64 lastIdentityMisses = 0;
              const ld_draw_perf nowPerf = g_ld_draw_perf;
              const double drawCount = (double)(nowPerf.draws - lastPerf.draws);
              const u64 totalTicks = nowPerf.totalTicks - lastPerf.totalTicks;
              const u64 fetchTicks = nowPerf.fetchTicks - lastPerf.fetchTicks;
              const u64 shaderTicks = nowPerf.shaderTicks - lastPerf.shaderTicks;
              const u64 bindTicks = nowPerf.bindTicks - lastPerf.bindTicks;
              const u64 stageTicks = fetchTicks + shaderTicks + bindTicks;
              fprintf(stderr, "[fps]   draws per frame: %.0f, %.1f ms in the renderer (%.2f us each): vertex fetch %.1f, "
                              "shader lookup %.1f, textures %.1f, constants+state+draw %.1f ms; vp hash reuse %llu/%llu\n",
                      drawCount / frameCount, ld_perf_ticks_to_ms(totalTicks) / frameCount,
                      drawCount > 0.0 ? ld_perf_ticks_to_ms(totalTicks) * 1000.0 / drawCount : 0.0,
                      ld_perf_ticks_to_ms(fetchTicks) / frameCount, ld_perf_ticks_to_ms(shaderTicks) / frameCount,
                      ld_perf_ticks_to_ms(bindTicks) / frameCount,
                      ld_perf_ticks_to_ms(totalTicks > stageTicks ? totalTicks - stageTicks : 0) / frameCount,
                      (unsigned long long)(g_ld_vp_identity_hits - lastIdentityHits),
                      (unsigned long long)(g_ld_vp_identity_hits - lastIdentityHits +
                                           g_ld_vp_identity_misses - lastIdentityMisses));
              lastPerf = nowPerf;
              lastIdentityHits = g_ld_vp_identity_hits;
              lastIdentityMisses = g_ld_vp_identity_misses; } }
          fps_t0 = now; fps_f0 = g_ld_frames;
      } }
    /* RSX_DUMP_EVERY=N: also dump every Nth frame, not just the first 8 --
     * a title whose content starts after the boot clears is invisible otherwise. */
    static int ld_dump_every = -1;
    if (ld_dump_every < 0) { const char* e = getenv("RSX_DUMP_EVERY"); ld_dump_every = e ? atoi(e) : 0; }
    if (LD_DIAG_ENABLED("RSX_DUMP") &&
        (g_ld_frames <= 8 || (ld_dump_every > 0 && (g_ld_frames % (u32)ld_dump_every) == 0))) {
        /* Dump the presented color surface (RENDER_TARGET state -> safe). */
        const u32 cur = current_surface();
        if (cur != LD_INVALID_SURFACE) {
            char path[256];
            snprintf(path, sizeof(path), "scratch\\ld_frame_%02u.ppm", g_ld_frames);
            ld_dump_surface_ppm(path, &g.surfaces[cur]);
        }
    }
#if !defined(YZ_PERF_CLEAN)
    if (rsx_live_draw_a010_probe_active()) {
        const u32 elapsed = g_ld_frames - g_ld_a010_probe_start_frame;
        const int targeted = getenv("RSX_A010_SURFACE_DUMP") != NULL;
        u32 sample_every = 16u;
        const char *sample_every_env = getenv("RSX_A010_SURFACE_EVERY");
        if (sample_every_env) {
            const int parsed = atoi(sample_every_env);
            if (parsed > 0 && parsed <= 64)
                sample_every = (u32)parsed;
        }
        /* Loading consumes roughly 150 fast flips before a010. Sampling every
         * 16 frames spans the load and complete AUTH window while keeping the
         * synchronous readbacks from becoming the scene's clock. */
        const int world_ready =
            InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0;
        /* Targeted readbacks are intentionally expensive: each surface dump
         * submits and fences the open D3D12 list.  Do not perturb the producer
         * while it is still assembling the first a010 world command chain;
         * begin targeted capture only after a real scene mesh was observed. */
        const int capture_once =
            targeted && getenv("RSX_A010_CAPTURE_ONCE") != NULL;
        const int explicit_probe_fallback =
            capture_once && getenv("RSX_A010_PROBE") != NULL &&
            elapsed >= 192u;
        /* The exact healthy-mesh tuple is the strongest capture gate, but a
         * broken natural scene may never produce it. An explicitly requested
         * one-shot probe therefore gets one bounded fallback after 192 scene
         * flips, late enough to be past a010's loading/transition frames. */
        if ((!targeted || world_ready || explicit_probe_fallback) &&
            (elapsed % sample_every) == 0 &&
            (!targeted || elapsed <= 208u ||
             (capture_once && g_ld_a010_probe_sample == 0u))) {
            u64 mask = g_ld_a010_probe_touched;
            const u32 cur = current_surface();
            if (targeted) {
                mask = 0;
                for (u32 i = 0; i < g.n_surfaces && i < 64; i++) {
                    const u32 off = g.surfaces[i].offset;
                    /* Working a010 replay: 0xE40000 and 0x1800000 carry
                     * 1,481/1,727 scene draws; 0x2D10000 and 0x2710000 are
                     * the two depth-derived color passes; 0x1440000 is the
                     * final seven-draw composite. */
                    if (off == 0x00E40000u || off == 0x01800000u ||
                        off == 0x02D10000u || off == 0x02710000u ||
                        off == 0x01440000u ||
                        i == target)
                        mask |= 1ull << i;
                }
            } else if (cur < 64) {
                mask |= 1ull << cur;
            }
            fprintf(stderr,
                    "[a010-probe] SAMPLE n=%u live_frame=%u elapsed=%u "
                    "present=%u current=%u buffer=%u touched=0x%016llX "
                    "surfaces=%u targeted=%d\n",
                    g_ld_a010_probe_sample, g_ld_frames, elapsed, target, cur,
                    buffer_id, (unsigned long long)mask, g.n_surfaces,
                    targeted);
            for (u32 i = 0; i < g.n_surfaces && i < 64; i++) {
                if (!(mask & (1ull << i))) continue;
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch\\a010_probe\\sample_%03u_frame_%05u_"
                         "surface_%02u_off_%08X.ppm",
                         g_ld_a010_probe_sample, g_ld_frames, i,
                         g.surfaces[i].offset);
                ld_dump_surface_ppm(path, &g.surfaces[i]);
                fprintf(stderr,
                        "[a010-probe] SURFACE sample=%u index=%u location=%u "
                        "offset=0x%08X size=%ux%u role=%s\n",
                        g_ld_a010_probe_sample, i, g.surfaces[i].location,
                        g.surfaces[i].offset, g.surfaces[i].w,
                        g.surfaces[i].h,
                        i == target ? "present" : "offscreen");
            }
            g_ld_a010_probe_touched = 0;
            g_ld_a010_probe_sample++;
            fflush(stderr);
            if (capture_once) {
                FILE* acceptance =
                    fopen("scratch\\a010_acceptance.txt", "a");
                if (acceptance) {
                    fprintf(acceptance,
                            "surface-capture sample=%u frame=%u mask=%016llX\n",
                            g_ld_a010_probe_sample - 1u, g_ld_frames,
                            (unsigned long long)mask);
                    fclose(acceptance);
                }
                InterlockedExchange(&g_ld_a010_probe_active, 0);
            }
        }
        if (elapsed >= 640 &&
            (!capture_once || g_ld_a010_probe_sample != 0u)) {
            fprintf(stderr,
                    "[a010-probe] END frame-cap live_frame=%u samples=%u\n",
                    g_ld_frames, g_ld_a010_probe_sample);
            fflush(stderr);
            InterlockedExchange(&g_ld_a010_probe_active, 0);
        }
    }
#endif

    /* new frame: the per-frame ring cursors were reset to the new bank by
     * ld_submit_frame. */
#if !defined(YZ_PERF_CLEAN)
    g_ld_frame_vertex_constant_ring_recycles = 0;
#endif
    g.depth_cleared = 0;
    /* Per-zeta resources model persistent RSX memory.  Do not mark them
     * uncleared at a host-frame boundary: the implicit-clear branch would
     * erase depth rendered in an earlier frame and demote had_write before a
     * later pass can sample that zeta.  Only an actual guest depth clear may
     * clear/demote a tracked zeta (sink_clear does that above). */
}

void rsx_live_draw_shutdown(void)
{
    if (!g.ready) return;
    ld_vertex_diag_emit("shutdown", 1);
    ld_present_measure_dump();
    /* let the GPU drain, then release. (Best-effort; process teardown also
     * reclaims.) */
    ld_flush(LD_FLUSH_SHUTDOWN);
#if defined(YZ_PERF_PROFILE)
    fprintf(stderr,
            "[rsx-perf-summary] frames=%u "
            "pso{look=%llu hit=%llu miss=%llu probes=%llu full=%llu "
            "cached=%u capacity=%u mem_kb=%.1f} "
            "reject{requests=%llu unique=%u "
            "world=%llu/%u character=%llu/%u "
            "ui=%llu/%u other=%llu/%u} "
            "hlsl{vs_lookup=%llu vs_hit=%llu vs_compile=%llu vs_unique=%u "
            "ps_lookup=%llu ps_hit=%llu ps_compile=%llu ps_unique=%u} "
            "time_ms{decompile=%.3f vs_compile=%.3f ps_compile=%.3f "
            "create_pso=%.3f flush=%.3f fence=%.3f draw_cpu=%.3f} "
            "vertex{path=%s used_avg=%.3f used_max=%u "
            "legacy_mb=%.3f actual_mb=%.3f fetch_pack_ms=%.3f} "
            "vb_sync{flushes=%llu flush_ms=%.3f wait_ms=%.3f} "
            "frame{count=%llu avg_ms=%.3f compile_free=%llu "
            "compile_free_avg_ms=%.3f} "
            "draw{input_v=%llu expanded_v=%llu vb_mb=%.3f} "
            "tex{cached=%u evictions=%llu upload_mb=%.3f "
            "decode=%llu decode_ms=%.3f} "
            "high{upload_mb=%.3f vb_mb=%.3f retired=%u}\n",
            g_ld_frames,
            (unsigned long long)g_ld_profile.total.pso_lookups,
            (unsigned long long)g_ld_profile.total.pso_hits,
            (unsigned long long)g_ld_profile.total.pso_misses,
            (unsigned long long)g_ld_profile.total.pso_probes,
            (unsigned long long)g_ld_profile.total.pso_full,
            g.n_psos,
            MAX_PSOS,
            (double)sizeof(g.psos) / 1024.0,
            (unsigned long long)g_ld_profile.total.pso_full,
            g_ld_profile.n_rejected_pso_keys,
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_WORLD],
            g_ld_profile.rejected_pso_unique[LD_REJECT_WORLD],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_CHARACTER],
            g_ld_profile.rejected_pso_unique[LD_REJECT_CHARACTER],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_UI],
            g_ld_profile.rejected_pso_unique[LD_REJECT_UI],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_OTHER],
            g_ld_profile.rejected_pso_unique[LD_REJECT_OTHER],
            (unsigned long long)g_ld_profile.total.vs_blob_lookups,
            (unsigned long long)g_ld_profile.total.vs_blob_hits,
            (unsigned long long)g_ld_profile.total.vs_compile_calls,
            g_ld_profile.n_vs_hashes,
            (unsigned long long)g_ld_profile.total.ps_blob_lookups,
            (unsigned long long)g_ld_profile.total.ps_blob_hits,
            (unsigned long long)g_ld_profile.total.ps_compile_calls,
            g_ld_profile.n_ps_hashes,
            ld_profile_ticks_ms(g_ld_profile.total.decompile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.vs_compile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.ps_compile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.create_pso_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.flush_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.fence_wait_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.sink_end_qpc),
            ld_vertex_mode_name(),
            g_ld_profile.total.used_attribute_draws
                ? (double)g_ld_profile.total.used_attribute_sum /
                      (double)g_ld_profile.total.used_attribute_draws
                : 0.0,
            g_ld_profile.total_used_attribute_max,
            (double)g_ld_profile.total.legacy_vertex_upload_bytes /
                (1024.0 * 1024.0),
            (double)g_ld_profile.total.vertex_upload_bytes /
                (1024.0 * 1024.0),
            ld_profile_ticks_ms(
                g_ld_profile.total.vertex_fetch_pack_qpc),
            (unsigned long long)
                g_ld_profile.total.flush_reason[LD_FLUSH_VERTEX_RING],
            ld_profile_ticks_ms(
                g_ld_profile.total
                    .flush_reason_qpc[LD_FLUSH_VERTEX_RING]),
            ld_profile_ticks_ms(
                g_ld_profile.total
                    .fence_reason_qpc[LD_FLUSH_VERTEX_RING]),
            (unsigned long long)g_ld_profile.measured_frames,
            g_ld_profile.measured_frames
                ? g_ld_profile.total_frame_ms /
                      (double)g_ld_profile.measured_frames
                : 0.0,
            (unsigned long long)g_ld_profile.compile_free_frames,
            g_ld_profile.compile_free_frames
                ? g_ld_profile.compile_free_frame_ms /
                      (double)g_ld_profile.compile_free_frames
                : 0.0,
            (unsigned long long)g_ld_profile.total.input_vertices,
            (unsigned long long)g_ld_profile.total.expanded_vertices,
            (double)g_ld_profile.total.vertex_upload_bytes /
                (1024.0 * 1024.0),
            g.n_textures,
            (unsigned long long)g_ld_texture_cache_evictions,
            (double)g_ld_profile.total.texture_upload_bytes /
                (1024.0 * 1024.0),
            (unsigned long long)
                g_ld_profile.total.texture_decode_calls,
            ld_profile_ticks_ms(
                g_ld_profile.total.texture_decode_qpc),
            (double)g_ld_profile.total_upload_high / (1024.0 * 1024.0),
            (double)g_ld_profile.total_vb_high / (1024.0 * 1024.0),
            g_ld_profile.total_retired_high);
    fprintf(stderr,
            "[shader-cache-summary] "
            "vs{count=%u capacity=%u lookups=%llu hits=%llu "
            "misses=%llu inserts=%llu full_rejects=%llu "
            "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
            "post2048_distinct=%llu post2048_repeats=%llu "
            "source_bytes=%llu blob_bytes=%llu} "
            "ps{count=%u capacity=%u lookups=%llu hits=%llu "
            "misses=%llu inserts=%llu full_rejects=%llu "
            "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
            "post2048_distinct=%llu post2048_repeats=%llu "
            "source_bytes=%llu blob_bytes=%llu}\n",
            g.vs_blobs.count, MAX_SHADER_BLOBS,
            (unsigned long long)g_ld_profile.total.vs_blob_lookups,
            (unsigned long long)g_ld_profile.total.vs_blob_hits,
            (unsigned long long)g_ld_profile.total.vs_blob_misses,
            (unsigned long long)g_ld_profile.total.vs_blob_inserts,
            (unsigned long long)g_ld_profile.total.vs_blob_full_rejects,
            (unsigned long long)g_ld_profile.total.vs_compile_calls,
            ld_profile_ticks_ms(g_ld_profile.total.vs_blob_lookup_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.vs_compile_qpc),
            (unsigned long long)
                g_ld_profile.total.vs_post_boundary_distinct,
            (unsigned long long)
                g_ld_profile.total.vs_post_boundary_repeats,
            (unsigned long long)g.vs_blobs.retained_source_bytes,
            (unsigned long long)g.vs_blobs.retained_blob_bytes,
            g.ps_blobs.count, MAX_SHADER_BLOBS,
            (unsigned long long)g_ld_profile.total.ps_blob_lookups,
            (unsigned long long)g_ld_profile.total.ps_blob_hits,
            (unsigned long long)g_ld_profile.total.ps_blob_misses,
            (unsigned long long)g_ld_profile.total.ps_blob_inserts,
            (unsigned long long)g_ld_profile.total.ps_blob_full_rejects,
            (unsigned long long)g_ld_profile.total.ps_compile_calls,
            ld_profile_ticks_ms(g_ld_profile.total.ps_blob_lookup_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.ps_compile_qpc),
            (unsigned long long)
                g_ld_profile.total.ps_post_boundary_distinct,
            (unsigned long long)
                g_ld_profile.total.ps_post_boundary_repeats,
            (unsigned long long)g.ps_blobs.retained_source_bytes,
            (unsigned long long)g.ps_blobs.retained_blob_bytes);
    fprintf(
        stderr,
        "[fp-constant-summary] mode=%s "
        "variants{exact_source=%u constant_specialized=%u canonical=%u "
        "collapsed_constants=%u collapsed_alpha=%u "
        "tracking_overflow=%llu} "
        "cb{allocations=%llu upload_bytes=%llu upload_ms=%.3f "
        "ring_capacity=%u capacity_failures=%llu "
        "flushes=%llu flush_ms=%.3f wait_ms=%.3f}\n",
        ld_fp_constant_mode_name(),
        g_ld_profile.n_ps_exact_source_hashes,
        g_ld_profile.n_ps_constant_specialized_hashes,
        g_ld_profile.n_ps_canonical_hashes,
        g_ld_profile.n_ps_constant_specialized_hashes >=
                g_ld_profile.n_ps_canonical_hashes
            ? g_ld_profile.n_ps_constant_specialized_hashes -
                  g_ld_profile.n_ps_canonical_hashes
            : 0,
        g_ld_profile.n_ps_exact_source_hashes >=
                g_ld_profile.n_ps_constant_specialized_hashes
            ? g_ld_profile.n_ps_exact_source_hashes -
                  g_ld_profile.n_ps_constant_specialized_hashes
            : 0,
        (unsigned long long)
            g_ld_profile.ps_variant_tracking_overflow,
        (unsigned long long)
            g_ld_profile.total.ps_constant_allocations,
        (unsigned long long)
            g_ld_profile.total.ps_constant_upload_bytes,
        ld_profile_ticks_ms(
            g_ld_profile.total.ps_constant_upload_qpc),
        PS_CB_RING_BYTES,
        (unsigned long long)
            g_ld_profile.total.ps_constant_capacity_failures,
        (unsigned long long)g_ld_profile.total.flush_reason[
            LD_FLUSH_PIXEL_CONSTANT_RING],
        ld_profile_ticks_ms(g_ld_profile.total.flush_reason_qpc[
            LD_FLUSH_PIXEL_CONSTANT_RING]),
        ld_profile_ticks_ms(g_ld_profile.total.fence_reason_qpc[
            LD_FLUSH_PIXEL_CONSTANT_RING]));
    fflush(stderr);
#endif
    if (g_ld_shader_disk_ready > 0) {
        fprintf(stderr,
                "[shader-disk-cache-summary] hits{vs=%llu ps=%llu} "
                "misses{vs=%llu ps=%llu} writes{vs=%llu ps=%llu} "
                "rejects=%llu\n",
                (unsigned long long)g_ld_shader_disk_hits[0],
                (unsigned long long)g_ld_shader_disk_hits[1],
                (unsigned long long)g_ld_shader_disk_misses[0],
                (unsigned long long)g_ld_shader_disk_misses[1],
                (unsigned long long)g_ld_shader_disk_writes[0],
                (unsigned long long)g_ld_shader_disk_writes[1],
                (unsigned long long)g_ld_shader_disk_rejects);
    }
    ld_pso_jobs_wait();
    for (u32 i = 0; i < g.n_psos; i++) if (g.psos[i].pso) g.psos[i].pso->lpVtbl->Release(g.psos[i].pso);
    for (u32 variant = 0; variant < 3; variant++)
    {
        if (g.present_pso[variant])
        {
            g.present_pso[variant]->lpVtbl->Release(g.present_pso[variant]);
        }
    }
    if (g.present_rootsig) g.present_rootsig->lpVtbl->Release(g.present_rootsig);
    if (g.present_movie) g.present_movie->lpVtbl->Release(g.present_movie);
    shader_blob_cache_release(&g.vs_blobs);
    shader_blob_cache_release(&g.ps_blobs);
    for (u32 i = 0; i < g.n_textures; i++) if (g.textures[i].tex) g.textures[i].tex->lpVtbl->Release(g.textures[i].tex);
    for (u32 i = 0; i < g.n_vtex; i++) if (g.vtex[i].tex) g.vtex[i].tex->lpVtbl->Release(g.vtex[i].tex);
    for (u32 i = 0; i < g.n_surfaces; i++) if (g.surfaces[i].tex) g.surfaces[i].tex->lpVtbl->Release(g.surfaces[i].tex);
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (g.zdepths[i].tex)
            g.zdepths[i].tex->lpVtbl->Release(g.zdepths[i].tex);
        if (g.zdepths[i].snapshot)
            g.zdepths[i].snapshot->lpVtbl->Release(
                g.zdepths[i].snapshot);
    }
    if (g.movie_upload) g.movie_upload->lpVtbl->Release(g.movie_upload);
    if (g.movie_overlay_readback) g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
    if (g.movie_overlay_rgba) free(g.movie_overlay_rgba);
    if (g.movie_overlay_mask) free(g.movie_overlay_mask);
    if (g.white_tex) g.white_tex->lpVtbl->Release(g.white_tex);
    if (g.depth) g.depth->lpVtbl->Release(g.depth);
    if (g.ps_cb) g.ps_cb->lpVtbl->Release(g.ps_cb);
    if (g.depth_snapshot_pso)
        g.depth_snapshot_pso->lpVtbl->Release(g.depth_snapshot_pso);
    if (g.depth_snapshot_rootsig)
        g.depth_snapshot_rootsig->lpVtbl->Release(
            g.depth_snapshot_rootsig);
    if (g.rootsig_x) g.rootsig_x->lpVtbl->Release(g.rootsig_x);
    if (g.swap) g.swap->lpVtbl->Release(g.swap);
    if (g_ld_info_queue) {
        g_ld_info_queue->lpVtbl->Release(g_ld_info_queue);
        g_ld_info_queue = NULL;
    }
#if defined(YZ_PERF_PROFILE)
    if (g_ld_profile.adapter) {
        g_ld_profile.adapter->lpVtbl->Release(g_ld_profile.adapter);
        g_ld_profile.adapter = NULL;
    }
#endif
    if (g.dev) g.dev->lpVtbl->Release(g.dev);
    memset(&g, 0, sizeof(g));
    if (dc.verts) { free(dc.verts); dc.verts = NULL; dc.cap_verts = 0; }
    if (dc.refs) { free(dc.refs); dc.refs = NULL; dc.cap_refs = 0; }
    if (dc.compact_verts) {
        free(dc.compact_verts);
        dc.compact_verts = NULL;
        dc.compact_capacity = 0;
    }
    if (dc.cuts) { free(dc.cuts); dc.cuts = NULL; dc.cap_cuts = 0; }
}

#endif /* _WIN32 */
