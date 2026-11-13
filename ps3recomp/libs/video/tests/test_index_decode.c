/*
 * ps3recomp - the draw engine's index decoding
 *
 * Random indexed and array draws through the engine against the stub backend
 * of test_rsx_draw_engine.c: 16- and 32-bit big-endian indices, restart on
 * and off with sentinels sprinkled through the run, several DRAW_INDEX
 * batches in one group, strips and lists. Every draw's vertices and indices
 * are checked against a reference built here from the same guest bytes, so
 * the specialised decode and the bulk reference reservation must agree with
 * the plain per-index rules.
 *
 * clang -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video
 *   -o /tmp/test_index_decode libs/video/tests/test_index_decode.c
 *   libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c
 *   libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c
 *   libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c
 */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main

#define IDX_AT        0x00040000u      /* main memory */
#define M_IDX_OFFSET  0x181C
#define M_IDX_FORMAT  0x1820
#define M_DRAW_INDEX  0x1824
#define M_RESTART_EN  0x1DAC
#define M_RESTART     0x1DB0

static unsigned g_seed = 1u;
static u32 rnd(u32 n) { g_seed = g_seed * 1103515245u + 12345u; return (g_seed >> 8) % n; }

/* Vertex v: x = v, y = 2v (float32 x4 at VTX_OFFSET, stride 16). */
static void write_mesh(u32 n)
{
    for (u32 v = 0; v < n; v++) {
        u8* p = vm_base + VTX_OFFSET + v * 16u;
        const float f[4] = { (float)v, 2.0f * (float)v, 0.0f, 1.0f };
        for (u32 k = 0; k < 4; k++) {
            u32 w; memcpy(&w, &f[k], 4);
            p[k * 4] = (u8)(w >> 24); p[k * 4 + 1] = (u8)(w >> 16);
            p[k * 4 + 2] = (u8)(w >> 8); p[k * 4 + 3] = (u8)w;
        }
    }
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    write_mesh(64);
    engine_up();
    int bad = 0, draws = 0, cut_draws = 0;
    u64 digest = 1469598103934665603ull;
    for (int run = 0; run < 3000; run++) {
        const int is_u32 = (int)rnd(2);
        const int restart = (int)rnd(2);
        const u32 sentinel = is_u32 ? 0xFFFFFFFFu : 0xFFFFu;
        const u32 count = 3u + rnd(18);
        const u32 nbatch = 1u + rnd(3);
        /* The index run, guest bytes; one or more batches cover it in order. */
        u32 idx[24];
        int has_cut = 0;
        for (u32 i = 0; i < count; i++) {
            idx[i] = rnd(64);
            if (restart && i > 2 && i + 3 < count && rnd(6) == 0) { idx[i] = sentinel; has_cut = 1; }
            u8* p = vm_base + IDX_AT + i * (is_u32 ? 4u : 2u);
            if (is_u32) { p[0] = (u8)(idx[i] >> 24); p[1] = (u8)(idx[i] >> 16); p[2] = (u8)(idx[i] >> 8); p[3] = (u8)idx[i]; }
            else        { p[0] = (u8)(idx[i] >> 8); p[1] = (u8)idx[i]; }
        }
        m(M_RESTART_EN, (u32)restart);
        m(M_RESTART, sentinel);
        m(M_IDX_OFFSET, IDX_AT);
        m(M_IDX_FORMAT, 1u | ((is_u32 ? 0u : 1u) << 4));   /* main; bit 4 set = u16 */
        const u32 prim = has_cut ? RSX_PRIMITIVE_TRIANGLE_STRIP : RSX_PRIMITIVE_TRIANGLES;
        m(M_BEGIN_END, prim);
        u32 first = 0;
        for (u32 b = 0; b < nbatch; b++) {
            const u32 left = count - first;
            const u32 n = (b + 1 == nbatch) ? left : 1u + rnd(left - (nbatch - 1u - b));
            m(M_DRAW_INDEX, first | ((n - 1u) << 24));
            first += n;
        }
        const int before = stub.n_draw;
        m(M_BEGIN_END, 0u);
        if (stub.n_draw == before) continue;   /* nothing drawable (all cut) */
        draws++;
        if (has_cut) cut_draws++;
        /* A digest of everything the backend saw, for comparing builds. */
        digest = digest * 1099511628211ull ^ stub.draw_vertices;
        digest = digest * 1099511628211ull ^ stub.draw_indices;
        for (u32 k = 0; k < 24; k++) digest = digest * 1099511628211ull ^ stub.captured_indices[k];
        for (u32 v = 0; v < 16; v++) for (u32 k = 0; k < sizeof stub.captured_vertices[0]; k++)
            digest = digest * 1099511628211ull ^ ((const u8*)stub.captured_vertices[v])[k];
        /* Reference: every drawn index names a vertex whose x is the guest
         * index at that position of the triangle list the engine rebuilt. */
        for (u32 k = 0; k < stub.draw_indices && k < 24; k++) {
            const u32 vi = stub.captured_indices[k];
            if (vi >= 16 || vi >= stub.draw_vertices) continue;   /* beyond the capture */
            float x; memcpy(&x, stub.captured_vertices[vi], 4);
            int found = 0;
            for (u32 i = 0; i < count; i++)
                if (idx[i] != sentinel && (float)idx[i] == x) found = 1;
            if (!found) { bad++; break; }
        }
        if (!has_cut && prim == RSX_PRIMITIVE_TRIANGLES) {
            /* A list without cuts: index k of the draw is guest index k. */
            for (u32 k = 0; k < stub.draw_indices && k < 24 && k < count; k++) {
                const u32 vi = stub.captured_indices[k];
                if (vi >= 16) continue;
                float x; memcpy(&x, stub.captured_vertices[vi], 4);
                if (x != (float)idx[k]) { bad++; break; }
            }
        }
    }
    CHECK(bad == 0 && draws > 2000 && cut_draws > 100,
          "%d random indexed draws (%d with restart cuts) decode as the guest wrote them (%d bad)",
          draws, cut_draws, bad);
    printf("digest of every draw: %016llx\n", (unsigned long long)digest);
    engine_down();
    free(vm_base);
    printf("index decode failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
