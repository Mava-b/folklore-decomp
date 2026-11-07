/* clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_vstreams.m libs/video/rsx_texture_layout.c -Wl,-dead_strip
 * -framework Metal -framework Cocoa -framework QuartzCore -o /tmp/test_metal_vstreams
 *
 * The Metal backend's split vertex streams (rsx_draw_backend.draw_vbufs, the
 * draw engine's per-stream vertex cache):
 *   - the vertex descriptor for a stream map puts each stream in its own
 *     buffer, its slots packed in ascending order, and refuses a map that
 *     names a stream out of range or leaves one empty;
 *   - a split draw records each stream as a persistent buffer or as staged
 *     bytes, and a draw naming a retired buffer is not recorded;
 *   - on the GPU, a pipeline made from that descriptor and a record encoded
 *     by eng_encode_draw read every attribute from the right stream. */
#include "../rsx_metal_backend.m"
#include <assert.h>
/* The present path is linked in through the backend table checked below. */
float cellAvconfExt_display_gamma(void) { return 1.0f; }

static const char* kMSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float4 a0 [[attribute(0)]]; float4 a1 [[attribute(1)]]; float4 a2 [[attribute(2)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 c; float ps [[point_size]]; };\n"
"vertex VOut vmain(VIn in [[stage_in]], uint vid [[vertex_id]]) {\n"
"    VOut o; o.pos = float4(-1.0 + (2.0 * vid + 1.0) / 4.0, 0.0, 0.0, 1.0);\n"
"    o.c = float4(in.a0.x, in.a1.y, in.a2.z, in.a0.w + in.a1.w + in.a2.w); o.ps = 1.0; return o; }\n"
"fragment float4 fmain(VOut in [[stage_in]]) { return in.c; }\n";

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);

        /* Three slots; slot 1 in stream 1, slots 0 and 2 in stream 0. */
        rsx_vertex_layout_plan layout;
        memset(&layout, 0, sizeof layout);
        layout.mask = 0x7u; layout.count = 3; layout.stride = 48;
        for (u8 a = 0; a < 3; a++) layout.attrs[a] = a;
        MTLVertexDescriptor* one = eng_vertex_descriptor(&layout, layout.stride);
        assert(one && one.attributes[2].offset == 32 && one.attributes[2].bufferIndex == MTL_VB_INDEX &&
               one.layouts[MTL_VB_INDEX].stride == 48);
        layout.stream_map = 1ull << 4;
        layout.n_streams = 2;
        MTLVertexDescriptor* vd = eng_vertex_descriptor(&layout, layout.stride);
        assert(vd);
        assert(vd.attributes[0].bufferIndex == MTL_VB_INDEX && vd.attributes[0].offset == 0);
        assert(vd.attributes[1].bufferIndex == MTL_VB_INDEX - 1 && vd.attributes[1].offset == 0);
        assert(vd.attributes[2].bufferIndex == MTL_VB_INDEX && vd.attributes[2].offset == 16);
        assert(vd.layouts[MTL_VB_INDEX].stride == 32 && vd.layouts[MTL_VB_INDEX - 1].stride == 16);
        rsx_vertex_layout_plan bad = layout;
        bad.stream_map = 2ull << 4;                  /* stream 2 of 2 */
        assert(!eng_vertex_descriptor(&bad, bad.stride));
        bad.stream_map = 0x111ull;                   /* stream 0 empty */
        assert(!eng_vertex_descriptor(&bad, bad.stride));

        /* Stream 0 resident, stream 1 staged. Vertex v: a0 = (v, 0, 0, 1),
         * a2 = (0, 0, 10 + v, 2), a1 = (0, 100 + v, 0, 4). */
        enum { N = 4 };
        float s0[N][8], s1[N][4];
        memset(s0, 0, sizeof s0); memset(s1, 0, sizeof s1);
        for (int v = 0; v < N; v++) {
            s0[v][0] = (float)v; s0[v][3] = 1.0f;
            s0[v][6] = 10.0f + v; s0[v][7] = 2.0f;
            s1[v][1] = 100.0f + v; s1[v][3] = 4.0f;
        }
        const u32 h0 = eng_vbuf_create(NULL, s0, sizeof s0);
        assert(h0);
        s_eng_pending.vp[2] = 4.0f; s_eng_pending.vp[3] = 1.0f;
        s_eng_pending.pipeline = 1;
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) s_eng_pending.samp[u] = -1;
        for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) s_eng_pending.vsamp[u] = -1;
        const rsx_be_vstream st[2] = { { h0, NULL, 32 }, { 0, s1, 16 } };
        eng_draw_vbufs(NULL, RSX_TOPOLOGY_POINTS, st, 2, N, NULL, 0);
        assert(s_eng_rec_count == 1);
        const EngRecord* r = &s_eng_rec[0];
        assert(r->kind == ENG_REC_DRAW && r->n_vb == 2 && r->vb_buf == h0 && r->vb2_buf[0] == 0 &&
               r->vertex_count == N && r->stride == 32 && r->index_count == 0);
        assert(!memcmp(s_eng_stage + r->vb2_off[0], s1, sizeof s1));
        /* Bad streams are not recorded: a zero stride, missing data, a dead buffer. */
        const rsx_be_vstream z[2] = { { h0, NULL, 32 }, { 0, s1, 0 } };
        eng_draw_vbufs(NULL, RSX_TOPOLOGY_POINTS, z, 2, N, NULL, 0);
        const rsx_be_vstream nd[2] = { { h0, NULL, 32 }, { 0, NULL, 16 } };
        eng_draw_vbufs(NULL, RSX_TOPOLOGY_POINTS, nd, 2, N, NULL, 0);
        const rsx_be_vstream dead[2] = { { 999, NULL, 32 }, { 0, s1, 16 } };
        eng_draw_vbufs(NULL, RSX_TOPOLOGY_POINTS, dead, 2, N, NULL, 0);
        assert(s_eng_rec_count == 1);

        /* On the GPU. */
        NSError* err = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:@(kMSL) options:nil error:&err];
        assert(lib);
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
        id<MTLRenderPipelineState> pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) { fprintf(stderr, "%s\n", [[err localizedDescription] UTF8String]); return 1; }
        s_eng_pipe[0].pso = pso;
        s_eng_pipe[0].ds = [s_dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        s_eng_pipe[0].cull = MTLCullModeNone;
        s_eng_pipe[0].winding = MTLWindingCounterClockwise;
        s_eng_pipe_count = 1;

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:N height:1 mipmapped:NO];
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
        eng_encode_draw(enc, r, stage);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        assert(!s_eng_encoding_failed && cb.status == MTLCommandBufferStatusCompleted);
        float px[N][4];
        [rt getBytes:px bytesPerRow:sizeof px fromRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0];
        for (int v = 0; v < N; v++) {
            if (px[v][0] != (float)v || px[v][1] != 100.0f + v || px[v][2] != 10.0f + v ||
                px[v][3] != 7.0f) {
                fprintf(stderr, "vertex %d: (%g %g %g %g)\n", v, (double)px[v][0], (double)px[v][1],
                        (double)px[v][2], (double)px[v][3]);
                return 1;
            }
        }

        s_eng_rec_count = 0;
        s_eng_stage_used = 0;
        eng_shutdown(NULL);
        assert(s_engine_backend.draw_vbufs == eng_draw_vbufs);
        puts("Metal split vertex streams: each attribute read from its own stream");
    }
}
