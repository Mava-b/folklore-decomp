// Guest setjmp/longjmp carried out on the host stack (runtime/ppu/ppu_sjlj.c).
//
// The guest program below is written the way tools/ppu_lifter.py lifts code
// with --sjlj: a newlib-style setjmp/longjmp pair (the longjmp body ends in
// ppu_sjlj_longjmp), a Lua-style "protected call" whose call to the setjmp is
// a PPU_SJLJ_ENTER site, a "throw" that longjmps to the innermost protected
// call, and bodies the protected call reaches through ps3_indirect_call.
//
//   1. a body that returns: status 0, nothing jumps
//   2. a throw 40 indirect calls deep: caught; r1, r2, r14-r31, CR and LR are
//      the protected call's again and the dispatcher's nesting counts are back
//   3. nested protected calls: the inner catches the inner throw, the outer
//      the outer one
//   4. one jmp_buf (one frame address) reused 2000 times: no slot leak
//   5. a throw through a jmp_buf whose protected call has returned: the
//      longjmp returns (the behaviour without host support) and is counted
//   6. a throw from a second guest context (an HLE callback's) to a protected
//      call made on the first: refused the same way
//   7. a throw from inside an HLE call that runs guest code on the caller's
//      context: refused the same way (the HLE frame's guards must run)
//
// Built by tools/test_ppu_sjlj.py against the real ppu_loader.cpp.
#include "ppu_recomp.h"
#include "ppu_sjlj.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
const func_entry function_table[1] = {{0, nullptr, nullptr}};   // registered by hand below
const uint64_t function_table_count = 0;
uint32_t ps3_hle_count(void) { return 0; }
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
void ps3_indirect_call(ppu_context* ctx);
int* ps3_ppu_dispatch_depth_slot(void);
int* ps3_ppu_vcall_sp_slot(void);
// ppu_hle.cpp's (not linked here)
static int s_hle_depth;
int* ps3_hle_depth_slot(void) { return &s_hle_depth; }
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
}

#define DRAIN_TRAMPOLINE(ctx) do { while (g_trampoline_fn) { void (*_tf)(void*) = g_trampoline_fn; \
    g_trampoline_fn = 0; _tf((void*)(ctx)); } } while (0)

// Guest addresses (code addresses are only keys for ps3_indirect_call).
static const uint32_t A_SETJMP = 0x10000, A_LONGJMP = 0x10100, A_PROTECT = 0x10200,
                      A_THROW = 0x10300, A_RETURN_OK = 0x10400, A_DEEP = 0x10500,
                      A_NESTED = 0x10600, A_INNER = 0x10700, A_CALLBACK = 0x10800,
                      A_IN_HLE = 0x10900;
static const uint32_t RET_PROTECT_SETJMP = A_PROTECT + 0x24;   // return address of its setjmp call
static const uint32_t STATE = 0x40000;   // "lua_State": +0 errorJmp, +8 depth arg, +0x10 code
static const uint32_t STACK_TOP = 0xF0000;

// struct longjmp_frame { u32 prev; pad; jmp_buf b (0xC0) at +0x10; u32 status at +0xD0 }
enum { LJ_PREV = 0, LJ_BUF = 0x10, LJ_STATUS = 0xD0, LJ_SIZE = 0xE0 };

static unsigned s_panics, s_bodies_after_throw;

// ---- newlib-style setjmp / longjmp (jmp_buf: r1 r2 r14..r31 lr cr) -----------
static void g_setjmp(ppu_context* ctx)
{
    ctx->gpr[3] = (ctx->gpr[3] + 15) & ~15ull;
    vm_write64(ctx->gpr[3] + 0x0, ctx->gpr[1]);
    vm_write64(ctx->gpr[3] + 0x8, ctx->gpr[2]);
    for (int r = 14; r < 32; r++) vm_write64(ctx->gpr[3] + 0x10 + 8 * (r - 14), ctx->gpr[r]);
    vm_write64(ctx->gpr[3] + 0xA0, ctx->lr);
    vm_write64(ctx->gpr[3] + 0xA8, ctx->cr);
    ctx->gpr[3] = 0;
}

