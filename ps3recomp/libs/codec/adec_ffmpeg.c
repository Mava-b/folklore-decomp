/*
 * ps3recomp - cellAdec's decoder back end (see adec_ffmpeg.h)
 */

#include "adec_ffmpeg.h"
#include "cellAdec.h"
#include "ffmpeg_runtime.h"
#include "../guest_struct.h"   /* vm_memcpy_to */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Output layout
 * -----------------------------------------------------------------------*/

static float rd_be_f32(const u8* p)
{
    u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static s32 rd_be_s32(const u8* p)
{
    return (s32)(((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]);
}

/* CellAdecParamAc3 (RPCS3 Modules/cellAdec.h), 0x30 bytes, big-endian:
 *   +0x00 u8  wordSize        0 int16, 1 float
 *   +0x01 u8  outputMode      1 1/0, 2 2/0, 3 3/0, 4 2/1, 5 3/1, 6 2/2, 7 3/2
 *   +0x02 u8  outLfeOn
 *   +0x04 f32 drcCutScaleFactor, +0x08 f32 drcBoostScaleFactor
 *   +0x0C u8  compressionMode, +0x0D numberOfChannels, +0x0E stereoMode,
 *   +0x0F u8  dualmonoMode,   +0x10 karaokeCapableMode
 *   +0x14 f32 pcmScaleFactor
 *   +0x18 s32 channelPointer0..5
 *
 * The channels come out in the AC3 bitstream's own order -- L, C, R, then the
 * surround(s), then LFE -- with the ones the output mode lacks left out.
 * channelPointerN, when the six of them are a permutation of 0..5, names the
 * slot in a 6-channel frame for the speaker in the Nth place of L C R Ls Rs
 * LFE (the AC3_InputChannel order); a title that asks for 3/2 + LFE uses it
 * to put the channels where its own mixer wants them. */
int adec_ac3_layout(const u8 param[0x30], AdecLayout* out)
{
    memset(out, 0, sizeof(*out));
    out->word_float = 1;
    out->scale = 1.0f;
    out->channels = 2;
    out->out_sp[0] = ADEC_SP_L;
    out->out_sp[1] = ADEC_SP_R;
    if (!param) return -1;

    const u8 word = param[0], mode = param[1], lfe = param[2];
    if (word > 1 || mode < 1 || mode > 7) return -1;
    out->word_float = word == 1;
    float sc = rd_be_f32(param + 0x14);
    if (sc > 0.0f && sc <= 16.0f) out->scale = sc;

    /* AC3 order for each mode (the bitstream's acmod order). A single
     * surround (2/1, 3/1) is written from the LS slot (see adec_format_pcm). */
    static const u8 order[8][5] = {
        {0},
        { ADEC_SP_C },
        { ADEC_SP_L, ADEC_SP_R },
        { ADEC_SP_L, ADEC_SP_C, ADEC_SP_R },
        { ADEC_SP_L, ADEC_SP_R, ADEC_SP_LS },
        { ADEC_SP_L, ADEC_SP_C, ADEC_SP_R, ADEC_SP_LS },
        { ADEC_SP_L, ADEC_SP_R, ADEC_SP_LS, ADEC_SP_RS },
        { ADEC_SP_L, ADEC_SP_C, ADEC_SP_R, ADEC_SP_LS, ADEC_SP_RS },
    };
    static const u8 count[8] = { 0, 1, 2, 3, 3, 4, 4, 5 };
    u32 n = count[mode];
    for (u32 i = 0; i < n; i++) out->out_sp[i] = order[mode][i];
    if (lfe) out->out_sp[n++] = ADEC_SP_LFE;
    out->channels = n;

    /* channelPointer remap, for the full 3/2 + LFE frame only. */
    if (mode == 7 && lfe) {
        static const u8 ac3_place[6] = { ADEC_SP_L, ADEC_SP_C, ADEC_SP_R,
                                         ADEC_SP_LS, ADEC_SP_RS, ADEC_SP_LFE };
        s32 cp[6];
        u32 seen = 0;
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            cp[i] = rd_be_s32(param + 0x18 + 4 * i);
            if (cp[i] < 0 || cp[i] > 5 || (seen & (1u << cp[i]))) { ok = 0; break; }
            seen |= 1u << cp[i];
        }
        if (ok)
            for (int i = 0; i < 6; i++) out->out_sp[cp[i]] = ac3_place[i];
    }
    return 0;
}

u32 adec_pcm_bytes(const AdecLayout* l, u32 samples)
{
    return samples * l->channels * (l->word_float ? 4u : 2u);
}

/* Fold matrix: out slot i = sum over speakers s of m[i][s] * in[s]. */
static void adec_fold_matrix(const AdecLayout* l, u32 have, float m[8][ADEC_SP_COUNT])
{
    enum { L = ADEC_SP_L, R = ADEC_SP_R, C = ADEC_SP_C, LFE = ADEC_SP_LFE,
           LS = ADEC_SP_LS, RS = ADEC_SP_RS };
    const float k = 0.70710678f;
    u32 want = 0;
    for (u32 i = 0; i < l->channels; i++) want |= ADEC_SP_BIT(l->out_sp[i]);
    /* A single surround is written from LS: 2/1 and 3/1 have LS, no RS. */
    const int mono_sur = (want & ADEC_SP_BIT(LS)) && !(want & ADEC_SP_BIT(RS));

    memset(m, 0, sizeof(float) * 8 * ADEC_SP_COUNT);
    for (u32 i = 0; i < l->channels; i++) {
        const u32 sp = l->out_sp[i];
        float* row = m[i];
        if (have & ADEC_SP_BIT(sp)) row[sp] = 1.0f;
        switch (sp) {
        case L: case R: {
            const u32 sur = sp == L ? LS : RS;
            if (!(want & ADEC_SP_BIT(C)) && (have & ADEC_SP_BIT(C))) row[C] += k;
            if (!(want & ADEC_SP_BIT(sur)) && !mono_sur && (have & ADEC_SP_BIT(sur)))
                row[sur] += k;
            /* Mono source into a stereo pair. */
            if (!(have & ADEC_SP_BIT(sp)) && (have & ADEC_SP_BIT(C)) &&
                !(have & (ADEC_SP_BIT(L) | ADEC_SP_BIT(R))))
                row[C] = 1.0f;
            break;
        }
        case C:
            if (!(want & (ADEC_SP_BIT(L) | ADEC_SP_BIT(R)))) {   /* 1/0 out */
                if (have & ADEC_SP_BIT(L)) row[L] += k;
                if (have & ADEC_SP_BIT(R)) row[R] += k;
                if (have & ADEC_SP_BIT(LS)) row[LS] += 0.5f;
                if (have & ADEC_SP_BIT(RS)) row[RS] += 0.5f;
            }
            break;
        case LS:
            if (mono_sur && (have & ADEC_SP_BIT(RS))) {
                row[LS] = (have & ADEC_SP_BIT(LS)) ? k : 0.0f;
                row[RS] = k;
            }
            break;
        default:
            break;
        }
    }
    /* Normalise: no output may sum to more than full scale. */
    float worst = 1.0f;
    for (u32 i = 0; i < l->channels; i++) {
        float s = 0.0f;
        for (u32 j = 0; j < ADEC_SP_COUNT; j++) s += m[i][j];
        if (s > worst) worst = s;
    }
    if (worst > 1.0f)
        for (u32 i = 0; i < l->channels; i++)
            for (u32 j = 0; j < ADEC_SP_COUNT; j++) m[i][j] /= worst;
}

u32 adec_format_pcm(u8* out, const AdecLayout* l, const AdecFfFrame* f)
{
    float m[8][ADEC_SP_COUNT];
    adec_fold_matrix(l, f->speakers, m);
    const u32 nch = l->channels;
    u8* o = out;
    for (u32 s = 0; s < f->samples; s++) {
        const float* in = f->pcm + (size_t)s * ADEC_SP_COUNT;
        for (u32 c = 0; c < nch; c++) {
            float v = 0.0f;
            for (u32 j = 0; j < ADEC_SP_COUNT; j++) v += m[c][j] * in[j];
            v *= l->scale;
            if (l->word_float) {
                u32 b;
                memcpy(&b, &v, 4);
                o[0] = (u8)(b >> 24); o[1] = (u8)(b >> 16); o[2] = (u8)(b >> 8); o[3] = (u8)b;
                o += 4;
            } else {
                float x = v * 32768.0f;
                s32 q = x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (s32)(x + (x >= 0 ? 0.5f : -0.5f));
                o[0] = (u8)((u16)q >> 8); o[1] = (u8)q;
                o += 2;
            }
        }
    }
    return (u32)(o - out);
}

u32 adec_store_pcm(u32 dst, const AdecLayout* l, const AdecFfFrame* f)
{
    const u32 bytes = adec_pcm_bytes(l, f->samples);
    u8* tmp = (u8*)malloc(bytes ? bytes : 1);
    if (!tmp) return 0;
    adec_format_pcm(tmp, l, f);
    vm_memcpy_to(dst, tmp, bytes);
    free(tmp);
    return bytes;
}

#ifdef PS3RECOMP_FFMPEG_RUNTIME

/* ---------------------------------------------------------------------------
 * Decoder
 * -----------------------------------------------------------------------*/

struct AdecFf {
    const PS3FFmpeg* ff;
    AVCodecContext* ctx;
    AVPacket* pkt;
    AVFrame* frame;
    u8* buf;          /* the AU plus FFmpeg's zeroed input padding */
    u32 buf_cap;
    float* pcm;       /* one block, ADEC_SP_COUNT floats a frame */
    u32 pcm_cap;      /* frames */
};

AdecFf* adec_ff_open(u32 codec_type)
{
    enum AVCodecID id;
    if (codec_type == CELL_ADEC_TYPE_AC3) id = AV_CODEC_ID_AC3;
    else return NULL;

    const PS3FFmpeg* ff = ps3_ffmpeg_get();
    if (!ff) return NULL;
    const AVCodec* codec = ff->avcodec_find_decoder(id);
    if (!codec) {
        printf("[cellAdec] libavcodec has no decoder for codecType %u\n", codec_type);
        return NULL;
    }
    AdecFf* d = (AdecFf*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->ff = ff;
    d->ctx = ff->avcodec_alloc_context3(codec);
    d->pkt = ff->av_packet_alloc();
    d->frame = ff->av_frame_alloc();
    if (d->ctx) d->ctx->thread_count = 1;
    if (!d->ctx || !d->pkt || !d->frame || ff->avcodec_open2(d->ctx, codec, NULL) < 0) {
        printf("[cellAdec] could not open the libavcodec decoder\n");
        adec_ff_close(d);
        return NULL;
    }
    return d;
}

void adec_ff_close(AdecFf* d)
{
    if (!d) return;
    if (d->ctx) d->ff->avcodec_free_context(&d->ctx);
    if (d->pkt) d->ff->av_packet_free(&d->pkt);
    if (d->frame) d->ff->av_frame_free(&d->frame);
    free(d->buf);
    free(d->pcm);
    free(d);
}

/* FFmpeg's speaker for a native-order channel bit, as one of ours; a back
 * centre (2/1, 3/1) is both surrounds at -3 dB. */
static int adec_ff_speaker(u64 bit)
{
    if (bit == AV_CH_FRONT_LEFT)     return ADEC_SP_L;
    if (bit == AV_CH_FRONT_RIGHT)    return ADEC_SP_R;
    if (bit == AV_CH_FRONT_CENTER)   return ADEC_SP_C;
    if (bit == AV_CH_LOW_FREQUENCY)  return ADEC_SP_LFE;
    if (bit == AV_CH_SIDE_LEFT || bit == AV_CH_BACK_LEFT)   return ADEC_SP_LS;
    if (bit == AV_CH_SIDE_RIGHT || bit == AV_CH_BACK_RIGHT) return ADEC_SP_RS;
    if (bit == AV_CH_BACK_CENTER)    return 100;
    return -1;
}

static int adec_ff_receive(AdecFf* d, adec_ff_emit_fn emit, void* cb_ctx)
{
    int emitted = 0;
    for (;;) {
        AVFrame* f = d->frame;
        int r = d->ff->avcodec_receive_frame(d->ctx, f);
        if (r < 0)
            return emitted;                          /* EAGAIN / EOF / error */
        const int nch = f->ch_layout.nb_channels;
        const u32 ns = (u32)f->nb_samples;
        const int planar = f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_S16P;
        const int is_f = f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_FLT;
        const int is_s = f->format == AV_SAMPLE_FMT_S16P || f->format == AV_SAMPLE_FMT_S16;
        if (nch <= 0 || nch > 8 || (!is_f && !is_s)) {
            static int _n = 0;
            if (_n++ < 4)
                printf("[cellAdec] skipping a block: %d channels, sample format %d\n",
                       nch, f->format);
            d->ff->av_frame_unref(f);
            continue;
        }
        if (ns > d->pcm_cap) {
            float* np = (float*)realloc(d->pcm, (size_t)ns * ADEC_SP_COUNT * sizeof(float));
            if (!np) { d->ff->av_frame_unref(f); continue; }
            d->pcm = np;
            d->pcm_cap = ns;
        }
        memset(d->pcm, 0, (size_t)ns * ADEC_SP_COUNT * sizeof(float));

        /* Which of our speakers each decoded channel is. */
        int sp[8];
        u32 speakers = 0;
        if (f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
            u64 mask = f->ch_layout.u.mask;
            int c = 0;
            for (int b = 0; b < 64 && c < nch; b++) {
                if (!(mask & (1ull << b))) continue;
                sp[c++] = adec_ff_speaker(1ull << b);
            }
            for (; c < nch; c++) sp[c] = -1;
        } else {
            /* Unspecified order: mono is C, stereo L R, then the 5.1 order. */
            static const int dflt[8] = { ADEC_SP_L, ADEC_SP_R, ADEC_SP_C, ADEC_SP_LFE,
                                         ADEC_SP_LS, ADEC_SP_RS, -1, -1 };
            for (int c = 0; c < nch; c++) sp[c] = nch == 1 ? ADEC_SP_C : dflt[c];
        }
        for (int c = 0; c < nch; c++) {
            if (sp[c] < 0) continue;
            const int both = sp[c] == 100;
            if (both) speakers |= ADEC_SP_BIT(ADEC_SP_LS) | ADEC_SP_BIT(ADEC_SP_RS);
            else speakers |= ADEC_SP_BIT(sp[c]);
            for (u32 s = 0; s < ns; s++) {
                float v;
                if (is_f) v = planar ? ((const float*)f->extended_data[c])[s]
                                     : ((const float*)f->data[0])[(size_t)s * nch + c];
                else v = (planar ? ((const int16_t*)f->extended_data[c])[s]
                                 : ((const int16_t*)f->data[0])[(size_t)s * nch + c]) / 32768.0f;
                float* o = d->pcm + (size_t)s * ADEC_SP_COUNT;
                if (both) { o[ADEC_SP_LS] += v * 0.70710678f; o[ADEC_SP_RS] += v * 0.70710678f; }
                else o[sp[c]] += v;
            }
        }

        AdecFfFrame out;
        out.pcm = d->pcm;
        out.samples = ns;
        out.sample_rate = (u32)f->sample_rate;
        out.speakers = speakers;
        out.bit_rate = d->ctx->bit_rate > 0 ? (u32)d->ctx->bit_rate : 0;
        emit(cb_ctx, &out);
        emitted++;
        d->ff->av_frame_unref(f);
    }
}

int adec_ff_decode(AdecFf* d, const u8* au, u32 size,
                   adec_ff_emit_fn emit, void* cb_ctx)
{
    if (size + AV_INPUT_BUFFER_PADDING_SIZE > d->buf_cap) {
        u8* nb = (u8*)realloc(d->buf, size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!nb) return AVERROR(ENOMEM);
        d->buf = nb;
        d->buf_cap = size + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    memcpy(d->buf, au, size);
    memset(d->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    AVPacket* pkt = d->pkt;
    pkt->data = d->buf;
    pkt->size = (int)size;
    pkt->pts = AV_NOPTS_VALUE;
    pkt->dts = AV_NOPTS_VALUE;

    int n = 0;
    int r = d->ff->avcodec_send_packet(d->ctx, pkt);
    if (r == AVERROR(EAGAIN)) {
        n += adec_ff_receive(d, emit, cb_ctx);
        r = d->ff->avcodec_send_packet(d->ctx, pkt);
    }
    pkt->data = NULL;
    pkt->size = 0;
    n += adec_ff_receive(d, emit, cb_ctx);
    return r < 0 ? r : n;
}

void adec_ff_reset(AdecFf* d)
{
    d->ff->avcodec_flush_buffers(d->ctx);
}

#else /* !PS3RECOMP_FFMPEG_RUNTIME */

AdecFf* adec_ff_open(u32 codec_type) { (void)codec_type; return NULL; }
void adec_ff_close(AdecFf* d) { (void)d; }
int  adec_ff_decode(AdecFf* d, const u8* au, u32 size,
                    adec_ff_emit_fn emit, void* ctx)
{ (void)d; (void)au; (void)size; (void)emit; (void)ctx; return -1; }
void adec_ff_reset(AdecFf* d) { (void)d; }

#endif /* PS3RECOMP_FFMPEG_RUNTIME */
