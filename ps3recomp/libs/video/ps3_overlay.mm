/*
 * ps3_overlay -- the in-game menu and performance HUD (see ps3_overlay.h).
 *
 * From the inFAMOUS 2 port (its libs/video/ps3_overlay.mm), adapted to this
 * toolkit: the settings the tabs drive are this tree's (no colour curves,
 * LUT presets, ultrawide, button icons or game languages here), the input
 * comes from the ps3_host_input block instead of NSEvents (so the menu works
 * with the window in the launcher process, tools/host_window), the display
 * options go to whoever owns the window, the settings survive an exec
 * (ps3_overlay_settings.h), and "Quit game" waits at the quit gate.
 *
 * Dear ImGui (third_party/imgui, MIT) draws it with its Metal renderer; the
 * platform side is done here: ImGui runs on the presenting thread and is fed
 * from the block there.
 *
 * Built with -fobjc-arc (CMakeLists.txt).
 */
#import <Metal/Metal.h>
#include "imgui.h"
#include "backends/imgui_impl_metal.h"
#include "ps3_overlay.h"
#include "ps3_overlay_settings.h"
#include "../../include/ps3emu/host_input.h"
#include "../../include/ps3emu/quit_gate.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"   /* the parser half is cellPad's */
#include "../input/pad_input_map.h"   /* PadInputMap, names, pim_rebind */
#pragma clang diagnostic pop

#include <float.h>
#include <math.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* ---- what the menu changes: the toolkit's live setters ------------------- */
extern "C" {
float rsx_metal_gfx_sharpen(void);
void  rsx_metal_gfx_set_sharpen(float v);
float rsx_metal_gfx_vibrance(void);
void  rsx_metal_gfx_set_vibrance(float v);
float rsx_metal_gfx_contrast(void);
void  rsx_metal_gfx_set_contrast(float v);
float rsx_metal_gfx_vignette(void);
void  rsx_metal_gfx_set_vignette(float v);
float rsx_metal_gfx_grain(void);
void  rsx_metal_gfx_set_grain(float v);
float rsx_metal_gfx_lut_strength(void);     /* -1: no PS3_LUT loaded */
void  rsx_metal_gfx_set_lut_strength(float v);
int   rsx_metal_gfx_aa(void);               /* 0 off, 1 FXAA, 2 SMAA */
void  rsx_metal_gfx_set_aa(int mode);
int   rsx_metal_gfx_aniso(void);            /* PS3_ANISO in effect (read at start) */
float rsx_metal_res_scale(void);            /* PS3_RES_SCALE in effect; -1 not possible here */
int   rsx_metal_gfx_upscale(void);          /* MetalFX: 1 on, 0 off, -1 not available */
void  rsx_metal_gfx_set_upscale(int on);
int   rsx_metal_vsync(void);                /* -1 not available */
void  rsx_metal_set_vsync(int on);
int   rsx_metal_window_mode(void);          /* 0 window, 1 fullscreen, 2 borderless; -1 n/a */
int   rsx_metal_window_mode_caps(void);     /* bit m: mode m can be set */
void  rsx_metal_set_window_mode(int mode);
void  rsx_metal_present_times(unsigned long long ns[4], unsigned* n);
int   rsx_metal_host_window(void);          /* 1: the window is the launcher's */
float cellAvconfExt_display_gamma_override(void);
void  cellAvconfExt_set_display_gamma_override(float g);
float cellAvconfExt_title_gamma(void);
int   cellAudio_host_volume(void);
void  cellAudio_host_set_volume(int pct);
int   cellPad_host_rumble_strength(void);
void  cellPad_host_set_rumble_strength(int pct);
void  cellPad_host_mouse_settings(int* on, float* sens, int* invert_x, int* invert_y);
void  cellPad_host_set_mouse_settings(float sens, int invert_x, int invert_y);
void  cellPad_host_set_mouse_camera(int on);
int   cellPad_host_input_mode(void);
void  cellPad_host_set_input_mode(int mode);
void  cellPad_host_input_map(PadInputMap* out);
void  cellPad_host_set_input_map(const PadInputMap* m);
void  cellPad_host_input_map_path(char* out, size_t n);
int   cellPad_host_save_input_map(void);
ps3_host_input* ps3_pad_host_input(void);   /* cellPad.c: the window's keys and mouse */
extern volatile int      g_pad_overlay_open;
extern volatile uint32_t g_pad_overlay_toggle, g_pad_overlay_nav, g_pad_overlay_nav2, g_pad_overlay_raw;
}

volatile int g_ps3_overlay_active;

/* The runtime library's link line asks for this symbol (CMakeLists.txt,
 * -u _ps3_overlay_link_anchor), so this object -- whose hooks the backend
 * only has weak stand-ins for -- is always linked into a game. */
extern "C" void ps3_overlay_link_anchor(void) {}

/* CELL_PAD_CTRL_* (libs/input/cellPad.h) */
enum {
    PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3,
    PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};

/* macOS virtual key codes the menu uses itself. */
enum { VK_ESC = 0x35, VK_F1 = 0x7A, VK_INSERT = 0x72, VK_BACKSPACE = 0x33, VK_DELETE = 0x75 };

/* ---- settings and their file (ps3_overlay_settings.h) ---------------------
 * Only the variables below are taken from the file. */
enum {
    V_GAMMA, V_SHARPEN, V_AA, V_ANISO, V_UPSCALE, V_VSYNC, V_WINDOW_MODE,
    V_VOLUME, V_RUMBLE, V_RUMBLE_STRENGTH, V_MOUSE_SENS, V_MOUSE_INVERT_X, V_MOUSE_INVERT_Y,
    V_HUD, V_HUD_CORNER, V_HUD_OPACITY, V_HUD_GRAPH, V_MOUSE, V_INPUT_MODE,
    V_VIBRANCE, V_CONTRAST, V_VIGNETTE, V_GRAIN, V_LUT_STRENGTH, V_RES_SCALE, V_N
};
static const char* const kVar[V_N] = {
    "PS3_DISPLAY_GAMMA", "PS3_SHARPEN", "PS3_AA", "PS3_ANISO", "PS3_UPSCALE", "PS3_VSYNC",
    "PS3_WINDOW_MODE", "PS3_AUDIO_VOLUME", "PS3_RUMBLE", "PS3_RUMBLE_STRENGTH", "PS3_MOUSE_SENS",
    "PS3_MOUSE_INVERT_X", "PS3_MOUSE_INVERT_Y", "PS3_OVERLAY_HUD", "PS3_OVERLAY_HUD_CORNER",
    "PS3_OVERLAY_HUD_OPACITY", "PS3_OVERLAY_HUD_GRAPH", "PS3_MOUSE", "PS3_INPUT_MODE",
    "PS3_VIBRANCE", "PS3_CONTRAST", "PS3_VIGNETTE", "PS3_GRAIN", "PS3_LUT_STRENGTH",
    "PS3_RES_SCALE",
};
/* Plain arrays: these are filled by a constructor, before C++ globals of
 * this file would be guaranteed to exist. */
static unsigned char s_env_set[V_N];      /* set by the player when the session started */
static char s_saved[V_N][PS3OS_VAL];      /* the file's value ("" = not in it) */
static char s_value[V_N][PS3OS_VAL];      /* what the menu set this image ("" = untouched) */
static char s_ini_path[1024];
static pthread_mutex_t s_set_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_dirty;

static bool env_on(const char* v)
{
    const char* e = getenv(v);
    return e && *e && strcmp(e, "0") != 0;
}
static bool overlay_off(void)
{
    const char* e = getenv("PS3_OVERLAY");
    return e && e[0] == '0' && e[1] == 0;
}
static bool headless_env(void)
{
    return env_on("PS3RECOMP_METAL_HEADLESS");
}
/* PS3_OVERLAY_TEST, taken by this image at start (PS3_OVERLAY_TEST_NEXT is
 * the next image's, after an exec). */
static char s_test_script[4096];
/* A headless test asked for the menu or the HUD. */
static bool test_asked(void)
{
    return env_on("PS3_OVERLAY_FORCE_OPEN") || env_on("PS3_OVERLAY_HUD") || s_test_script[0];
}

__attribute__((constructor)) static void ps3_overlay_preload(void)
{
    if (overlay_off()) return;
    /* The test script is this image's; the next image (exitspawn) gets
     * PS3_OVERLAY_TEST_NEXT, or none. Before main: no thread reads the
     * environment yet. */
    { const char* t = getenv("PS3_OVERLAY_TEST");
      if (t) snprintf(s_test_script, sizeof s_test_script, "%s", t);
      const char* next = getenv("PS3_OVERLAY_TEST_NEXT");
      if (next) { setenv("PS3_OVERLAY_TEST", next, 1); unsetenv("PS3_OVERLAY_TEST_NEXT"); }
      else if (t) unsetenv("PS3_OVERLAY_TEST"); }
    const char* p = getenv("PS3_OVERLAY_INI");
    if (p && *p) snprintf(s_ini_path, sizeof s_ini_path, "%s", p);
    /* A headless run reads the file only when a test asked for the menu. */
    const int read = !headless_env() || test_asked();
    const bool later = getenv(PS3OS_SESSION) != NULL;
    const int applied = ps3os_preload(kVar, V_N, s_env_set, s_saved, s_ini_path, read);
    if (s_ini_path[0] && read) {
        int kept = 0;
        for (int i = 0; i < V_N; i++) if (s_saved[i][0] && s_env_set[i]) kept++;
        if (later)
            fprintf(stderr, "[overlay] %s: settings carried over from the previous image\n", s_ini_path);
        else
            fprintf(stderr, "[overlay] %s: %d setting(s) applied, %d left to the environment\n",
                    s_ini_path, applied, kept);
    }
}

static void set_value(int v, const char* fmt, ...)
{
    char buf[PS3OS_VAL];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    pthread_mutex_lock(&s_set_lock);
    if (strcmp(s_value[v], buf)) { snprintf(s_value[v], sizeof s_value[v], "%s", buf); s_dirty = true; }
    pthread_mutex_unlock(&s_set_lock);
}

/* Write the file: what it had plus what the menu changed. 1 = written. */
static int settings_save(void)
{
    if (!s_ini_path[0]) return 0;
    pthread_mutex_lock(&s_set_lock);
    const int ok = ps3os_save(s_ini_path, kVar, V_N, s_saved, s_value,
        "# In-game menu settings (F1 / Cmd+, / L3+R3). Same names and values as the\n"
        "# environment variables; a variable set when the game starts wins over its line.\n");
    if (ok) s_dirty = false;
    pthread_mutex_unlock(&s_set_lock);
    return ok;
}