static void g_longjmp(ppu_context* ctx)
{
    const uint64_t _sjlj_buf = ctx->gpr[3];
    ctx->gpr[3] = (ctx->gpr[3] + 15) & ~15ull;
    ctx->gpr[1] = vm_read64(ctx->gpr[3] + 0x0);
    ctx->gpr[2] = vm_read64(ctx->gpr[3] + 0x8);
    for (int r = 14; r < 32; r++) ctx->gpr[r] = vm_read64(ctx->gpr[3] + 0x10 + 8 * (r - 14));
    ctx->lr = vm_read64(ctx->gpr[3] + 0xA0);
    ctx->cr = (uint32_t)vm_read64(ctx->gpr[3] + 0xA8);
    ctx->gpr[3] = ctx->gpr[4] ? ctx->gpr[4] : 1;
    ppu_sjlj_longjmp(ctx, _sjlj_buf); return;
}

// ---- protect(L = r3, body = r4) -> r3 = status ------------------------------
// Its exits call ppu_sjlj_leave, as the lifter emits them for a body with a
// setjmp site.
static void g_protect(ppu_context* ctx)
{
    uint64_t _cs_31 = ctx->gpr[31];
    vm_write64(ctx->gpr[1] - 0x200, ctx->gpr[1]); ctx->gpr[1] -= 0x200;
    vm_write64(ctx->gpr[1] + 0x210, ctx->lr);
    vm_write64(ctx->gpr[1] + 0x1F8, ctx->gpr[31]);
    ctx->gpr[31] = ctx->gpr[3];                                 // L
    vm_write64(ctx->gpr[1] + 0x1F0, ctx->gpr[4]);               // body
    const uint64_t lj = ctx->gpr[1] + 0x70;
    vm_write32(lj + LJ_PREV, vm_read32(ctx->gpr[31]));
    vm_write32(lj + LJ_STATUS, 0);
    vm_write32(ctx->gpr[31], (uint32_t)lj);                     // L->errorJmp = &lj
    ctx->gpr[3] = lj + LJ_BUF;
    ctx->lr = RET_PROTECT_SETJMP; if (PPU_SJLJ_ENTER(ctx, RET_PROTECT_SETJMP) == 0) g_setjmp(ctx); DRAIN_TRAMPOLINE(ctx);
    if (ctx->gpr[3] == 0) {
        ctx->ctr = vm_read64(ctx->gpr[1] + 0x1F0);
        ctx->gpr[3] = ctx->gpr[31];
        ctx->lr = A_PROTECT + 0x40; ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
    }
    const uint64_t lj2 = ctx->gpr[1] + 0x70;
    vm_write32(ctx->gpr[31], vm_read32(lj2 + LJ_PREV));         // L->errorJmp = lj.prev
    ctx->gpr[3] = vm_read32(lj2 + LJ_STATUS);
    ctx->lr = vm_read64(ctx->gpr[1] + 0x210);
    ctx->gpr[1] += 0x200;
    ctx->gpr[31] = _cs_31;
    { ppu_sjlj_leave(PPU_SJLJ_FRAME()); return; }
    ppu_sjlj_leave(PPU_SJLJ_FRAME());  /* sjlj: end of body */
}

// ---- throw(L = r3, code = r4) -----------------------------------------------
static void g_throw(ppu_context* ctx)
{
    const uint32_t lj = vm_read32(ctx->gpr[3]);
    if (lj) {
        vm_write32(lj + LJ_STATUS, (uint32_t)ctx->gpr[4]);
        ctx->gpr[3] = lj + LJ_BUF;
        ctx->gpr[4] = 1;
        ctx->lr = A_THROW + 0x20; g_longjmp(ctx); DRAIN_TRAMPOLINE(ctx);
    }
    ++s_panics;   // luaD_throw's no-handler path: G(L)->panic(L); exit(1)
}

static void g_return_ok(ppu_context* ctx) { ctx->gpr[3] = 0; }

