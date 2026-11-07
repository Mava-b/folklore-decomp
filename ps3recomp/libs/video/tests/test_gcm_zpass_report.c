/* NV4097_GET_REPORT of type 1 (ZPASS_PIXEL_CNT) with a draw engine that
 * counts samples on the host GPU (cellGcmSys.c, the deferred report queue).
 *
 * The walker reaches a GET_REPORT long before the GPU has run the draws it
 * counts, so the report waits for the engine's count -- and every semaphore
 * release, back-end label and reference after it waits behind it, because on
 * the RSX they land after the report does. The engine is stubbed: the test
 * says when a ticket's count is ready.
 *
 *   - a measured report is stored only when its count is ready, with that
 *     count, never the old 0xFFFF;
 *   - an NV406E release, a back-end label (0x1D6C/0x1D70) and a reference
 *     behind it wait, and land after it, in FIFO order;
 *   - a release with nothing pending is written at once, as before;
 *   - the walker idle with a label queued behind a report kicks the engine
 *     (RSX_ZPASS_KICK), and an old enough report is forced (RSX_ZPASS_WAIT);
 *     a count still missing then reports the conservative 0xFFFF;
 *   - with host counting off the walker answers 0xFFFF at once, as before.
 *
 * clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_zpass_report.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_zpass_report && /tmp/test_gcm_zpass_report
 *   (gcc on Linux: -ffunction-sections -fdata-sections -Wl,--gc-sections)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }

#define REPORT_IO 0x00008000u            /* the report, in the IO-mapped ring */
#define REPORT_EA (IO_ADDR + REPORT_IO)
#define SEMA_OFF  0x10u
#define LABEL_EA  (GCM_LABEL_GUEST_BASE + SEMA_OFF)
#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u

/* ---- the stubbed engine -------------------------------------------------- */
static int eng_on = 1, host_counting = 1;
static u32 next_ticket = 100;
static int ticket_ready;          /* 1: counts are in; -1: lost */
static u32 ticket_count = 1234;
static int kicks, waits, polls;
int  rsx_draw_engine_enabled(void) { return eng_on; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_draw_engine_zpass_supported(void) { return host_counting; }
u32  rsx_draw_engine_zpass_report(void) { return host_counting ? next_ticket++ : 0; }
int  rsx_draw_engine_zpass_result(u32 ticket, int submit, u32* value)
{
    (void)ticket;
    if (submit == RSX_ZPASS_KICK) kicks++;
    else if (submit == RSX_ZPASS_WAIT) waits++;
    else polls++;
    if (ticket_ready < 0) return -1;
    if (!ticket_ready) return 0;
    *value = ticket_count;
    return 1;
}

/* The back-end label path of rsx_commands.c: records what reached it, and
 * whether the report had landed by then. */
static u32 bl_methods[8], bl_n, bl_report_at_call;
int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state; (void)data;
    if (method == 0x1D6Cu || method == 0x1D70u) {
        if (bl_n < 8) bl_methods[bl_n++] = method;
        if (method == 0x1D70u) bl_report_at_call = vm_read32(REPORT_EA + 8u);
    }
    return 0;
}

static uint32_t g_heap = 0x00400000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static u32 put_cmd(u32 io, u32 subch, u32 method, u32 count, const u32* args)
{
    gw32(IO_ADDR + io, (count << 18) | (subch << 13) | method);
    for (u32 i = 0; i < count; i++) gw32(IO_ADDR + io + 4 + i * 4, args[i]);
    return io + 4 + count * 4;
}
static u32 put1(u32 io, u32 method, u32 arg) { return put_cmd(io, 0, method, 1, &arg); }

static u32 s_ctrl, s_io;
static void run_to(u32 put)
{
    vm_write32(s_ctrl + 0, put);
    cellGcm_rsx_process_fifo();
}

/* GET_REPORT(ZPASS), then an NV406E release, a back-end label and a
 * reference: the frame-start sequence Resistance issues after its queries. */