/* ---- state ---------------------------------------------------------------- */
static id<MTLDevice> s_dev;
static bool s_enabled, s_headless, s_force_open, s_imgui_ready, s_pt;
static volatile int s_open;
static volatile int s_hud;
static int   s_hud_corner = 1;          /* 0 top left, 1 top right, 2 bottom left, 3 bottom right */
static float s_hud_opacity = 0.6f;
static bool  s_hud_graph = true;
static uint32_t s_seen_toggle, s_seen_keys_toggle;
static char  s_title[64] = "ps3recomp";
static int   s_tab, s_tab_want = -1;   /* the menu's tab; one to switch to */
static const int kTabs = 4;
/* "Quit game": 1 = open the confirmation next frame, 2 = it is showing.
 * s_quit_cancel: Esc or Circle closes it (both close the whole menu otherwise);
 * s_quit_press: PS3_OVERLAY_TEST's "confirm". */
static volatile int  s_quit_ask;
static volatile bool s_quit_cancel, s_quit_press, s_quitting;
static double s_t0;                    /* ps3_overlay_init */

static const char* L(const char* en, const char* pt) { return s_pt ? pt : en; }

static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Notices (any thread). */
struct Notice { char text[128]; double until; };
static Notice s_notice[3];
static pthread_mutex_t s_notice_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile double s_notice_until;

/* Drawn this frame or not. While nothing is drawn the present still calls
 * in, so the menu's keys (counted by the window) and L3+R3 are seen. */
static void update_active(void)
{
    g_ps3_overlay_active = s_enabled;
}
/* ---- The PS3's system dialogs (cellMsgDialog) drawn here -----------------
 * When the game runs inside the host window it has no window of its own for
 * the backend's NSAlert sheet, so yes/no and OK prompts are drawn by the menu
 * instead, over the game: Cross/A or Return answer yes/OK, Circle/B or Escape
 * answer no. The prompt that opened it must be released first, so the press
 * that led to it does not answer it. The game sees no pad while it is up. */
extern "C" int cellPad_host_confirm_buttons(void);
extern "C" volatile int g_cellpad_block_guest_input;
static char s_dlg_text[1024];
static int s_dlg_yes_no, s_dlg_default_no, s_dlg_armed;
static int s_dlg_seq;                       /* the prompt on screen, 0 = none */
static void (*s_dlg_done)(int seq, int yes);

extern "C" int ps3_overlay_show_dialog(const char* text, int yes_no, int default_no, int seq,
                                       void (*done)(int seq, int yes))
{
    if (!s_enabled || !done || !seq) return -1;
    snprintf(s_dlg_text, sizeof s_dlg_text, "%s", text ? text : "");
    s_dlg_yes_no = yes_no; s_dlg_default_no = default_no; s_dlg_done = done; s_dlg_armed = 0;
    g_cellpad_block_guest_input = 1;
    __atomic_store_n(&s_dlg_seq, seq, __ATOMIC_RELEASE);
    return 0;
}

