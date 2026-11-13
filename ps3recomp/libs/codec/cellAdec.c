/*
 * ps3recomp - cellAdec HLE implementation
 *
 * AC3 decodes through libavcodec when the runtime is built with
 * PS3RECOMP_FFMPEG_RUNTIME and the library is present (adec_ffmpeg.c,
 * ffmpeg_runtime.c): every AU is decoded, each block is laid out the way the
 * StartSeq parameters ask (word size, output mode, LFE, channel order),
 * queued, and announced with PCMOUT; GetPcmItem / GetPcm hand the queue out
 * oldest first. Every other codec type, and every build or run without
 * FFmpeg, keeps the silence-only decoder this always was: AUDONE and PCMOUT
 * per AU, and a block of zeros.
 *
 * The AU's bytes and its CellAdecAuInfo words are read before AUDONE: AUDONE
 * hands the AU back to the title's demuxer, which may refill it (and reuse the
 * info block) at once.
 */

#include "cellAdec.h"
#include "adec_ffmpeg.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include <math.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/ppu/ppu_context.h" /* g_active_ctx -> the guest lr */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cbFunc is a GUEST OPD */
#include "../../runtime/memory/vm.h"     /* VM_HLE_INJECT_BASE */
#include "ps3emu/log_first.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK s_adec_lock = SRWLOCK_INIT;
#define ADEC_LOCK()   AcquireSRWLockExclusive(&s_adec_lock)
#define ADEC_UNLOCK() ReleaseSRWLockExclusive(&s_adec_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_adec_lock = PTHREAD_MUTEX_INITIALIZER;
#define ADEC_LOCK()   pthread_mutex_lock(&s_adec_lock)
#define ADEC_UNLOCK() pthread_mutex_unlock(&s_adec_lock)
#endif

/* CellAdecPcmItem as the guest reads it, from RPCS3 Modules/cellAdec.h:339.
 * ps1_netemu copies the whole thing out with six 8-byte loads from offsets
 * 0x00..0x28 (at 0x000ED964), and that copy is what confirms these offsets
 * rather than a guess about padding: auInfo lands at 0x18, not 0x14, because it
 * contains a u64. The block must be 0x30 readable GUEST bytes. */
#define PCMITEM_PCM_HANDLE   0x00u
#define PCMITEM_STATUS       0x04u
#define PCMITEM_START_ADDR   0x08u
#define PCMITEM_SIZE         0x0Cu
#define PCMITEM_BSI_INFO     0x10u
#define PCMITEM_AU_START     0x18u
#define PCMITEM_AU_SIZE      0x1Cu
#define PCMITEM_AU_PTS_HI    0x20u
#define PCMITEM_AU_PTS_LO    0x24u
#define PCMITEM_AU_USERDATA  0x28u
#define PCMITEM_BYTES        0x30u

/* The silence-only decoder's block: 1024 stereo float samples, 8 KB, whatever
 * the codec. Resistance: Fall of Man's movie player sizes its ring from the
 * bsiInfo pcmSize this reports, so the value stays exactly what it was. */
#define ADEC_STUB_PCM_BYTES  (1024u * 2u * 4u)

/* Guest-visible scratch, in the HLE inject window rather than from the guest's
 * own heap. Taking 8 KB out of that heap in cellAdecOpen stalled the title
 * before it even reached cellAdecStartSeq -- ps1_netemu allocates its own
 * buffers from the same bump allocator, and perturbing it is not worth it for
 * blocks whose lifetime is the whole process.
 *
 * +0x100000..+0x500000 is clear of everything else in the window: labels
 * +0x0000, control +0x2000, callback +0x2F00, the offset tables +0x3000 /
 * +0x5000, sys_rsx's device/driver-info/reports pages at +0x30000 / +0x31000 /
 * +0x38000, cellVdec's pic items at +0x50000. One MB per decoder: ADEC_SLOTS
 * blocks of 64 KB, each the 0x30-byte PcmItem at +0, the bsiInfo it points to
 * at +0x80 and the PCM at +0x100 (an AC3 block is at most 1536 x 6 x 4 =
 * 36 KB). */
