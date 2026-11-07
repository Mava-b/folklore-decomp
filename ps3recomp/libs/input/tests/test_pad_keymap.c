/* cellPad's keyboard/mouse pad: the window's ps3_host_input block -> port 0
 * through the built-in FPS layout or PS3_INPUT_MAP; the mouse camera on the
 * right stick; wheel notches as presses; what the game tells the window
 * (game_flags); PS3_INPUT_MODE; a controller on port 0 merged with the keys;
 * the in-game menu (libs/video/ps3_overlay.mm): held neutral while it is
 * open, the window told to give it the pointer, what closed it kept from the
 * game until let go, L3+R3, and the live setters it changes things with.
 *
 *   clang -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_pad_keymap.c $(sdl2-config --libs) -o /tmp/test_pad_keymap && /tmp/test_pad_keymap
 */
#include "../cellPad.c"
#include <assert.h>
#include <unistd.h>
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

/* The window's block: the shared one, as the window host hands it over. */
static ps3_host_input s_win;

static void press_only(const unsigned* vks, int n)
{
    for (int i = 0; i < 4; i++) atomic_store(&s_win.keys[i], 0);
    for (int i = 0; i < n; i++) ps3_host_input_key(&s_win, vks[i], 1);
}

static u16 buttons_for(unsigned vk)
{
    press_only(&vk, 1);
    pad_poll_keys(0);
    return s_host_state[0].buttons;
}

static void reload_settings(void)
{
    s_map_state = 0;
    pad_map_load();
    s_mouse_on = -1;
    s_input_mode = -1;
}

static void test_fps_layout(void)
{
    unsetenv("PS3_INPUT_MAP");
    reload_settings();
    static const struct { unsigned vk; u16 btn; } k[] = {
        {0x31, CELL_PAD_CTRL_CROSS},                                  /* Space: jump */
        {0x08, CELL_PAD_CTRL_CIRCLE}, {0x3B, CELL_PAD_CTRL_CIRCLE},   /* C, Control: crouch */
        {0x33, CELL_PAD_CTRL_CIRCLE},                                 /* Backspace: back in menus */
        {0x0F, CELL_PAD_CTRL_SQUARE}, {0x0E, CELL_PAD_CTRL_SQUARE},   /* R, E: reload/use */
        {0x0C, CELL_PAD_CTRL_TRIANGLE},                               /* Q: switch weapon */
        {0x05, CELL_PAD_CTRL_L2}, {0x12, CELL_PAD_CTRL_R2},           /* G grenade, 1 weapon select */
        {0x06, CELL_PAD_CTRL_L1}, {0x07, CELL_PAD_CTRL_R1},           /* Z aim, X fire */
        {0x38, CELL_PAD_CTRL_L3}, {0x09, CELL_PAD_CTRL_R3},           /* Shift, V */
        {0x24, CELL_PAD_CTRL_START}, {0x4C, CELL_PAD_CTRL_START},
        {0x35, CELL_PAD_CTRL_START}, {0x23, CELL_PAD_CTRL_START},     /* Return, Enter, Esc, P */
        {0x30, CELL_PAD_CTRL_SELECT}, {0x2E, CELL_PAD_CTRL_SELECT},   /* Tab, M: map */
        {0x7E, CELL_PAD_CTRL_UP}, {0x7D, CELL_PAD_CTRL_DOWN},
        {0x7B, CELL_PAD_CTRL_LEFT}, {0x7C, CELL_PAD_CTRL_RIGHT},
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        assert(buttons_for(k[i].vk) == k[i].btn);
        const PadHostState* hs = &s_host_state[0];
        assert(hs->connected && hs->analog_lx == 128 && hs->analog_ly == 128);
        assert(hs->trigger_l2 == ((k[i].btn & CELL_PAD_CTRL_L2) ? 255 : 0));
        assert(hs->trigger_r2 == ((k[i].btn & CELL_PAD_CTRL_R2) ? 255 : 0));
    }
    /* WASD walks, IJKL looks */
    unsigned wd[2] = {0x0D, 0x02};
    press_only(wd, 2); pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0 && s_host_state[0].analog_ly == 0 && s_host_state[0].analog_lx == 255);
    unsigned as[2] = {0x00, 0x01};
    press_only(as, 2); pad_poll_keys(0);
    assert(s_host_state[0].analog_lx == 0 && s_host_state[0].analog_ly == 255);
    unsigned ad[2] = {0x00, 0x02};                 /* opposite keys: 127, as always */
    press_only(ad, 2); pad_poll_keys(0);
    assert(s_host_state[0].analog_lx == 127);
    unsigned jk[2] = {0x26, 0x28};
    press_only(jk, 2); pad_poll_keys(0);
    assert(s_host_state[0].analog_rx == 0 && s_host_state[0].analog_ry == 255);
    /* nothing held: centred, nothing pressed (not 0,0 = up-left) */
    press_only(NULL, 0); pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0);
    assert(s_host_state[0].analog_lx == 128 && s_host_state[0].analog_ly == 128);
    assert(s_host_state[0].analog_rx == 128 && s_host_state[0].analog_ry == 128);
    /* unbound keys do nothing */
    assert(buttons_for(0x11) == 0 && buttons_for(0x7A) == 0);   /* T, F1 */

    /* mouse buttons: left fire, right aim, middle weapon select, side buttons */
    press_only(NULL, 0);
    static const struct { uint32_t b; u16 btn; } mb[] = {
        {1, CELL_PAD_CTRL_R1}, {2, CELL_PAD_CTRL_L1}, {4, CELL_PAD_CTRL_R2},
        {8, CELL_PAD_CTRL_L2}, {16, CELL_PAD_CTRL_SQUARE}, {32, 0},
        {1 | 2, CELL_PAD_CTRL_R1 | CELL_PAD_CTRL_L1},
    };
    for (unsigned i = 0; i < sizeof mb / sizeof mb[0]; i++) {
        atomic_store(&s_win.mouse_buttons, mb[i].b);
        pad_poll_keys(0);
        assert(s_host_state[0].buttons == mb[i].btn);
    }
    atomic_store(&s_win.mouse_buttons, 0);
    pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0);

    /* two wheel notches (either way): two separate presses of Triangle */
    usleep(150 * 1000);
    atomic_store(&s_win.wheel, -32);
    pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_TRIANGLE);
    usleep(85 * 1000);
    pad_poll_keys(0); assert(s_host_state[0].buttons == 0);
    usleep(50 * 1000);
    pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_TRIANGLE);
    usleep(130 * 1000);
    pad_poll_keys(0); assert(s_host_state[0].buttons == 0);
    assert(atomic_load(&s_win.wheel) == 0);       /* drained */

    /* the game tells the window to capture the mouse on a click */
    assert(atomic_load(&s_win.game_flags) ==
           (PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MOUSE_CAMERA | PS3_HOST_INPUT_CAPTURE));
}