extern "C" void ps3_overlay_dismiss_dialog(int seq)
{
    int want = seq;
    if (__atomic_compare_exchange_n(&s_dlg_seq, &want, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        g_cellpad_block_guest_input = 0;
}

static bool dialog_up(void) { return __atomic_load_n(&s_dlg_seq, __ATOMIC_ACQUIRE) != 0; }

static bool vk_down(const ps3_host_input* in, unsigned vk)
{
    return in && vk < 128 && (atomic_load(&in->keys[vk >> 5]) >> (vk & 31)) & 1u;
}

static void dialog_answer(int seq, int yes)
{
    int want = seq;
    if (!__atomic_compare_exchange_n(&s_dlg_seq, &want, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    /* cellPad keeps a button still held out of the game until it is let go */
    g_cellpad_block_guest_input = 0;
    s_dlg_done(seq, yes);
}

static void draw_dialog(ps3_host_input* in)
{
    const int seq = __atomic_load_n(&s_dlg_seq, __ATOMIC_ACQUIRE);
    if (!seq) return;
    const int pad = cellPad_host_confirm_buttons();
    const bool enter = vk_down(in, 0x24) || vk_down(in, 0x4C), escape = vk_down(in, 0x35);
    if (!s_dlg_armed) {
        s_dlg_armed = pad == 0 && !enter && !escape;
    } else if ((pad & 1) || enter) {
        dialog_answer(seq, s_dlg_yes_no && s_dlg_default_no && enter && !(pad & 1) ? 0 : 1);
        return;
    } else if (((pad & 2) || escape) && s_dlg_yes_no) {
        dialog_answer(seq, 0);
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(io.DisplaySize.x * 0.8f, FLT_MAX));
    if (ImGui::Begin("###ps3dialog", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushTextWrapPos(io.DisplaySize.x * 0.75f);
        ImGui::TextUnformatted(s_dlg_text);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (s_dlg_yes_no) {
            const bool yes = ImGui::Button(L("Yes  (\u2715 / A / Return)", "Sim  (\u2715 / A / Enter)"));
            ImGui::SameLine();
            const bool no = ImGui::Button(L("No  (\u25CB / B / Esc)", "N\u00E3o  (\u25CB / B / Esc)"));
            if (yes) dialog_answer(seq, 1); else if (no) dialog_answer(seq, 0);
        } else if (ImGui::Button(L("OK  (\u2715 / A / Return)", "OK  (\u2715 / A / Enter)"))) {
            dialog_answer(seq, 1);
        }
    }
    ImGui::End();
}

static bool anything_to_draw(double now)
{
    return s_open || s_hud || s_notice_until > now || dialog_up();
}

extern "C" void ps3_overlay_notice(const char* text)
{
    if (!s_enabled || !text) return;
    pthread_mutex_lock(&s_notice_lock);
    memmove(&s_notice[1], &s_notice[0], sizeof s_notice[0] * 2);
    snprintf(s_notice[0].text, sizeof s_notice[0].text, "%s", text);
    s_notice[0].until = now_s() + 3.0;
    s_notice_until = s_notice[0].until;
    pthread_mutex_unlock(&s_notice_lock);
    fprintf(stderr, "[overlay] %s\n", text);
}

/* ---- rebinding (Controls tab) -----------------------------------------------
 * The tab starts a capture for one cell (a PS3 control on one device); the
 * next key, mouse button or wheel turn (from the window's block), or newly
 * pressed controller button (g_pad_overlay_raw) ends it. The menu is open
 * throughout, so the game holds neutral and none of it reaches the game. */
enum { CAP_KEYS, CAP_MOUSE, CAP_PAD };                 /* the table's columns */
enum { CAP_PENDING, CAP_BIND, CAP_CANCEL, CAP_UNBIND };
static const double kCapTimeout = 8.0;                 /* seconds, then it gives up */
static int      s_cap_dev = -1, s_cap_ctl, s_cap_res;
static uint32_t s_cap_val, s_cap_pad_base, s_cap_mouse_base, s_cap_key_base[4];
static double   s_cap_t0, s_cap_wheel;

static int cap_device(void)
{
    return (s_cap_dev >= 0 && s_cap_res == CAP_PENDING) ? s_cap_dev : -1;
}
static void cap_finish(int res, uint32_t val)
{
    if (s_cap_dev >= 0 && s_cap_res == CAP_PENDING) { s_cap_res = res; s_cap_val = val; }
}

/* What the window does with the mouse: the pointer for the menu while it is
 * open. cellPad writes the same on its next poll; this covers a game that
 * is not polling the pad right now. */
static void window_flags(int open)
{
    ps3_host_input* in = ps3_pad_host_input();
    if (!in) return;
    if (open) atomic_store(&in->game_flags, PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MENU);
    else atomic_fetch_and(&in->game_flags, ~(uint32_t)PS3_HOST_INPUT_MENU);
}

static void set_open(int o)
{
    if (!s_enabled || (o != 0) == (s_open != 0)) return;
    if (!o) cap_finish(CAP_CANCEL, 0);
    s_open = o;
    g_pad_overlay_open = o;
    window_flags(o);
    if (!o) {
        ps3_host_input* in = ps3_pad_host_input();
        if (in) atomic_store(&in->wheel, 0);            /* the menu's scrolling is not the game's */
    }
    if (!o && s_dirty) {
        if (settings_save()) ps3_overlay_notice(L("Settings saved", "Ajustes salvos"));
        else if (!s_ini_path[0]) ps3_overlay_notice(L("Settings kept for this session (no PS3_OVERLAY_INI)",
                                                      "Ajustes valem só nesta sessão (sem PS3_OVERLAY_INI)"));
        else ps3_overlay_notice(L("Could not save the settings", "Não foi possível salvar os ajustes"));
    }
    update_active();
}

extern "C" int ps3_overlay_menu_open(void) { return s_enabled && s_open; }

extern "C" int ps3_overlay_composes_headless(void) { return s_enabled && s_headless; }

/* PS3_OVERLAY_TEST's "shot=<file>": taken by the backend after the frame. */
static char s_shot[1024], s_shot_out[1024];
static volatile int s_shot_ready;      /* 1: s_shot is for the frame just drawn */
/* Same thread as ps3_overlay_render (the backend calls it after the frame). */
extern "C" const char* ps3_overlay_shot_take(void)
{
    if (!s_shot_ready || !s_shot[0]) return NULL;
    s_shot_ready = 0;
    snprintf(s_shot_out, sizeof s_shot_out, "%s", s_shot);
    s_shot[0] = 0;
    return s_shot_out;
}

extern "C" void ps3_overlay_before_exec(void)
{
    if (overlay_off()) return;
    if (s_dirty) settings_save();
    pthread_mutex_lock(&s_set_lock);
    ps3os_export_session(kVar, V_N, s_value);
    pthread_mutex_unlock(&s_set_lock);
}

/* ---- quitting ------------------------------------------------------------------
 * The toolkit's own "Quit game", for a port that has no quit path of its own:
 * the quit gate (no save or checkpoint write starts any more, the ones under
 * way finish, at most 30 s), the logs flushed, exit status 0. A port that
 * has one defines ps3_host_quit itself (the Resistance port's main.cpp hands
 * it to its clean-exit thread, which does the same). */
static void* quit_thread(void* arg)
{
    const char* why = (const char*)arg;
    fprintf(stderr, "[overlay] quit (%s)\n", why ? why : "?");
    const int left = ps3_quit_gate_wait(30000);
    if (left) fprintf(stderr, "[overlay] quit: %d save(s)/write(s) still open after 30 s\n", left);
    fflush(stdout); fflush(stderr);
    _exit(0);
    return NULL;
}
extern "C" __attribute__((weak)) void ps3_host_quit(const char* why)
{
    pthread_t th;
    if (pthread_create(&th, NULL, quit_thread, (void*)why) == 0) pthread_detach(th);
    else quit_thread((void*)why);
}

static void do_quit(void)
{
    if (s_quitting) return;
    s_quitting = true;
    s_quit_ask = 0;
    set_open(0);
    ps3_overlay_notice(ps3_quit_gate_busy() ? L("Quitting after the save in progress...",
                                                "Saindo depois do salvamento em andamento...")
                                            : L("Quitting the game...", "Saindo do jogo..."));
    ps3_host_quit("in-game menu");
}

/* ---- PS3_OVERLAY_TEST ------------------------------------------------------------
 * A timed script for headless runs, "<seconds>:<action>[,<seconds>:<action>...]",
 * the seconds counted from the backend's start; PS3_OVERLAY_TEST_NEXT is the
 * script of the image an exec brings up (each image takes its own). Actions: open, close, tab=<0..3>,
 * hud=<0|1>, quit (the "Quit the game?" question), confirm (its Quit button: the
 * real quit path), cancel, set=<VARIABLE>:<value> (as the menu would set it),
 * shot=<file.ppm> (the next composed frame, written by the backend). */
struct TestStep { double t; char act[16]; char arg[1024]; bool done; };
static TestStep* s_test;
static int s_test_n;

static void test_parse(const char* e)
{
    if (!e || !*e) return;
    int cap = 1;
    for (const char* p = e; *p; p++) if (*p == ',') cap++;
    s_test = (TestStep*)calloc((size_t)cap, sizeof *s_test);
    if (!s_test) return;
    char* copy = strdup(e);
    for (char* tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        char* colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        TestStep& st = s_test[s_test_n];
        st.t = atof(tok);
        char* a = colon + 1;
        char* eq = strchr(a, '=');
        if (eq) { *eq = 0; snprintf(st.arg, sizeof st.arg, "%s", eq + 1); }
        snprintf(st.act, sizeof st.act, "%s", a);
        s_test_n++;
    }
    free(copy);
    fprintf(stderr, "[overlay] test script: %d step(s)\n", s_test_n);
}

/* ---- performance numbers ---------------------------------------------------- */
enum { FT_N = 180 };
static float  s_ft[FT_N];               /* ms between presents */
static int    s_ft_i, s_ft_count;
static double s_last_present;
struct Stats { double fps, avg_ms, max_ms, cores, enc_ms, wait_ms; int busy; bool valid; };
static Stats  s_stats;

static void stats_tick(double now)
{
    static double t0, cpu0;
    static unsigned long long pt0[4]; static unsigned pn0;
    static int frames;
    frames++;
    if (!t0) {
        t0 = now;
        struct rusage ru; getrusage(RUSAGE_SELF, &ru);
        cpu0 = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
        rsx_metal_present_times(pt0, &pn0);
        frames = 0;
        return;
    }
    const double dt = now - t0;
    if (dt < 1.0) return;
    Stats st = {};
    st.fps = frames / dt;
    { struct rusage ru; getrusage(RUSAGE_SELF, &ru);
      const double cpu = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
      st.cores = (cpu - cpu0) / dt; cpu0 = cpu; }
    /* Threads busy at least 80% (Mach's decayed usage): with this title the
     * SPU threads, the PPU main thread and the RSX/present thread. */
    { thread_act_array_t th = NULL; mach_msg_type_number_t n = 0;
      if (task_threads(mach_task_self(), &th, &n) == KERN_SUCCESS) {
          for (mach_msg_type_number_t i = 0; i < n; i++) {
              thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
              if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS &&
                  !(bi.flags & TH_FLAGS_IDLE) && bi.cpu_usage >= TH_USAGE_SCALE * 8 / 10) st.busy++;
              mach_port_deallocate(mach_task_self(), th[i]);
          }
          vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
      } }
    { unsigned long long pt[4]; unsigned pn;
      rsx_metal_present_times(pt, &pn);
      if (pn > pn0 && pt[0] >= pt0[0] && pt[2] >= pt0[2]) {
          st.wait_ms = (pt[0] - pt0[0]) / 1e6 / (pn - pn0);
          st.enc_ms  = (pt[2] - pt0[2]) / 1e6 / (pn - pn0);
      }
      memcpy(pt0, pt, sizeof pt0); pn0 = pn; }
    { const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
      double sum = 0, mx = 0; int k = 0;
      for (int i = 0; i < n && i < frames; i++) {
          const float v = s_ft[(s_ft_i - 1 - i + FT_N) % FT_N];
          sum += v; if (v > mx) mx = v; k++;
      }
      st.avg_ms = k ? sum / k : 0; st.max_ms = mx; }
    st.valid = true;
    s_stats = st;
    t0 = now; frames = 0;
}

/* ---- ImGui ---------------------------------------------------------------------- */
static void imgui_setup(void)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;     /* the menu keeps its own file */
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "ps3recomp_overlay";
    /* A system font with the accents Portuguese needs; ImGui's own otherwise. */
    static const char* const fonts[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
    };
    ImFont* font = NULL;
    for (const char* f : fonts) if (access(f, R_OK) == 0 && (font = io.Fonts->AddFontFromFileTTF(f, 17.0f))) break;
    if (!font) io.Fonts->AddFontDefaultVector();

    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.FontSizeBase = 17.0f;
    st.WindowRounding = 10.0f; st.FrameRounding = 5.0f; st.GrabRounding = 5.0f;
    st.TabRounding = 5.0f; st.PopupRounding = 6.0f;
    st.WindowPadding = ImVec2(18, 14); st.FramePadding = ImVec2(10, 5); st.ItemSpacing = ImVec2(10, 8);
    st.WindowBorderSize = 1.0f; st.GrabMinSize = 14.0f;
    /* near-black with a warm amber accent */
    ImVec4* c = st.Colors;
    const ImVec4 acc(0.96f, 0.62f, 0.18f, 1.0f), acc_dim(0.55f, 0.34f, 0.10f, 1.0f);
    c[ImGuiCol_WindowBg]        = ImVec4(0.05f, 0.05f, 0.05f, 0.94f);
    c[ImGuiCol_Border]          = ImVec4(0.96f, 0.62f, 0.18f, 0.35f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.15f, 0.14f, 0.13f, 1.0f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.24f, 0.20f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.30f, 0.24f, 0.16f, 1.0f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.10f, 0.08f, 0.06f, 1.0f);
    c[ImGuiCol_CheckMark]       = acc;
    c[ImGuiCol_SliderGrab]      = acc_dim;
    c[ImGuiCol_SliderGrabActive]= acc;
    c[ImGuiCol_Button]          = ImVec4(0.18f, 0.16f, 0.14f, 1.0f);
    c[ImGuiCol_ButtonHovered]   = acc_dim;
    c[ImGuiCol_ButtonActive]    = acc;
    c[ImGuiCol_Header]          = ImVec4(0.38f, 0.25f, 0.10f, 1.0f);
    c[ImGuiCol_HeaderHovered]   = acc_dim;
    c[ImGuiCol_Tab]             = ImVec4(0.14f, 0.12f, 0.10f, 1.0f);
    c[ImGuiCol_TabHovered]      = acc_dim;
    c[ImGuiCol_TabSelected]     = ImVec4(0.48f, 0.30f, 0.10f, 1.0f);
    c[ImGuiCol_PlotLines]       = acc;
    c[ImGuiCol_NavCursor]       = ImVec4(0.30f, 0.80f, 1.0f, 1.0f);   /* cool blue: where the pad is */
    c[ImGuiCol_Separator]       = ImVec4(0.96f, 0.62f, 0.18f, 0.25f);

    ImGui_ImplMetal_Init(s_dev);
    s_imgui_ready = true;
}

extern "C" void ps3_overlay_init(void* mtl_device, int headless, const char* title)
{
    s_dev = (__bridge id<MTLDevice>)mtl_device;
    s_headless = headless != 0;
    s_enabled = false;
    g_ps3_overlay_active = 0;
    s_t0 = now_s();
    if (overlay_off()) return;
    s_force_open = env_on("PS3_OVERLAY_FORCE_OPEN");
    s_hud = env_on("PS3_OVERLAY_HUD");
    const char* test = s_test_script;
    /* Headless: nothing at all unless a test asks for it. */
    if (s_headless && !test_asked()) return;
    { const char* e = getenv("PS3_OVERLAY_HUD_CORNER"); if (e && *e) s_hud_corner = atoi(e) & 3; }
    { const char* e = getenv("PS3_OVERLAY_HUD_OPACITY");
      if (e && *e) { float v = (float)atof(e); s_hud_opacity = v < 0.1f ? 0.1f : v > 1.0f ? 1.0f : v; } }
    { const char* e = getenv("PS3_OVERLAY_HUD_GRAPH"); if (e && *e) s_hud_graph = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3_OVERLAY_LANG");
      if (!e || !*e) e = getenv("PS3_LANG");
      s_pt = e && !strncasecmp(e, "pt", 2); }
    { const char* e = getenv("PS3_TITLE");
      if (!e || !*e) e = title;
      if (e && *e) snprintf(s_title, sizeof s_title, "%s", e); }
    s_seen_toggle = __atomic_load_n(&g_pad_overlay_toggle, __ATOMIC_ACQUIRE);
    { ps3_host_input* in = ps3_pad_host_input();
      s_seen_keys_toggle = in ? atomic_load(&in->menu_toggle) : 0; }
    /* PS3_OVERLAY_TAB=0..3: the tab it opens on (with FORCE_OPEN, the one a
     * headless test draws). */
    { const char* e = getenv("PS3_OVERLAY_TAB");
      if (e && *e) { const int t = atoi(e); s_tab_want = t >= 0 && t < kTabs ? t : 0; } }
    test_parse(test);
    s_enabled = true;
    if (s_force_open) set_open(1);
    /* PS3_OVERLAY_FORCE_OPEN=quit: open on the "Quit the game?" confirmation
     * (a headless frame dump shows it; nothing is pressed). */
    { const char* e = getenv("PS3_OVERLAY_FORCE_OPEN"); if (e && !strcmp(e, "quit")) s_quit_ask = 1; }
    update_active();
    fprintf(stderr, "[overlay] in-game menu: F1 (fn+F1), Insert/Help or Cmd+, ; L3+R3 on a controller%s%s%s\n",
            s_hud ? "; HUD on" : "", s_force_open ? "; forced open" : "",
            s_ini_path[0] ? "" : "; settings not saved (no PS3_OVERLAY_INI)");
}

extern "C" void ps3_overlay_shutdown(void)
{
    if (s_imgui_ready) {
        ImGui_ImplMetal_Shutdown();
        ImGui::DestroyContext();
        s_imgui_ready = false;
    }
    s_enabled = false;
    g_ps3_overlay_active = 0;
    g_pad_overlay_open = 0;
}

/* The menu's view of the settings, read from the modules when it opens. */
struct Ui {
    bool  gamma_on; float gamma; float title_gamma;
    float sharpen, vibrance, contrast, vignette, grain, lut;
    int   aa, aniso, aniso_start, upscale, vsync, wmode, wmode_caps;
    int   volume;
    bool  rumble; int rumble_strength;
    int   mouse_on; float mouse_sens; bool inv_x, inv_y;
    int   input_mode;
    int   res, res_start;   /* PS3_RES_SCALE: index in kResScale; -1 n/a */
    bool  host;             /* the window is the launcher's */
};
/* PS3_RES_SCALE choices of the menu (the variable takes any 1..4). */
static const char* const kResScale[] = { "1", "1.5", "2", "3" };
static int res_scale_index(double v)
{
    int best = 0;
    for (int i = 1; i < 4; i++) if (fabs(atof(kResScale[i]) - v) < fabs(atof(kResScale[best]) - v)) best = i;
    return best;
}
static Ui s_ui;

static void ui_load(void)
{
    const float g = cellAvconfExt_display_gamma_override();
    s_ui.gamma_on = g > 0.0f;
    s_ui.title_gamma = cellAvconfExt_title_gamma();
    s_ui.gamma = g > 0.0f ? g : s_ui.title_gamma;
    s_ui.sharpen = rsx_metal_gfx_sharpen();
    s_ui.aa = rsx_metal_gfx_aa();
    s_ui.vibrance = rsx_metal_gfx_vibrance();
    s_ui.contrast = rsx_metal_gfx_contrast();
    s_ui.vignette = rsx_metal_gfx_vignette();
    s_ui.grain = rsx_metal_gfx_grain();
    s_ui.lut = rsx_metal_gfx_lut_strength();
    s_ui.aniso_start = rsx_metal_gfx_aniso();
    s_ui.aniso = s_value[V_ANISO][0] ? atoi(s_value[V_ANISO]) : s_ui.aniso_start;
    { const float rs = rsx_metal_res_scale();
      s_ui.res_start = rs < 0.0f ? -1 : res_scale_index(rs);
      s_ui.res = s_value[V_RES_SCALE][0] ? res_scale_index(atof(s_value[V_RES_SCALE])) : s_ui.res_start; }
    s_ui.upscale = rsx_metal_gfx_upscale();
    s_ui.vsync = rsx_metal_vsync();
    s_ui.wmode = rsx_metal_window_mode();
    s_ui.wmode_caps = rsx_metal_window_mode_caps();
    s_ui.host = rsx_metal_host_window() != 0;
    s_ui.volume = cellAudio_host_volume();
    const int r = cellPad_host_rumble_strength();
    s_ui.rumble = r > 0;
    if (r > 0) s_ui.rumble_strength = r; else if (!s_ui.rumble_strength) s_ui.rumble_strength = 100;
    int ix, iy;
    cellPad_host_mouse_settings(&s_ui.mouse_on, &s_ui.mouse_sens, &ix, &iy);
    s_ui.inv_x = ix != 0; s_ui.inv_y = iy != 0;
    s_ui.input_mode = cellPad_host_input_mode();
}

/* A setting from its variable's text, as the menu would set it
 * (PS3_OVERLAY_TEST's set=). 0 = not a setting this applies live. */
static int apply_setting(int v, const char* t)
{
    const float f = (float)atof(t);
    const int   i = atoi(t);
    switch (v) {
    case V_GAMMA:     cellAvconfExt_set_display_gamma_override(f); break;
    case V_SHARPEN:   rsx_metal_gfx_set_sharpen(f); break;
    case V_VIBRANCE:  rsx_metal_gfx_set_vibrance(f); break;
    case V_CONTRAST:  rsx_metal_gfx_set_contrast(f); break;
    case V_VIGNETTE:  rsx_metal_gfx_set_vignette(f); break;
    case V_GRAIN:     rsx_metal_gfx_set_grain(f); break;
    case V_LUT_STRENGTH: rsx_metal_gfx_set_lut_strength(f); break;
    case V_AA:        rsx_metal_gfx_set_aa(!strcasecmp(t, "fxaa") ? 1 : !strcasecmp(t, "smaa") ? 2 : 0); break;
    case V_VOLUME:    cellAudio_host_set_volume(i); break;
    case V_HUD:       s_hud = i != 0; break;
    case V_VSYNC:     rsx_metal_set_vsync(i != 0); break;
    case V_UPSCALE:   rsx_metal_gfx_set_upscale(!strcasecmp(t, "metalfx")); break;
    case V_WINDOW_MODE:
        rsx_metal_set_window_mode(!strcasecmp(t, "fullscreen") ? 1 : !strcasecmp(t, "borderless") ? 2 : 0); break;
    case V_RUMBLE_STRENGTH: cellPad_host_set_rumble_strength(i); break;
    case V_ANISO: case V_RES_SCALE: break;      /* at the next start */
    default: return 0;
    }
    return 1;
}

/* "env" after a setting whose variable was set at start: it wins next time. */
static void env_mark(int v)
{
    if (!s_env_set[v]) return;
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.20f, 1.0f), "env");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(L("%s was set when the game started (the launcher or the shell).\n"
                            "The change applies now, but that variable wins again next time.",
                            "%s estava definida quando o jogo abriu (o launcher ou o terminal).\n"
                            "A mudança vale agora, mas essa variável vence de novo na próxima vez."), kVar[v]);
}

static void hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.64f, 0.60f, 1.0f));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void section(const char* text)
{
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.96f, 0.62f, 0.18f, 1.0f), "%s", text);
    ImGui::Separator();
}