#define ADEC_SLOTS          16u
#define ADEC_SLOT_STRIDE    0x10000u
#define ADEC_BASE_EA(h)     (VM_HLE_INJECT_BASE + 0x100000u + (u32)(h) * 0x100000u)
#define ADEC_ITEM_EA(h, s)  (ADEC_BASE_EA(h) + (u32)(s) * ADEC_SLOT_STRIDE)
#define ADEC_BSI_EA(h, s)   (ADEC_ITEM_EA(h, s) + 0x80u)
#define ADEC_BSI_BYTES      0x80u
#define ADEC_PCM_EA(h, s)   (ADEC_ITEM_EA(h, s) + 0x100u)
#define ADEC_PCM_MAX        (ADEC_SLOT_STRIDE - 0x100u)

/* One AC3 block is 1536 samples: 2880 ticks of the 90 kHz clock. */
#define ADEC_AC3_PTS_STEP   2880u
#define CODEC_TS_INVALID    0xFFFFFFFFu

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_ADEC 4

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;         /* guest EA of the callback's OPD */
    u32 cbArg;          /* guest EA handed back to it     */
    int seqStarted;
    AdecFf* ff;         /* NULL: the silence-only decoder */
    AdecLayout layout;

    /* Decoded blocks, oldest first: q[(head + i) % ADEC_SLOTS] for i < count
     * is a slot index; itemed[slot] says GetPcmItem already handed it out. */
    u32 q[ADEC_SLOTS];
    u32 head, count;
    u8  itemed[ADEC_SLOTS];
    u32 next_slot;

    /* PTS interpolation for AUs that carry none (RPCS3 correct_pts_value). */
    u32 prev_pts_hi, prev_pts_lo;
    int prev_pts_valid;

    /* Diagnostics */
    u64 aus, frames, errors, dropped, pcm_bytes, taken;
    double acc_sq;      /* sum of squares of the decoded L/R since the last report */
    u64 acc_n;
    float acc_peak;
    double sp_sq[ADEC_SP_COUNT];   /* per decoded speaker, same interval */
} AdecSlot;

static AdecSlot s_adec[MAX_ADEC];

/* Fire one guest callback.
 *
 * The guest's cbFunc is a guest EA naming an OPD, so it goes through the
 * recompiled code's dispatcher. It used to be cast to a host function pointer
 * and called directly, which jumped to the guest address as host code:
 *
 *     [CRASH] code=0xC0000005 rip=00000000001B5F00
 *     [CRASH] last HLE NID (cellAdecDecodeAu)
 *
 * 0x1B5F00 is the guest OPD. That crash is what this function exists to stop.
 *
 * ponytail: dispatched SYNCHRONOUSLY on the caller's thread rather than from a
 * decoder thread as lv2 does, and never under s_adec_lock: a title's PCMOUT
 * handler may call GetPcmItem right away. The ceiling is a guest callback that
 * blocks waiting on the thread that called DecodeAu -- give this its own
 * thread if a title ever deadlocks here. */
static void adec_notify(CellAdecHandle handle, u32 msg_type, s32 msg_data)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) return;
    const u32 cb = s_adec[handle].cbFunc, arg = s_adec[handle].cbArg;
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          printf("[cellAdec] notify h=%u msg=%u data=0x%X cb=0x%08X arg=0x%08X caller=%d\n",
                 handle, msg_type, (unsigned)msg_data, cb, arg,
                 g_ps3_guest_caller ? 1 : 0); }
    if (!cb || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(cb, (u64)handle, (u64)msg_type,
                       (u64)(s64)msg_data, (u64)arg, 0, 0, 0, 0);
}

/* One line of decode statistics (and reset the level meter). Caller holds
 * the lock. */
static void adec_report(CellAdecHandle h, const char* why)
{
    AdecSlot* a = &s_adec[h];
    const double rms = a->acc_n ? sqrt(a->acc_sq / (double)a->acc_n) : 0.0;
    printf("[cellAdec] h=%u %s: codecType=%u %s au=%llu blocks=%llu errors=%llu "
           "dropped=%llu taken=%llu pcm=%llu KB, level rms %.4f peak %.4f\n",
           h, why, a->codecType, a->ff ? "libavcodec" : "silence",
           (unsigned long long)a->aus, (unsigned long long)a->frames,
           (unsigned long long)a->errors, (unsigned long long)a->dropped,
           (unsigned long long)a->taken, (unsigned long long)(a->pcm_bytes >> 10),
           rms, a->acc_peak);
    if (a->ff && a->acc_n) {
        static const char* nm[ADEC_SP_COUNT] = { "L", "R", "C", "LFE", "Ls", "Rs" };
        printf("[cellAdec] h=%u decoded speakers:", h);
        for (int i = 0; i < ADEC_SP_COUNT; i++)
            printf(" %s %.4f", nm[i], sqrt(a->sp_sq[i] / (double)(a->acc_n / 2)));
        printf("\n");
    }
    memset(a->sp_sq, 0, sizeof a->sp_sq);
    a->acc_sq = 0.0;
    a->acc_n = 0;
    a->acc_peak = 0.0f;
}

