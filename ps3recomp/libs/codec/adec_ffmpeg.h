/*
 * ps3recomp - cellAdec's decoder back end (libavcodec, loaded at run time)
 *
 * cellAdec.c owns the guest-facing state (handles, callbacks, the PCM queue,
 * pcm items); this file turns one access unit into zero or more blocks of
 * decoded audio, and lays a block out in guest memory the way a codec's
 * StartSeq parameters ask for. Without PS3RECOMP_FFMPEG_RUNTIME, or when
 * FFmpeg is not found, adec_ff_open returns NULL and cellAdec keeps its
 * silence-only path.
 */

#ifndef PS3RECOMP_ADEC_FFMPEG_H
#define PS3RECOMP_ADEC_FFMPEG_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Speakers, in this order, in every AdecFfFrame (and the order cellAudio's
 * 8-channel ports use for the first six). */
enum {
    ADEC_SP_L, ADEC_SP_R, ADEC_SP_C, ADEC_SP_LFE, ADEC_SP_LS, ADEC_SP_RS,
    ADEC_SP_COUNT
};
#define ADEC_SP_BIT(s) (1u << (s))

/* One decoded block: `samples` frames of ADEC_SP_COUNT host floats
 * (interleaved; a speaker the stream does not carry is 0 and its bit is clear
 * in `speakers`). Valid until the next adec_ff_decode / adec_ff_reset. */
typedef struct AdecFfFrame {
    const float* pcm;
    u32 samples;
    u32 sample_rate;
    u32 speakers;     /* ADEC_SP_BIT mask */
    u32 bit_rate;     /* bits/s, 0 unknown */
} AdecFfFrame;

typedef void (*adec_ff_emit_fn)(void* ctx, const AdecFfFrame* frame);

typedef struct AdecFf AdecFf;

/* A decoder for a cellAdec codec type, or NULL when there is none to be had
 * (no FFmpeg, PS3RECOMP_FFMPEG=0, or a type not wired up: AC3 is). */
AdecFf* adec_ff_open(u32 codec_type);
void    adec_ff_close(AdecFf* d);

/* Decode one AU, calling emit for each block that comes out. Returns the
 * number of blocks emitted, or a negative FFmpeg error for a damaged AU; the
 * decoder stays usable. */
int  adec_ff_decode(AdecFf* d, const u8* au, u32 size,
                    adec_ff_emit_fn emit, void* ctx);

/* Drop everything held and start over (cellAdecStartSeq). */
void adec_ff_reset(AdecFf* d);

/* ---------------------------------------------------------------------------
 * Output layout (no FFmpeg needed; unit tested on its own)
 * -----------------------------------------------------------------------*/

/* How a title wants its PCM: `channels` interleaved samples per frame, each
 * a big-endian float (word_float) or big-endian s16; out_sp[i] is the speaker
 * written to slot i. */
typedef struct AdecLayout {
    u32 channels;
    u32 out_sp[8];
    int word_float;
    float scale;      /* applied to every sample; 1.0 = as decoded */
} AdecLayout;

/* The layout CellAdecParamAc3 (the 0x30 guest bytes cellAdecStartSeq gets for
 * an AC3 decoder) describes. Returns 0, or -1 for a parameter block it cannot
 * make sense of (the layout is then 2.0 float). */
int  adec_ac3_layout(const u8 param[0x30], AdecLayout* out);

/* Bytes adec_store_pcm writes for `samples` frames. */
u32  adec_pcm_bytes(const AdecLayout* l, u32 samples);

/* Store one block at guest address dst in layout l, folding speakers the
 * layout lacks into the ones it has (centre into left/right at -3 dB,
 * surrounds into the fronts or a single surround, LFE dropped), scaled so the
 * fold cannot clip. Returns the bytes written. */
u32  adec_store_pcm(u32 dst, const AdecLayout* l, const AdecFfFrame* f);

/* The same, into a host buffer of big-endian samples (what adec_store_pcm
 * copies to the guest). out must hold adec_pcm_bytes(l, f->samples). */
u32  adec_format_pcm(u8* out, const AdecLayout* l, const AdecFfFrame* f);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_ADEC_FFMPEG_H */