static void test_mouse_camera(void)
{
    unsetenv("PS3_INPUT_MAP");
    unsetenv("PS3_MOUSE_SENS");
    unsetenv("PS3_MOUSE_INVERT_Y");
    reload_settings();
    press_only(NULL, 0);
    pad_poll_keys(0);
    usleep(16 * 1000);
    /* a move right and up: the right stick goes right and up, past the dead zone */
    atomic_store(&s_win.mouse_dx, 10 * 16);
    atomic_store(&s_win.mouse_dy, -10 * 16);
    pad_poll_keys(0);
    const int rx = s_host_state[0].analog_rx, ry = s_host_state[0].analog_ry;
    assert(rx > 128 + 30 && ry < 128 - 30);
    assert(atomic_load(&s_win.mouse_dx) == 0 && atomic_load(&s_win.mouse_dy) == 0);   /* drained */
    /* the stick eases back once the mouse stops */
    for (int i = 0; i < 30; i++) { usleep(5 * 1000); pad_poll_keys(0); }
    assert(s_host_state[0].analog_rx == 128 && s_host_state[0].analog_ry == 128);

    /* PS3_MOUSE_INVERT_Y=1: up looks down */
    setenv("PS3_MOUSE_INVERT_Y", "1", 1);
    s_mouse_on = -1;
    usleep(16 * 1000);
    atomic_store(&s_win.mouse_dy, -10 * 16);
    pad_poll_keys(0);
    assert(s_host_state[0].analog_ry > 128 + 30);
    unsetenv("PS3_MOUSE_INVERT_Y");
    for (int i = 0; i < 30; i++) { usleep(5 * 1000); pad_poll_keys(0); }

    /* PS3_MOUSE=0: no camera, no buttons, no capture -- and the motion is
     * still drained, so nothing jumps when it comes back */
    setenv("PS3_MOUSE", "0", 1);
    s_mouse_on = -1;
    atomic_store(&s_win.mouse_dx, 50 * 16);
    atomic_store(&s_win.mouse_buttons, 1);
    pad_poll_keys(0);
    assert(s_host_state[0].analog_rx == 128 && s_host_state[0].buttons == 0);
    assert(atomic_load(&s_win.mouse_dx) == 0);
    assert(atomic_load(&s_win.game_flags) == PS3_HOST_INPUT_ACTIVE);
    atomic_store(&s_win.mouse_buttons, 0);
    unsetenv("PS3_MOUSE");

    /* PS3_MOUSE_CAPTURE=0: the camera stays, the window does not capture */
    setenv("PS3_MOUSE_CAPTURE", "0", 1);
    s_mouse_on = -1;
    pad_poll_keys(0);
    assert(atomic_load(&s_win.game_flags) == (PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MOUSE_CAMERA));
    unsetenv("PS3_MOUSE_CAPTURE");
    s_mouse_on = -1;
}

