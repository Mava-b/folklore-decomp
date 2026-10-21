/*
 * spurs_trace.h -- a "golden" trace of everything SPURS does, for writing and
 * checking a replacement for Sony's libsre.
 *
 * SPURS_GOLDEN_TRACE=<file> turns it on. One text line per event:
 *
 *   - every call the game makes into libsre (NID, name, r3..r10, the result),
 *     with the memory behind each pointer argument before and after the call;
 *   - every DMA and lock-line atomic an SPU issues, with who issued it (the
 *     SPURS kernel, a policy module such as the job-chain manager, or a job),
 *     the address, size, a content hash, and the bytes themselves the first
 *     time a given (address, size, content) is seen;
 *   - every entry into and exit from a job's code, with its registers and the
 *     job context and descriptor it was handed.
 *
 * SPURS_GOLDEN_TRACE_MODE=shape (the default) writes each distinct kind of
 * event once and counts the rest, small enough for a whole play session;
 * =full writes every event (minutes of play fill gigabytes). Off, the cost is
 * one load and a branch per hook. SPURS_GOLDEN_TRACE_MB caps the file
 * (default 1024 MB in shape mode, 4096 MB in full).
 */
#ifndef PS3RECOMP_SPURS_TRACE_H
#define PS3RECOMP_SPURS_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

/* Non-zero while the trace is on (cheap to test before calling a hook). */
extern volatile int g_spurs_trace_on;

/* Read SPURS_GOLDEN_TRACE once; returns g_spurs_trace_on. */
int spurs_trace_init(void);

/* PPU: a call into libsre, before it runs (arguments in gpr[3..10]). */
void spurs_trace_api_call(uint32_t nid, const uint64_t* gpr, uint32_t lr);

/* PPU: the same call after it returned. argumentGpr holds r0..r10 as they
 * were before the call (for the "after" memory dump), result the new r3. */
void spurs_trace_api_return(uint32_t nid, const uint64_t* argumentGpr, uint32_t result);

/* SPU: a completed GET (cmd & 0xF0 == 0x40) or PUT (0x20) of `size` bytes
 * between LS `lsa` and guest `ea`. */
void spurs_trace_dma(struct spu_context* ctx, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size);

/* SPU: a lock-line atomic (GETLLAR / PUTLLC / PUTLLUC) after it ran. */
void spurs_trace_atomic(struct spu_context* ctx, uint32_t cmd, uint32_t ea, uint32_t lsa, uint32_t atomicStat);

/* SPU: an indirect branch resolved to code owned by `codeImage` (a job's
 * placed binary; 0 = anything else). Records job entry and exit. */
void spurs_trace_branch(struct spu_context* ctx, int codeImage);

/* SPU: a write to the outbound mailbox (channel 28) or the outbound
 * interrupt mailbox (30) -- how an SPU sends an event or message to the PPU. */
void spurs_trace_channel(struct spu_context* ctx, uint32_t channel, uint32_t value);

/* Anything else worth a line (free text, already formatted). */
void spurs_trace_note(const char* text);

#ifdef __cplusplus
}
#endif

#endif
