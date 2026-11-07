/* clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_bind_diff.m libs/video/rsx_texture_layout.c -Wl,-dead_strip
 * -framework Metal -framework Cocoa -framework QuartzCore -o /tmp/test_metal_bind_diff
 *
 * eng_encode_draw binds only what differs from the draw before it in the same
 * encoder. Several draws in one encoder -- from a persistent buffer, from the
 * stage at two offsets (the setVertexBufferOffset path), back to the buffer,
 * with two viewports and stencil references -- must each draw their own
 * vertices, and the bound-state record must name what was set last. */
#include "../rsx_metal_backend.m"
#include <assert.h>
float cellAvconfExt_display_gamma(void) { return 1.0f; }

static const char* kMSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float4 a0 [[attribute(0)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 c; float ps [[point_size]]; };\n"
"vertex VOut vmain(VIn in [[stage_in]]) {\n"
"    VOut o; o.pos = float4(-1.0 + (2.0 * in.a0.x + 1.0) / 8.0, 0.0, 0.0, 1.0);\n"
"    o.c = in.a0; o.ps = 1.0; return o; }\n"
"fragment float4 fmain(VOut in [[stage_in]]) { return in.c; }\n";

enum { W = 8 };

static void put(float (*v)[4], int i, float x, float tag)
{
    v[i][0] = x; v[i][1] = tag; v[i][2] = 0; v[i][3] = 1;
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        NSError* err = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:@(kMSL) options:nil error:&err];
        assert(lib);
        rsx_vertex_layout_plan layout;
        memset(&layout, 0, sizeof layout);
        layout.mask = 1; layout.count = 1; layout.stride = 16;
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        pd.vertexDescriptor = eng_vertex_descriptor(&layout, 16);
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
        id<MTLRenderPipelineState> pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        assert(pso);
        s_eng_pipe[0].pso = pso;
        s_eng_pipe[0].ds = [s_dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        s_eng_pipe[0].cull = MTLCullModeNone;
        s_eng_pipe[0].winding = MTLWindingCounterClockwise;
        s_eng_pipe_count = 1;

        float a[2][4], b[2][4], c[2][4], d[2][4];
        put(a, 0, 0, 1); put(a, 1, 1, 1);
        put(b, 0, 2, 2); put(b, 1, 3, 2);
        put(c, 0, 4, 3); put(c, 1, 5, 3);
        put(d, 0, 6, 4); put(d, 1, 7, 4);
        const u32 ha = eng_vbuf_create(NULL, a, sizeof a);
        const u32 hd = eng_vbuf_create(NULL, d, sizeof d);
        assert(ha && hd);
        s_eng_pending.pipeline = 1;
        s_eng_pending.vp[2] = W; s_eng_pending.vp[3] = 1;
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) s_eng_pending.samp[u] = -1;
        for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) s_eng_pending.vsamp[u] = -1;
        eng_draw_vbuf(NULL, RSX_TOPOLOGY_POINTS, ha, 2, 16, NULL, 0);
        eng_draw(NULL, RSX_TOPOLOGY_POINTS, b, 2, 16, NULL, 0);
        s_eng_pending.stencil_ref = 7;
        eng_draw(NULL, RSX_TOPOLOGY_POINTS, c, 2, 16, NULL, 0);
        s_eng_pending.vp[2] = W + 0.0f;      /* the same viewport again */
        eng_draw_vbuf(NULL, RSX_TOPOLOGY_POINTS, hd, 2, 16, NULL, 0);
        assert(s_eng_rec_count == 4 && s_eng_rec[1].vb_off != s_eng_rec[2].vb_off);

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:1 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> rt = [s_dev newTextureWithDescriptor:td];
        id<MTLBuffer> stage = [s_dev newBufferWithBytes:s_eng_stage length:s_eng_stage_used
                                                options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [s_dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(-1, -1, -1, -1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        s_eng_encoding_failed = 0;
        eng_bound_reset();
        for (u32 i = 0; i < s_eng_rec_count; i++) eng_encode_draw(enc, &s_eng_rec[i], stage);
        /* What is bound now is the last draw's. */
        assert(s_eng_bound.vb[0] == ENG_PTR(s_vbuf[hd - 1]) && s_eng_bound.vb_off[0] == 0);
        assert(s_eng_bound.pso == ENG_PTR(pso) && s_eng_bound.stencil_ref == 7 &&
               s_eng_bound.have_vp && s_eng_bound.vp.width == W);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        assert(!s_eng_encoding_failed && cb.status == MTLCommandBufferStatusCompleted);
        float px[W][4];
        [rt getBytes:px bytesPerRow:sizeof px fromRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0];
        for (int x = 0; x < W; x++) {
            const float want = (float)(x / 2 + 1);
            if (px[x][0] != (float)x || px[x][1] != want) {
                fprintf(stderr, "pixel %d: (%g %g), want (%d %g)\n", x, (double)px[x][0],
                        (double)px[x][1], x, (double)want);
                return 1;
            }
        }
        s_eng_rec_count = 0;
        s_eng_stage_used = 0;
        eng_shutdown(NULL);
        puts("Metal encode binds only what changed, and every draw reads its own vertices");
    }
}
