/*
 * ps3recomp -- cellAdec through the guest API, with and without libavcodec
 *
 * Built twice by CMakeLists.txt (PS3RECOMP_BUILD_TESTS), like test_vdec_decode:
 *   test_adec_stub    no PS3RECOMP_FFMPEG_RUNTIME
 *   test_adec_ffmpeg  with it, against an AC3 stream gen_adec_media.cmake makes
 *
 * Modes:
 *   layout          CellAdecParamAc3 -> output layout, and the fold-down of
 *                   5.1 into the smaller layouts (no FFmpeg needed)
 *   stub   [ES]     the silence-only decoder: one AUDONE + PCMOUT per AU, an
 *                   8 KB block of zeros, bsiInfo pcmSize 8192 -- what every
 *                   build without FFmpeg (and Resistance's movie player, which
 *                   sizes its ring from it) always had
 *   decode ES       AC3 3/2 + LFE, AU by AU: Resistance's StartSeq parameters
 *                   (float, 3/2, LFE on) must give 6 channels of 1536 samples
 *                   in the order L C R Ls Rs LFE, each channel carrying the
 *                   tone the generator put on that speaker (and nothing much
 *                   of the others); s16 2.0 must give the folded stereo.
 */
#include "../cellAdec.c"
#include "../adec_ffmpeg.c"
#include "../ffmpeg_runtime.c"

#include <math.h>
#include <stdlib.h>

/* What the runtime would provide. */
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base = 0x01000000u;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
PPU_THREAD_LOCAL ppu_context* g_active_ctx;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Guest layout */
#define MEM_SIZE   (64u << 20)
#define G_TYPE     0x10000u
#define G_CB       0x10010u
#define G_HANDLE   0x10020u
#define G_AUINFO   0x10040u
#define G_PARAM    0x10080u
#define G_ITEMPTR  0x100C0u
#define G_AU       0x00100000u
#define G_PCM      0x00200000u
#define CB_OPD     0x00001000u

static u32 s_msgs[1 << 16];
static int s_nmsg;
static void fake_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                            uint64_t a7)
{
    (void)a0; (void)a2; (void)a4; (void)a5; (void)a6; (void)a7;
    CHECK(opd == CB_OPD && a3 == 0xC0FFEEu);
    if (s_nmsg < (1 << 16)) s_msgs[s_nmsg++] = (u32)a1;
}
ps3_guest_caller_fn g_ps3_guest_caller = fake_guest_call;

static int count_msgs(u32 type)
{
    int n = 0;
    for (int i = 0; i < s_nmsg; i++) n += s_msgs[i] == type;
    return n;
}

