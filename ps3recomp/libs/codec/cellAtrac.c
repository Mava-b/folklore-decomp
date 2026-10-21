/*
 * ps3recomp - cellAtrac HLE
 *
 * All-data-on-memory decoding of ATRAC3plus RIFF files, with looping. The
 * title hands over the whole .at3 file (SetDataAndGetMemSize), creates a
 * decoder, and then pulls one frame of float PCM per cellAtracDecode.
 *
 * Sample positions. FFmpeg's decoder output is a linear stream of 2048-sample
 * frames. The file's first audible sample sits at index
 *     first = fact[1] + 0x170
 * (0x170 is ATRAC3plus's own decoder delay -- the same constant PPSSPP uses),
 * and fact[0] audible samples follow. `smpl` loop points count from fact[2]
 * (fact[1] when the chunk has no third word). Checked on The Simpsons Arcade
 * Game's three tracks: first + fact[0] lands exactly on the last frame
 * boundary of the data chunk, and each loop spans the whole track.
 *
 * ponytail: no streaming (GetStreamDataInfo/AddStreamData/second buffer) and
 * no plain ATRAC3 -- nothing that runs needs them yet. A title that streams
 * gets a loud log line from SetDataAndGetMemSize; add the ring-buffer
 * bookkeeping then (PPSSPP's sceAtrac is the model).
 */

#include "cellAtrac.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../third_party/at3_standalone/at3_bridge.h"
#include <stdio.h>
#include <string.h>

#define AT3P_SAMPLES   2048
#define AT3P_DELAY     0x170
#define ATRAC_MAX_SLOTS 16

typedef struct {
    u32 channels, block_align;
    u32 data_off, data_bytes;  /* the data chunk, relative to the file start */
    s32 first, end;            /* decoder-stream indices; end is exclusive   */
    s32 loop_start, loop_end;  /* same space, loop_end exclusive; -1 = none  */
} AtracTrackInfo;

typedef struct {
    u32 handle_ea;             /* 0 = free */
    const u8* file;
    AtracTrackInfo t;
    void* dec;
    s32 pos;                   /* next decoder-stream index to output */
    s32 loop_num;              /* -1 forever, 0 none, n more laps */
    s32 next_frame;            /* frame the decoder expects next */
    s32 pcm_frame;             /* frame held in pcm[], -1 = none */
    float pcm[2][AT3P_SAMPLES];
} AtracSlot;

static AtracSlot s_slots[ATRAC_MAX_SLOTS];

static u32 le32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

/* The ATRAC3plus subformat GUID of a WAVE_FORMAT_EXTENSIBLE fmt chunk. */
static const u8 k_at3p_guid[16] = { 0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                    0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62 };

/* Parses a RIFF .at3 header. Returns 0 or a CELL_ATRAC_ERROR_*. */
int atrac_parse_riff(const u8* f, u32 bytes, AtracTrackInfo* t)
{
    memset(t, 0, sizeof(*t));
    t->loop_start = t->loop_end = -1;
    if (bytes < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4))
        return (int)CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
    u32 total = 0, off1 = 0, off2 = 0, has_fact = 0, lstart = 0, lend = 0, has_loop = 0;
    u32 o = 12;
    while (o + 8 <= bytes && !t->data_off) {
        const u8* c = f + o;
        const u32 cs = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            if (o + 8 + 40 > bytes || cs < 40 || le16(c + 8) != 0xFFFE ||
                memcmp(c + 8 + 24, k_at3p_guid, 16))
                return (int)CELL_ATRAC_ERROR_UNKNOWN_FORMAT;   /* ATRAC3plus only */
            t->channels = le16(c + 10);
            t->block_align = le16(c + 20);
        } else if (!memcmp(c, "fact", 4) && cs >= 8 && o + 16 <= bytes) {
            has_fact = 1;
            total = le32(c + 8);
            off1 = le32(c + 12);
            off2 = (cs >= 12 && o + 20 <= bytes) ? le32(c + 16) : off1;
        } else if (!memcmp(c, "smpl", 4) && cs >= 36 + 24 && o + 8 + 60 <= bytes &&
                   le32(c + 8 + 28)) {
            has_loop = 1;
            lstart = le32(c + 8 + 36 + 8);
            lend = le32(c + 8 + 36 + 12);
        } else if (!memcmp(c, "data", 4)) {
            t->data_off = o + 8;
            t->data_bytes = cs;
        }
        o += 8 + cs;
    }
    if (!t->data_off || !t->block_align || t->channels < 1 || t->channels > 2)
        return (int)CELL_ATRAC_ERROR_ILLEGAL_DATA;
    const s32 stream = (s32)(t->data_bytes / t->block_align) * AT3P_SAMPLES;
    t->first = (s32)off1 + AT3P_DELAY;
    t->end = has_fact ? t->first + (s32)total : stream;
    if (t->end > stream) t->end = stream;
    if (has_loop && lend >= lstart && lstart >= off2) {
        t->loop_start = t->first + (s32)(lstart - off2);
        t->loop_end = t->first + (s32)(lend - off2) + 1;
        if (t->loop_end > t->end) t->loop_end = t->end;
    }
    return 0;
}