static void restart_note(bool changed, const char* more_en, const char* more_pt)
{
    /* Resistance: the campaign is another executable (exitspawn), so a
     * restart-only option set in the front end applies when the campaign
     * starts too. */
    char b[256];
    snprintf(b, sizeof b, "%s%s%s", L("Applies when the game starts again (or the next executable loads).",
                                      "Vale quando o jogo abrir de novo (ou o próximo executável carregar)."),
             more_en ? " " : "", more_en ? L(more_en, more_pt) : "");
    if (changed) ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.20f, 1.0f), "%s", b);
    else hint(b);
}

static void tab_image(void)
{
    section(L("Picture", "Ajustes da imagem"));
    if (ImGui::Checkbox(L("Override the game's brightness", "Substituir o brilho do jogo"), &s_ui.gamma_on)) {
        cellAvconfExt_set_display_gamma_override(s_ui.gamma_on ? s_ui.gamma : 0.0f);
        if (s_ui.gamma_on) set_value(V_GAMMA, "%.2f", s_ui.gamma); else set_value(V_GAMMA, "0");
    }
    env_mark(V_GAMMA);
    ImGui::BeginDisabled(!s_ui.gamma_on);
    if (ImGui::SliderFloat(L("Gamma", "Gama"), &s_ui.gamma, 0.80f, 1.20f, "%.2f")) {
        cellAvconfExt_set_display_gamma_override(s_ui.gamma);
        set_value(V_GAMMA, "%.2f", s_ui.gamma);
    }
    ImGui::EndDisabled();
    { char b[160];
      snprintf(b, sizeof b, L("Off: the game's own brightness option (now %.2f), as on the console. Higher = darker.",
                              "Desligado: a opção de brilho do próprio jogo (agora %.2f), como no console. Maior = mais escuro."),
               (double)s_ui.title_gamma);
      hint(b); }

    if (ImGui::SliderFloat(L("Sharpening", "Nitidez"), &s_ui.sharpen, 0.0f, 1.0f, s_ui.sharpen > 0.0f ? "%.2f" : L("off", "desligada"))) {
        rsx_metal_gfx_set_sharpen(s_ui.sharpen);
        set_value(V_SHARPEN, "%.2f", s_ui.sharpen);
    }
    env_mark(V_SHARPEN);
    if (ImGui::SliderFloat(L("Vibrance", "Vibrance"), &s_ui.vibrance, -1.0f, 1.0f,
                           s_ui.vibrance != 0.0f ? "%+.2f" : L("off", "desligada"))) {
        rsx_metal_gfx_set_vibrance(s_ui.vibrance);
        set_value(V_VIBRANCE, "%.2f", s_ui.vibrance);
    }
    env_mark(V_VIBRANCE);
    if (ImGui::SliderFloat(L("Contrast", "Contraste"), &s_ui.contrast, 0.5f, 1.5f,
                           s_ui.contrast != 1.0f ? "%.2f" : L("normal", "normal"))) {
        rsx_metal_gfx_set_contrast(s_ui.contrast);
        set_value(V_CONTRAST, "%.2f", s_ui.contrast);
    }
    env_mark(V_CONTRAST);
    if (ImGui::SliderFloat(L("Vignette", "Vinheta"), &s_ui.vignette, 0.0f, 1.0f,
                           s_ui.vignette > 0.0f ? "%.2f" : L("off", "desligada"))) {
        rsx_metal_gfx_set_vignette(s_ui.vignette);
        set_value(V_VIGNETTE, "%.2f", s_ui.vignette);
    }
    env_mark(V_VIGNETTE);
    if (ImGui::SliderFloat(L("Film grain", "Granulado"), &s_ui.grain, 0.0f, 1.0f,
                           s_ui.grain > 0.0f ? "%.2f" : L("off", "desligado"))) {
        rsx_metal_gfx_set_grain(s_ui.grain);
        set_value(V_GRAIN, "%.2f", s_ui.grain);
    }
    env_mark(V_GRAIN);
    if (s_ui.lut >= 0.0f) {
        if (ImGui::SliderFloat(L("Colour LUT strength", "Força da LUT de cor"), &s_ui.lut, 0.0f, 1.0f, "%.2f")) {
            rsx_metal_gfx_set_lut_strength(s_ui.lut);
            set_value(V_LUT_STRENGTH, "%.2f", s_ui.lut);
        }
        env_mark(V_LUT_STRENGTH);
    } else {
        hint(L("Colour LUT: none loaded (PS3_LUT=<file.png> at start).",
               "LUT de cor: nenhuma carregada (PS3_LUT=<arquivo.png> ao abrir)."));
    }
    { static const char* const aa_val[3] = { "off", "fxaa", "smaa" };
      const char* aa_name[3] = { L("off", "desligado"), "FXAA", "SMAA" };
      if (ImGui::Combo(L("Anti-aliasing", "Antisserrilhamento"), &s_ui.aa, aa_name, 3)) {
          rsx_metal_gfx_set_aa(s_ui.aa);
          s_ui.aa = rsx_metal_gfx_aa();
          set_value(V_AA, "%s", aa_val[s_ui.aa]);
      } }
    env_mark(V_AA);

    ImGui::BeginDisabled(s_ui.upscale < 0);
    { bool up = s_ui.upscale > 0;
      if (ImGui::Checkbox(L("MetalFX upscaling to the window", "Escala MetalFX até a janela"), &up)) {
          rsx_metal_gfx_set_upscale(up);
          s_ui.upscale = up;
          set_value(V_UPSCALE, "%s", up ? "metalfx" : "off");
      } }
    ImGui::EndDisabled();
    env_mark(V_UPSCALE);
    if (s_ui.upscale < 0)
        hint(s_ui.host ? L("Not available: MetalFX is missing or failed on this Mac (see the log).",
                           "Indisponível: o MetalFX falta ou falhou neste Mac (veja o log).")
                       : L("Only with the game's window (the launcher, tools/host_window).",
                           "Só com a janela do jogo (o launcher, tools/host_window)."));

    static const int an_v[] = { 0, 2, 4, 8, 16 };
    static const char* const an_en[] = { "off", "2x", "4x", "8x", "16x" };
    static const char* const an_pt[] = { "desligado", "2x", "4x", "8x", "16x" };
    int ai = 0;
    for (int i = 0; i < 5; i++) if (s_ui.aniso >= an_v[i]) ai = i;
    if (ImGui::Combo(L("Anisotropic filtering", "Filtragem anisotrópica"), &ai, s_pt ? an_pt : an_en, 5)) {
        s_ui.aniso = an_v[ai];
        set_value(V_ANISO, "%d", s_ui.aniso);
    }
    env_mark(V_ANISO);
    restart_note(s_ui.aniso != s_ui.aniso_start, NULL, NULL);

    /* PS3_RES_SCALE: the game's render targets at 1.5, 2 or 3 times 720p. */
    ImGui::BeginDisabled(s_ui.res_start < 0);
    { const char* rn_en[4] = { "720p (native)", "1080p (1.5x)", "1440p (2x)", "4K (3x)" };
      const char* rn_pt[4] = { "720p (nativa)", "1080p (1,5x)", "1440p (2x)", "4K (3x)" };
      int ri = s_ui.res < 0 ? 0 : s_ui.res;
      if (ImGui::Combo(L("Internal resolution", "Resolução interna"), &ri, s_pt ? rn_pt : rn_en, 4)) {
          s_ui.res = ri;
          set_value(V_RES_SCALE, "%s", kResScale[ri]);
      } }
    ImGui::EndDisabled();
    env_mark(V_RES_SCALE);
    if (s_ui.res_start < 0)
        hint(L("Not available here (the vtable path).", "Indisponível aqui (caminho vtable)."));
    else
        restart_note(s_ui.res != s_ui.res_start, "Higher costs GPU time (see the HUD).",
                     "Mais alta custa tempo de GPU (veja o HUD).");

    section(L("Display", "Tela"));
    ImGui::BeginDisabled(s_ui.vsync < 0);
    { bool v = s_ui.vsync > 0;
      if (ImGui::Checkbox(L("Vsync", "Vsync (sincronia vertical)"), &v)) {
          rsx_metal_set_vsync(v); s_ui.vsync = v;
          set_value(V_VSYNC, "%d", v ? 1 : 0);
      } }
    ImGui::EndDisabled();
    env_mark(V_VSYNC);
    ImGui::BeginDisabled(s_ui.wmode < 0);
    { const char* modes[] = { L("Window", "Janela"), L("Fullscreen (macOS)", "Tela cheia (macOS)"),
                              L("Borderless fullscreen", "Tela cheia sem bordas") };
      const int n_modes = (s_ui.wmode_caps & 4) ? 3 : 2;
      int m = s_ui.wmode < 0 ? 0 : s_ui.wmode;
      if (ImGui::Combo(L("Display mode", "Modo de exibição"), &m, modes, n_modes)) {
          rsx_metal_set_window_mode(m); s_ui.wmode = m;
          static const char* const names[] = { "windowed", "fullscreen", "borderless" };
          set_value(V_WINDOW_MODE, "%s", names[m]);
      } }
    ImGui::EndDisabled();
    env_mark(V_WINDOW_MODE);
    if (s_ui.vsync < 0 && s_ui.wmode < 0)
        hint(L("Not available here (no window, or a launcher that predates the menu).",
               "Indisponível aqui (sem janela, ou um launcher mais antigo que o menu)."));
}

