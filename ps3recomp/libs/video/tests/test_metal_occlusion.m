/* The Metal backend's ZPASS counting and polygon offset, on the GPU, with
 * synthetic programs and no title data: samples counted per query, nothing
 * reported before the draws are submitted, a query that drew nothing behind
 * the depth buffer reports 0, scissored draws count only their pixels, and
 * the depth bias (set_depth_bias) moves a coplanar draw in front of or behind
 * the depth it is tested against.
 *
 * clang -fobjc-arc -I include -I libs/video
 *   libs/video/tests/test_metal_occlusion.m libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp -lc++
 *   -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore
 *   -o /tmp/test_metal_occlusion && /tmp/test_metal_occlusion
 */
#include "../rsx_metal_backend.m"
#include <stdio.h>
float cellAvconfExt_display_gamma(void) { return 1.0f; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } \
                            else printf("[PASS] %s\n", name); } while (0)

/* A full-viewport triangle at a fixed depth per vertex function. */
static const char* fixture =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"static V at(uint i, float z) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],z,1)}; }\n"
"vertex V vs_front(uint i [[vertex_id]]) { return at(i, 0.25f); }\n"
"vertex V vs_mid(uint i [[vertex_id]])   { return at(i, 0.5f); }\n"
"vertex V vs_back(uint i [[vertex_id]])  { return at(i, 0.75f); }\n"
"vertex V vs_step(uint i [[vertex_id]])  { return at(i, 0.5f + 100.0f / 16777216.0f); }\n"
"fragment float4 fs(V v [[stage_in]]) { return float4(1,0,0,1); }\n";

static id<MTLLibrary> s_lib;

/* Put a pipeline straight into the backend's table: the HLSL translator is
 * not linked into this test, and what is under test is the encoding. */
static u32 add_pipeline(const char* vs, MTLCompareFunction cmp, BOOL write)
{
    MTLRenderPipelineDescriptor* p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [s_lib newFunctionWithName:[NSString stringWithUTF8String:vs]];
    p.fragmentFunction = [s_lib newFunctionWithName:@"fs"];
    p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    p.depthAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    p.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    NSError* err = nil;
    id<MTLRenderPipelineState> pso = [s_dev newRenderPipelineStateWithDescriptor:p error:&err];
    if (!pso) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 0; }
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = cmp;
    ds.depthWriteEnabled = write;
    EngPipeline* e = &s_eng_pipe[s_eng_pipe_count++];
    e->pso = pso;
    e->ds = [s_dev newDepthStencilStateWithDescriptor:ds];
    e->cull = MTLCullModeNone;
    e->winding = MTLWindingCounterClockwise;
    e->building = 0;
    return s_eng_pipe_count;
}

static void draw(u32 pipeline, u32 query, float slope, float units, u32 bits)
{
    static const float verts[3][4];
    eng_bind_pipeline(NULL, pipeline);
    eng_set_depth_bias(NULL, slope, units, bits);
    eng_set_occlusion(NULL, query);
    eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, verts, 3, 16, NULL, 0);
}

