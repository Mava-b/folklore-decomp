/*
 * ps3_overlay -- the in-game menu and performance HUD of the Metal backend
 * (Dear ImGui, third_party/imgui), drawn over the finished picture (after
 * the present pass: gamma, AA, sharpening, ...) at the target's size: the
 * window's drawable, the launcher's IOSurface in host-surface mode
 * (PS3RECOMP_HOST_SURFACES, tools/host_window), or the headless target.
 * From the inFAMOUS 2 port, adapted to this toolkit.
 *
 * Opening it: F1 (fn+F1 on a Mac keyboard), Insert (Help on older Mac
 * keyboards) or Cmd+, on the keyboard, L3+R3 together on a controller. Esc,
 * the same keys or Circle close it. While it is open the game's pad,
 * keyboard and mouse input is held neutral (the game keeps running: pause it
 * first with START) and the mouse is released. The menu changes the picture,
 * sound and controller options live and saves them to PS3_OVERLAY_INI.
 *
 * Tabs: Image (gamma, sharpening, vibrance, contrast, vignette, grain, the
 * LUT's strength, FXAA/SMAA, MetalFX, anisotropic filtering, internal
 * resolution, vsync, display mode), Audio (master volume), Controls (input
 * mode, vibration, mouse camera, the bindings table that writes the controls
 * file), Performance (the HUD). "Quit game" ends the game the way closing the
 * window does (ps3_host_quit), after any save or checkpoint write in progress
 * (include/ps3emu/quit_gate.h).
 *
 * Input: no NSEvent of its own. Whoever owns the window (the launcher in
 * host-surface mode, the backend's own window otherwise) records keys and
 * mouse in the ps3_host_input block (ps3emu/host_input.h, through
 * libs/input/host_input_cocoa.h) and counts the menu's keys there; while the
 * menu is open the game asks for the pointer instead of the mouse camera
 * (PS3_HOST_INPUT_MENU). The menu reads that block on the presenting thread,
 * so the same code works with the window in another process.
 *
 * Settings, from strongest to weakest:
 *   1. an environment variable that is set when the game starts
 *      (PS3_SHARPEN, PS3_AUDIO_VOLUME, ...);
 *   2. the menu's saved file, PS3_OVERLAY_INI=<path> (unset or empty =
 *      nothing is loaded or saved); only what the menu changed is written;
 *   3. the built-in defaults.
 * A change made in the menu applies at once either way; the menu marks a
 * setting whose variable was set ("env"), because that variable wins again
 * at the next start. Across an exec (exitspawn) the session carries over,
 * see ps3_overlay_settings.h. The window host reads PS3_VSYNC,
 * PS3_WINDOW_MODE and PS3_UPSCALE from the same file.
 *
 * Knobs: PS3_OVERLAY=0 (no menu, no HUD, nothing loaded), PS3_OVERLAY_PAD=0
 * (no L3+R3), PS3_OVERLAY_HUD=1 (HUD on, also headless), PS3_OVERLAY_LANG=
 * en|pt (default from PS3_LANG). Headless tests: PS3_OVERLAY_FORCE_OPEN=1
 * (the menu drawn open; the frame dump then reads the composed image) or
 * =quit (open on the "Quit the game?" question), PS3_OVERLAY_TAB=0..3
 * (Image, Audio, Controls, Performance), and PS3_OVERLAY_TEST, a timed
 * script (see ps3_overlay.mm; PS3_OVERLAY_TEST_NEXT is the one for the
 * image an exec brings up). A headless run without any of these is
 * untouched: nothing is created and no file is read.
 *
 * Threads: ps3_overlay_render runs on the thread that presents (the RSX
 * thread); the pad hooks live in cellPad.c as plain globals so that file
 * keeps building on its own.
 */
#ifndef PS3RECOMP_PS3_OVERLAY_H
#define PS3RECOMP_PS3_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while anything is to be drawn (menu open, HUD on, a notice up),
 * or while the menu waits for its keys: the present pass's one branch. */
extern volatile int g_ps3_overlay_active;

/* Presenting thread, at backend init: device is the MTLDevice (bridged);
 * title the window's (PS3_TITLE wins over it). */
void ps3_overlay_init(void* mtl_device, int headless, const char* title);
void ps3_overlay_shutdown(void);

/* Presenting thread, after the present pass: draws into the target `dst`
 * (an id<MTLTexture>) with its own render pass on `cb` (an
 * id<MTLCommandBuffer>). Call only while g_ps3_overlay_active. */
void ps3_overlay_render(void* cb, void* dst);

/* Menu open (the game's input is held neutral). */
int ps3_overlay_menu_open(void);

/* A notice in a corner for a few seconds (any thread). */
void ps3_overlay_notice(const char* text);

/* Headless + PS3_OVERLAY_FORCE_OPEN/HUD/TEST: the frame dump should read the
 * composed target instead of the game's surface. */
int ps3_overlay_composes_headless(void);

/* PS3_OVERLAY_TEST's "shot": the path the next composed frame is to be
 * written to (taken once), or NULL. The backend writes it after the frame's
 * command buffer has completed. */
const char* ps3_overlay_shot_take(void);

/* Just before the process image is replaced (exitspawn): save what the menu
 * changed and hand this session's settings to the next image. */
void ps3_overlay_before_exec(void);

/* "Quit game": ends the game, after a save in progress. The toolkit's own
 * version waits at the quit gate and exits 0; a port defines its own (the
 * Resistance port's main.cpp routes it through its clean-exit thread). */
void ps3_host_quit(const char* why);

#ifdef __cplusplus
}
#endif

#endif
