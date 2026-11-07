/* test_overlay_host_surface.m's fixtures for what the in-game menu drives
 * (cellPad, cellAudio, cellAvconfExt, the quit gate). */
#include <stddef.h>
#include <stdint.h>
#include "ps3emu/host_input.h"
#include "../../input/pad_input_map.h"

static ps3_host_input* t_input;
ps3_host_input* ps3_pad_host_input(void) { return t_input; }
void ps3_pad_host_input_attach(ps3_host_input* in) { t_input = in; }
volatile int      g_pad_overlay_open;
volatile uint32_t g_pad_overlay_toggle, g_pad_overlay_nav = 0x80800000u, g_pad_overlay_nav2 = 0x8080u, g_pad_overlay_raw;
static float t_gamma_override; static int t_volume = 100, t_rumble = 100, t_mode;
float cellAvconfExt_display_gamma_override(void) { return t_gamma_override; }
void  cellAvconfExt_set_display_gamma_override(float g) { t_gamma_override = g; }
float cellAvconfExt_title_gamma(void) { return 1.0f; }
int   cellAudio_host_volume(void) { return t_volume; }
void  cellAudio_host_set_volume(int pct) { t_volume = pct; }
int   cellPad_host_rumble_strength(void) { return t_rumble; }
void  cellPad_host_set_rumble_strength(int pct) { t_rumble = pct; }
void  cellPad_host_mouse_settings(int* on, float* sens, int* ix, int* iy) { *on = 1; *sens = 1.0f; *ix = *iy = 0; }
void  cellPad_host_set_mouse_settings(float s, int ix, int iy) { (void)s; (void)ix; (void)iy; }
void  cellPad_host_set_mouse_camera(int on) { (void)on; }
int   cellPad_host_input_mode(void) { return t_mode; }
void  cellPad_host_set_input_mode(int m) { t_mode = m; }
static PadInputMap t_map;
void  cellPad_host_input_map(PadInputMap* out) { *out = t_map; }
void  cellPad_host_set_input_map(const PadInputMap* m) { t_map = *m; }
void  cellPad_host_input_map_path(char* out, size_t n) { if (n) out[0] = 0; }
int   cellPad_host_save_input_map(void) { return 0; }
void  ps3_quit_gate_close(void) {}
int   ps3_quit_gate_busy(void) { return 0; }
int   ps3_quit_gate_wait(int ms) { (void)ms; return 0; }
