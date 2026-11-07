/*
 * ps3recomp - a title's own setjmp/longjmp, carried out on the host stack.
 * See ppu_sjlj.h for the scheme; this file keeps the host side of it.
 *
 * Per host thread there is a small table of slots, one per guest setjmp whose
 * caller is still running: the guest jmp_buf address it was made with, the
 * host jmp_buf the rewritten call site setjmp'd into, the host frame of that
 * caller, and the guest state a longjmp to it must arrive with (the guest
 * context, r1, the return address). The lifted caller frees its slots on each
 * exit (ppu_sjlj_leave); a longjmp frees those of the frames it skips. Host
 * stacks grow down, so a slot whose frame is below the frame now running
 * belongs to a call that is gone: each new setjmp drops those as well.
 *
 * A host longjmp skips every frame between the longjmp and the setjmp site.
 * Lifted functions own nothing that needs a destructor, but the runtime's
 * indirect-call dispatcher counts its nesting per thread (the recursion guard
 * and the vcall stack), and a frame of it that is skipped never takes its
 * count back down; those counts are restored to their value at the setjmp.
 * g_trampoline_fn is cleared (a skipped drain loop could have left it set)
 * and g_active_ctx is put back to what it was at the setjmp.
 *
 * A longjmp is only carried out when the slot for its jmp_buf was made on the
 * same guest context (an HLE -> guest callback runs on a context of its own,
 * and jumping out of it would skip the HLE code that called it) at the same
 * HLE call nesting (an HLE handler that runs guest code on the caller's
 * context has guards a jump would skip), its host frame is above the current
 * one, and the guest r1/LR the longjmp body just restored are the ones the
 * setjmp was made with. Anything else returns to the lifted longjmp, as
 * before this support existed, after a log line.
 *
 * Environment:
 *   PPU_SJLJ=0      never jump (every longjmp returns, as without --sjlj)
 *   PPU_SJLJ_LOG=1  log every setjmp-site jump (first 64, then every 4096th)
 */
#include "ppu_sjlj.h"
#include "ppu_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's own ppu_context: same layout as the one the lifter's
 * HEADER_PREAMBLE writes into ppu_recomp.h, which this file does not need. */
#include "ppu_context.h"

extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
int* ps3_ppu_dispatch_depth_slot(void);
int* ps3_ppu_vcall_sp_slot(void);
int* ps3_hle_depth_slot(void);

#define SJLJ_SLOTS 128

typedef struct sjlj_slot {
    jmp_buf      jb;
    uintptr_t    frame;    /* host frame of the function that made the setjmp */
    ppu_context* ctx;      /* guest context it was made on                     */
    ppu_context* active;   /* g_active_ctx then                                */
    uint32_t     buf;      /* guest jmp_buf (r3 at the call)                   */
    uint32_t     r1;       /* guest r1 at the call                             */
    uint32_t     ret;      /* guest return address of the call                 */
    int          depth;    /* indirect-call nesting then                       */
    int          vsp;      /* vcall stack pointer then                         */
    int          hle;      /* HLE call nesting then                            */
    int          used;
} sjlj_slot;

typedef struct sjlj_thread {
    sjlj_slot slot[SJLJ_SLOTS];
    int       top;         /* slots [0, top) may be in use */
    int       live;
    jmp_buf   scratch;     /* handed out when no slot is free; never jumped to */
} sjlj_thread;

static PPU_THREAD_LOCAL sjlj_thread* t_sjlj;

static volatile uint64_t s_enters, s_jumps, s_fallbacks;

static int sjlj_env(const char* name, int dflt)
{
    const char* e = getenv(name);
    return e && *e ? atoi(e) : dflt;
}

static int sjlj_enabled(void)
{
    static int on = -1;
    if (on < 0) on = sjlj_env("PPU_SJLJ", 1) != 0;
    return on;
}

static int sjlj_log_on(void)
{
    static int on = -1;
    if (on < 0) on = sjlj_env("PPU_SJLJ_LOG", 0) != 0;
    return on;
}

static sjlj_thread* sjlj_self(void)
{
    if (!t_sjlj) t_sjlj = (sjlj_thread*)calloc(1, sizeof(sjlj_thread));
    return t_sjlj;
}

static void sjlj_free(sjlj_thread* t, sjlj_slot* s)
{
    s->used = 0;
    t->live--;
}

static void sjlj_trim(sjlj_thread* t)
{
    while (t->top > 0 && !t->slot[t->top - 1].used) t->top--;
}

/* Drop the slots of frames below `frame` (they have returned, or are being
 * unwound). */
static void sjlj_drop_below(sjlj_thread* t, uintptr_t frame)
{
    for (int i = 0; i < t->top; i++) {
        sjlj_slot* s = &t->slot[i];
        if (s->used && s->frame < frame) sjlj_free(t, s);
    }
    sjlj_trim(t);
}

