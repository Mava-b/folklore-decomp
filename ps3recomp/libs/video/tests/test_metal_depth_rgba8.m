/* A Z24S8 depth target sampled through A8R8G8B8, both ways the engine can
 * build it: the GPU copy (depth_snapshot_rgba8) against the guest-memory path
 * (depth_readback, the engine's 24-bit quantization, the A8R8G8B8 texture
 * decode, an uploaded texture wearing the crossbar). The two must agree byte
 * for byte, including the rounding ties and the ends of the range.
 * clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_depth_rgba8.m libs/video/rsx_texture_layout.c
 * -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore
 * -o /tmp/test_metal_depth_rgba8 */
#include "../rsx_metal_backend.m"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The backend's other dependencies, unused here. */
float cellAvconfExt_display_gamma(void) { return 1.0f; }
int rsx_hlsl_to_msl(const char* hlsl, int stage, char* out, u32 out_size, char* log, u32 log_size)
{ (void)hlsl; (void)stage; (void)out; (void)out_size; (void)log; (void)log_size; return -1; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

enum { W = 64, H = 9, N = W * H };

static const char* fixture =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],0,1)}; }\n"
"struct D { float d [[depth(any)]]; };\n"
"fragment D fs(V v [[stage_in]], constant float* z [[buffer(0)]]) { uint2 q = uint2(v.p.xy); return {z[q.y * 64u + q.x]}; }\n"
"fragment float4 tap_fs(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { return t.read(uint2(v.p.xy)); }\n";

static int complete(id<MTLCommandBuffer> cb)
{ [cb commit]; [cb waitUntilCompleted]; return cb.status == MTLCommandBufferStatusCompleted && !cb.error; }

static id<MTLRenderPipelineState> pipeline(id<MTLLibrary> lib, const char* fn, int depth)
{
    MTLRenderPipelineDescriptor* p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [lib newFunctionWithName:@"vs"];
    p.fragmentFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    if (depth) { p.depthAttachmentPixelFormat = MTL_DEPTH_FORMAT; p.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT; }
    else p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    NSError* error = nil;
    id<MTLRenderPipelineState> r = [s_dev newRenderPipelineStateWithDescriptor:p error:&error];
    CHECK(r != nil, "fixture pipeline");
    return r;
}

/* Stencil is written too (0x5A): the guest-memory copy stores 0 there, and so
 * must the GPU copy. */
static int write_depth(u32 handle, id<MTLRenderPipelineState> p, const float* z)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = eng_obj(handle); rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.texture = eng_obj(handle); rp.stencilAttachment.loadAction = MTLLoadActionClear;
    rp.stencilAttachment.clearStencil = 0x5A; rp.stencilAttachment.storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionAlways; ds.depthWriteEnabled = YES;
    [e setRenderPipelineState:p];
    [e setDepthStencilState:[s_dev newDepthStencilStateWithDescriptor:ds]];
    [e setFragmentBytes:z length:N * sizeof(float) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    return complete(cb);
}

/* The guest-memory path, as rsx_draw_engine.c eng_zdepth_to_guest stores the
 * zeta and the texture cache then decodes it: RGBA8 rows of w x h. */
static int guest_path(u32 depth, u32 w, u32 h, u8* rgba)
{
    float d[N];
    if (s_engine_backend.depth_readback(NULL, depth, 0, 0, w, h, d, w * 4u) != 0) return 0;
    u8 packed[N * 4];
    for (u32 i = 0; i < w * h; i++) {
        float f = d[i];
        if (!(f > 0.0f)) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const u32 v = (u32)((double)f * 16777215.0 + 0.5);
        packed[4 * i] = (u8)(v >> 16); packed[4 * i + 1] = (u8)(v >> 8);
        packed[4 * i + 2] = (u8)v; packed[4 * i + 3] = 0;
    }
    rsx_tex_layout tl;
    rsx_texture_layout_pitched(0xE5, w, h, w * 4u, &tl);
    rsx_texture_decode(rgba, w * 4u, packed, w, h, &tl, 0);
    return 1;
}

