/* cellPad's side of test_host_input_cocoa.m, built as C: cellPad.c's
 * win32_compat.h and AppKit cannot share one translation unit (BOOL). */
#include "../cellPad.c"
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* One keyboard/mouse poll of port 0. */
void test_pad_poll(unsigned* buttons, int* lx, int* ly, int* rx, int* ry)
{
    pad_poll_keys(0);
    const PadHostState* hs = &s_host_state[0];
    *buttons = hs->buttons;
    *lx = hs->analog_lx; *ly = hs->analog_ly; *rx = hs->analog_rx; *ry = hs->analog_ry;
}
/* Read the mouse settings from the environment again. */
void test_pad_reload_mouse(void) { s_mouse_on = -1; }
/* The in-game menu open or closed (ps3_overlay.mm sets it). */
void test_pad_menu(int open) { g_pad_overlay_open = open; }