/* ---------------------------------------------------------------------------
 * The PCM queue (caller holds the lock)
 * -----------------------------------------------------------------------*/

/* A free slot for the next block. When the title has let every slot fill, the
 * oldest block is dropped: a stalled consumer loses audio rather than wedging
 * the demuxer thread that calls DecodeAu. */
static u32 adec_take_slot(AdecSlot* a)
{
    if (a->count == ADEC_SLOTS) {
        a->head = (a->head + 1) % ADEC_SLOTS;
        a->count--;
        a->dropped++;
        { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            printf("[cellAdec] PCM queue full (%u blocks): the oldest is dropped\n", ADEC_SLOTS); }
    }
    for (u32 tries = 0; tries < ADEC_SLOTS; tries++) {
        const u32 s = (a->next_slot + tries) % ADEC_SLOTS;
        int used = 0;
        for (u32 i = 0; i < a->count; i++)
            if (a->q[(a->head + i) % ADEC_SLOTS] == s) { used = 1; break; }
        if (!used) {
            a->next_slot = (s + 1) % ADEC_SLOTS;
            return s;
        }
    }
    return 0;   /* unreachable: count < ADEC_SLOTS leaves one free */
}

static void adec_push(AdecSlot* a, u32 slot)
{
    a->q[(a->head + a->count) % ADEC_SLOTS] = slot;
    a->count++;
    a->itemed[slot] = 0;
}

/* The AU's timestamp for the block that comes out of it: its own, or the
 * previous block's plus one block when the title gave none. */
static void adec_block_pts(AdecSlot* a, u32 hi, u32 lo, u32* out_hi, u32* out_lo)
{
    if (hi == CODEC_TS_INVALID && lo == CODEC_TS_INVALID && a->prev_pts_valid) {
        u32 nlo = a->prev_pts_lo + ADEC_AC3_PTS_STEP;
        hi = nlo < a->prev_pts_lo ? ((a->prev_pts_hi + 1) & 1) : a->prev_pts_hi;
        lo = nlo;
    }
    if (hi != CODEC_TS_INVALID && lo != CODEC_TS_INVALID) {
        a->prev_pts_hi = hi;
        a->prev_pts_lo = lo;
        a->prev_pts_valid = 1;
    }
    *out_hi = hi;
    *out_lo = lo;
}

/* The AU's header fields, for the AC3 bitstream info block. */
typedef struct {
    int valid;
    u8 fscod, frmsizecod, bsid, bsmod, acmod, cmixlev, surmixlev, dsurmod, lfeon;
} Ac3Header;

static void ac3_parse_header(const u8* p, u32 n, Ac3Header* h)
{
    memset(h, 0, sizeof(*h));
    if (n < 8 || p[0] != 0x0B || p[1] != 0x77) return;
    h->fscod = p[4] >> 6;
    h->frmsizecod = p[4] & 0x3F;
    h->bsid = p[5] >> 3;
    h->bsmod = p[5] & 7;
    /* acmod(3), then cmixlev / surmixlev / dsurmod when present, then lfeon. */
    u32 bits = ((u32)p[6] << 8) | p[7];
    int pos = 16;
    #define AC3_TAKE(nb) ((pos -= (nb)), (u8)((bits >> pos) & ((1u << (nb)) - 1u)))
    h->acmod = AC3_TAKE(3);
    if ((h->acmod & 1) && h->acmod != 1) h->cmixlev = AC3_TAKE(2);
    if (h->acmod & 4) h->surmixlev = AC3_TAKE(2);
    if (h->acmod == 2) h->dsurmod = AC3_TAKE(2);
    h->lfeon = AC3_TAKE(1);
    #undef AC3_TAKE
    h->valid = 1;
}

/* CellAdecBsiAc3 (RPCS3 Modules/cellAdec.h):
 *   +0x00 codecType +0x04 versionInfo +0x08 totalCallCarryRun +0x0C totalCallNum
 *   +0x10 bitrateValue +0x14 pcmSize +0x18 esSizeBit +0x1C errorCode
 *   +0x20 u8 libInfoFlag, validity, channelValue, fsIndex, outputQuantization,
 *         outputChannel, monoFlag
 *   +0x28 u32 bsi[2]  +0x30 u16 frmSizeCod  +0x32 u8 acmod, lfeOn, karaokeMode,
 *         cmixlev, surmixlev, dsurmod, copyright, original, bsmod, bsid, ... */
