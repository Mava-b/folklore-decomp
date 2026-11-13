/*
 * ps3recomp - the draw engine's branching vertex programs
 *
 * A vertex program's branch targets are absolute transform-program slots and
 * BRB/CLB test SET_TRANSFORM_BRANCH_BITS (0x1FF8). The engine bakes both
 * into the generated program, so:
 *   - a program loaded at slot S has its targets made relative to S (a
 *     program at slot 8 that branches to slot 11 goes to its own 4th
 *     instruction, not past its end);
 *   - the same bytes at another start slot are another pipeline;
 *   - for a program with BRB/CLB the branch bits are pipeline identity, and
 *     each value takes or skips the branch;
 *   - a program without them keeps its pipeline when the bits change.
 * Checked through the engine against the stub backend of
 * test_rsx_draw_engine.c, with the generated vertex program captured.
 *
 * clang -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video
 *   -o /tmp/test_vp_branch_engine libs/video/tests/test_vp_branch_engine.c
 *   libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c
 *   libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c
 *   libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c
 */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"

static char g_vs[262144];

static u32 capture_pipeline(void* u, const char* vs, const char* ps,
                            const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* layout,
                            u32 stride, rsx_be_format rt, u32 rt_count)
{
    if (vs) snprintf(g_vs, sizeof g_vs, "%s", vs);
    return stub_pipeline_create(u, vs, ps, rs, layout, stride, rt, rt_count);
}

/* Flow instruction `sca_op` (0x09 BRI, 0x11 BRB, 0x12 CLB) to the absolute
 * slot `target`; a boolean branch tests branch bit `bit` against `if_set`. */
static void vp_branch(u8* p, u32 sca_op, u32 target, u32 bit, u32 if_set)
{
    const u32 d0 = (0x3Fu << 15) | (7u << 10) | (((target >> 9) & 1u) << 23);
    const u32 d1 = sca_op << 27;
    const u32 d2 = (target >> 3) & 0x3Fu;
    const u32 d3 = (0x3Fu << 7) | (bit << 23) | (if_set << 28) | ((target & 7u) << 29);
    rsx_test_put_le32(p + 0, d0); rsx_test_put_le32(p + 4, d1);
    rsx_test_put_le32(p + 8, d2); rsx_test_put_le32(p + 12, d3);
}

/* Upload `n` instructions at slot `slot` and start the program there. */
static void load_vp_at(u32 slot, const u8* prog, u32 n)
{
    m(0x1E9C, slot);
    for (u32 k = 0; k < n; k++)
        for (u32 i = 0; i < 4; i++) {
            u32 w; memcpy(&w, prog + k * 16 + i * 4, 4);
            m(0xB80 + ((k * 4 + i) & 31u) * 4, w);
        }
    m(0x1EA0, slot);
}

/*   S+0 MOV o0, v0
 *   S+1 BRB !b3 -> S+3     (taken while bit 3 is clear)
 *   S+2 MOV o1, v1         (skipped when taken)
 *   S+3 MOV o2, v2  END */
static void make_brb_prog(u8* prog, u32 start)
{
    rsx_test_vp_mov_out(prog + 0 * 16, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    vp_branch(prog + 1 * 16, 0x11, start + 3, 3, 0);
    rsx_test_vp_mov_out(prog + 2 * 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(prog + 3 * 16, 2, RSX_TEST_VP_SWZ_IDENT, 2, 1);
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    for (u32 i = 0; i < 12; i++) { u8* p = vm_base + VTX_OFFSET + i * 4; p[0] = 0x3F; p[1] = 0x80; }
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.pipeline_create = capture_pipeline;
    engine_up_with(&be);
    rsx_test_fp_mov_r0_col0(vm_base + TEX_OFFSET + 0x1000u, RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, (TEX_OFFSET + 0x1000u) | 2u);

    u8 prog[4 * 16];
    /* 1. At slot 8, bit 3 clear: the branch is taken, to the program's own
     * slot 3 (absolute 11). */
    make_brb_prog(prog, 8);
    m(0x1FF8, 0);
    load_vp_at(8, prog, 4);
    draw_triangle();
    const u32 p_taken = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 1 && p_taken != 0, "the BRB program builds a pipeline (%d)",
          stub.n_pipeline_create);
    CHECK(strstr(g_vs, "_pc = 3;") && !strstr(g_vs, "_pc = 11;") && !strstr(g_vs, "not modelled"),
          "taken at slot 8: the target is the program's own slot 3");

    /* 2. Bit 3 set: not taken, a new pipeline that runs slot 2. */
    m(0x1FF8, 1u << 3);
    draw_triangle();
    const u32 p_skip = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 2 && p_skip != p_taken,
          "other branch bits: another pipeline (%d)", stub.n_pipeline_create);
    CHECK(strstr(g_vs, "_pc = 2;") != NULL, "not taken: falls through to slot 2");

    /* 3. Back to the first bits: the first pipeline, nothing built. */
    m(0x1FF8, 0);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 2 && stub.bound_pipeline == p_taken,
          "the first bits again find the first pipeline (%d)", stub.n_pipeline_create);

    /* 4. The same bytes at slot 0 branch to absolute slot 11, past the end:
     * another program, so another pipeline. */
    load_vp_at(0, prog, 4);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 3 && stub.bound_pipeline != p_taken,
          "the same bytes at slot 0 are another pipeline (%d)", stub.n_pipeline_create);

    /* 5. A program without BRB/CLB keeps its pipeline across bit changes. */
    u8 plain[16];
    rsx_test_vp_mov_out(plain, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
    load_vp_at(0, plain, 1);
    draw_triangle();
    const int built = stub.n_pipeline_create;
    const u32 p_plain = stub.bound_pipeline;
    m(0x1FF8, 0x5u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == built && stub.bound_pipeline == p_plain,
          "no BRB/CLB: the branch bits are not identity (%d new)", stub.n_pipeline_create - built);
    /* ...and not its start slot either. */
    load_vp_at(16, plain, 1);
    draw_triangle();
    CHECK(stub.n_pipeline_create == built && stub.bound_pipeline == p_plain,
          "no flow: the start slot is not identity (%d new)", stub.n_pipeline_create - built);

    engine_down();
    free(vm_base);
    printf("vp branch engine failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
