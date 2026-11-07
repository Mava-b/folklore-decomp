/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * An SPU that issues GETLLAR holds a reservation on a 128-byte line and then
 * spins, waiting for someone else to change it. On real hardware a store by
 * ANY other processor to that line kills the reservation and raises
 * SPU_EVENT_LR on the reserving SPU, which is how a parked SPURS idle service
 * learns that the PPU has posted work. Emulated, the two halves live on
 * different host threads with nothing between them, so the SPU half alone gets
 * PUTLLC right (it compares the line before committing) while the PPU half
 * never tells anyone it wrote. The service sleeps through the signal.
 *
 * This is the missing half. Every line an SPU reserves is marked in a bitmap;
 * the PPU store paths (runtime/ppu/ppu_memory.h and runtime/ppu/ppu_loader.cpp)
 * check the bitmap, and on a hit commit the store under the same lock-line lock
 * the SPU's GETLLAR/PUTLLC transaction takes, then notify. That does two things
 * at once: the store can no longer land inside a PUTLLC's compare/commit window
 * (where it would be silently reverted), and the reserving SPU gets its event.
 *
 * The bitmap check is a shift and a load against a global that stays zero until
 * an SPU actually reserves something, so a title that never runs SPU code pays
 * one predictable branch per store.
 */

#ifndef SPU_COHERENCY_H
#define SPU_COHERENCY_H

#include "spu_context.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU external event: lock-line reservation lost. */
#define SPU_EVENT_LR  0x400u

/* The lock-line lock. One process-wide spinlock serializing every lock-line
 * transaction, SPU and PPU alike: the SPU's GETLLAR snapshot and PUTLLC
 * compare-and-commit, and a PPU store or stwcx to a line some SPU has
 * reserved. The critical sections are two 128-byte memcpy and a memcmp. */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);

/* Mark `ea`'s 128-byte line as SPU-reserved and remember `ctx` as a possible
 * reservation holder. Called from the SPU's GETLLAR with the lock held.
 *
 * Lines are marked and never unmarked. A line the SPURS kernel reserves is
 * reserved again microseconds later, so unmarking would buy nothing; a stale
 * mark costs one locked PPU store to a line the kernel owns anyway. */
void spu_coh_reserve(spu_context* ctx, uint32_t ea);

/* GETLLAR fast path: publish ctx's reservation of ea WITHOUT the lock-line
 * lock, when ctx is already a registered holder and ea's line already marked.
 * Returns 0 when the caller must take the lock and call spu_coh_reserve
 * instead. On 1 the caller copies the line, also without the lock. */
int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea);

/* Is `addr`'s line marked? The PPU store fast path -- a range-free O(1) bitmap
 * lookup guarded by a global that is zero until the first GETLLAR. */
int spu_coh_is_reserved(uint32_t addr);

/* Fast-path table for the lifted code's inline loads and stores (see
 * ppu_recomp.h): one byte per 128-byte line, bit 0 = loads take the
 * out-of-line accessor, bit 1 = stores do. vm_slow_lines_mark marks every
 * line overlapping [lo, hi) plus the line in front of it. */
extern unsigned char g_vm_slow_lines[];
void vm_slow_lines_mark(uint64_t lo, uint64_t hi, unsigned bits);

/* A store by the PPU landed on `ea`'s line. Raise SPU_EVENT_LR on every SPU
 * whose live reservation covers it, drop that reservation, and wake the host
 * thread in case it is parked in `rdch SPU_RdEventStat`.
 *
 * Expects the lock-line lock held, so that the store this is reporting and the
 * reservation state it clears are seen by an SPU's PUTLLC as one transaction. */
void spu_coh_notify_write(uint32_t ea);
/* Remove a context from the reserving set. MUST be called before a
 * transient (stack-allocated) spu_context goes out of scope. */
void spu_coh_unregister(spu_context* ctx);
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi);  /* thread exit */
/* As above, but leaves `self`'s own reservation alone: an SPU's own MFC
 * write does not cost it its reservation on hardware. */
void spu_coh_notify_write_except(uint32_t ea, const void* self);

/* An SPU committed a write to `ea`'s 128-byte line (PUTLLC, PUTLLUC or a DMA
 * PUT). Every PPU lwarx/ldarx reservation inside that line is lost, as on
 * hardware, where the PPU reservation granule is the same 128-byte line.
 *
 * Without this a PPU store-conditional only compared its own word, so a
 * libsre update that reads a neighbouring field inside its reservation could
 * commit against a value an SPU had already changed. cellSpursShutdownWorkload
 * does exactly that: lwarx the workload state, read the workload's SPU status
 * byte 16 bytes away, then store SHUTTING_DOWN (status set) or REMOVABLE. If
 * the SPURS kernel cleared the status and went idle between the two, the PPU
 * still committed SHUTTING_DOWN, the kernel had already exited, and nobody
 * ever completed the shutdown: Folklore froze on its install screen with the
 * main thread waiting in cellSpursWaitForWorkloadShutdown.
 *
 * Call with the lock-line lock held. The PPU runtime installs the hook; with
 * no PPU runtime linked (SPU unit tests) this does nothing.
 *
 * Upstream now does the same from inside spu_coh_notify_write(_except), which
 * call ppu_resv_break_line (ppu_loader.cpp) before walking the SPU holders.
 * Every caller of this helper sits right after one of those notifiers, so it
 * is a second, idempotent break of the same line; kept so the Folklore call
 * sites and the loader's hook stay valid until they are retired together. */
extern void (*g_spu_break_ppu_resv_hook)(uint32_t line);
static inline void spu_coh_break_ppu(uint32_t ea)
{
    if (g_spu_break_ppu_resv_hook)
    {
        g_spu_break_ppu_resv_hook(ea & ~127u);
    }
}

/* Scoped host waiters for real writes to a guest 128-byte line. Add, remove,
 * and callbacks all run under the lock-line lock. A watch remains caller-owned
 * until remove returns; callbacks must not re-enter coherence or alter watches.
 * They may wake a host waiter, but must not manufacture guest state. */
typedef struct spu_coh_watch {
    uint32_t line;
    void (*wake)(void* opaque);
    void* opaque;
    struct spu_coh_watch* next;
} spu_coh_watch;
void spu_coh_watch_add(spu_coh_watch* watch, uint32_t ea,
                       void (*wake)(void*), void* opaque);
void spu_coh_watch_remove(spu_coh_watch* watch);

/* Count of LR events delivered. A parked SPURS service that never wakes with
 * this at zero is a coherence miss; nonzero moves the question downstream. */
extern unsigned long g_spu_lr_raise;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COHERENCY_H */
