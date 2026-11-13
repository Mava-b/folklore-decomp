/* PS3_RES_SCALE in the Metal backend: the knob's spellings; targets made at
 * the scaled size (a seeded colour target filled nearest-neighbour); views and
 * copies that inherit it; viewports and scissors that follow; and readbacks,
 * colour and depth, that hand the guest its own pixels back. Needs a Metal
 * device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_res_scale.m \
 *     libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp -lc++ -Wl,-dead_strip \
 *     -framework Metal -framework Cocoa -framework QuartzCore -o /tmp/test_metal_res_scale && /tmp/test_metal_res_scale
 */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <math.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

static const char* fixture =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],0,1)}; }\n"
/* every 2x2 block one value: the guest pixel it stands for */
"struct D { float d [[depth(any)]]; }; fragment D fs(V v [[stage_in]]) {"
" return {0.125f + floor(v.p.x * 0.5f) * 0.125f + floor(v.p.y * 0.5f) * 0.0625f}; }\n";
static float expected(u32 x, u32 y) { return .125f + x * .125f + y * .0625f; }

int main(void)
{
    @autoreleasepool {
        /* the knob */
        assert(res_scale_from(NULL) == 1.0f && res_scale_from("") == 1.0f);
        assert(res_scale_from("2") == 2.0f && res_scale_from("1.5") == 1.5f && res_scale_from("2x") == 2.0f);
        assert(res_scale_from("1080p") == 1.5f && res_scale_from("1440P") == 2.0f && res_scale_from("4k") == 3.0f);
        assert(res_scale_from("720p") == 1.0f && res_scale_from("1.6") == 1.5f);   /* quarter steps */
        assert(res_scale_from("5") == 1.0f && res_scale_from("0.5") == 1.0f && res_scale_from("big") == 1.0f);

        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("test_metal_res_scale: no Metal device, skipped"); return 0; }
        s_queue = [s_dev newCommandQueue];
        s_res_scale = 2.0f;
        assert(res_px(4) == 8 && res_px(3) == 6 && res_px(0) == 0);
        assert(eng_init(NULL, 4, 3) == 0);

        /* a seeded colour target: 8x6, each guest pixel a 2x2 block */
        u8 seed[4 * 3 * 4];
        for (u32 i = 0; i < 12; i++) { seed[i * 4] = (u8)(i * 20); seed[i * 4 + 1] = (u8)(255 - i); seed[i * 4 + 2] = 7; seed[i * 4 + 3] = 255; }
        const u32 color = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, seed, 16);
        assert(color && eng_obj_is_scaled(color));
        assert([eng_obj(color) width] == 8 && [eng_obj(color) height] == 6);
        u8 big[8 * 6 * 4];
        [eng_obj(color) getBytes:big bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 6) mipmapLevel:0];
        assert(!memcmp(big + (3 * 8 + 5) * 4, seed + (1 * 4 + 2) * 4, 4));   /* (5,3) -> guest (2,1) */
        /* read back at the guest's size: the seed, exactly */
        u8 back[4 * 3 * 4];
        memset(back, 0xCD, sizeof back);
        eng_readback(NULL, color, 0, 0, 4, 3, back, 16);
        assert(!memcmp(back, seed, sizeof seed));
        u8 roi[2 * 2 * 4 + 4];
        memset(roi, 0xCD, sizeof roi);
        eng_readback(NULL, color, 1, 1, 2, 2, roi, 12);                     /* padded rows */
        assert(!memcmp(roi, seed + (1 * 4 + 1) * 4, 8) && !memcmp(roi + 12, seed + (2 * 4 + 1) * 4, 8));
        assert(roi[8] == 0xCD);

        /* a view and a copy of it are scaled too; a guest texture is not */
        const u32 view = eng_surface_view(NULL, color, 0xAAE4, 0x85);
        assert(view && eng_obj_is_scaled(view));
        const u32 copy = eng_color_snapshot(NULL, color);
        assert(copy && eng_obj_is_scaled(copy) && [eng_obj(copy) width] == 8);
        const u32 tex = eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, 1, 1, 0xAAE4, 0x85);
        assert(tex && !eng_obj_is_scaled(tex) && [eng_obj(tex) width] == 4);

        /* viewport and scissor follow the targets */
        eng_set_viewport(NULL, 0.0f, 0.0f, 4.0f, 3.0f);
        assert(s_eng_pending.vp[2] == 8.0f && s_eng_pending.vp[3] == 6.0f);
        eng_set_scissor(NULL, 1, 1, 3, 2);
        assert(s_eng_pending.sc[0] == 2 && s_eng_pending.sc[1] == 2 && s_eng_pending.sc[2] == 6 && s_eng_pending.sc[3] == 4);

        /* depth: 8x6, its snapshot 8x6, its readback the guest's 4x3 */
        const u32 depth = eng_depth_target_create(NULL, 4, 3);
        assert(depth && eng_obj_is_scaled(depth) && [eng_obj(depth) width] == 8);
        NSError* err = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:@(fixture) options:nil error:&err];
        assert(lib);
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.depthAttachmentPixelFormat = MTL_DEPTH_FORMAT; pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
        id<MTLRenderPipelineState> pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        assert(pso);
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = eng_obj(depth); rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = eng_obj(depth); rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
        dsd.depthCompareFunction = MTLCompareFunctionAlways; dsd.depthWriteEnabled = YES;
        [e setRenderPipelineState:pso]; [e setDepthStencilState:[s_dev newDepthStencilStateWithDescriptor:dsd]];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        assert(cb.status == MTLCommandBufferStatusCompleted);
        const u32 snap = eng_depth_snapshot(NULL, depth, 4, 3);
        assert(snap && eng_obj_is_scaled(snap) && [eng_obj(snap) width] == 8 && [eng_obj(snap) height] == 6);
        float dz[4 * 3 + 2];
        for (int i = 0; i < 14; i++) dz[i] = -7.0f;
        assert(eng_depth_readback(NULL, depth, 0, 0, 4, 3, dz, 16) == 0);
        for (u32 y = 0; y < 3; y++)
            for (u32 x = 0; x < 4; x++) assert(fabsf(dz[y * 4 + x] - expected(x, y)) < 1e-6f);
        float droi[3] = { -9, -9, -9 };
        assert(eng_depth_readback(NULL, depth, 2, 1, 2, 1, droi, 12) == 0);
        assert(fabsf(droi[0] - expected(2, 1)) < 1e-6f && fabsf(droi[1] - expected(3, 1)) < 1e-6f && droi[2] == -9);
        const u32 rgba = eng_depth_as_color(NULL, depth, 4, 3, RSX_TEX_FMT_A8R8G8B8);
        assert(rgba && eng_obj_is_scaled(rgba) && [eng_obj(rgba) width] == 8);

        /* at 1, nothing is scaled */
        s_res_scale = 1.0f;
        const u32 plain = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, seed, 16);
        assert(plain && !eng_obj_is_scaled(plain) && [eng_obj(plain) width] == 4);
        eng_set_viewport(NULL, 0.0f, 0.0f, 4.0f, 3.0f);
        assert(s_eng_pending.vp[2] == 4.0f);
        puts("test_metal_res_scale: ok");
    }
    return 0;
}