static void emit_frame(u32 label_value)
{
    s_io = put1(s_io, 0x1A8u, 0xFEED0001u);                 /* SET_CONTEXT_DMA_REPORT: main */
    s_io = put1(s_io, 0x1800u, (1u << 24) | REPORT_IO);       /* GET_REPORT ZPASS_PIXEL_CNT */
    s_io = put1(s_io, 0x64u, SEMA_OFF);                      /* NV406E SEMAPHORE_OFFSET */
    s_io = put1(s_io, 0x6Cu, label_value);                   /* NV406E SEMAPHORE_RELEASE */
    s_io = put1(s_io, 0x1D6Cu, 0x20u);                       /* back-end label offset */
    s_io = put1(s_io, 0x1D70u, 0x77u);                       /* back-end label release */
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    s_fifo_getoff = 0;
    vm_write32(s_ctrl + 4, 0);

    /* 1. Counting on, count not ready: nothing behind the report lands. */
    gw32(REPORT_EA + 8u, 0xDEADu);
    emit_frame(0x11u);
    run_to(s_io);
    check(vm_read32(REPORT_EA + 8u) == 0xDEADu, "a pending ZPASS report is not written");
    check(vm_read32(LABEL_EA) == 0u, "a release behind a pending report waits");
    check(bl_n == 0, "back-end label methods behind a pending report wait");
    check(kicks > 0, "the idle walker kicks the engine for a label waiting behind a report");

    /* 2. The count arrives: the next pass stores the report, then the rest. */
    ticket_ready = 1;
    run_to(s_io);
    check(vm_read32(REPORT_EA + 8u) == 1234u, "the report carries the measured count");
    check(vm_read32(LABEL_EA) == 0x11u, "the release lands once the report has");
    check(bl_n == 2 && bl_methods[0] == 0x1D6Cu && bl_methods[1] == 0x1D70u,
          "back-end label methods run in FIFO order");
    check(bl_report_at_call == 1234u, "the back-end label lands after the report");

    /* 3. Nothing pending: a release is written at once, as before. */
    s_io = put1(s_io, 0x64u, SEMA_OFF);
    s_io = put1(s_io, 0x6Cu, 0x22u);
    ticket_ready = 0;
    run_to(s_io);
    check(vm_read32(LABEL_EA) == 0x22u, "with no report pending a release is immediate");

    /* 4. A count that never arrives: forced after GCM_ZPASS_WAIT_MS, then the
     *    conservative answer, and the label behind it lands. */
    ticket_ready = -1;
    emit_frame(0x33u);
    run_to(s_io);
    check(vm_read32(REPORT_EA + 8u) == 0xFFFFu, "a lost count reports visible (0xFFFF)");
    check(vm_read32(LABEL_EA) == 0x33u, "and does not hold the label behind it");

    ticket_ready = 0;
    waits = 0;
    gw32(REPORT_EA + 8u, 0xDEADu);
    emit_frame(0x44u);
    run_to(s_io);
    check(vm_read32(LABEL_EA) == 0x33u, "a late count holds its label for now");
    s_dq[s_dq_head % GCM_DQ_LEN].t0 -= 100000u;            /* age the report */
    run_to(s_io);
    check(waits > 0, "an old report is forced with RSX_ZPASS_WAIT");
    check(vm_read32(REPORT_EA + 8u) == 0xFFFFu && vm_read32(LABEL_EA) == 0x44u,
          "a report still late after the wait answers visible, the label lands");

    /* 5. Host counting off: the old constant, at once, and labels too. */
    host_counting = 0;
    gw32(REPORT_EA + 8u, 0xDEADu);
    emit_frame(0x55u);
    run_to(s_io);
    check(vm_read32(REPORT_EA + 8u) == 0xFFFFu, "without host counting ZPASS reports 0xFFFF");
    check(vm_read32(LABEL_EA) == 0x55u, "and the release is immediate");

    printf("[gcm_zpass_report] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