static AtracSlot* slot_of(u32 handle_ea)
{
    for (int i = 0; i < ATRAC_MAX_SLOTS; i++)
        if (s_slots[i].handle_ea == handle_ea) return &s_slots[i];
    return NULL;
}

/* Puts decoder-stream frame k in s->pcm, decoding forward from wherever the
 * decoder is. The decoder overlaps adjacent frames, so a jump backwards
 * restarts one frame early and discards that frame's output. */
static int decode_frame(AtracSlot* s, s32 k)
{
    if (s->pcm_frame == k) return 0;
    if (s->next_frame > k) {
        at3p_flush(s->dec);
        s->next_frame = k > 0 ? k - 1 : 0;
    }
    while (s->next_frame <= k) {
        const u32 at = s->t.data_off + (u32)s->next_frame * s->t.block_align;
        int n = 0;
        if (at3p_decode(s->dec, s->pcm[0], s->pcm[1], &n, s->file + at,
                        (int)s->t.block_align) < 0 || n != AT3P_SAMPLES) {
            /* A bad frame decodes as silence rather than ending the track. */
            memset(s->pcm, 0, sizeof(s->pcm));
        }
        s->next_frame++;
    }
    s->pcm_frame = k;
    return 0;
}

static int looping(const AtracSlot* s) { return s->t.loop_start >= 0 && s->loop_num != 0; }

s32 cellAtracSetDataAndGetMemSize(u32 pHandle, u32 pucBufferAddr, u32 uiReadByte,
                                  u32 uiBufferByte, u32 puiWorkMemByte)
{
    if (!pHandle || !pucBufferAddr) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    AtracSlot* s = slot_of(pHandle);
    if (!s) s = slot_of(0);
    if (!s) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    AtracTrackInfo t;
    const u8* file = vm_base + pucBufferAddr;
    int rc = atrac_parse_riff(file, uiReadByte, &t);
    if (rc) {
        fprintf(stderr, "[cellAtrac] SetData: not an ATRAC3plus file (0x%08X)\n", (u32)rc);
        return rc;
    }
    if (uiReadByte < t.data_off + t.data_bytes)
        fprintf(stderr, "[cellAtrac] SetData: %u of %u bytes on memory -- STREAMING is "
                        "not implemented; this track will decode garbage\n",
                uiReadByte, t.data_off + t.data_bytes);
    if (s->dec) at3p_close(s->dec);
    memset(s, 0, sizeof(*s));
    s->handle_ea = pHandle;
    s->file = file;
    s->t = t;
    s->pos = t.first;
    s->next_frame = 0;         /* start of the stream, like the console */
    s->pcm_frame = -1;
    (void)uiBufferByte;
    if (puiWorkMemByte) vm_write32(puiWorkMemByte, 0x1000);
    fprintf(stderr, "[cellAtrac] SetData handle=0x%08X ch=%u frames=%u first=%d end=%d loop=[%d,%d)\n",
            pHandle, t.channels, t.data_bytes / t.block_align, t.first, t.end,
            t.loop_start, t.loop_end);
    return CELL_OK;
}

s32 cellAtracCreateDecoder(u32 pHandle, u32 pucWorkMem, u32 uiPpuThreadPriority,
                           u32 uiSpuThreadPriority)
{
    (void)pucWorkMem; (void)uiPpuThreadPriority; (void)uiSpuThreadPriority;
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->dec) return (s32)CELL_ATRAC_ERROR_DECODER_WAS_CREATED;
    int ba = (int)s->t.block_align;
    s->dec = at3p_open((int)s->t.channels, &ba);
    return s->dec ? CELL_OK : (s32)CELL_ATRAC_ERROR_API_FAIL;
}

s32 cellAtracCreateDecoderExt(u32 pHandle, u32 pucWorkMem, u32 uiPpuThreadPriority,
                              u32 pExtRes)
{
    (void)pExtRes;
    return cellAtracCreateDecoder(pHandle, pucWorkMem, uiPpuThreadPriority, 0);
}

s32 cellAtracDeleteDecoder(u32 pHandle)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->dec) at3p_close(s->dec);
    memset(s, 0, sizeof(*s));
    return CELL_OK;
}

/* One call returns at most one frame's worth of samples per channel,
 * interleaved big-endian floats. */