static void adec_write_bsi_ac3(u32 bsi, const AdecSlot* a, const Ac3Header* hd,
                               u32 pcm_size, u32 au_size, u32 bit_rate)
{
    static const u16 kbps[19] = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
                                  192, 224, 256, 320, 384, 448, 512, 576, 640 };
    vm_write32(bsi + 0x00, a->codecType);
    vm_write32(bsi + 0x0C, (u32)a->frames);
    vm_write32(bsi + 0x10, hd->valid && hd->frmsizecod / 2 < 19
                               ? kbps[hd->frmsizecod / 2] * 1000u : bit_rate);
    vm_write32(bsi + 0x14, pcm_size);
    vm_write32(bsi + 0x18, au_size * 8u);
    vm_write32(bsi + 0x1C, 0);
    vm_write8 (bsi + 0x21, (u8)hd->valid);
    vm_write8 (bsi + 0x22, (u8)a->layout.channels);
    vm_write8 (bsi + 0x23, hd->fscod);
    vm_write8 (bsi + 0x24, (u8)(a->layout.word_float ? 1 : 0));
    vm_write8 (bsi + 0x26, (u8)(hd->acmod == 1));
    vm_write16(bsi + 0x30, hd->frmsizecod);
    vm_write8 (bsi + 0x32, hd->acmod);
    vm_write8 (bsi + 0x33, hd->lfeon);
    vm_write8 (bsi + 0x35, hd->cmixlev);
    vm_write8 (bsi + 0x36, hd->surmixlev);
    vm_write8 (bsi + 0x37, hd->dsurmod);
    vm_write8 (bsi + 0x3A, hd->bsmod);
    vm_write8 (bsi + 0x3B, hd->bsid);
}

/* The CellAdecAuInfo words the PcmItem echoes, read before AUDONE. */
typedef struct {
    u32 start, size, userdata_hi, userdata_lo;
} AdecAuEcho;

/* Fill a slot's PcmItem. */
static void adec_write_item(CellAdecHandle h, u32 slot, u32 pcm_size, const AdecAuEcho* au,
                            u32 pts_hi, u32 pts_lo)
{
    const u32 it = ADEC_ITEM_EA(h, slot);
    vm_write32(it + PCMITEM_PCM_HANDLE, slot);
    vm_write32(it + PCMITEM_STATUS,     0);              /* CELL_OK */
    vm_write32(it + PCMITEM_START_ADDR, ADEC_PCM_EA(h, slot));
    vm_write32(it + PCMITEM_SIZE,       pcm_size);
    /* bsiInfo must point at a codec info block, not be null. Resistance:
     * Fall of Man sizes each AC3 block from bsiInfo+0x14; with a null
     * pointer that read address 0x14 and came back 0, so its ring buffer
     * never filled, it never started its audio port, and its movie player
     * waited forever. */
    vm_write32(it + PCMITEM_BSI_INFO,   ADEC_BSI_EA(h, slot));
    /* Echo the AU back, which is what a real decoder does -- the guest
     * matches returned PCM against the AU it submitted. */
    vm_write32(it + PCMITEM_AU_START,   au->start);
    vm_write32(it + PCMITEM_AU_SIZE,    au->size);
    vm_write32(it + PCMITEM_AU_PTS_HI,  pts_hi);
    vm_write32(it + PCMITEM_AU_PTS_LO,  pts_lo);
    vm_write32(it + PCMITEM_AU_USERDATA,     au->userdata_hi);
    vm_write32(it + PCMITEM_AU_USERDATA + 4, au->userdata_lo);
}

/* ---------------------------------------------------------------------------
 * Decoding one AU
 * -----------------------------------------------------------------------*/

typedef struct {
    CellAdecHandle h;
    u32 au_ea;          /* the CellAdecAuInfo */
    AdecAuEcho echo;    /* its words, read before AUDONE */
    u32 au_size;
    u32 pts_hi, pts_lo; /* the AU's own */
    Ac3Header hd;
    u32 emitted;
} AdecDecodeCtx;

