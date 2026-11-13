/* The draw engine's ZPASS bookkeeping and polygon offset, against the stub
 * backend of test_rsx_draw_engine.c; no GPU, no title data.
 *
 *   - SET_POLY_OFFSET_FILL_ENABLE with SCALE_FACTOR/BIAS reaches the backend
 *     as set_depth_bias(slope, units, depth bits) before the draw, Z16 or
 *     Z24 by the surface's zeta format, and 0/0 once it is disabled;
 *   - while SET_ZPASS_PIXEL_COUNT_ENABLE is on every draw is given the open
 *     query (0 while it is off); CLEAR_REPORT_VALUE(1) and GET_REPORT each
 *     close it, so a report sums exactly the draws since the last clear;
 *   - a report with no draw since the clear is 0 at once;
 *   - a report whose draws are only recorded is pending until the engine
 *     submits: RSX_ZPASS_POLL leaves it, RSX_ZPASS_KICK submits it, and a
 *     count the backend lost comes back as -1.
 *
 * clang -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video
 *   -o /tmp/test_zpass_engine libs/video/tests/test_zpass_engine.c
 *   libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c libs/video/rsx_vertex_compact.c
 *   libs/video/rsx_texture_layout.c libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c
 */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main

#define M_POLY_OFFSET_FILL   0x0A68
#define M_POLY_OFFSET_FACTOR 0x0A78
#define M_POLY_OFFSET_BIAS   0x0A7C
#define M_CLEAR_REPORT       0x17C8
#define M_ZPASS_ENABLE       0x17CC

static u32 fbits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* What the backend was told before the last draw. */
static float bias_slope = -99, bias_units = -99;
static u32 bias_bits, bias_calls;
static void zb_depth_bias(void* u, float slope, float units, u32 bits)
{ (void)u; bias_slope = slope; bias_units = units; bias_bits = bits; bias_calls++; }

/* Queries the draws were counted into, and a fake GPU: a query is
 * "recorded" until the engine submits, then done with 10 samples a draw. */
#define NQ 64
static u32 draw_query[NQ], n_draw_queries, pending_query;
static u32 q_draws[NQ], q_submitted[NQ];
static int q_lost[NQ], submits;
static void zb_set_occlusion(void* u, u32 q) { (void)u; pending_query = q; }
static void zb_draw(void* u, rsx_topology t, const void* v, u32 n, u32 s, const u32* i, u32 ni)
{
    stub_draw(u, t, v, n, s, i, ni);
    if (n_draw_queries < NQ) draw_query[n_draw_queries++] = pending_query;
    if (pending_query && pending_query < NQ) q_draws[pending_query]++;
}
static void zb_submit(void* u, u32 reason)
{
    (void)u; (void)reason; submits++;
    for (u32 q = 0; q < NQ; q++) q_submitted[q] = q_draws[q];
}
static int zb_result(void* u, u32 q, u64* samples)
{
    (void)u;
    if (!q || q >= NQ || !q_draws[q] || q_lost[q]) return -1;
    if (q_submitted[q] != q_draws[q]) return 2;
    *samples = 10ull * q_draws[q];
    return 1;
}