static void test_mapped_layout(void)
{
    char path[] = "/tmp/pad_keymap_XXXXXX";
    int fd = mkstemp(path);
    FILE* f = fdopen(fd, "w");
    fputs("[keyboard]\n"
          "cross  = \"F, Shift\"\n"     /* Shift leaves L3 */
          "circle = \"\"\n"             /* C, Control, Backspace do nothing now */
          "ls_up  = \"Up\"\n"           /* the arrow walks forward ... */
          "up     = \"T\"\n"            /* ... and the D-pad up moved to T */
          "[mouse_buttons]\n"
          "r1 = \"Button4\"\n"          /* Button4 leaves L2 */
          "up = \"WheelUp\"\n"          /* WheelUp leaves Triangle */
          "[mouse]\n"
          "capture = false\n"
          "sensitivity = 2.0\n", f);
    fclose(f);
    setenv("PS3_INPUT_MAP", path, 1);
    reload_settings();
    remove(path);

    assert(buttons_for(0x03) == CELL_PAD_CTRL_CROSS);
    assert(buttons_for(0x38) == CELL_PAD_CTRL_CROSS);
    assert(buttons_for(0x31) == 0);                     /* cross is F and Shift only now */
    assert(buttons_for(0x08) == 0 && buttons_for(0x3B) == 0);
    unsigned k = 0x7E; press_only(&k, 1); pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0 && s_host_state[0].analog_ly == 0);
    assert(buttons_for(0x11) == CELL_PAD_CTRL_UP);
    assert(buttons_for(0x0E) == CELL_PAD_CTRL_SQUARE);   /* untouched */
    press_only(NULL, 0);
    atomic_store(&s_win.mouse_buttons, 1); pad_poll_keys(0); assert(s_host_state[0].buttons == 0);
    atomic_store(&s_win.mouse_buttons, 8); pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_R1);
    atomic_store(&s_win.mouse_buttons, 2); pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_L1);
    atomic_store(&s_win.mouse_buttons, 0);
    usleep(150 * 1000);
    atomic_store(&s_win.wheel, 16);
    pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_UP);
    usleep(150 * 1000);
    pad_poll_keys(0);
    atomic_store(&s_win.wheel, -16);                     /* down: still Triangle */
    pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_TRIANGLE);
    usleep(150 * 1000);
    pad_poll_keys(0);
    assert(s_mouse_sens == 2.0f);
    /* capture: the file says no, the variable wins */
    assert(atomic_load(&s_win.game_flags) == (PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MOUSE_CAMERA));
    setenv("PS3_MOUSE_CAPTURE", "1", 1);
    s_mouse_on = -1;
    pad_poll_keys(0);
    assert(atomic_load(&s_win.game_flags) & PS3_HOST_INPUT_CAPTURE);
    unsetenv("PS3_MOUSE_CAPTURE");
    unsetenv("PS3_INPUT_MAP");
    reload_settings();
}