s32 cellAtracDecode(u32 pHandle, u32 pfOutAddr, u32 puiSamples, u32 puiFinishflag,
                    u32 piRemainFrame)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (!s->dec) return (s32)CELL_ATRAC_ERROR_NO_DECODER;
    if (puiSamples) vm_write32(puiSamples, 0);
    if (s->pos >= (looping(s) ? s->t.loop_end : s->t.end)) {
        if (!looping(s)) {
            if (puiFinishflag) vm_write32(puiFinishflag, 1);
            return (s32)CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED;
        }
        s->pos = s->t.loop_start;
        if (s->loop_num > 0) s->loop_num--;
    }
    const s32 limit = looping(s) ? s->t.loop_end : s->t.end;
    const s32 k = s->pos / AT3P_SAMPLES, at = s->pos - k * AT3P_SAMPLES;
    s32 n = AT3P_SAMPLES - at;
    if (n > limit - s->pos) n = limit - s->pos;
    decode_frame(s, k);
    const u32 ch = s->t.channels;
    for (s32 i = 0; i < n; i++)
        for (u32 c = 0; c < ch; c++) {
            u32 bits;
            memcpy(&bits, &s->pcm[c][at + i], 4);
            vm_write32(pfOutAddr + ((u32)i * ch + c) * 4, bits);
        }
    s->pos += n;
    if (puiSamples) vm_write32(puiSamples, (u32)n);
    if (puiFinishflag) vm_write32(puiFinishflag, !looping(s) && s->pos >= s->t.end);
    if (piRemainFrame) vm_write32(piRemainFrame, (u32)CELL_ATRAC_ALLDATA_IS_ON_MEMORY);
    return CELL_OK;
}

s32 cellAtracGetRemainFrame(u32 pHandle, u32 piRemainFrame)
{
    if (!slot_of(pHandle)) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    vm_write32(piRemainFrame, (u32)CELL_ATRAC_ALLDATA_IS_ON_MEMORY);
    return CELL_OK;
}

s32 cellAtracGetChannel(u32 pHandle, u32 puiChannel)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    vm_write32(puiChannel, s->t.channels);
    return CELL_OK;
}

s32 cellAtracGetMaxSample(u32 pHandle, u32 puiMaxSample)
{
    if (!slot_of(pHandle)) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    vm_write32(puiMaxSample, AT3P_SAMPLES);
    return CELL_OK;
}

/* The API counts samples from the first audible one. */
s32 cellAtracGetNextSample(u32 pHandle, u32 puiNextSample)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const s32 k = s->pos / AT3P_SAMPLES;
    vm_write32(puiNextSample, (u32)(AT3P_SAMPLES - (s->pos - k * AT3P_SAMPLES)));
    return CELL_OK;
}

s32 cellAtracGetSoundInfo(u32 pHandle, u32 piEndSample, u32 piLoopStartSample,
                          u32 piLoopEndSample)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const AtracTrackInfo* t = &s->t;
    vm_write32(piEndSample, (u32)(t->end - t->first - 1));
    vm_write32(piLoopStartSample, (u32)(t->loop_start < 0 ? -1 : t->loop_start - t->first));
    vm_write32(piLoopEndSample, (u32)(t->loop_end < 0 ? -1 : t->loop_end - t->first - 1));
    return CELL_OK;
}

s32 cellAtracGetNextDecodePosition(u32 pHandle, u32 puiSamplePosition)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->pos >= s->t.end) return (s32)CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED;
    vm_write32(puiSamplePosition, (u32)(s->pos - s->t.first));
    return CELL_OK;
}

s32 cellAtracGetBitrate(u32 pHandle, u32 puiBitrate)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    /* kbps: block_align bytes per 2048 samples at 48 kHz. */
    vm_write32(puiBitrate, s->t.block_align * 8u * 48000u / AT3P_SAMPLES / 1000u);
    return CELL_OK;
}

s32 cellAtracGetLoopInfo(u32 pHandle, u32 piLoopNum, u32 puiLoopStatus)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    vm_write32(piLoopNum, (u32)s->loop_num);
    vm_write32(puiLoopStatus, s->t.loop_start >= 0);
    return CELL_OK;
}

s32 cellAtracSetLoopNum(u32 pHandle, s32 iLoopNum)
{
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    if (s->t.loop_start < 0 && iLoopNum != 0) return (s32)CELL_ATRAC_ERROR_UNSET_LOOP_NUM;
    s->loop_num = iLoopNum;
    return CELL_OK;
}

s32 cellAtracResetPlayPosition(u32 pHandle, u32 uiSample, u32 uiWriteByte)
{
    (void)uiWriteByte;
    AtracSlot* s = slot_of(pHandle);
    if (!s) return (s32)CELL_ATRAC_ERROR_UNSET_DATA;
    const s32 p = s->t.first + (s32)uiSample;
    if (p >= s->t.end) return (s32)CELL_ATRAC_ERROR_ILLEGAL_SAMPLE;
    s->pos = p;
    return CELL_OK;
}
