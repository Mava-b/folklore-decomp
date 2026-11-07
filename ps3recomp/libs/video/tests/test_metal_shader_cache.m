/* PS3_SHADER_CACHE and PS3_SHADER_ASYNC on the engine's pipeline path.
 *
 * clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_shader_cache.m
 *   libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp
 *   libs/video/rsx_shader_spirv.cpp (and the glslang / spirv-cross libraries the
 *   backend links) -framework Metal -framework Cocoa -framework QuartzCore
 *
 * Run as: test_metal_shader_cache <cache dir> <sync|async>. A first run into
 * an empty directory writes one translation per stage; a second run reads
 * them back; an async run builds the pipeline on the queue, its draw is
 * skipped until the state is ready and then encodes. */
#include "rsx_metal_backend.m"
#include <assert.h>
u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 off) { (void)local; return off; }

static const char* k_vs =
    "struct VSIn { float4 a0 : ATTR0; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = i.a0; return o; }\n";
static const char* k_ps =
    "float4 main() : SV_Target0 { return float4(1.0, 0.5, 0.25, 1.0); }\n";

static u32 count_msl(const char* dir)
{
    u32 n = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* de;
    while ((de = readdir(d))) if (strstr(de->d_name, ".msl")) n++;
    closedir(d);
    return n;
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        assert(argc == 3);
        setenv("PS3_SHADER_CACHE", argv[1], 1);
        const int async = !strcmp(argv[2], "async");
        if (async) setenv("PS3_SHADER_ASYNC", "1", 1);
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { fprintf(stderr, "no Metal device\n"); return 2; }
        s_queue = [s_dev newCommandQueue];
        s_guest_shaders = rsx_hlsl_to_msl_available();
        if (!s_guest_shaders) { fprintf(stderr, "no HLSL->MSL translator\n"); return 2; }

        rsx_be_render_state rs; memset(&rs, 0, sizeof rs);
        rs.color_mask = 0xFFFFFFFFu;
        rsx_vertex_layout_plan layout; memset(&layout, 0, sizeof layout);
        layout.count = 1; layout.stride = 16;
        const u32 h = eng_pipeline_create(NULL, k_vs, k_ps, &rs, &layout, 16,
                                          RSX_BE_FMT_R8G8B8A8, 1);
        assert(h);
        if (async) {
            /* The handle exists at once; the state follows on the queue. */
            dispatch_sync(s_sc_queue, ^{});
        }
        assert(__atomic_load_n(&s_eng_pipe[h - 1].building, __ATOMIC_ACQUIRE) == 0);
        assert(s_eng_pipe[h - 1].pso != nil);

        /* A pipeline still building is skipped, not an encoding failure. */
        __atomic_store_n(&s_eng_pipe[h - 1].building, 1, __ATOMIC_RELEASE);
        EngRecord r; memset(&r, 0, sizeof r); r.pipeline = h;
        const u32 skipped = s_sc_async_skipped;
        eng_encode_draw(nil, &r, nil);
        assert(s_sc_async_skipped == skipped + 1 && !s_eng_encoding_failed);
        __atomic_store_n(&s_eng_pipe[h - 1].building, 0, __ATOMIC_RELEASE);

        printf("shader cache: %s %u hit(s) %u new, %u file(s) in %s\n", argv[2],
               s_sc_hits, s_sc_miss, count_msl(s_sc_dir), s_sc_dir);
        assert(s_sc_hits + s_sc_miss == 2);
        assert(count_msl(s_sc_dir) == 2);
        eng_shutdown(NULL);
    }
    return 0;
}