static int result(u32 query, u64* n) { *n = ~0ull; return eng_occlusion_result(NULL, query, n); }

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice(); CHECK(s_dev != nil, "Metal device"); if (!s_dev) return 1;
        s_queue = [s_dev newCommandQueue];
        CHECK(eng_init(NULL, 8, 8) == 0, "backend helpers");
        NSError* err = nil;
        s_lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:fixture] options:nil error:&err];
        if (!s_lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
        const u32 color = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
        const u32 depth = eng_depth_target_create(NULL, 8, 8);
        CHECK(color && depth, "targets");
        const u32 mid_write = add_pipeline("vs_mid", MTLCompareFunctionLess, YES);
        const u32 back_test = add_pipeline("vs_back", MTLCompareFunctionLess, NO);
        const u32 front_test = add_pipeline("vs_front", MTLCompareFunctionLess, NO);
        const u32 mid_less = add_pipeline("vs_mid", MTLCompareFunctionLess, NO);
        const u32 mid_lequal = add_pipeline("vs_mid", MTLCompareFunctionLessEqual, NO);
        CHECK(mid_write && back_test && front_test && mid_less && mid_lequal, "pipelines");

        eng_bind_targets(NULL, &color, 1, depth);
        eng_set_viewport(NULL, 0, 0, 8, 8);
        eng_set_scissor(NULL, 0, 0, 8, 8);
        eng_set_stencil_ref(NULL, 0);
        eng_clear_depth_stencil(NULL, depth, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 1.0f, 0);

        u64 n;
        /* 1: the whole 8x8 target at 0.5, writing depth. */
        draw(mid_write, 1, 0, 0, 24);
        /* 2: everything behind it. */
        draw(back_test, 2, 0, 0, 24);
        /* 3: in front of it, but scissored to 4x2. */
        eng_set_scissor(NULL, 2, 3, 4, 2);
        draw(front_test, 3, 0, 0, 24);
        eng_set_scissor(NULL, 0, 0, 8, 8);
        /* 4: two draws into one query: 64 + 64. */
        draw(front_test, 4, 0, 0, 24);
        draw(front_test, 4, 0, 0, 24);
        /* A draw that is not counted. */
        draw(front_test, 0, 0, 0, 24);
        /* 5, 6: coplanar with the depth it tests against, LESS: rejected
         * without a bias, passes when pulled toward the viewer (-1 slope,
         * -256 units, Resistance's decal offset). 7: LEQUAL pushed away. */
        draw(mid_less, 5, 0, 0, 24);
        draw(mid_less, 6, -1.0f, -256.0f, 24);
        draw(mid_lequal, 7, 0, 256.0f, 24);

        CHECK(result(1, &n) == 2, "a recorded, unsubmitted query says so");
        CHECK(result(99, &n) == -1, "an unknown query is unknown");
        eng_submit_and_wait(NULL, 0);
        CHECK(result(1, &n) == 1 && n == 64, "full-target draw counts every sample");
        CHECK(result(2, &n) == 1 && n == 0, "a draw behind the depth buffer counts 0");
        CHECK(result(3, &n) == 1 && n == 8, "a scissored draw counts its pixels only");
        CHECK(result(4, &n) == 1 && n == 128, "two draws in one query add up");
        CHECK(result(5, &n) == 1 && n == 0, "coplanar LESS without bias is rejected");
        CHECK(result(6, &n) == 1 && n == 64, "negative polygon offset pulls the draw in front");
        CHECK(result(7, &n) == 1 && n == 0, "positive polygon offset pushes it behind (LEQUAL)");


        /* A Z16 unit is 256 of Z24's. Against a depth 100 Z24 units behind
         * 0.5, a draw at 0.5 pushed back one Z24 unit still passes LESS; one
         * pushed back one Z16 unit (256 Z24 units) does not. */
        const u32 depth2 = eng_depth_target_create(NULL, 8, 8);
        const u32 step_write = add_pipeline("vs_step", MTLCompareFunctionAlways, YES);
        eng_bind_targets(NULL, &color, 1, depth2);
        eng_clear_depth_stencil(NULL, depth2, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 1.0f, 0);
        draw(step_write, 0, 0, 0, 24);
        draw(mid_less, 11, 0, 1.0f, 24);
        draw(mid_less, 12, 0, 1.0f, 16);
        eng_submit_and_wait(NULL, 0);
        CHECK(result(11, &n) == 1 && n == 64, "one Z24 unit of bias stays in front of a 100-unit step");
        CHECK(result(12, &n) == 1 && n == 0, "one Z16 unit of bias is 256 Z24 units");
        eng_bind_targets(NULL, &color, 1, depth);

        /* Queries across commits: the slots of the first commit are free. */
        draw(front_test, 9, 0, 0, 24);
        eng_submit_and_wait(NULL, 0);
        CHECK(result(9, &n) == 1 && n == 64, "a later commit counts again");
        CHECK(result(1, &n) == 1 && n == 64, "an earlier result stays readable");

        /* An asynchronous commit (a windowed present): the count is pending
         * while the command buffer runs and harvested once it has completed. */
        draw(front_test, 13, 0, 0, 24);
        {
            id<MTLCommandBuffer> cb = [s_queue commandBuffer];
            eng_encode_records(cb, s_eng_stage_used ? s_eng_stage_buf : nil);
            [cb commit];
            eng_vis_committed(cb, 0);
            s_eng_rec_count = 0; s_eng_stage_used = 0;
            const int r = result(13, &n);
            CHECK(r == 0 || r == 1, "an in-flight query is pending, never 'unsubmitted'");
            [cb waitUntilCompleted];
            CHECK(result(13, &n) == 1 && n == 64, "harvested once its command buffer completes");
        }

        printf("%d checks, %d failures\n", checks, failures);
        eng_shutdown(NULL);
        return failures ? 1 : 0;
    }
}