static void test_input_mode_and_merge(void)
{
    press_only(NULL, 0);
    /* a controller on port 0 holding Cross and the left stick right; the
     * keyboard adds Square and moves the right stick */
    PadHostState* hs = &s_host_state[0];
    const unsigned keys[2] = {0x0F, 0x25};   /* R, L */
    press_only(keys, 2);
    memset(hs, 0, sizeof *hs);
    hs->connected = 1; hs->buttons = CELL_PAD_CTRL_CROSS;
    hs->analog_lx = 255; hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = 90;
    pad_poll_keys(1);
    assert(hs->buttons == (CELL_PAD_CTRL_CROSS | CELL_PAD_CTRL_SQUARE));
    assert(hs->analog_lx == 255 && hs->analog_rx == 255 && hs->analog_ly == 128);
    assert(hs->trigger_l2 == 90);            /* an analog trigger keeps its own value */

    /* PS3_INPUT_MODE=controller: the keys do nothing, the mouse does not ask
     * for the cursor */
    setenv("PS3_INPUT_MODE", "controller", 1);
    s_input_mode = -1;
    hs->buttons = CELL_PAD_CTRL_CROSS; hs->analog_rx = 128;
    pad_poll_keys(1);
    assert(hs->buttons == CELL_PAD_CTRL_CROSS && hs->analog_rx == 128);
    assert(atomic_load(&s_win.game_flags) == PS3_HOST_INPUT_ACTIVE);
    assert(pad_input_mode_from("Gamepad") == PAD_MODE_CONTROLLER);
    assert(pad_input_mode_from("KBM") == PAD_MODE_KEYBOARD);
    assert(pad_input_mode_from("nonsense") == PAD_MODE_BOTH);
    assert(pad_input_mode_from(NULL) == PAD_MODE_BOTH);
    unsetenv("PS3_INPUT_MODE");
    s_input_mode = -1;
    press_only(NULL, 0);
}

static void test_attach(void)
{
    /* Until the backend attaches the window host's block, the local one is
     * read (the backend's own window fills it); detaching goes back to it. */
    ps3_pad_host_input_attach(NULL);
    assert(ps3_pad_host_input() == &s_input_local);
    unsigned k = 0x31;
    press_only(&k, 1);                       /* in the window host's block */
    pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0);
    ps3_host_input_key(&s_input_local, 0x31, 1);
    pad_poll_keys(0);
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);
    ps3_host_input_release_all(&s_input_local);
    ps3_pad_host_input_attach(&s_win);
    assert(ps3_pad_host_input() == &s_win);
    pad_poll_keys(0);
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);
    press_only(NULL, 0);
}