static void tab_audio(void)
{
    section(L("Sound", "Som"));
    if (ImGui::SliderInt(L("Master volume", "Volume geral"), &s_ui.volume, 0, 100, "%d%%")) {
        cellAudio_host_set_volume(s_ui.volume);
        set_value(V_VOLUME, "%d", s_ui.volume);
    }
    env_mark(V_VOLUME);
    hint(L("What the Mac plays; the game's own mix and its volume options are untouched.",
           "O que o Mac toca; a mixagem do jogo e as opções de volume dele não mudam."));
}

/* ---- the Controls tab: the bindings table ----------------------------------- */
/* PS3 controls in the table's order, with their labels. */
static const struct { int ctl; const char* en; const char* pt; } kRows[] = {
    { PIM_CROSS, "Cross", "Xis" }, { PIM_CIRCLE, "Circle", "Bola" },
    { PIM_SQUARE, "Square", "Quadrado" }, { PIM_TRIANGLE, "Triangle", "Triângulo" },
    { PIM_L1, "L1", "L1" }, { PIM_R1, "R1", "R1" }, { PIM_L2, "L2", "L2" }, { PIM_R2, "R2", "R2" },
    { PIM_L3, "L3", "L3" }, { PIM_R3, "R3", "R3" }, { PIM_START, "START", "START" }, { PIM_SELECT, "SELECT", "SELECT" },
    { PIM_UP, "D-pad up", "Direcional cima" }, { PIM_DOWN, "D-pad down", "Direcional baixo" },
    { PIM_LEFT, "D-pad left", "Direcional esquerda" }, { PIM_RIGHT, "D-pad right", "Direcional direita" },
    { PIM_LS_UP, "Left stick up", "Analógico esq. cima" }, { PIM_LS_DOWN, "Left stick down", "Analógico esq. baixo" },
    { PIM_LS_LEFT, "Left stick left", "Analógico esq. esquerda" }, { PIM_LS_RIGHT, "Left stick right", "Analógico esq. direita" },
    { PIM_RS_UP, "Right stick up", "Analógico dir. cima" }, { PIM_RS_DOWN, "Right stick down", "Analógico dir. baixo" },
    { PIM_RS_LEFT, "Right stick left", "Analógico dir. esquerda" }, { PIM_RS_RIGHT, "Right stick right", "Analógico dir. direita" },
};
static const char* row_label(int ctl)
{
    for (auto& r : kRows) if (r.ctl == ctl) return L(r.en, r.pt);
    return "?";
}
static const char* mouse_input_label(uint32_t bit)
{
    switch (bit) {
    case 1u: return L("Left", "Esquerdo");   case 2u: return L("Right", "Direito");
    case 4u: return L("Middle", "Meio");
    case PIM_MOUSE_WHEEL_UP: return L("Wheel up", "Roda p/ cima");
    case PIM_MOUSE_WHEEL_DOWN: return L("Wheel down", "Roda p/ baixo");
    default: break;
    }
    static char b[8][24];
    for (int i = 3; i < 8; i++) if (bit == 1u << i) {
        snprintf(b[i], sizeof b[i], L("Button %d", "Botão %d"), i + 1); return b[i]; }
    return "?";
}
/* SDL controller input -> what it is on a PlayStation pad. */
static const char* pad_input_label(int bit)
{
    switch (bit) {
    case 0: return L("Cross", "Xis");     case 1: return L("Circle", "Bola");
    case 2: return L("Square", "Quadrado"); case 3: return L("Triangle", "Triângulo");
    case 4: return "Create/Share";        case 5: return "PS";
    case 6: return "Options";             case 7: return "L3";
    case 8: return "R3";                  case 9: return "L1";
    case 10: return "R1";
    case 11: return L("D-pad up", "Direcional cima");      case 12: return L("D-pad down", "Direcional baixo");
    case 13: return L("D-pad left", "Direcional esq.");    case 14: return L("D-pad right", "Direcional dir.");
    case 15: return L("Mic", "Microfone");
    case 16: return "Paddle 1"; case 17: return "Paddle 2"; case 18: return "Paddle 3"; case 19: return "Paddle 4";
    case 20: return "Touchpad";
    case PIM_PAD_TRIGGER_LEFT: return "L2"; case PIM_PAD_TRIGGER_RIGHT: return "R2";
    default: return "?";
    }
}
/* One cell's text: the inputs of control `ctl` on device `dev`. */
static void cell_text(const PadInputMap& m, int dev, int ctl, char* out, size_t n)
{
    size_t len = 0; out[0] = 0;
    auto add = [&](const char* name) {
        int w = snprintf(out + len, n - len, "%s%s", len ? ", " : "", name);
        if (w > 0) len += (size_t)w;
        if (len >= n) len = n - 1;
    };
    char tmp[8];
    if (dev == CAP_KEYS) {
        for (unsigned vk = 0; vk < 128; vk++)
            if (m.key[ctl][vk >> 5] >> (vk & 31) & 1u) add(pim_key_label(vk, tmp, sizeof tmp));
    } else if (dev == CAP_MOUSE) {
        for (int b = 0; b < 32; b++) if (m.mouse[ctl] >> b & 1u) add(mouse_input_label(1u << b));
    } else {
        for (int b = 0; b < 32; b++) if (m.pad[ctl] >> b & 1u) add(pad_input_label(b));
    }
    if (!len) snprintf(out, n, "-");
}

static char s_bind_note[256];

/* A change to the live map: on to cellPad at once, then into the file. */
static void bindings_apply(const PadInputMap& m)
{
    cellPad_host_set_input_map(&m);
    const int r = cellPad_host_save_input_map();
    char path[1024];
    cellPad_host_input_map_path(path, sizeof path);
    const char* base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (r > 0) snprintf(s_bind_note, sizeof s_bind_note, L("Saved to %s.", "Salvo em %s."), base);
    else if (r == 0) snprintf(s_bind_note, sizeof s_bind_note, "%s",
                              L("Applies now; not saved (no PS3_INPUT_MAP, no PS3_OVERLAY_INI).",
                                "Vale agora; não foi salvo (sem PS3_INPUT_MAP nem PS3_OVERLAY_INI)."));
    else snprintf(s_bind_note, sizeof s_bind_note, L("Applies now, but %s could not be written.",
                                                     "Vale agora, mas não foi possível gravar %s."), path);
}

/* Presenting thread, every frame the menu is open: a key, mouse button or
 * wheel turn from the window's block, or a newly pressed controller button,
 * ends a capture; it gives up after kCapTimeout. */