static u8* read_file(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* p = (u8*)malloc((size_t)n + 1);
    CHECK(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* Resistance: Fall of Man's CellAdecParamAc3, as its movie player passes it
 * to cellAdecStartSeq (logged from the title): float, 3/2, LFE on, DRC 1/1,
 * line out, 6 channels, PCM scale 1, channel pointers 0..5. */
static const u8 k_res_param[0x30] = {
    0x01, 0x07, 0x01, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
    0x02, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
    0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 5,
};

static float be_f32(u32 ea)
{
    u32 v = vm_read32(ea);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* ---------------------------------------------------------------------------
 * layout: no decoder
 * -----------------------------------------------------------------------*/
static int mode_layout(void)
{
    AdecLayout l;
    CHECK(adec_ac3_layout(k_res_param, &l) == 0);
    CHECK(l.channels == 6 && l.word_float && fabsf(l.scale - 1.0f) < 1e-6f);
    const u32 want[6] = { ADEC_SP_L, ADEC_SP_C, ADEC_SP_R, ADEC_SP_LS, ADEC_SP_RS, ADEC_SP_LFE };
    for (int i = 0; i < 6; i++) CHECK(l.out_sp[i] == want[i]);
    CHECK(adec_pcm_bytes(&l, 1536) == 36864);   /* the 0x9000 stride the title uses */

    /* channelPointer: L to slot 0, C 2, R 1, Ls 4, Rs 5, LFE 3 (cellAudio order). */
    u8 p[0x30];
    memcpy(p, k_res_param, sizeof p);
    const u8 cp[6] = { 0, 2, 1, 4, 5, 3 };
    for (int i = 0; i < 6; i++) p[0x18 + 4 * i + 3] = cp[i];
    CHECK(adec_ac3_layout(p, &l) == 0);
    const u32 want2[6] = { ADEC_SP_L, ADEC_SP_R, ADEC_SP_C, ADEC_SP_LFE, ADEC_SP_LS, ADEC_SP_RS };
    for (int i = 0; i < 6; i++) CHECK(l.out_sp[i] == want2[i]);
    /* Not a permutation: ignored. */
    p[0x18 + 3] = 5;
    CHECK(adec_ac3_layout(p, &l) == 0);
    for (int i = 0; i < 6; i++) CHECK(l.out_sp[i] == want[i]);

    /* s16 2.0, no LFE. */
    memcpy(p, k_res_param, sizeof p);
    p[0] = 0; p[1] = 2; p[2] = 0;
    CHECK(adec_ac3_layout(p, &l) == 0);
    CHECK(l.channels == 2 && !l.word_float && l.out_sp[0] == ADEC_SP_L && l.out_sp[1] == ADEC_SP_R);
    CHECK(adec_pcm_bytes(&l, 1536) == 6144);

    /* Junk: 2.0 float, and an error. */
    memset(p, 0xEE, sizeof p);
    CHECK(adec_ac3_layout(p, &l) < 0);
    CHECK(l.channels == 2 && l.word_float);

    /* Fold-down: one frame of a full 5.1 source into 2.0 float. */
    float in[ADEC_SP_COUNT] = { 0 };
    in[ADEC_SP_L] = 0.5f; in[ADEC_SP_C] = 0.4f; in[ADEC_SP_LS] = 0.2f; in[ADEC_SP_LFE] = 0.9f;
    AdecFfFrame fr = { in, 1, 48000, 0x3F, 0 };
    memcpy(p, k_res_param, sizeof p);
    p[1] = 2; p[2] = 0;
    CHECK(adec_ac3_layout(p, &l) == 0);
    u8 out[64];
    CHECK(adec_format_pcm(out, &l, &fr) == 8);
    u32 b; float lv, rv;
    b = ((u32)out[0] << 24) | ((u32)out[1] << 16) | ((u32)out[2] << 8) | out[3]; memcpy(&lv, &b, 4);
    b = ((u32)out[4] << 24) | ((u32)out[5] << 16) | ((u32)out[6] << 8) | out[7]; memcpy(&rv, &b, 4);
    const float k = 0.70710678f, norm = 1.0f / (1.0f + k + k);   /* L + kC + kLs, normalised */
    CHECK(fabsf(lv - (0.5f + k * 0.4f + k * 0.2f) * norm) < 1e-5f);
    CHECK(fabsf(rv - (k * 0.4f) * norm) < 1e-5f);              /* LFE is left out */

    /* s16 big-endian, clamped. */
    p[0] = 0;
    CHECK(adec_ac3_layout(p, &l) == 0);
    in[ADEC_SP_L] = 4.0f; in[ADEC_SP_C] = 0.0f; in[ADEC_SP_LS] = 0.0f;
    in[ADEC_SP_R] = -0.25f;
    CHECK(adec_format_pcm(out, &l, &fr) == 4);
    CHECK(out[0] == 0x7F && out[1] == 0xFF);
    const s16 r16 = (s16)(((u16)out[2] << 8) | out[3]);
    CHECK(r16 == (s16)(-0.25f * norm * 32768.0f - 0.5f));

    /* 3/2 + LFE source into a 3/2 + LFE layout: the identity. */
    CHECK(adec_ac3_layout(k_res_param, &l) == 0);
    for (int i = 0; i < ADEC_SP_COUNT; i++) in[i] = 0.1f * (float)(i + 1);
    CHECK(adec_format_pcm(out, &l, &fr) == 24);
    for (int i = 0; i < 6; i++) {
        const u8* q = out + 4 * i;
        b = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
        float v; memcpy(&v, &b, 4);
        CHECK(fabsf(v - in[l.out_sp[i]]) < 1e-6f);
    }
    printf("layout: ok\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Driving cellAdec
 * -----------------------------------------------------------------------*/
static u32 open_decoder(const u8* param)
{
    vm_write32(G_TYPE, CELL_ADEC_TYPE_AC3);
    vm_write32(G_CB, CB_OPD);
    vm_write32(G_CB + 4, 0xC0FFEEu);
    CHECK(cellAdecOpen((const CellAdecType*)(uintptr_t)G_TYPE, NULL,
                       (const CellAdecCb*)(uintptr_t)G_CB,
                       (CellAdecHandle*)(uintptr_t)G_HANDLE) == CELL_OK);
    const u32 h = vm_read32(G_HANDLE);
    vm_memcpy_to(G_PARAM, param, 0x30);
    CHECK(cellAdecStartSeq(h, (void*)(uintptr_t)G_PARAM) == CELL_OK);
    return h;
}

static void submit(u32 h, const u8* au, u32 size, u32 pts)
{
    vm_memcpy_to(G_AU, au, size);
    vm_write32(G_AUINFO + 0x00, G_AU);
    vm_write32(G_AUINFO + 0x04, size);
    vm_write32(G_AUINFO + 0x08, pts == ~0u ? ~0u : 0);
    vm_write32(G_AUINFO + 0x0C, pts);
    vm_write64(G_AUINFO + 0x10, 0x1234ull);
    CHECK(cellAdecDecodeAu(h, (const CellAdecAuInfo*)(uintptr_t)G_AUINFO) == CELL_OK);
}

/* AC3 sync frames: 0x0B77, length from fscod (48 kHz) / frmsizecod. */
static int split_ac3(const u8* es, size_t len, size_t* off, int max)
{
    static const u16 words48[19] = { 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
                                     384, 448, 512, 640, 768, 896, 1024, 1152, 1280 };
    int n = 0;
    size_t i = 0;
    while (i + 6 < len && n < max) {
        CHECK(es[i] == 0x0B && es[i + 1] == 0x77);
        CHECK((es[i + 4] >> 6) == 0);                    /* 48 kHz */
        const u32 code = es[i + 4] & 0x3F;
        CHECK(code / 2 < 19);
        off[n++] = i;
        i += 2u * words48[code / 2];
    }
    off[n] = i;
    return n;
}

static int mode_stub(int argc, char** argv)
{
    u8 dummy[1792];
    memset(dummy, 0x5A, sizeof dummy);
    const u8* es = dummy;
    size_t len = sizeof dummy;
    size_t off[4096];
    int nau = 1;
    off[0] = 0; off[1] = len;
    if (argc > 2) {
        es = read_file(argv[2], &len);
        nau = split_ac3(es, len, off, 64);
    }
    const u32 h = open_decoder(k_res_param);
    CHECK(s_adec[h].ff == NULL);
    for (int i = 0; i < nau; i++) {
        s_nmsg = 0;
        vm_memset(G_PCM, 0x77, 0x2000);
        submit(h, es + off[i], (u32)(off[i + 1] - off[i]), ~0u);
        CHECK(count_msgs(CELL_ADEC_MSG_TYPE_AUDONE) == 1 && count_msgs(CELL_ADEC_MSG_TYPE_PCMOUT) == 1);
        CHECK(cellAdecGetPcmItem(h, (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR) == CELL_OK);
        const u32 it = vm_read32(G_ITEMPTR);
        CHECK(vm_read32(it + PCMITEM_SIZE) == 8192);
        CHECK(vm_read32(vm_read32(it + PCMITEM_BSI_INFO) + 0x14) == 8192);
        CHECK(vm_read32(it + PCMITEM_AU_START) == G_AU);
        CHECK(cellAdecGetPcm(h, (void*)(uintptr_t)G_PCM) == CELL_OK);
        for (u32 o = 0; o < 8192; o += 4) CHECK(vm_read32(G_PCM + o) == 0);
        CHECK(cellAdecGetPcm(h, (void*)(uintptr_t)G_PCM) == (s32)CELL_ADEC_ERROR_EMPTY);
    }
    CHECK(cellAdecEndSeq(h) == CELL_OK);
    CHECK(cellAdecClose(h) == CELL_OK);
    printf("stub: %d AU(s) ok\n", nau);
    return 0;
}

/* The generator's tones (gen_adec_media.cmake), by speaker. */
static const float k_amp[ADEC_SP_COUNT] = {
    [ADEC_SP_L] = 0.10f, [ADEC_SP_R] = 0.20f, [ADEC_SP_C] = 0.30f,
    [ADEC_SP_LFE] = 0.40f, [ADEC_SP_LS] = 0.25f, [ADEC_SP_RS] = 0.15f,
};

static int mode_decode(int argc, char** argv)
{
    CHECK(argc > 2);
    size_t len;
    u8* es = read_file(argv[2], &len);
    static size_t off[4096];
    const int nau = split_ac3(es, len, off, 4000);
    CHECK(nau > 40);

    const u32 h = open_decoder(k_res_param);
    if (!s_adec[h].ff) { printf("no libavcodec AC3 decoder: skipped\n"); return 77; }

    /* 3/2 + LFE float, the title's own parameters. Queue a few AUs before
     * taking them, the way a demuxer runs ahead of the player. */
    double sq[6] = { 0 };
    u64 n = 0;
    int taken = 0;
    s_nmsg = 0;
    for (int i = 0; i < nau; i++) {
        submit(h, es + off[i], (u32)(off[i + 1] - off[i]), i == 0 ? 90000u : ~0u);
        if (i % 3 != 2 && i != nau - 1) continue;
        for (;;) {
            if (cellAdecGetPcmItem(h, (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR) != CELL_OK) break;
            const u32 it = vm_read32(G_ITEMPTR);
            CHECK(vm_read32(it + PCMITEM_SIZE) == 36864);
            const u32 bsi = vm_read32(it + PCMITEM_BSI_INFO);
            CHECK(vm_read32(bsi + 0x14) == 36864);
            CHECK(vm_read8(bsi + 0x32) == 7 && vm_read8(bsi + 0x33) == 1);   /* acmod 3/2, LFE */
            /* PTS: the first AU's, then one AC3 block (2880 ticks) apart. */
            CHECK(vm_read32(it + PCMITEM_AU_PTS_LO) == 90000u + 2880u * (u32)taken);
            CHECK(vm_read64(it + PCMITEM_AU_USERDATA) == 0x1234ull);
            CHECK(cellAdecGetPcm(h, (void*)(uintptr_t)G_PCM) == CELL_OK);
            if (taken >= 8)   /* past the encoder's start-up */
                for (u32 s = 0; s < 1536; s++, n++)
                    for (int c = 0; c < 6; c++) {
                        const float v = be_f32(G_PCM + (s * 6 + (u32)c) * 4);
                        sq[c] += (double)v * v;
                    }
            taken++;
        }
    }
    CHECK(taken == nau);
    CHECK(count_msgs(CELL_ADEC_MSG_TYPE_PCMOUT) == nau && count_msgs(CELL_ADEC_MSG_TYPE_AUDONE) == nau);
    CHECK(s_adec[h].errors == 0 && s_adec[h].dropped == 0);
    const AdecLayout* l = &s_adec[h].layout;
    for (int c = 0; c < 6; c++) {
        const double rms = sqrt(sq[c] / (double)n);
        const double want = k_amp[l->out_sp[c]] / sqrt(2.0);
        printf("  slot %d (speaker %u): rms %.4f, tone %.4f\n", c, l->out_sp[c], rms, want);
        CHECK(fabs(rms - want) < 0.15 * want);
    }
    CHECK(cellAdecEndSeq(h) == CELL_OK);

    /* The same stream as s16 2.0: L = L + kC + kLs folded and normalised. */
    u8 p[0x30];
    memcpy(p, k_res_param, sizeof p);
    p[0] = 0; p[1] = 2; p[2] = 0;
    vm_memcpy_to(G_PARAM, p, 0x30);
    CHECK(cellAdecStartSeq(h, (void*)(uintptr_t)G_PARAM) == CELL_OK);
    double s2[2] = { 0 };
    n = 0;
    for (int i = 0; i < nau; i++) {
        submit(h, es + off[i], (u32)(off[i + 1] - off[i]), ~0u);
        CHECK(cellAdecGetPcmItem(h, (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR) == CELL_OK);
        CHECK(vm_read32(vm_read32(G_ITEMPTR) + PCMITEM_SIZE) == 6144);
        CHECK(cellAdecGetPcm(h, (void*)(uintptr_t)G_PCM) == CELL_OK);
        if (i < 8) continue;
        for (u32 s = 0; s < 1536 * 2; s++) {
            const s16 v = (s16)vm_read16(G_PCM + s * 2);
            s2[s & 1] += (double)v * v / (32768.0 * 32768.0);
        }
        n += 1536;
    }
    const double k = 0.70710678, norm = 1.0 / (1.0 + 2 * k);
    /* Different tones per speaker: the powers add. */
    const double wl = norm * sqrt(0.5 * (0.01 + k * k * 0.09 + k * k * 0.0625));
    const double wr = norm * sqrt(0.5 * (0.04 + k * k * 0.09 + k * k * 0.0225));
    const double gl = sqrt(s2[0] / (double)n), gr = sqrt(s2[1] / (double)n);
    printf("  s16 2.0: L rms %.4f (want %.4f), R rms %.4f (want %.4f)\n", gl, wl, gr, wr);
    CHECK(fabs(gl - wl) < 0.15 * wl && fabs(gr - wr) < 0.15 * wr);

    /* A damaged AU still gets one PCMOUT, of silence, the right size. */
    u8 bad[1792];
    memset(bad, 0, sizeof bad);
    s_nmsg = 0;
    submit(h, bad, sizeof bad, ~0u);
    CHECK(count_msgs(CELL_ADEC_MSG_TYPE_PCMOUT) == 1);
    CHECK(cellAdecGetPcmItem(h, (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR) == CELL_OK);
    CHECK(vm_read32(vm_read32(G_ITEMPTR) + PCMITEM_SIZE) == 6144);
    CHECK(cellAdecGetPcm(h, NULL) == CELL_OK);
    CHECK(cellAdecGetPcmItem(h, (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR) == (s32)CELL_ADEC_ERROR_EMPTY);

    /* A consumer that stops taking blocks: the queue keeps the newest. */
    for (int i = 0; i < 40; i++) submit(h, es + off[i], (u32)(off[i + 1] - off[i]), ~0u);
    CHECK(s_adec[h].count == ADEC_SLOTS && s_adec[h].dropped == 40 - ADEC_SLOTS);
    CHECK(cellAdecClose(h) == CELL_OK);
    printf("decode: %d AUs ok\n", nau);
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, MEM_SIZE);
    CHECK(vm_base);
    ppu_vm_size = MEM_SIZE;
    const char* mode = argc > 1 ? argv[1] : "layout";
    if (strcmp(mode, "layout") == 0) return mode_layout();
    if (strcmp(mode, "stub") == 0)   return mode_stub(argc, argv);
    if (strcmp(mode, "decode") == 0) return mode_decode(argc, argv);
    printf("unknown mode %s\n", mode);
    return 2;
}