static int gpu_path(u32 depth, u32 w, u32 h, u8* rgba, u32* handle)
{
    const u32 snap = s_engine_backend.depth_snapshot_rgba8(NULL, depth, w, h);
    if (handle) *handle = snap;
    if (!snap) return 0;
    eng_submit_and_wait(NULL, 0);
    id<MTLTexture> t = eng_obj(snap);
    if (!t || [t pixelFormat] != MTLPixelFormatRGBA8Unorm || [t width] != w || [t height] != h) return 0;
    [t getBytes:rgba bytesPerRow:w * 4u fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    return 1;
}

/* What a shader reads from texture `tex`, every texel, as floats. */
static int tap(id<MTLRenderPipelineState> p, u32 tex, u32 w, u32 h, float* out)
{
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [s_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p]; [e setFragmentTexture:eng_obj(tex) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    if (!complete(cb)) return 0;
    [dst getBytes:out bytesPerRow:w * 16u fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    return 1;
}

static u32 first_diff(const u8* a, const u8* b, u32 n)
{
    for (u32 i = 0; i < n; i++) if (a[i] != b[i]) return i;
    return n;
}

static float next_up(float f) { return nextafterf(f, 2.0f); }
static float next_dn(float f) { return nextafterf(f, -1.0f); }

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice(); CHECK(s_dev != nil, "Metal device"); if (!s_dev) return 1;
        s_queue = [s_dev newCommandQueue];
        CHECK(eng_init(NULL, W, H) == 0, "backend helpers (the helper library compiles)");
        CHECK(s_engine_backend.depth_snapshot_rgba8 != NULL, "backend advertises depth_snapshot_rgba8");
        if (!s_engine_backend.depth_snapshot_rgba8) { printf("%d checks, %d failures\n", checks, failures); return 1; }
        NSError* error = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:fixture] options:nil error:&error];
        CHECK(lib != nil, "fixture shaders"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        id<MTLRenderPipelineState> zp = pipeline(lib, "fs", 1), tp = pipeline(lib, "tap_fs", 0);

        /* The depths: the ends, the 24-bit ties (k + 0.5) / 16777215 and
         * their neighbours, exact steps, the powers of two (0.5 * 16777215 is
         * itself a tie), tiny values, the float just below 1, then noise. */
        float z[N];
        u32 n = 0;
        const float fixed[] = { 0.0f, 1.0f, 0.5f, 0.25f, 0.75f, 0.125f, 1.0f / 3.0f, 2.0f / 3.0f,
                                next_dn(1.0f), next_dn(next_dn(1.0f)), 0x1p-24f, 0x1p-25f, 3 * 0x1p-25f,
                                0x1p-26f, 0x1p-40f, 0x1p-126f, next_up(0.5f), next_dn(0.5f),
                                0.999f, 0.001f, 0.9999999f };
        for (u32 i = 0; i < sizeof fixed / sizeof fixed[0]; i++) z[n++] = fixed[i];
        const u32 ks[] = { 1, 2, 3, 255, 256, 65535, 65536, 0x7FFFFF, 0x800000, 0xABCDEF,
                           0xFFFFFD, 0xFFFFFE, 0x123456, 0xFEDCBA, 0x00FF00, 0x0000FF, 0xFF0000 };
        for (u32 i = 0; i < sizeof ks / sizeof ks[0]; i++) {
            const float step = (float)((double)ks[i] / 16777215.0);
            const float tie = (float)(((double)ks[i] + 0.5) / 16777215.0);
            z[n++] = step; z[n++] = next_up(step); z[n++] = next_dn(step);
            z[n++] = tie;  z[n++] = next_up(tie);  z[n++] = next_dn(tie);
        }
        srand(12345);
        while (n < N) {
            float f = (float)rand() / (float)RAND_MAX;
            if (n & 1) f = 1.0f - f * f * f * 0.01f;      /* crowd the far plane, as scenes do */
            z[n++] = f;
        }

        u32 depth = eng_depth_target_create(NULL, W, H); CHECK(depth != 0, "depth target");
        CHECK(write_depth(depth, zp, z), "depth and stencil written");

        u8 gpu[N * 4], ref[N * 4];
        u32 snap = 0;
        CHECK(gpu_path(depth, W, H, gpu, &snap), "GPU copy built and read");
        CHECK(guest_path(depth, W, H, ref), "guest-memory path built");
        u32 d = first_diff(gpu, ref, N * 4);
        if (d < N * 4) fprintf(stderr, "  texel %u (z %.9g): gpu %02X %02X %02X %02X, guest %02X %02X %02X %02X\n",
                               d / 4, z[d / 4], gpu[d & ~3u], gpu[(d & ~3u) + 1], gpu[(d & ~3u) + 2], gpu[(d & ~3u) + 3],
                               ref[d & ~3u], ref[(d & ~3u) + 1], ref[(d & ~3u) + 2], ref[(d & ~3u) + 3]);
        CHECK(d == N * 4, "GPU copy equals the guest-memory decode byte for byte");
        /* Spot checks of the layout itself: depth 0.5 -> 0x800000 (a tie,
         * rounded up), 1 -> 0xFFFFFF, stencil not carried. */
        CHECK(gpu[2 * 4 + 3] == 0x80 && gpu[2 * 4 + 0] == 0x00 && gpu[2 * 4 + 1] == 0x00 && gpu[2 * 4 + 2] == 0x00,
              "0.5 reads A=0x80 R=0 G=0 B=0");
        CHECK(gpu[1 * 4 + 3] == 0xFF && gpu[1 * 4 + 0] == 0xFF && gpu[1 * 4 + 1] == 0xFF && gpu[1 * 4 + 2] == 0x00,
              "1.0 reads A=R=G=0xFF, B (stencil) 0");

        /* Same size again: the same persistent copy, re-rendered in stream
         * order, and the R32F snapshot / readback paths leave it alone. */
        u32 again = 0;
        CHECK(gpu_path(depth, W, H, gpu, &again) && again == snap, "same size reuses the copy");
        CHECK(s_eng_depth_rgba_of[depth - 1] == snap, "readback did not release the colour-format copy");

        /* New depth: the copy follows it. */
        for (u32 i = 0; i < N; i++) z[i] = 1.0f - z[i];
        CHECK(write_depth(depth, zp, z), "depth rewritten");
        CHECK(gpu_path(depth, W, H, gpu, &again) && again == snap, "re-rendered in place");
        CHECK(guest_path(depth, W, H, ref), "guest path of the new depth");
        CHECK(first_diff(gpu, ref, N * 4) == N * 4, "re-rendered copy equals the guest-memory decode");

        /* A texture smaller than its zeta: the top-left region 1:1. */
        u8 crop[N * 4], cref[N * 4];
        u32 small = 0;
        CHECK(gpu_path(depth, 40, 5, crop, &small) && small && small != snap, "cropped copy built");
        CHECK(s_eng_obj_retired[snap - 1] || !s_eng_obj[snap - 1], "the other-size copy was released");
        CHECK(guest_path(depth, 40, 5, cref), "cropped guest path");
        CHECK(first_diff(crop, cref, 40 * 5 * 4) == 40 * 5 * 4, "cropped copy equals the guest decode of that region");

        /* Through the unit's crossbar: the engine binds a surface_view of the
         * copy; the guest path an uploaded texture with the remap baked in. */
        CHECK(gpu_path(depth, W, H, gpu, &snap) && guest_path(depth, W, H, ref), "full-size copies again");
        const u32 remaps[] = { 0xAAE4u, 0xAA93u, 0xAA1Bu, 0xAA4Eu, 0x5500u | 0xE4u, 0x00E4u, 0xA9C6u };
        static float a[N * 4], b[N * 4];
        for (u32 i = 0; i < sizeof remaps / sizeof remaps[0]; i++) {
            const u32 view = eng_surface_view(NULL, snap, remaps[i], 0xE5);
            const u32 up = eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, W, H, 1, 1, remaps[i], 0xE5);
            eng_texture_upload(NULL, up, 0, 0, W, H, ref, W * 4u, H);
            char name[96];
            snprintf(name, sizeof name, "crossbar %04X: the view samples as the uploaded texture", remaps[i]);
            CHECK(view && up && tap(tp, view, W, H, a) && tap(tp, up, W, H, b) && !memcmp(a, b, sizeof a), name);
            eng_obj_release(NULL, up);
        }

        /* The depth target owns the copy: releasing it frees the copy and its views. */
        eng_submit_and_wait(NULL, 0);
        eng_obj_release(NULL, depth);
        CHECK(s_eng_depth_rgba_of[depth - 1] == 0, "the depth target's release forgets the copy");
        CHECK(s_eng_obj_retired[snap - 1] || !s_eng_obj[snap - 1], "and releases it");
        eng_collect_retired_objects();
        int none = 1;
        for (u32 i = 0; i < s_eng_obj_count; i++) none &= s_eng_obj[i] == nil;
        CHECK(none, "no live objects left");
        CHECK(!s_eng_encoding_failed, "every record encoded");

        printf("%d checks, %d failures\n", checks, failures);
        eng_shutdown(NULL);
        return failures ? 1 : 0;
    }
}