static void cap_poll(const uint32_t keys[4], uint32_t mouse, int32_t wheel)
{
    if (s_cap_dev < 0) return;
    if (s_cap_res == CAP_PENDING) {
        /* keys pressed since the capture started (the one that chose the
         * cell, Enter or a click, does not count while it is held) */
        for (int w = 0; w < 4 && s_cap_res == CAP_PENDING; w++) {
            const uint32_t fresh = keys[w] & ~s_cap_key_base[w];
            s_cap_key_base[w] &= keys[w];
            for (int b = 0; fresh && b < 32; b++) {
                if (!(fresh >> b & 1u)) continue;
                const unsigned vk = (unsigned)(w * 32 + b);
                if (vk == VK_F1 || vk == VK_INSERT) continue;   /* the menu's own */
                if (vk == VK_ESC) cap_finish(CAP_CANCEL, 0);
                else if (s_cap_dev == CAP_KEYS) cap_finish(CAP_BIND, vk);
                else if (vk == VK_BACKSPACE || vk == VK_DELETE) cap_finish(CAP_UNBIND, 0);
                break;
            }
        }
        if (s_cap_res == CAP_PENDING && s_cap_dev == CAP_MOUSE) {
            const uint32_t fresh = mouse & ~s_cap_mouse_base;
            s_cap_mouse_base &= mouse;
            s_cap_wheel += wheel / 16.0;
            if (fresh) cap_finish(CAP_BIND, 1u << __builtin_ctz(fresh));
            else if (s_cap_wheel >= 1.0) cap_finish(CAP_BIND, PIM_MOUSE_WHEEL_UP);
            else if (s_cap_wheel <= -1.0) cap_finish(CAP_BIND, PIM_MOUSE_WHEEL_DOWN);
        }
        if (s_cap_res == CAP_PENDING && s_cap_dev == CAP_PAD) {
            const uint32_t raw = g_pad_overlay_raw;
            const uint32_t fresh = raw & ~s_cap_pad_base;
            s_cap_pad_base &= raw;            /* let go of a held one: it counts when pressed again */
            if (fresh) cap_finish(CAP_BIND, (uint32_t)__builtin_ctz(fresh));
        }
        if (s_cap_res == CAP_PENDING && now_s() - s_cap_t0 > kCapTimeout) s_cap_res = CAP_CANCEL;
    }
    if (s_cap_res == CAP_PENDING) return;
    const int dev = s_cap_dev, ctl = s_cap_ctl, res = s_cap_res;
    const uint32_t val = s_cap_val;
    s_cap_dev = -1;
    if (res == CAP_CANCEL) return;
    static const int sec[3] = { PIM_SEC_KEYBOARD, PIM_SEC_MOUSE_BUTTONS, PIM_SEC_CONTROLLER };
    PadInputMap m;
    cellPad_host_input_map(&m);
    uint32_t taken = 0;
    if (res == CAP_BIND) taken = pim_rebind(&m, sec[dev], ctl, val);
    else pim_unbind(&m, sec[dev], ctl);
    bindings_apply(m);
    if (taken) {   /* say where it came from */
        char from[160] = ""; size_t len = 0;
        for (auto& r : kRows)
            if (taken >> r.ctl & 1u) {
                int w = snprintf(from + len, sizeof from - len, "%s%s", len ? ", " : "", L(r.en, r.pt));
                if (w > 0) len += (size_t)w;
                if (len >= sizeof from) len = sizeof from - 1;
            }
        const size_t n0 = strlen(s_bind_note);
        snprintf(s_bind_note + n0, sizeof s_bind_note - n0, L(" Taken from: %s.", " Tirado de: %s."), from);
    }
}

static void cap_start(int dev, int ctl, const uint32_t keys[4], uint32_t mouse)
{
    s_cap_dev = dev; s_cap_ctl = ctl; s_cap_res = CAP_PENDING; s_cap_val = 0;
    s_cap_t0 = now_s(); s_cap_wheel = 0;
    s_cap_pad_base = g_pad_overlay_raw;   /* what is held now (the Cross that chose the cell) does not count */
    s_cap_mouse_base = mouse | 1u;        /* nor the click */
    for (int i = 0; i < 4; i++) s_cap_key_base[i] = keys[i];
    s_bind_note[0] = 0;
}

static uint32_t s_keys_now[4], s_mouse_now;   /* this frame's, for cap_start */