/* One decoded block: into a slot, onto the queue. Runs under the lock. */
static void adec_emit_block(void* user, const AdecFfFrame* f)
{
    AdecDecodeCtx* c = (AdecDecodeCtx*)user;
    AdecSlot* a = &s_adec[c->h];
    AdecFfFrame fr = *f;
    u32 bytes = adec_pcm_bytes(&a->layout, fr.samples);
    if (bytes > ADEC_PCM_MAX) {
        fr.samples = ADEC_PCM_MAX / (a->layout.channels * (a->layout.word_float ? 4u : 2u));
        bytes = adec_pcm_bytes(&a->layout, fr.samples);
    }
    const u32 slot = adec_take_slot(a);
    adec_store_pcm(ADEC_PCM_EA(c->h, slot), &a->layout, &fr);

    u32 hi, lo;
    if (c->emitted == 0) adec_block_pts(a, c->pts_hi, c->pts_lo, &hi, &lo);
    else adec_block_pts(a, CODEC_TS_INVALID, CODEC_TS_INVALID, &hi, &lo);
    a->frames++;
    const u32 bsi = ADEC_BSI_EA(c->h, slot);
    for (u32 o = 0; o < ADEC_BSI_BYTES; o += 4) vm_write32(bsi + o, 0);
    adec_write_bsi_ac3(bsi, a, &c->hd, bytes, c->au_size, fr.bit_rate);
    adec_write_item(c->h, slot, bytes, &c->echo, hi, lo);
    adec_push(a, slot);
    a->pcm_bytes += bytes;
    c->emitted++;

    for (u32 s = 0; s < fr.samples; s++) {
        for (int k = 0; k < ADEC_SP_COUNT; k++) {
            const float v = fr.pcm[(size_t)s * ADEC_SP_COUNT + k];
            a->sp_sq[k] += (double)v * v;
        }
        const float l = fr.pcm[(size_t)s * ADEC_SP_COUNT + ADEC_SP_L];
        const float r = fr.pcm[(size_t)s * ADEC_SP_COUNT + ADEC_SP_R];
        a->acc_sq += (double)l * l + (double)r * r;
        const float al = fabsf(l), ar = fabsf(r);
        if (al > a->acc_peak) a->acc_peak = al;
        if (ar > a->acc_peak) a->acc_peak = ar;
    }
    a->acc_n += 2u * fr.samples;
}

/* A block of zeros, for the silence-only decoder and for an AU the decoder
 * rejected (the title expects one PCMOUT per AU and counts them). */