// deep(L): recurse STATE+8 levels through ps3_indirect_call with its own frames
// and clobbered non-volatiles, then throw code 2.
static void g_deep(ppu_context* ctx)
{
    vm_write64(ctx->gpr[1] - 0x80, ctx->gpr[1]); ctx->gpr[1] -= 0x80;
    for (int r = 14; r < 32; r++) ctx->gpr[r] = 0xDEAD0000u + r;
    ctx->cr = 0x12345678u;
    ctx->gpr[2] = 0x7777;
    const uint32_t left = vm_read32(STATE + 8);
    if (left) {
        vm_write32(STATE + 8, left - 1);
        ctx->gpr[3] = STATE;
        ctx->ctr = A_DEEP;
        ctx->lr = A_DEEP + 0x30; ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
    } else {
        ctx->gpr[3] = STATE; ctx->gpr[4] = 2;
        ctx->lr = A_DEEP + 0x40; g_throw(ctx); DRAIN_TRAMPOLINE(ctx);
    }
    ++s_bodies_after_throw;      // never reached when the throw is carried out
    ctx->gpr[1] += 0x80;
}

static uint32_t s_inner_status;
static void g_inner(ppu_context* ctx)
{
    ctx->gpr[3] = STATE; ctx->gpr[4] = 2;
    ctx->lr = A_INNER + 0x10; g_throw(ctx); DRAIN_TRAMPOLINE(ctx);
    ++s_bodies_after_throw;
}

// nested(L): protect(L, inner) must give 2, then throw 3 to the outer protect
static void g_nested(ppu_context* ctx)
{
    vm_write64(ctx->gpr[1] - 0x80, ctx->gpr[1]); ctx->gpr[1] -= 0x80;
    ctx->gpr[3] = STATE; ctx->gpr[4] = A_INNER;
    ctx->lr = A_NESTED + 0x10; g_protect(ctx); DRAIN_TRAMPOLINE(ctx);
    s_inner_status = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = STATE; ctx->gpr[4] = 3;
    ctx->lr = A_NESTED + 0x20; g_throw(ctx); DRAIN_TRAMPOLINE(ctx);
    ++s_bodies_after_throw;
    ctx->gpr[1] += 0x80;
}

// callback body: throws from a second context, the way an HLE callback would
static void g_callback(ppu_context* ctx)
{
    ppu_context cb;
    std::memset(&cb, 0, sizeof cb);
    cb.gpr[1] = 0x80000;
    cb.gpr[3] = STATE; cb.gpr[4] = 4;
    ppu_context* saved = g_active_ctx;
    g_active_ctx = &cb;
    g_throw(&cb);
    g_active_ctx = saved;
    ctx->gpr[3] = 0;
}

// an HLE handler that runs guest code on the caller's context (ps3_hle_call
// counts its nesting), and the guest code throws
static void g_in_hle(ppu_context* ctx)
{
    ++*ps3_hle_depth_slot();
    ctx->gpr[3] = STATE; ctx->gpr[4] = 6;
    const ppu_context before = *ctx;
    g_throw(ctx);
    *ctx = before;   // the refused longjmp's body restored the protected call's registers
    --*ps3_hle_depth_slot();
    ctx->gpr[3] = 0;
}

static unsigned s_fails;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    s_fails += !ok;
}

static uint32_t run_protect(ppu_context* ctx, uint32_t body)
{
    ctx->gpr[3] = STATE; ctx->gpr[4] = body;
    ctx->lr = 0x10F00; g_protect(ctx); DRAIN_TRAMPOLINE(ctx);
    return (uint32_t)ctx->gpr[3];
}