static void bindings_table(void)
{
    PadInputMap m;
    cellPad_host_input_map(&m);
    const int cap_dev = cap_device(), cap_ctl = s_cap_ctl;
    const double left = kCapTimeout - (now_s() - s_cap_t0);

    const ImVec4 orange(1.0f, 0.60f, 0.20f, 1.0f);
    if (cap_dev >= 0) {
        char what[160];
        snprintf(what, sizeof what,
                 cap_dev == CAP_KEYS  ? L("%s: press a key (Esc cancels, %.0f s)", "%s: aperte uma tecla (Esc cancela, %.0f s)") :
                 cap_dev == CAP_MOUSE ? L("%s: click a mouse button or turn the wheel (Esc cancels, Backspace unbinds, %.0f s)",
                                          "%s: clique um botão do mouse ou gire a roda (Esc cancela, Backspace limpa, %.0f s)") :
                                        L("%s: press a button on the controller (Esc cancels, %.0f s)",
                                          "%s: aperte um botão do controle (Esc cancela, %.0f s)"),
                 row_label(cap_ctl), left > 0 ? left : 0.0);
        ImGui::TextColored(orange, "%s", what);
        if (cap_dev != CAP_MOUSE) {
            if (ImGui::SmallButton(L("Unbind", "Limpar"))) cap_finish(CAP_UNBIND, 0);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("Cancel", "Cancelar"))) cap_finish(CAP_CANCEL, 0);
        }
    } else {
        hint(L("Click a cell (or pick it with the controller and press Cross), then press the new key, mouse button "
               "or controller button. Right-click a cell to unbind it. The change applies at once.",
               "Clique numa célula (ou escolha com o controle e aperte Xis) e aperte a nova tecla, botão do mouse "
               "ou botão do controle. Botão direito numa célula a limpa. A mudança vale na hora."));
    }

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("binds", 4, tf)) {
        ImGui::TableSetupColumn(L("PS3", "PS3"), ImGuiTableColumnFlags_WidthStretch, 1.25f);
        ImGui::TableSetupColumn(L("Keyboard", "Teclado"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(L("Mouse", "Mouse"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(L("Controller", "Controle"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        char text[160], id[200];
        for (auto& r : kRows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(L(r.en, r.pt));
            for (int dev = 0; dev < 3; dev++) {
                ImGui::TableSetColumnIndex(dev + 1);
                const bool here = cap_dev == dev && cap_ctl == r.ctl;
                if (here) snprintf(text, sizeof text, "%s", L("press...", "aperte..."));
                else cell_text(m, dev, r.ctl, text, sizeof text);
                snprintf(id, sizeof id, "%s##b%d_%d", text, dev, r.ctl);
                if (here) ImGui::PushStyleColor(ImGuiCol_Text, orange);
                if (ImGui::Button(id, ImVec2(-FLT_MIN, 0)) && cap_dev < 0) cap_start(dev, r.ctl, s_keys_now, s_mouse_now);
                if (here) ImGui::PopStyleColor();
                if (!here && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) &&
                    ImGui::CalcTextSize(text).x > ImGui::GetItemRectSize().x - 8.0f)
                    ImGui::SetTooltip("%s", text);   /* the whole list when the cell cuts it */
                if (cap_dev < 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    PadInputMap u = m;
                    static const int sec[3] = { PIM_SEC_KEYBOARD, PIM_SEC_MOUSE_BUTTONS, PIM_SEC_CONTROLLER };
                    pim_unbind(&u, sec[dev], r.ctl);
                    bindings_apply(u);
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(cap_dev >= 0);
    if (ImGui::Button(L("Restore defaults", "Restaurar padrões"))) {
        PadInputMap d;
        pim_defaults(&d);
        bindings_apply(d);
        snprintf(s_bind_note + strlen(s_bind_note), sizeof s_bind_note - strlen(s_bind_note), "%s",
                 L(" Default layout restored.", " Layout padrão restaurado."));
    }
    ImGui::EndDisabled();
    if (s_bind_note[0]) hint(s_bind_note);
    hint(L("Esc, F1, Insert and the Cmd shortcuts belong to the menu and the window and cannot be bound. "
           "The controller's sticks always move the PS3 sticks.",
           "Esc, F1, Insert e os atalhos com Cmd são do menu e da janela e não podem ser usados. "
           "Os analógicos do controle sempre movem os analógicos do PS3."));
}

static void tab_controls(void)
{
    section(L("Play with", "Jogar com"));
    { const char* modes[] = { L("Controller, keyboard and mouse", "Controle, teclado e mouse"),
                              L("Controller only", "Só o controle"),
                              L("Keyboard and mouse only", "Só teclado e mouse") };
      if (ImGui::Combo(L("Input", "Entrada"), &s_ui.input_mode, modes, 3)) {
          cellPad_host_set_input_mode(s_ui.input_mode);
          static const char* const names[] = { "both", "controller", "keyboard" };
          set_value(V_INPUT_MODE, "%s", names[s_ui.input_mode]);
      } }
    env_mark(V_INPUT_MODE);
    hint(L("Applies at once. With the keyboard and mouse only, L3+R3 on a controller still opens this menu.",
           "Vale na hora. Com só teclado e mouse, L3+R3 no controle ainda abre este menu."));

    section(L("Controller", "Controle"));
    if (ImGui::Checkbox(L("Vibration", "Vibração"), &s_ui.rumble)) {
        cellPad_host_set_rumble_strength(s_ui.rumble ? s_ui.rumble_strength : 0);
        set_value(V_RUMBLE, "%d", s_ui.rumble ? 1 : 0);
        if (s_ui.rumble) set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength);
    }
    env_mark(V_RUMBLE);
    ImGui::BeginDisabled(!s_ui.rumble);
    if (ImGui::SliderInt(L("Vibration strength", "Força da vibração"), &s_ui.rumble_strength, 10, 200, "%d%%")) {
        cellPad_host_set_rumble_strength(s_ui.rumble_strength);
        set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength);
    }
    ImGui::EndDisabled();
    env_mark(V_RUMBLE_STRENGTH);
    hint(L("Takes effect with the game's next vibration. Above 100% strengthens light rumble.",
           "Vale na próxima vibração do jogo. Acima de 100% reforça vibrações fracas."));

    section(L("Mouse camera", "Câmera no mouse"));
    { bool on = s_ui.mouse_on != 0;
      if (ImGui::Checkbox(L("The mouse turns the camera", "O mouse gira a câmera"), &on)) {
          s_ui.mouse_on = on;
          cellPad_host_set_mouse_camera(on);
          set_value(V_MOUSE, "%d", on ? 1 : 0);
      } }
    env_mark(V_MOUSE);
    ImGui::BeginDisabled(!s_ui.mouse_on);
    bool ch = false;
    if (ImGui::SliderFloat(L("Sensitivity", "Sensibilidade"), &s_ui.mouse_sens, 0.10f, 5.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic)) {
        ch = true; set_value(V_MOUSE_SENS, "%.2f", s_ui.mouse_sens);
    }
    env_mark(V_MOUSE_SENS);
    if (ImGui::Checkbox(L("Invert vertical", "Inverter vertical"), &s_ui.inv_y)) { ch = true; set_value(V_MOUSE_INVERT_Y, "%d", s_ui.inv_y ? 1 : 0); }
    env_mark(V_MOUSE_INVERT_Y);
    ImGui::SameLine(0, 30);
    if (ImGui::Checkbox(L("Invert horizontal", "Inverter horizontal"), &s_ui.inv_x)) { ch = true; set_value(V_MOUSE_INVERT_X, "%d", s_ui.inv_x ? 1 : 0); }
    env_mark(V_MOUSE_INVERT_X);
    if (ch) cellPad_host_set_mouse_settings(s_ui.mouse_sens, s_ui.inv_x, s_ui.inv_y);
    ImGui::EndDisabled();
    if (!s_ui.mouse_on) hint(L("Off: the mouse buttons do nothing in the game either.",
                               "Desligada: os botões do mouse também não fazem nada no jogo."));

    section(L("Buttons", "Botões"));
    bindings_table();

    section(L("This menu", "Este menu"));
    hint(L("Opens and closes with F1 (fn+F1 on a Mac keyboard), Insert or Cmd+, ; L3+R3 together on a controller. "
           "Esc or Circle closes it. While it is open the game gets no input (it keeps running: pause it first with START). "
           "L1/R1 switch tabs.",
           "Abre e fecha com F1 (fn+F1 no teclado do Mac), Insert ou Cmd+, ; L3+R3 juntos no controle. "
           "Esc ou Bola fecha. Aberto, o jogo não recebe comandos (mas continua rodando: pause antes com START). "
           "L1/R1 trocam de aba."));
}

static void stats_lines(bool compact)
{
    const Stats& st = s_stats;
    if (!st.valid) { ImGui::TextUnformatted(L("measuring...", "medindo...")); return; }
    ImGui::Text("%.1f fps   %.1f ms (max %.0f)", st.fps, st.avg_ms, st.max_ms);
    if (s_hud_graph || !compact) {
        float buf[FT_N]; const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
        for (int i = 0; i < n; i++) buf[i] = s_ft[(s_ft_i - n + i + FT_N) % FT_N];
        char ov[32]; snprintf(ov, sizeof ov, "%.0f ms", st.max_ms);
        ImGui::PlotLines("##ft", buf, n, 0, ov, 0.0f, st.max_ms > 50.0 ? (float)st.max_ms : 50.0f,
                         ImVec2(compact ? 230.0f : 420.0f, compact ? 40.0f : 70.0f));
    }
    ImGui::Text(L("CPU %.1f cores, %d busy threads", "CPU %.1f núcleos, %d threads ocupadas"), st.cores, st.busy);
    ImGui::Text(L("RSX encode %.1f ms, GPU wait %.1f ms", "RSX codifica %.1f ms, espera GPU %.1f ms"), st.enc_ms, st.wait_ms);
}

static void tab_perf(void)
{
    section(L("Performance overlay", "Painel de desempenho"));
    { bool h = s_hud;
      if (ImGui::Checkbox(L("Show it while playing", "Mostrar durante o jogo"), &h)) {
          s_hud = h; set_value(V_HUD, "%d", h ? 1 : 0); update_active();
      } }
    env_mark(V_HUD);
    const char* corners[] = { L("Top left", "Superior esquerdo"), L("Top right", "Superior direito"),
                              L("Bottom left", "Inferior esquerdo"), L("Bottom right", "Inferior direito") };
    if (ImGui::Combo(L("Corner", "Canto"), &s_hud_corner, corners, 4)) set_value(V_HUD_CORNER, "%d", s_hud_corner);
    if (ImGui::SliderFloat(L("Opacity", "Opacidade"), &s_hud_opacity, 0.1f, 1.0f, "%.2f")) set_value(V_HUD_OPACITY, "%.2f", s_hud_opacity);
    if (ImGui::Checkbox(L("Frame time graph", "Gráfico do tempo de quadro"), &s_hud_graph)) set_value(V_HUD_GRAPH, "%d", s_hud_graph ? 1 : 0);
    section(L("Now", "Agora"));
    stats_lines(false);
    hint(L("fps and frame time count the presented frames. Busy threads: those using 80% or more of a core "
           "(here mostly the SPU threads and the PPU). RSX encode: CPU time turning a frame's draws into Metal "
           "commands; GPU wait: time the presenter waited for a frame in flight. Timelines: PS3_FTRACE.",
           "fps e tempo de quadro contam os quadros apresentados. Threads ocupadas: as que usam 80% ou mais de um núcleo "
           "(aqui sobretudo as das SPUs e a PPU). RSX codifica: tempo de CPU para transformar os draws de um quadro em "
           "comandos Metal; espera GPU: tempo que o apresentador esperou um quadro em voo. Linhas do tempo: PS3_FTRACE."));
}

/* "Quit game?": Quit saves the menu's settings (closing it does that), then
 * ps3_host_quit ends the game the way closing the window does: after the
 * save or checkpoint write in progress, exit status 0. Cancel has the
 * focus, so a stray Cross does not quit. */
static void quit_confirm(void)
{
    if (s_quit_ask == 1) { ImGui::OpenPopup("###quitask"); s_quit_ask = 2; }
    if (s_quit_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    char title[64];
    snprintf(title, sizeof title, "%s###quitask", L("Quit the game?", "Sair do jogo?"));
    if (!ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings)) {
        s_quit_ask = 0; s_quit_cancel = false; return;
    }
    ImGui::TextUnformatted(L("Progress since the last checkpoint will be lost.",
                             "O progresso desde o último checkpoint será perdido."));
    hint(L("A save or checkpoint being written is finished first.",
           "Um salvamento ou checkpoint sendo gravado termina antes."));
    ImGui::Spacing();
    const bool quit = ImGui::Button(L("Quit", "Sair")) || s_quit_press;
    ImGui::SameLine();
    const bool cancel = ImGui::Button(L("Cancel", "Cancelar"));
    ImGui::SetItemDefaultFocus();   /* Cancel */
    if (cancel || s_quit_cancel) {
        s_quit_ask = 0; s_quit_cancel = false;
        ImGui::CloseCurrentPopup();
    } else if (quit) {
        s_quit_press = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        do_quit();
        return;
    }
    ImGui::EndPopup();
}

static void draw_menu(void)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size(700.0f, 660.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(size.x < io.DisplaySize.x - 20 ? size.x : io.DisplaySize.x - 20,
                                    size.y < io.DisplaySize.y - 20 ? size.y : io.DisplaySize.y - 20), ImGuiCond_Always);
    char title[96];
    snprintf(title, sizeof title, "%s  -  %s###ps3menu", s_title, L("Menu", "Menu"));
    if (!ImGui::Begin(title, NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End(); return;
    }
    const char* names[kTabs] = { L("Image", "Imagem"), L("Audio", "Áudio"), L("Controls", "Controles"),
                                 L("Performance", "Desempenho") };
    if (ImGui::BeginTabBar("tabs")) {
        for (int i = 0; i < kTabs; i++) {
            const ImGuiTabItemFlags f = (s_tab_want == i) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(names[i], NULL, f)) {
                s_tab = i;
                ImGui::BeginChild("page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
                switch (i) { case 0: tab_image(); break; case 1: tab_audio(); break;
                             case 2: tab_controls(); break; default: tab_perf(); break; }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        s_tab_want = -1;
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button(L("Close", "Fechar"))) set_open(0);
    ImGui::SameLine();
    if (ImGui::Button(L("Quit game", "Sair do jogo"))) s_quit_ask = 1;
    ImGui::SameLine();
    hint(s_ini_path[0] ? L("Changes apply at once and are saved when the menu closes.",
                           "As mudanças valem na hora e são salvas ao fechar o menu.")
                       : L("Changes apply at once (not saved: no PS3_OVERLAY_INI).",
                           "As mudanças valem na hora (não são salvas: sem PS3_OVERLAY_INI)."));
    quit_confirm();
    ImGui::End();
}

static void draw_hud(void)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float pad = 12.0f;
    const ImVec2 pos((s_hud_corner & 1) ? io.DisplaySize.x - pad : pad, (s_hud_corner & 2) ? io.DisplaySize.y - pad : pad);
    const ImVec2 pivot((s_hud_corner & 1) ? 1.0f : 0.0f, (s_hud_corner & 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(s_hud_opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));
    if (ImGui::Begin("##hud", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::PushFont(NULL, 15.0f);
        stats_lines(true);
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void draw_notices(double now)
{
    Notice n[3];
    pthread_mutex_lock(&s_notice_lock);
    memcpy(n, s_notice, sizeof n);
    pthread_mutex_unlock(&s_notice_lock);
    const ImGuiIO& io = ImGui::GetIO();
    /* Opposite the HUD's corner, at the bottom. */
    const bool left = (s_hud_corner == 3);
    float y = io.DisplaySize.y - 14.0f;
    for (int i = 0; i < 3; i++) {
        if (n[i].until <= now) continue;
        const float a = (float)((n[i].until - now) < 0.5 ? (n[i].until - now) / 0.5 : 1.0);
        ImGui::SetNextWindowPos(ImVec2(left ? 14.0f : io.DisplaySize.x - 14.0f, y), ImGuiCond_Always,
                                ImVec2(left ? 0.0f : 1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.85f * a);
        char id[16]; snprintf(id, sizeof id, "##note%d", i);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        if (ImGui::Begin(id, NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted(n[i].text);
            y -= ImGui::GetWindowHeight() + 6.0f;
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

/* ---- input: the window's block and the controller -------------------------------
 * The window (host_input_cocoa.h) records keys as a bitmap, counts the menu's
 * keys (F1, Insert, Cmd+,) in menu_toggle so a tap between two frames is not
 * lost, and while the menu is open keeps the pointer, the buttons, the left
 * clicks and the wheel for it. */
static ImGuiKey key_from_vk(unsigned vk)
{
    switch (vk) {
    case 0x7E: return ImGuiKey_UpArrow;    case 0x7D: return ImGuiKey_DownArrow;
    case 0x7B: return ImGuiKey_LeftArrow;  case 0x7C: return ImGuiKey_RightArrow;
    case 0x30: return ImGuiKey_Tab;        case 0x24: return ImGuiKey_Enter;
    case 0x4C: return ImGuiKey_KeypadEnter; case 0x31: return ImGuiKey_Space;
    case 0x33: return ImGuiKey_Backspace;  case 0x75: return ImGuiKey_Delete;
    case 0x73: return ImGuiKey_Home;       case 0x77: return ImGuiKey_End;
    case 0x74: return ImGuiKey_PageUp;     case 0x79: return ImGuiKey_PageDown;
    case 0x38: case 0x3C: return ImGuiMod_Shift;
    case 0x3B: case 0x3E: return ImGuiMod_Ctrl;
    case 0x3A: case 0x3D: return ImGuiMod_Alt;
    default:   return ImGuiKey_None;
    }
}

/* Toggles from the keyboard (the window's count) and L3+R3 (cellPad's). */
static void poll_toggles(ps3_host_input* in)
{
    const uint32_t t = __atomic_load_n(&g_pad_overlay_toggle, __ATOMIC_ACQUIRE);
    if (t != s_seen_toggle) { s_seen_toggle = t; set_open(!s_open); }
    if (in) {
        const uint32_t k = atomic_load(&in->menu_toggle);
        if (k != s_seen_keys_toggle) {
            /* an odd number of presses since the last frame toggles */
            if ((k - s_seen_keys_toggle) & 1u) set_open(!s_open);
            s_seen_keys_toggle = k;
        }
    }
}

static void feed_input(ImGuiIO& io, ps3_host_input* in)
{
    static uint32_t prev_keys[4], prev_mouse, seen_clicks;
    static bool primed;
    uint32_t keys[4] = { 0, 0, 0, 0 }, mouse = 0, clicks = 0, ptr = PS3_HOST_POINTER_NONE;
    int32_t wheel = 0;
    if (in) {
        for (int i = 0; i < 4; i++) keys[i] = atomic_load(&in->keys[i]);
        mouse  = atomic_load(&in->mouse_buttons);
        clicks = atomic_load(&in->menu_clicks);
        ptr    = atomic_load(&in->pointer);
        wheel  = atomic_exchange(&in->wheel, 0);
    }
    if (!primed) { memcpy(prev_keys, keys, sizeof keys); prev_mouse = mouse; seen_clicks = clicks; primed = true; }
    memcpy(s_keys_now, keys, sizeof keys); s_mouse_now = mouse;
    const bool capturing = cap_device() >= 0;
    if (s_open) cap_poll(keys, mouse, wheel);

    /* Keys: edges of the bitmap. While a cell waits for its new input the
     * keys go to the capture only. */
    for (int w = 0; w < 4; w++) {
        const uint32_t ch = keys[w] ^ prev_keys[w];
        for (int b = 0; ch && b < 32; b++) {
            if (!(ch >> b & 1u)) continue;
            const unsigned vk = (unsigned)(w * 32 + b);
            const bool down = keys[w] >> b & 1u;
            if (vk == VK_ESC) {
                if (down && s_open && !capturing) {
                    if (s_quit_ask) s_quit_cancel = true;
                    else set_open(0);
                }
                continue;
            }
            if (capturing && down) continue;
            const ImGuiKey k = key_from_vk(vk);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
        }
    }
    memcpy(prev_keys, keys, sizeof keys);

    /* The pointer, over the picture, in the menu's units. */
    if (ptr != PS3_HOST_POINTER_NONE) {
        double nx, ny;
        ps3_host_input_pointer_unpack(ptr, &nx, &ny);
        io.AddMousePosEvent((float)(nx * io.DisplaySize.x), (float)(ny * io.DisplaySize.y));
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    /* Buttons: left 1, right 2, middle 4. A click that went down and up
     * between two frames (a slow frame) still clicks. */
    const uint32_t mb[3] = { 1u, 2u, 4u };
    const bool missed_click = clicks != seen_clicks && !(mouse & 1u) && !(prev_mouse & 1u);
    seen_clicks = clicks;
    if (!capturing) {
        if (missed_click) { io.AddMouseButtonEvent(0, true); io.AddMouseButtonEvent(0, false); }
        for (int b = 0; b < 3; b++)
            if ((mouse ^ prev_mouse) & mb[b]) io.AddMouseButtonEvent(b, (mouse & mb[b]) != 0);
        if (wheel) io.AddMouseWheelEvent(0.0f, (float)wheel / 16.0f);
    }
    prev_mouse = mouse;

    /* The controller, as cellPad saw it before holding the game's copy neutral.
     * While a cell waits for its new input the pad works nothing in the menu
     * (its next button is the binding, see cap_poll). */
    static uint16_t prev, held_off;
    const uint32_t nav = capturing ? 0x80800000u : g_pad_overlay_nav;
    const uint32_t nav2 = capturing ? 0x8080u : g_pad_overlay_nav2;
    /* ... nor does the button that ended it, until it is let go (Cross bound
     * to a cell would otherwise press that cell again at once). */
    const uint16_t b_now = (uint16_t)g_pad_overlay_nav;
    held_off = capturing ? b_now : (uint16_t)(held_off & b_now);
    const uint16_t b = (uint16_t)nav & (uint16_t)~held_off;
    const int lx = (nav >> 16) & 255, ly = (nav >> 24) & 255, ry = (nav2 >> 8) & 255;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (b & PAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (b & PAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (b & PAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (b & PAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (b & PAD_CROSS) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (b & PAD_CIRCLE) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  (b & PAD_SQUARE) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    (b & PAD_TRIANGLE) != 0);
    auto axis = [](int v, int dir) {   /* dir -1: towards 0, +1: towards 255 */
        float f = dir < 0 ? (128 - v) / 127.0f : (v - 128) / 127.0f;
        f = (f - 0.25f) / 0.75f;
        return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    };
    struct { ImGuiKey k; float v; } sticks[] = {
        { ImGuiKey_GamepadLStickLeft, axis(lx, -1) }, { ImGuiKey_GamepadLStickRight, axis(lx, 1) },
        { ImGuiKey_GamepadLStickUp, axis(ly, -1) },   { ImGuiKey_GamepadLStickDown, axis(ly, 1) },
        { ImGuiKey_GamepadRStickUp, axis(ry, -1) },   { ImGuiKey_GamepadRStickDown, axis(ry, 1) },
    };
    for (auto& s : sticks) io.AddKeyAnalogEvent(s.k, s.v > 0.1f, s.v);
    const uint16_t pressed = b & (uint16_t)~prev;
    prev = b;
    if (s_quit_ask) { if (pressed & PAD_CIRCLE) s_quit_cancel = true; return; }
    if (pressed & PAD_L1) s_tab_want = (s_tab + kTabs - 1) % kTabs;
    if (pressed & PAD_R1) s_tab_want = (s_tab + 1) % kTabs;
    if ((pressed & PAD_CIRCLE) && !ImGui::IsAnyItemActive()) set_open(0);
}

/* PS3_OVERLAY_TEST: the steps whose time has come. */
static void test_run(double now)
{
    for (int i = 0; i < s_test_n; i++) {
        TestStep& st = s_test[i];
        if (st.done || now - s_t0 < st.t) continue;
        st.done = true;
        fprintf(stderr, "[overlay] test %.0f s: %s%s%s\n", st.t, st.act, st.arg[0] ? "=" : "", st.arg);
        if (!strcmp(st.act, "open")) set_open(1);
        else if (!strcmp(st.act, "close")) set_open(0);
        else if (!strcmp(st.act, "tab")) { set_open(1); s_tab_want = atoi(st.arg) % kTabs; }
        else if (!strcmp(st.act, "hud")) { s_hud = atoi(st.arg) != 0; set_value(V_HUD, "%d", s_hud ? 1 : 0); }
        else if (!strcmp(st.act, "quit")) { set_open(1); s_quit_ask = 1; }
        else if (!strcmp(st.act, "confirm")) s_quit_press = true;
        else if (!strcmp(st.act, "cancel")) s_quit_cancel = true;
        else if (!strcmp(st.act, "shot")) { snprintf(s_shot, sizeof s_shot, "%s", st.arg); s_shot_ready = 0; s_shot[sizeof s_shot - 1] = 0; }
        else if (!strcmp(st.act, "set")) {
            char* colon = strchr(st.arg, ':');
            if (!colon) continue;
            *colon = 0;
            for (int v = 0; v < V_N; v++)
                if (!strcmp(kVar[v], st.arg)) {
                    apply_setting(v, colon + 1);
                    set_value(v, "%s", colon + 1);
                    if (s_open) ui_load();
                }
        }
    }
}

extern "C" void ps3_overlay_render(void* cbp, void* dstp)
{
    if (!s_enabled || !dstp) return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cbp;
        id<MTLTexture> dst = (__bridge id<MTLTexture>)dstp;
        const double now = now_s();
        ps3_host_input* in = ps3_pad_host_input();
        /* Quitting (a save may still be finishing): only the notice. */
        if (s_test_n && !s_quitting) test_run(now);
        if (!s_quitting) poll_toggles(in);
        if (s_last_present > 0) {
            s_ft[s_ft_i] = (float)((now - s_last_present) * 1000.0);
            s_ft_i = (s_ft_i + 1) % FT_N; s_ft_count++;
        }
        const double dt = s_last_present > 0 ? now - s_last_present : 1.0 / 30.0;
        s_last_present = now;
        if (s_hud || s_open) stats_tick(now);
        const bool notices = s_notice_until > now;
        if (s_shot[0] && !s_shot_ready) s_shot_ready = 1;   /* this frame is the shot */
        if (!anything_to_draw(now)) { s_last_present = 0; return; }

        if (!s_imgui_ready) imgui_setup();
        static int was_open;
        if (s_open && !was_open) ui_load();
        was_open = s_open;

        ImGuiIO& io = ImGui::GetIO();
        const float W = (float)[dst width], H = (float)[dst height];
        const float scale = H / 720.0f > 0.25f ? H / 720.0f : 0.25f;   /* the UI is sized for 720 lines */
        io.DisplaySize = ImVec2(W / scale, H / scale);
        io.DisplayFramebufferScale = ImVec2(scale, scale);
        io.DeltaTime = (float)(dt > 0.0001 && dt < 1.0 ? dt : 1.0 / 30.0);
        io.MouseDrawCursor = false;      /* the window shows the system's cursor (released while open) */
        if (s_open || dialog_up()) feed_input(io, in);
        else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture     = dst;
        rp.colorAttachments[0].loadAction  = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        ImGui_ImplMetal_NewFrame(rp);
        ImGui::NewFrame();
        if (s_open) draw_menu();
        if (s_hud && !s_open) draw_hud();
        if (notices) draw_notices(now);
        if (!s_open) draw_dialog(in);
        ImGui::Render();
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (enc) {
            [enc pushDebugGroup:@"ps3 overlay"];
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc popDebugGroup];
            [enc endEncoding];
        }
    }
}
