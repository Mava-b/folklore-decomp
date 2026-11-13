/*
 * ps3recomp - the draw engine's program memo and pipeline index
 *
 * Per draw the engine reuses what it derived from a program's bytes when the
 * bytes compare equal, and finds the pipeline by key through an index instead
 * of scanning the table. Checked through the engine against the stub backend
 * of test_rsx_draw_engine.c:
 *   - a fragment or vertex program rewritten in place (same address, other
 *     bytes) gets its own pipeline, and going back to the first bytes finds
 *     the first pipeline again;
 *   - more programs than the memo holds, cycled, each keep their own pipeline
 *     and none is built twice;
 *   - hundreds of pipelines are each found again by key.
 *
 * clang -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video
 *   -o /tmp/test_program_memo libs/video/tests/test_program_memo.c
 *   libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c
 *   libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c
 *   libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c
 */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"

#define FP_AT(i) (TEX_OFFSET + 0x1000u + (u32)(i) * 0x40u)

static void load_vp(u32 swz)
{
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, swz, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}

static u32 fp_swz(u32 i)   /* a distinct swizzle for each i < 256 */
{
    return RSX_TEST_FP_SWZ(i & 3u, (i >> 2) & 3u, (i >> 4) & 3u, (i >> 6) & 3u);
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    for (u32 i = 0; i < 12; i++) { u8* p = vm_base + VTX_OFFSET + i * 4; p[0] = 0x3F; p[1] = 0x80; }
    engine_up();
    load_vp(RSX_TEST_VP_SWZ_IDENT);

    /* 1. In place. */
    rsx_test_fp_mov_r0_col0(vm_base + FP_AT(0), RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, FP_AT(0) | 2u);
    draw_triangle();
    const u32 p_a = stub.bound_pipeline;
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1 && stub.bound_pipeline == p_a,
          "the same program twice: one pipeline (%d)", stub.n_pipeline_create);
    rsx_test_fp_mov_r0_col0(vm_base + FP_AT(0), RSX_TEST_FP_SWZ(3, 2, 1, 0));
    draw_triangle();
    const u32 p_b = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 2 && p_b != p_a,
          "a fragment program rewritten at the same address gets its own pipeline (%d)",
          stub.n_pipeline_create);
    rsx_test_fp_mov_r0_col0(vm_base + FP_AT(0), RSX_TEST_FP_SWZ_IDENT);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 2 && stub.bound_pipeline == p_a,
          "back to the first bytes: the first pipeline, nothing built");
    load_vp(RSX_TEST_VP_SWZ(1, 0, 2, 3));
    draw_triangle();
    const u32 p_c = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 3 && p_c != p_a,
          "a vertex program rewritten in place gets its own pipeline (%d)", stub.n_pipeline_create);
    load_vp(RSX_TEST_VP_SWZ_IDENT);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 3 && stub.bound_pipeline == p_a,
          "and its first bytes the first pipeline again");

    /* 2. More programs than the memo holds, cycled. */
    enum { N = 300 };
    static u32 handle[N];
    const int before = stub.n_pipeline_create;
    for (u32 i = 0; i < N; i++) {
        rsx_test_fp_mov_r0_col0(vm_base + FP_AT(i + 1), fp_swz(i + 1));
        m(0x8E4, FP_AT(i + 1) | 2u);
        draw_triangle();
        handle[i] = stub.bound_pipeline;
    }
    /* Swizzles repeat every 256: programs with the same bytes at different
     * addresses share a pipeline. Swizzle 228 (0,1,2,3) is part 1's identity and
     * swizzle 27 (3,2,1,0) part 1's rewrite, so 254 are new. */
    int pairs_ok = 1;
    for (u32 i = 0; i < N; i++)
        for (u32 j = i + 1; j < N; j++)
            if ((handle[i] == handle[j]) != (((i + 1) & 0xFFu) == ((j + 1) & 0xFFu))) pairs_ok = 0;
    for (u32 i = 0; i < N; i++) {
        if ((((i + 1) & 0xFFu) == 228) != (handle[i] == p_a)) pairs_ok = 0;
        if ((((i + 1) & 0xFFu) == 27) != (handle[i] == p_b)) pairs_ok = 0;
    }
    CHECK(stub.n_pipeline_create - before == 254 && pairs_ok,
          "%d programs: %d new pipelines, one per distinct program (want 254)",
          N, stub.n_pipeline_create - before);
    const int built = stub.n_pipeline_create;
    int same = 1;
    for (int pass = 0; pass < 2; pass++)
        for (u32 i = 0; i < N; i++) {
            m(0x8E4, FP_AT(i + 1) | 2u);
            draw_triangle();
            if (stub.bound_pipeline != handle[i]) same = 0;
        }
    CHECK(same && stub.n_pipeline_create == built,
          "cycled twice more: every draw finds its own pipeline, none built again (%d new)",
          stub.n_pipeline_create - built);

    engine_down();
    /* After a restart the memo and the index start over too. */
    engine_up();
    load_vp(RSX_TEST_VP_SWZ_IDENT);
    m(0x8E4, FP_AT(0) | 2u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1 && stub.bound_pipeline != 0,
          "after a restart the pipeline is built again (%d)", stub.n_pipeline_create);
    engine_down();
    free(vm_base);
    printf("program memo failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