int main(void)
{
    setenv("PS3RECOMP_ENG_ZPASS", "1", 1);   /* host counting is opt-in */
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;

    rsx_draw_backend be = g_stub_backend;
    be.set_depth_bias = zb_depth_bias;
    be.set_occlusion = zb_set_occlusion;
    be.occlusion_result = zb_result;
    be.draw = zb_draw;
    be.submit_and_wait = zb_submit;
    engine_up_with(&be);

    /* ---- polygon offset ---- */
    draw_triangle();
    CHECK(stub.n_draw == 1, "the draw executed");
    CHECK(bias_calls == 1 && bias_slope == 0.0f && bias_units == 0.0f,
          "no offset: the backend is told 0/0 (slope %g units %g)", bias_slope, bias_units);
    m(M_POLY_OFFSET_FACTOR, fbits(-1.0f));
    m(M_POLY_OFFSET_BIAS, fbits(-256.0f));
    draw_triangle();
    CHECK(bias_slope == 0.0f && bias_units == 0.0f, "factor/bias alone change nothing while FILL is off");
    m(M_POLY_OFFSET_FILL, 1);
    draw_triangle();
    CHECK(bias_slope == -1.0f && bias_units == -256.0f && bias_bits == 24u,
          "FILL on: -1/-256 against a Z24 zeta (slope %g units %g bits %u)", bias_slope, bias_units, bias_bits);
    m(M_SURFACE_FORMAT, 8u | (1u << 5));          /* A8R8G8B8, Z16 */
    m(M_POLY_OFFSET_FACTOR, fbits(0.25f));
    m(M_POLY_OFFSET_BIAS, fbits(200.0f));
    draw_triangle();
    CHECK(bias_slope == 0.25f && bias_units == 200.0f && bias_bits == 16u,
          "a Z16 zeta says 16 bits (slope %g units %g bits %u)", bias_slope, bias_units, bias_bits);
    m(M_SURFACE_FORMAT, 8u | (2u << 5));          /* back to Z24S8 */
    m(M_POLY_OFFSET_FILL, 0);
    draw_triangle();
    CHECK(bias_slope == 0.0f && bias_units == 0.0f, "FILL off again: 0/0");

    /* ---- ZPASS ---- */
    const u32 base = n_draw_queries;
    draw_triangle();                                /* counting off */
    CHECK(draw_query[base] == 0, "with ZPASS counting off a draw counts into nothing");
    CHECK(rsx_draw_engine_zpass_supported(), "the backend can count");
    m(M_ZPASS_ENABLE, 1);
    m(M_CLEAR_REPORT, 1);
    draw_triangle();
    const u32 t1 = rsx_draw_engine_zpass_report();
    m(M_CLEAR_REPORT, 1);
    draw_triangle();
    draw_triangle();
    const u32 t2 = rsx_draw_engine_zpass_report();
    draw_triangle();                                /* no clear: t3 sums t2's draws too */
    const u32 t3 = rsx_draw_engine_zpass_report();
    m(M_CLEAR_REPORT, 1);
    const u32 t4 = rsx_draw_engine_zpass_report();  /* nothing drawn since the clear */
    const u32 qa = draw_query[base + 1], qb = draw_query[base + 2], qc = draw_query[base + 4];
    CHECK(t1 && t2 && t3 && t4 && t1 != t2, "reports get tickets");
    CHECK(qa && qb && qa != qb && draw_query[base + 3] == qb,
          "a clear opens a new query; draws until the next report share it");
    CHECK(qc && qc != qb, "a report closes the query: later draws get a new one");

    u32 v = 0xDEAD;
    CHECK(rsx_draw_engine_zpass_result(t4, RSX_ZPASS_POLL, &v) == 1 && v == 0,
          "a report with no draw since the clear is 0 at once");
    CHECK(rsx_draw_engine_zpass_result(t1, RSX_ZPASS_POLL, &v) == 0 && submits == 0,
          "recorded draws: POLL leaves the report pending and submits nothing");
    CHECK(rsx_draw_engine_zpass_result(t1, RSX_ZPASS_KICK, &v) == 1 && v == 10 && submits == 1,
          "KICK submits recorded work and the count comes back (%u)", v);
    CHECK(rsx_draw_engine_zpass_result(t2, RSX_ZPASS_POLL, &v) == 1 && v == 20,
          "two draws in one query count twice (%u)", v);
    CHECK(rsx_draw_engine_zpass_result(t3, RSX_ZPASS_POLL, &v) == 1 && v == 30,
          "without a clear the count keeps adding up (%u)", v);
    q_lost[qa] = 1;
    CHECK(rsx_draw_engine_zpass_result(t1, RSX_ZPASS_POLL, &v) == -1, "a lost count is -1");
    CHECK(rsx_draw_engine_zpass_result(0x7FFFFFFFu, RSX_ZPASS_POLL, &v) == -1, "an unknown ticket is -1");

    m(M_ZPASS_ENABLE, 0);
    const u32 before = n_draw_queries;
    draw_triangle();
    CHECK(draw_query[before] == 0, "counting disabled again: draws count into nothing");

    engine_down();
    free(vm_base);
    printf("zpass engine failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