int main()
{
    vm_base = (uint8_t*)std::calloc(1, 0x100000);
    ppu_register_function(A_RETURN_OK, g_return_ok);
    ppu_register_function(A_DEEP, g_deep);
    ppu_register_function(A_NESTED, g_nested);
    ppu_register_function(A_INNER, g_inner);
    ppu_register_function(A_CALLBACK, g_callback);
    ppu_register_function(A_IN_HLE, g_in_hle);

    static ppu_context ctx;
    ctx.gpr[1] = STACK_TOP;
    ctx.gpr[2] = 0x1234;
    for (int r = 14; r < 32; r++) ctx.gpr[r] = 0xC0DE0000u + r;
    ctx.cr = 0x22220000u;
    g_active_ctx = &ctx;
    ppu_sjlj_stats st0, st;
    ppu_sjlj_get_stats(&st0);

    // 1
    uint32_t status = run_protect(&ctx, A_RETURN_OK);
    ppu_sjlj_get_stats(&st);
    check(status == 0 && st.jumps == st0.jumps && st.enters == st0.enters + 1,
          "a body that returns: status 0, no jump");

    // 2
    vm_write32(STATE + 8, 40);
    status = run_protect(&ctx, A_DEEP);
    ppu_sjlj_get_stats(&st);
    bool regs = ctx.gpr[1] == STACK_TOP && ctx.gpr[2] == 0x1234 && ctx.cr == 0x22220000u;
    for (int r = 14; r < 32; r++) regs = regs && ctx.gpr[r] == 0xC0DE0000u + r;
    check(status == 2 && st.jumps == st0.jumps + 1 && s_bodies_after_throw == 0,
          "a throw 40 indirect calls deep is caught by the protected call (status 2)");
    check(regs, "r1, r2, r14-r31 and CR are the caller's again");
    check(*ps3_ppu_dispatch_depth_slot() == 0 && *ps3_ppu_vcall_sp_slot() == 0,
          "the dispatcher's nesting counts are back to 0 after jumping over 41 frames");
    check(g_trampoline_fn == nullptr && g_active_ctx == &ctx, "trampoline cleared, active context kept");
    check(vm_read32(STATE) == 0, "the protected call unlinked its handler");

    // 3
    status = run_protect(&ctx, A_NESTED);
    check(s_inner_status == 2 && status == 3 && s_bodies_after_throw == 0,
          "nested protected calls: inner caught 2, outer caught 3");

    // 4
    unsigned caught = 0;
    for (int i = 0; i < 2000; i++) {
        vm_write32(STATE + 8, (uint32_t)(i % 7));
        caught += run_protect(&ctx, A_DEEP) == 2;
        if (i % 3 == 0) caught += run_protect(&ctx, A_RETURN_OK) == 0 ? 0 : 1000000;
    }
    ppu_sjlj_get_stats(&st);
    check(caught == 2000, "2000 throws through one reused jmp_buf are all caught");
    check(st.live <= 2, "no setjmp slot leaks (live slots after the loop <= 2)");
    char msg[96];
    std::snprintf(msg, sizeof msg, "  (live slots %u, setjmp sites entered %llu, jumps %llu)",
                  st.live, (unsigned long long)st.enters, (unsigned long long)st.jumps);
    std::puts(msg);

    // 5: a stale handler: the protected call that set it up has returned
    const uint64_t stale = STACK_TOP - 0x200 + 0x70;
    vm_write32(STATE, (uint32_t)stale);
    const unsigned panics0 = s_panics;
    const uint64_t fb0 = st.fallbacks;
    const ppu_context before = ctx;
    ctx.gpr[3] = STATE; ctx.gpr[4] = 5;
    g_throw(&ctx);
    ctx = before;   // the guest longjmp body did restore the stale registers
    ppu_sjlj_get_stats(&st);
    check(s_panics == panics0 + 1 && st.fallbacks == fb0 + 1,
          "a longjmp to a returned setjmp frame returns (old behaviour) and is counted");
    vm_write32(STATE, 0);

    // 6
    status = run_protect(&ctx, A_CALLBACK);
    ppu_sjlj_get_stats(&st);
    // (status 4: the throw stored its code before its longjmp was refused)
    check(status == 4 && s_panics == panics0 + 2 && st.fallbacks == fb0 + 2,
          "a longjmp from another guest context (an HLE callback) is refused");
    check(g_active_ctx == &ctx && ctx.gpr[1] == STACK_TOP, "the first context is intact");

    // 7
    status = run_protect(&ctx, A_IN_HLE);
    ppu_sjlj_get_stats(&st);
    check(status == 6 && s_panics == panics0 + 3 && st.fallbacks == fb0 + 3 &&
          *ps3_ppu_dispatch_depth_slot() == 0,
          "a longjmp over an HLE frame (guest code run on the same context) is refused");

    return s_fails ? 1 : 0;
}