static void adec_emit_silence(AdecDecodeCtx* c, u32 bytes)
{
    AdecSlot* a = &s_adec[c->h];
    const u32 slot = adec_take_slot(a);
    const u32 pcm = ADEC_PCM_EA(c->h, slot);
    for (u32 o = 0; o < bytes; o += 4) vm_write32(pcm + o, 0);
    u32 hi, lo;
    adec_block_pts(a, c->pts_hi, c->pts_lo, &hi, &lo);
    a->frames++;
    const u32 bsi = ADEC_BSI_EA(c->h, slot);
    for (u32 o = 0; o < ADEC_BSI_BYTES; o += 4) vm_write32(bsi + o, 0);
    if (a->ff) adec_write_bsi_ac3(bsi, a, &c->hd, bytes, c->au_size, 0);
    else vm_write32(bsi + 0x14, bytes);   /* pcmSize: all the stub ever filled */
    adec_write_item(c->h, slot, bytes, &c->echo, hi, lo);
    adec_push(a, slot);
    a->pcm_bytes += bytes;
    c->emitted++;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* cellAdecQueryAttr: how much work memory the decoder wants, before Open.
 * CellAdecAttr (guest, big-endian): +0 u32 workMemSize, +4 u32 adecVerUpper,
 * +8 u32 adecVerLower. Decoding is host-side and never touches the buffer;
 * see cellVdecQueryAttr. */
s32 cellAdecQueryAttr(const CellAdecType* type, void* attr)
{
    u32 ea = (u32)(uintptr_t)attr;
    if (!type || !ea)
        return (s32)CELL_ADEC_ERROR_ARG;
    vm_write32(ea + 0x0, 0x40000);    /* workMemSize: 256 KB */
    vm_write32(ea + 0x4, 0x00010000); /* adecVerUpper */
    vm_write32(ea + 0x8, 0);          /* adecVerLower */
    return CELL_OK;
}

/* cellAdecOpen(type, res, cb, handle) -- FOUR arguments.
 *
 * This took five, splitting the guest's CellAdecCb struct into cbFunc + cbArg,
 * which pushed `handle` off r6 onto r7. r7 held whatever happened to be there, so
 * the null check failed and every Open returned CELL_ADEC_ERROR_ARG. ps1_netemu
 * opens a decoder for CD-DA/XA audio inside its CD-ROM constructor and gives up on
 * the error -- so the disc was never mounted and the emulator sat in its run loop
 * with nothing to run. Signature confirmed against RPCS3 (Modules/cellAdec.cpp).
 *
 * cb is a guest pointer to { u32 cbFunc; u32 cbArg; }. */
s32 cellAdecOpen(const CellAdecType* type, const CellAdecResource* res,
                 const CellAdecCb* cb, CellAdecHandle* handle)
{
    (void)res;

    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;   /* audioCodecType */
    u32 cb_ea      = (u32)(uintptr_t)cb;
    u32 handle_ea  = (u32)(uintptr_t)handle;
    printf("[cellAdec] Open(codecType=%u, cb=0x%08X, handle=0x%08X)\n",
           codec_type, cb_ea, handle_ea);

    if (!type || !handle_ea)
        return (s32)CELL_ADEC_ERROR_ARG;

    /* Outside the lock: the first open loads FFmpeg. */
    AdecFf* ff = adec_ff_open(codec_type);

    ADEC_LOCK();
    for (int i = 0; i < MAX_ADEC; i++) {
        if (!s_adec[i].in_use) {
            AdecSlot* a = &s_adec[i];
            memset(a, 0, sizeof(AdecSlot));
            a->in_use    = 1;
            a->codecType = codec_type;
            a->ff        = ff;
            adec_ac3_layout(NULL, &a->layout);   /* 2.0 float until StartSeq */
            if (cb_ea) {
                a->cbFunc = vm_read32(cb_ea + 0);
                a->cbArg  = vm_read32(cb_ea + 4);
            }
            ADEC_UNLOCK();
            /* The PcmItem is handed to the guest BY POINTER, so it cannot live
             * in host memory -- returning &host_struct is what crashed here
             * once already. */
            vm_write32(handle_ea, (u32)i);
            printf("[cellAdec] Open -> handle=%u items=0x%08X decoder=%s\n",
                   i, ADEC_BASE_EA(i), ff ? "libavcodec" : "silence");
            return CELL_OK;
        }
    }
    ADEC_UNLOCK();
    adec_ff_close(ff);
    return (s32)CELL_ADEC_ERROR_BUSY;
}

s32 cellAdecClose(CellAdecHandle handle)
{
    printf("[cellAdec] Close(handle=%u)\n", handle);

    ADEC_LOCK();
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) {
        ADEC_UNLOCK();
        return (s32)CELL_ADEC_ERROR_ARG;
    }
    adec_report(handle, "close");
    AdecFf* ff = s_adec[handle].ff;
    s_adec[handle].ff = NULL;
    s_adec[handle].in_use = 0;
    ADEC_UNLOCK();
    adec_ff_close(ff);
    return CELL_OK;
}

/* param: the codec's parameter block (CellAdecParamAc3 for AC3). */
s32 cellAdecStartSeq(CellAdecHandle handle, void* param)
{
    const u32 pea = GUEST_EA(param);
    u8 p[0x30];
    if (pea) vm_memcpy_from(p, pea, sizeof p);
    {
        char hex[0x30 * 2 + 1];
        for (u32 i = 0; i < (pea ? sizeof p : 0u); i++) snprintf(hex + i * 2, 3, "%02X", p[i]);
        if (!pea) hex[0] = 0;
        printf("[cellAdec] StartSeq(handle=%u param=0x%08X %s)\n", handle, pea, hex);
    }

    ADEC_LOCK();
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) {
        ADEC_UNLOCK();
        return (s32)CELL_ADEC_ERROR_ARG;
    }
    AdecSlot* a = &s_adec[handle];
    a->seqStarted = 1;
    a->head = a->count = 0;
    a->prev_pts_valid = 0;
    if (a->ff) {
        adec_ff_reset(a->ff);
        if (a->codecType == CELL_ADEC_TYPE_AC3 && adec_ac3_layout(pea ? p : NULL, &a->layout) < 0)
            printf("[cellAdec] StartSeq: AC3 parameters not understood; 2.0 float\n");
        printf("[cellAdec] StartSeq: %u channel(s) %s, speakers", a->layout.channels,
               a->layout.word_float ? "float" : "s16");
        static const char* names[ADEC_SP_COUNT] = { "L", "R", "C", "LFE", "Ls", "Rs" };
        for (u32 i = 0; i < a->layout.channels; i++) printf(" %s", names[a->layout.out_sp[i]]);
        printf(", scale %.3f\n", a->layout.scale);
    }
    ADEC_UNLOCK();
    return CELL_OK;
}