static void test_in_game_menu(const char* dir)
{
    unsetenv("PS3_INPUT_MAP");
    reload_settings();
    /* Open: W and Space held, a mouse button down: nothing reaches port 0,
     * the window is told the menu has the mouse, motion is dropped and the
     * wheel is left to the menu. */
    unsigned wsp[2] = {0x0D, 0x31};
    press_only(wsp, 2);
    atomic_store(&s_win.mouse_buttons, 1);
    atomic_store(&s_win.mouse_dx, 40 * 16);
    atomic_store(&s_win.wheel, 32);
    g_pad_overlay_open = 1;
    pad_poll_keys(0);
    pad_overlay_neutral();
    assert(s_host_state[0].buttons == 0 && s_host_state[0].analog_ly == 128 && s_host_state[0].analog_rx == 128);
    assert(atomic_load(&s_win.game_flags) == (PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MENU));
    assert(atomic_load(&s_win.mouse_dx) == 0 && atomic_load(&s_win.wheel) == 32);
    atomic_store(&s_win.wheel, 0);

    /* Closed with Esc (pressed after the last poll the menu saw) still held:
     * neither Esc (START) nor the keys and the button held across the close
     * reach the game until they are let go. */
    g_pad_overlay_open = 0;
    unsigned esc = 0x35;
    unsigned held3[3] = {0x0D, 0x31, 0x35};
    press_only(held3, 3);
    pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0 && s_host_state[0].analog_ly == 128);
    assert(atomic_load(&s_win.game_flags) & PS3_HOST_INPUT_MOUSE_CAMERA);
    press_only(NULL, 0);
    atomic_store(&s_win.mouse_buttons, 0);
    pad_poll_keys(0);
    press_only(&esc, 1);
    pad_poll_keys(0);
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_START);   /* pressed again: the game's */
    press_only(NULL, 0);
    pad_poll_keys(0);

    /* L3+R3 on a controller: one count per press of the pair; neither reaches
     * the title while both are held. The menu then navigates with port 0. */
    const uint32_t t0 = g_pad_overlay_toggle;
    s_host_state[0].connected = 1;
    s_host_state[0].buttons = CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3 | CELL_PAD_CTRL_CROSS;
    pad_overlay_gate();
    assert(g_pad_overlay_toggle == t0 + 1);
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);
    assert((g_pad_overlay_nav & 0xFFFF) == (CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3 | CELL_PAD_CTRL_CROSS));
    s_host_state[0].buttons = CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3;
    pad_overlay_gate();
    assert(g_pad_overlay_toggle == t0 + 1 && s_host_state[0].buttons == 0);   /* still held: once */
    s_host_state[0].buttons = 0;
    pad_overlay_gate();
    /* A controller button held when the menu closes stays out until let go
     * (the Circle that closed it, pressed after the last poll it saw). */
    g_pad_overlay_open = 1;
    s_host_state[0].buttons = 0;
    pad_overlay_gate();
    g_pad_overlay_open = 0;
    s_host_state[0].buttons = CELL_PAD_CTRL_CIRCLE;
    pad_overlay_gate();
    assert(s_host_state[0].buttons == 0);
    s_host_state[0].buttons = 0;
    pad_overlay_gate();
    s_host_state[0].buttons = CELL_PAD_CTRL_CIRCLE;
    pad_overlay_gate();
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_CIRCLE);
    memset(&s_host_state[0], 0, sizeof s_host_state[0]);

    /* The live setters. */
    cellPad_host_set_input_mode(PAD_MODE_CONTROLLER);
    assert(cellPad_host_input_mode() == PAD_MODE_CONTROLLER && buttons_for(0x31) == 0);
    cellPad_host_set_input_mode(PAD_MODE_BOTH);
    assert(buttons_for(0x31) == CELL_PAD_CTRL_CROSS);
    cellPad_host_set_rumble_strength(250);
    assert(cellPad_host_rumble_strength() == 200);
    cellPad_host_set_rumble_strength(0);
    assert(pad_rumble_strength() == 0);
    s_rumble_pct = -1;
    int on; float sens; int ix, iy;
    cellPad_host_set_mouse_settings(2.5f, 1, 0);
    cellPad_host_mouse_settings(&on, &sens, &ix, &iy);
    assert(on == 1 && sens == 2.5f && ix == 1 && iy == 0);
    cellPad_host_set_mouse_camera(0);
    cellPad_host_mouse_settings(&on, &sens, &ix, &iy);
    assert(on == 0);
    s_mouse_on = -1;

    /* A rebinding: live at once, saved next to PS3_OVERLAY_INI when
     * PS3_INPUT_MAP is not set, and read from there at the next start. */
    char ini[512], toml[600], path[1024];
    snprintf(ini, sizeof ini, "%s/overlay.ini", dir);
    snprintf(toml, sizeof toml, "%s/input.toml", dir);
    setenv("PS3_OVERLAY_INI", ini, 1);
    PadInputMap m;
    cellPad_host_input_map(&m);
    pim_rebind(&m, PIM_SEC_KEYBOARD, PIM_SQUARE, 0x31);      /* Space -> Square */
    cellPad_host_set_input_map(&m);
    assert(buttons_for(0x31) == CELL_PAD_CTRL_SQUARE);
    cellPad_host_input_map_path(path, sizeof path);
    assert(!strcmp(path, toml));
    assert(cellPad_host_save_input_map() == 1 && access(toml, R_OK) == 0);
    reload_settings();                                        /* the next start */
    assert(buttons_for(0x31) == CELL_PAD_CTRL_SQUARE);
    setenv("PS3_INPUT_MAP", "", 1);                           /* set but empty: not saved */
    assert(cellPad_host_save_input_map() == 0);
    unsetenv("PS3_INPUT_MAP");
    unsetenv("PS3_OVERLAY_INI");
    unlink(toml);
    reload_settings();
    assert(buttons_for(0x31) == CELL_PAD_CTRL_CROSS);
    press_only(NULL, 0);
}

int main(void)
{
    unsetenv("PS3_MOUSE");
    unsetenv("PS3_MOUSE_CAPTURE");
    unsetenv("PS3_INPUT_MODE");
    ps3_pad_host_input_attach(&s_win);
    test_fps_layout();
    test_mouse_camera();
    test_mapped_layout();
    test_input_mode_and_merge();
    test_attach();
    { char dir[512];
      const char* tmp = getenv("TMPDIR");
      snprintf(dir, sizeof dir, "%s/test_pad_keymap.XXXXXX", tmp && *tmp ? tmp : "/tmp");
      assert(mkdtemp(dir));
      test_in_game_menu(dir);
      rmdir(dir); }
    printf("test_pad_keymap: ok\n");
    return 0;
}
