/*
 * ps3recomp - a title's own setjmp/longjmp, carried out on the host stack
 *
 * A title that links a libc setjmp/longjmp (newlib's, in a PS3 SDK title)
 * cannot use it as lifted code: the guest longjmp restores r1, r2, the
 * non-volatile GPRs/FPRs/VRs, CR and LR from the jmp_buf and "returns" from
 * the setjmp call a second time -- but a static recompilation runs every guest
 * function as a host function, so restoring the guest registers unwinds no
 * host frame. The lifted longjmp just returns to its caller, which carries on
 * as if the jump had happened. Lua's error handling is the common victim:
 * luaD_throw longjmps to the luaD_rawrunprotected that pcall made, and when
 * the longjmp returns it falls into the "no handler" path (panic, exit(1)).
 *
 * The lifter's --sjlj option names the title's setjmp and longjmp functions.
 * Each direct call to the setjmp becomes
 *
 *     if (PPU_SJLJ_ENTER(ctx, <return address>) == 0) func_<setjmp>(ctx);
 *
 * a HOST setjmp in the caller's own frame, into a host jmp_buf this runtime
 * keeps per host thread, keyed by the guest jmp_buf address (r3), before the
 * guest setjmp body saves the guest registers as usual. The longjmp's body
 * runs as lifted (it restores the guest registers and computes the return
 * value) and then calls ppu_sjlj_longjmp, which host-longjmps to the matching
 * setjmp site: the call there "returns" a second time, with the guest state
 * the jmp_buf held and r3 = the longjmp value. Every exit of a function with
 * setjmp sites calls ppu_sjlj_leave, so the runtime knows when a setjmp stops
 * being a valid target. When there is no matching live
 * site (the setjmp was not one of the rewritten calls, its frame has
 * returned, it was made on another guest context -- e.g. outside the HLE
 * callback the longjmp runs in), ppu_sjlj_longjmp logs it and returns, which
 * is the behaviour without this support.
 *
 * Generated code includes this header only where it uses it, so a lift
 * without --sjlj does not change.
 */
#ifndef PS3RECOMP_PPU_SJLJ_H
#define PS3RECOMP_PPU_SJLJ_H

#include <setjmp.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ppu_context;

/* The host jmp_buf for the guest setjmp the caller is about to make with
 * jmp_buf = ctx->gpr[3]. ret_addr is the guest return address of the call (the
 * LR the guest setjmp saves), host_frame the frame address of the lifted
 * function making the call. Never NULL: without a free slot it is a scratch
 * buffer no longjmp is ever sent to. */
jmp_buf* ppu_sjlj_enter(struct ppu_context* ctx, uint32_t ret_addr, void* host_frame);

/* Called on every exit of a lifted function that has setjmp sites (with
 * PPU_SJLJ_FRAME() of that function): its setjmps, and any made deeper, are
 * no longer valid targets. */
void ppu_sjlj_leave(void* host_frame);

/* Called by the lifted guest longjmp after it restored the guest registers
 * from the jmp_buf at `buf` (r3 on entry). Does not return when the matching
 * setjmp site is live; returns (after a log line) when it is not. */
void ppu_sjlj_longjmp(struct ppu_context* ctx, uint64_t buf);

/* Counters for tests and diagnostics. */
typedef struct ppu_sjlj_stats {
    uint64_t enters;      /* setjmp sites entered                         */
    uint64_t jumps;       /* longjmps carried out                         */
    uint64_t fallbacks;   /* longjmps with no live site (returned instead) */
    uint32_t live;        /* slots in use on this host thread             */
} ppu_sjlj_stats;
void ppu_sjlj_get_stats(ppu_sjlj_stats* out);

#ifdef __cplusplus
}
#endif

/* No signal-mask save: the guest does not expect one, and it is a syscall on
 * macOS. */
#if defined(_WIN32)
#  define PPU_SJLJ_HOST_SETJMP(jb)     setjmp(jb)
#  define PPU_SJLJ_HOST_LONGJMP(jb, v) longjmp((jb), (v))
#else
#  define PPU_SJLJ_HOST_SETJMP(jb)     _setjmp(jb)
#  define PPU_SJLJ_HOST_LONGJMP(jb, v) _longjmp((jb), (v))
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
#  define PPU_SJLJ_FRAME() ((void*)_AddressOfReturnAddress())
#else
#  define PPU_SJLJ_FRAME() __builtin_frame_address(0)
#endif

/* The setjmp has to be invoked in the frame that later resumes, so this is a
 * macro used as the whole controlling expression of an `if` (one of the forms
 * C allows setjmp in): `if (PPU_SJLJ_ENTER(ctx, ret) == 0) <guest setjmp>`. */
#define PPU_SJLJ_ENTER(ctx, ret) \
    PPU_SJLJ_HOST_SETJMP(*ppu_sjlj_enter((ctx), (ret), PPU_SJLJ_FRAME()))

#endif /* PS3RECOMP_PPU_SJLJ_H */