s32 cellAdecEndSeq(CellAdecHandle handle)
{
    /* ADEC_WHO=1: name the guest function driving this. The title calls EndSeq
     * hundreds of times against a single StartSeq, and the image has eight
     * EndSeq call sites -- the backtrace says which one. */
    { static int _n = -1;
      if (_n < 0) _n = getenv("ADEC_WHO") ? 0 : -2;
      if (_n >= 0 && _n < 8) { _n++;
          /* The guest lr, not a host backtrace. ppu_guest_caller maps host
           * frames to the nearest lifted function, and here it landed on
           * func_00013040 -- a single `blr`, so plainly a mis-attribution.
           * lr is written by the `bl` to the import stub, so it is exactly
           * (call site + 4) for a call like this one. */
          extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
          printf("[cellAdec] EndSeq from guest lr=0x%08X tid=%llu\n",
                 g_active_ctx ? (u32)g_active_ctx->lr : 0u,
                 g_active_ctx ? (unsigned long long)g_active_ctx->thread_id : 0ull); } }
    printf("[cellAdec] EndSeq(handle=%u)\n", handle);

    ADEC_LOCK();
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) {
        ADEC_UNLOCK();
        return (s32)CELL_ADEC_ERROR_ARG;
    }
    s_adec[handle].seqStarted = 0;
    if (s_adec[handle].aus) adec_report(handle, "end of sequence");
    ADEC_UNLOCK();

    adec_notify(handle, CELL_ADEC_MSG_TYPE_SEQDONE, CELL_OK);

    return CELL_OK;
}

s32 cellAdecDecodeAu(CellAdecHandle handle, const CellAdecAuInfo* auInfo)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecDecodeCtx c;
    memset(&c, 0, sizeof c);
    c.h = handle;
    c.au_ea = GUEST_EA(auInfo);
    const u32 au_addr = vm_read32(c.au_ea + (u32)offsetof(CellAdecAuInfo, startAddr));
    c.au_size = vm_read32(c.au_ea + (u32)offsetof(CellAdecAuInfo, size));
    c.pts_hi = vm_read32(c.au_ea + 0x08);
    c.pts_lo = vm_read32(c.au_ea + 0x0C);
    c.echo.start       = au_addr;
    c.echo.size        = c.au_size;
    c.echo.userdata_hi = vm_read32(c.au_ea + 0x10);
    c.echo.userdata_lo = vm_read32(c.au_ea + 0x14);

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 8))
        printf("[cellAdec] DecodeAu(handle=%u, addr=0x%X, size=%u, pts=%08X:%08X)\n",
               handle, au_addr, c.au_size, c.pts_hi, c.pts_lo); }

    /* The AU's bytes are copied out before AUDONE, too: AUDONE hands their
     * buffer back to the title's demuxer, which may refill it at once. */
    u8 stack_au[4096];
    u8* au = NULL;
    const u32 n = c.au_size > 0x100000u ? 0x100000u : c.au_size;
    ADEC_LOCK();
    const int decode = s_adec[handle].in_use && s_adec[handle].ff != NULL;
    ADEC_UNLOCK();
    if (decode && au_addr && n) {
        au = n <= sizeof stack_au ? stack_au : (u8*)malloc(n);
        if (au) vm_memcpy_from(au, au_addr, n);
    }

    /* Step 1: report the AU consumed. msgData is the AU INFO ADDRESS, not a
     * status -- see RPCS3 Modules/cellAdec.cpp:1345. A decoder that tracks its
     * outstanding AUs by address gets a null back if this is CELL_OK. */
    adec_notify(handle, CELL_ADEC_MSG_TYPE_AUDONE, (s32)c.au_ea);

    /* Step 2: decode into the PCM queue. */
    ADEC_LOCK();
    AdecSlot* a = &s_adec[handle];
    if (!a->in_use) {
        ADEC_UNLOCK();
        if (au && au != stack_au) free(au);
        return (s32)CELL_ADEC_ERROR_ARG;
    }
    a->aus++;
    if (a->ff && au) {
        if (a->codecType == CELL_ADEC_TYPE_AC3) ac3_parse_header(au, n, &c.hd);
        const int r = adec_ff_decode(a->ff, au, n, adec_emit_block, &c);
        if (r < 0) {
            a->errors++;
            { static int _n = 0; if (PS3_LOG_FIRST(_n, 8))
                printf("[cellAdec] AU %llu (size %u, starts %02X %02X) did not decode (%d)\n",
                       (unsigned long long)a->aus, n, au[0], n > 1 ? au[1] : 0, r); }
        }
        /* One PCMOUT per AU, which is what the title counts: a rejected AU
         * gets a block of silence the size a good one would have had. */
        if (c.emitted == 0)
            adec_emit_silence(&c, adec_pcm_bytes(&a->layout, 1536u));
    } else if (a->ff) {
        /* No AU bytes to decode (none given, or no memory for the copy). */
        adec_emit_silence(&c, adec_pcm_bytes(&a->layout, 1536u));
    } else {
        adec_emit_silence(&c, ADEC_STUB_PCM_BYTES);
    }
    if (au && au != stack_au) free(au);
    /* Every 2000 AUs (about a minute of AC3); every 156 (5 s, the cellAudio
     * PS3_AUDIO_STATS interval) when PS3_AUDIO_STATS is set. */
    { static int every = 0;
      if (!every) every = getenv("PS3_AUDIO_STATS") ? 156 : 2000;
      if ((a->aus % (u64)every) == 0) adec_report(handle, "decoding"); }
    const u32 emitted = c.emitted;
    ADEC_UNLOCK();

    for (u32 i = 0; i < emitted; i++)
        adec_notify(handle, CELL_ADEC_MSG_TYPE_PCMOUT, CELL_OK);

    return CELL_OK;
}