jmp_buf* ppu_sjlj_enter(struct ppu_context* ctx, uint32_t ret_addr, void* host_frame)
{
    sjlj_thread* t = sjlj_self();
    static jmp_buf s_oom_scratch;   /* calloc failed: shared, never jumped to */
    if (!t) return &s_oom_scratch;
    __atomic_fetch_add(&s_enters, 1, __ATOMIC_RELAXED);
    if (!sjlj_enabled()) return &t->scratch;

    const uintptr_t frame = (uintptr_t)host_frame;
    const uint32_t buf = (uint32_t)ctx->gpr[3];
    sjlj_slot* hit = NULL;
    sjlj_slot* free_slot = NULL;
    for (int i = 0; i < t->top; i++) {
        sjlj_slot* s = &t->slot[i];
        if (s->used && s->frame < frame) sjlj_free(t, s);  /* returned */
        if (!s->used) { if (!free_slot) free_slot = s; continue; }
        if (s->buf == buf) hit = s;   /* the guest reuses this jmp_buf */
    }
    sjlj_slot* s = hit ? hit : free_slot;
    if (!s && t->top < SJLJ_SLOTS) s = &t->slot[t->top++];
    if (!s) {
        static int n = 0;
        if (n < 8) {
            n++;
            fprintf(stderr, "[sjlj] %d setjmp frames live on this thread; the setjmp "
                            "returning to 0x%08X (jmp_buf 0x%08X) gets no host context\n",
                    SJLJ_SLOTS, ret_addr, buf);
        }
        return &t->scratch;
    }
    if (!s->used) {
        s->used = 1;
        t->live++;
    }
    s->frame  = frame;
    s->ctx    = ctx;
    s->active = g_active_ctx;
    s->buf    = buf;
    s->r1     = (uint32_t)ctx->gpr[1];
    s->ret    = ret_addr;
    s->depth  = *ps3_ppu_dispatch_depth_slot();
    s->vsp    = *ps3_ppu_vcall_sp_slot();
    s->hle    = *ps3_hle_depth_slot();
    return &s->jb;
}

void ppu_sjlj_leave(void* host_frame)
{
    sjlj_thread* t = t_sjlj;
    if (!t || !t->live) return;
    const uintptr_t frame = (uintptr_t)host_frame;
    for (int i = 0; i < t->top; i++) {
        sjlj_slot* s = &t->slot[i];
        if (s->used && s->frame <= frame) sjlj_free(t, s);
    }
    sjlj_trim(t);
}

static void sjlj_fallback(struct ppu_context* ctx, uint32_t buf, const char* why)
{
    uint64_t n = __atomic_add_fetch(&s_fallbacks, 1, __ATOMIC_RELAXED);
    if (n <= 16 || (n & 1023) == 0)
        fprintf(stderr, "[sjlj] longjmp(jmp_buf 0x%08X) not carried out: %s "
                        "(r1=0x%08X lr=0x%08X tid=%llu, #%llu); the longjmp returns\n",
                buf, why, (uint32_t)ctx->gpr[1], (uint32_t)ctx->lr,
                (unsigned long long)ctx->thread_id, (unsigned long long)n);
}

void ppu_sjlj_longjmp(struct ppu_context* ctx, uint64_t buf64)
{
    const uint32_t buf = (uint32_t)buf64;
    sjlj_thread* t = t_sjlj;
    if (!sjlj_enabled()) { sjlj_fallback(ctx, buf, "PPU_SJLJ=0"); return; }
    sjlj_slot* s = NULL;
    if (t)
        for (int i = 0; i < t->top; i++)
            if (t->slot[i].used && t->slot[i].buf == buf) { s = &t->slot[i]; break; }
    if (!s) { sjlj_fallback(ctx, buf, "no setjmp made with it on this thread"); return; }
    if (s->ctx != ctx) {
        sjlj_fallback(ctx, buf, "its setjmp was made on another guest context "
                                "(jumping out of an HLE callback)");
        return;
    }
    if (*ps3_hle_depth_slot() != s->hle) {
        sjlj_fallback(ctx, buf, "it would jump over an HLE call that runs guest code");
        return;
    }
    if ((uintptr_t)PPU_SJLJ_FRAME() >= s->frame) {
        sjlj_free(t, s);
        sjlj_trim(t);
        sjlj_fallback(ctx, buf, "the function that made the setjmp has returned");
        return;
    }
    if ((uint32_t)ctx->gpr[1] != s->r1 || (uint32_t)ctx->lr != s->ret) {
        char why[160];
        snprintf(why, sizeof why, "the jmp_buf holds r1=0x%08X lr=0x%08X, its setjmp "
                                  "was made with r1=0x%08X returning to 0x%08X",
                 (uint32_t)ctx->gpr[1], (uint32_t)ctx->lr, s->r1, s->ret);
        sjlj_fallback(ctx, buf, why);
        return;
    }
    sjlj_drop_below(t, s->frame);

    *ps3_ppu_dispatch_depth_slot() = s->depth;
    *ps3_ppu_vcall_sp_slot() = s->vsp;
    g_trampoline_fn = 0;
    g_active_ctx = s->active;

    uint64_t n = __atomic_add_fetch(&s_jumps, 1, __ATOMIC_RELAXED);
    if (sjlj_log_on() && (n <= 64 || (n & 4095) == 0))
        fprintf(stderr, "[sjlj] longjmp #%llu: jmp_buf 0x%08X -> setjmp returning to "
                        "0x%08X, value %lld (r1=0x%08X tid=%llu)\n",
                (unsigned long long)n, buf, s->ret, (long long)ctx->gpr[3], s->r1,
                (unsigned long long)ctx->thread_id);
    PPU_SJLJ_HOST_LONGJMP(s->jb, 1);
}

void ppu_sjlj_get_stats(ppu_sjlj_stats* out)
{
    out->enters = s_enters;
    out->jumps = s_jumps;
    out->fallbacks = s_fallbacks;
    out->live = t_sjlj ? (uint32_t)t_sjlj->live : 0;
}