/* GetPcm: the oldest block, copied into the title's buffer (outBuffer may be
 * null to just release it), and its slot freed. */
s32 cellAdecGetPcm(CellAdecHandle handle, void* outBuffer)
{
    const u32 out = GUEST_EA(outBuffer);
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 6))
        printf("[cellAdec] GetPcm(handle=%u out=0x%08X)\n", handle, out); }

    ADEC_LOCK();
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) {
        ADEC_UNLOCK();
        return (s32)CELL_ADEC_ERROR_ARG;
    }
    AdecSlot* a = &s_adec[handle];
    if (a->count == 0) {
        ADEC_UNLOCK();
        return (s32)CELL_ADEC_ERROR_EMPTY;
    }
    const u32 slot = a->q[a->head];
    a->head = (a->head + 1) % ADEC_SLOTS;
    a->count--;
    a->taken++;
    const u32 it = ADEC_ITEM_EA(handle, slot);
    const u32 src = vm_read32(it + PCMITEM_START_ADDR);
    u32 size = vm_read32(it + PCMITEM_SIZE);
    if (size > ADEC_PCM_MAX) size = ADEC_PCM_MAX;
    /* outBuffer is the GUEST's buffer. Leaving it untouched hands the title
     * whatever happened to be there, which it then plays. */
    if (out && size) {
        u8* tmp = (u8*)malloc(size);
        if (tmp) {
            vm_memcpy_from(tmp, src, size);
            vm_memcpy_to(out, tmp, size);
            free(tmp);
        }
    }
    ADEC_UNLOCK();
    return CELL_OK;
}

/* GetPcmItem: the oldest block GetPcmItem has not handed out yet. */
s32 cellAdecGetPcmItem(CellAdecHandle handle, const CellAdecPcmItem** pcmItem)
{
    ADEC_LOCK();
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) {
        ADEC_UNLOCK();
        return (s32)CELL_ADEC_ERROR_ARG;
    }
    AdecSlot* a = &s_adec[handle];
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 6))
        printf("[cellAdec] GetPcmItem(handle=%u out=0x%08X queued=%u)\n",
               handle, GUEST_EA(pcmItem), a->count); }
    if (!pcmItem) {
        ADEC_UNLOCK();
        return (s32)CELL_ADEC_ERROR_FATAL;
    }
    for (u32 i = 0; i < a->count; i++) {
        const u32 slot = a->q[(a->head + i) % ADEC_SLOTS];
        if (a->itemed[slot]) continue;
        a->itemed[slot] = 1;
        /* pcmItem is a GUEST CellAdecPcmItem**. Writing &host_struct here put a
         * HOST address into guest memory; the guest dereferenced it and died on a
         * wild address (rip 0x870D0E, fault 0xCFFDFFF0). Write the guest EA. */
        vm_write32(GUEST_EA(pcmItem), ADEC_ITEM_EA(handle, slot));
        ADEC_UNLOCK();
        return CELL_OK;
    }
    ADEC_UNLOCK();
    return (s32)CELL_ADEC_ERROR_EMPTY;
}
