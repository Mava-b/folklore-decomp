/* The PS3_INPUT_MAP parser (pad_input_map.h): defaults, rebinding, the value
 * forms, the [mouse] settings and unknown names.
 *
 *   cc -std=gnu17 -Wall -Wextra libs/input/tests/test_pad_input_map.c \
 *      -o /tmp/test_pad_input_map && /tmp/test_pad_input_map
 */
#include "../pad_input_map.h"
#include <assert.h>
#include <unistd.h>

static int key_of(const PadInputMap* m, int ctl, unsigned vk)
{
    return (m->key[ctl][vk >> 5] >> (vk & 31)) & 1;
}

static int n_keys(const PadInputMap* m, int ctl)
{
    int n = 0;
    for (unsigned vk = 0; vk < 128; vk++) n += key_of(m, ctl, vk);
    return n;
}

/* The built-in layout: a first-person shooter on keyboard and mouse. */
static void test_defaults(void)
{
    PadInputMap m;
    pim_defaults(&m);
    static const struct { int ctl; unsigned vk; } fps[] = {
        {PIM_UP, 0x7E}, {PIM_DOWN, 0x7D}, {PIM_LEFT, 0x7B}, {PIM_RIGHT, 0x7C},
        {PIM_CROSS, 0x31},                                       /* Space: jump */
        {PIM_CIRCLE, 0x08}, {PIM_CIRCLE, 0x3B}, {PIM_CIRCLE, 0x33},   /* C, Control, Backspace: crouch/back */
        {PIM_SQUARE, 0x0F}, {PIM_SQUARE, 0x0E},                  /* R, E: reload/use */
        {PIM_TRIANGLE, 0x0C},                                    /* Q: switch weapon */
        {PIM_L1, 0x06}, {PIM_R1, 0x07}, {PIM_L2, 0x05}, {PIM_R2, 0x12},   /* Z X G 1 */
        {PIM_L3, 0x38}, {PIM_R3, 0x09},                          /* Shift, V */
        {PIM_START, 0x24}, {PIM_START, 0x4C}, {PIM_START, 0x35}, {PIM_START, 0x23},
        {PIM_SELECT, 0x30}, {PIM_SELECT, 0x2E},
        {PIM_LS_RIGHT, 0x02}, {PIM_LS_LEFT, 0x00}, {PIM_LS_DOWN, 0x01}, {PIM_LS_UP, 0x0D},
        {PIM_RS_RIGHT, 0x25}, {PIM_RS_LEFT, 0x26}, {PIM_RS_DOWN, 0x28}, {PIM_RS_UP, 0x22},
    };
    int total = 0;
    for (unsigned i = 0; i < sizeof fps / sizeof fps[0]; i++) assert(key_of(&m, fps[i].ctl, fps[i].vk));
    for (int c = 0; c < PIM_N_CONTROLS; c++) total += n_keys(&m, c);
    assert(total == (int)(sizeof fps / sizeof fps[0]));   /* nothing else bound ... */
    for (unsigned vk = 0; vk < 128; vk++) {              /* ... and no key on two controls */
        int on = 0;
        for (int c = 0; c < PIM_N_CONTROLS; c++) on += key_of(&m, c, vk);
        assert(on <= 1);
    }
    assert(m.mouse[PIM_R1] == 1 && m.mouse[PIM_L1] == 2 && m.mouse[PIM_R2] == 4);   /* fire, aim, weapon select */
    assert(m.mouse[PIM_L2] == 8 && m.mouse[PIM_SQUARE] == 16);                     /* side buttons */
    assert(m.mouse[PIM_TRIANGLE] == (PIM_MOUSE_WHEEL_UP | PIM_MOUSE_WHEEL_DOWN));
    for (int c = 0; c < PIM_N_CONTROLS; c++)
        if (c != PIM_R1 && c != PIM_R2 && c != PIM_L1 && c != PIM_L2 && c != PIM_SQUARE && c != PIM_TRIANGLE)
            assert(m.mouse[c] == 0);
    /* controller: each SDL button its namesake, triggers L2/R2, sticks unbound */
    assert(m.pad[PIM_CROSS] == 1u << 0 && m.pad[PIM_CIRCLE] == 1u << 1);
    assert(m.pad[PIM_SQUARE] == 1u << 2 && m.pad[PIM_TRIANGLE] == 1u << 3);
    assert(m.pad[PIM_SELECT] == 1u << 4 && m.pad[PIM_START] == 1u << 6);
    assert(m.pad[PIM_L3] == 1u << 7 && m.pad[PIM_R3] == 1u << 8);
    assert(m.pad[PIM_L1] == 1u << 9 && m.pad[PIM_R1] == 1u << 10);
    assert(m.pad[PIM_UP] == 1u << 11 && m.pad[PIM_DOWN] == 1u << 12);
    assert(m.pad[PIM_LEFT] == 1u << 13 && m.pad[PIM_RIGHT] == 1u << 14);
    assert(m.pad[PIM_L2] == 1u << PIM_PAD_TRIGGER_LEFT && m.pad[PIM_R2] == 1u << PIM_PAD_TRIGGER_RIGHT);
    for (int c = PIM_LS_UP; c < PIM_N_CONTROLS; c++) assert(m.pad[c] == 0);
    assert(m.mouse_camera == -1 && m.mouse_sens == 0.0f && m.mouse_deadzone == -1);
    assert(m.n_set == 0 && m.n_warnings == 0);
}

static void test_empty_and_comments(void)
{
    PadInputMap a, b;
    pim_defaults(&a);
    pim_defaults(&b);
    assert(pim_parse(&b, "", NULL, "t") == 0);
    assert(pim_parse(&b, "# only a comment\n\n   ; another\n[keyboard]\n", NULL, "t") == 0);
    assert(!memcmp(&a, &b, sizeof a));
}

static void test_rebind(void)
{
    PadInputMap m;
    pim_defaults(&m);
    const char* text =
        "# a remap\r\n"
        "[Keyboard]\r\n"
        "cross = \"F\"            # one key\r\n"
        "start = 'Return, Escape'\r\n"
        "select = [\"Tab\", \"backspace\"]\r\n"
        "L3 = \"\"\r\n"
        "r3 = none\r\n"
        "ls_up = Up\r\n"
        "circle = \"Shift, RightOption, 0x0A\"\r\n"
        "[mouse_buttons]\n"
        "r1 = \"Right\"\n"
        "r2 = \"Left\"\n"
        "up = \"WheelUp\"\n"
        "down = \"wheeldown, Button4\"\n"
        "l1 = \"\"\n"
        "[controller]\n"
        "cross = \"b\"\n"
        "circle = \"cross\"\n"
        "select = \"touchpad, back\"\n"
        "r1 = \"righttrigger\"\n"
        "r2 = \"rightshoulder\"\n"
        "rs_up = \"paddle1\"\n";
    assert(pim_parse(&m, text, stderr, "rebind") == 0);
    assert(n_keys(&m, PIM_CROSS) == 1 && key_of(&m, PIM_CROSS, 0x03));
    assert(n_keys(&m, PIM_START) == 2 && key_of(&m, PIM_START, 0x24) && key_of(&m, PIM_START, 0x35));
    assert(n_keys(&m, PIM_SELECT) == 2 && key_of(&m, PIM_SELECT, 0x30) && key_of(&m, PIM_SELECT, 0x33));
    assert(n_keys(&m, PIM_L3) == 0 && n_keys(&m, PIM_R3) == 0);
    assert(n_keys(&m, PIM_LS_UP) == 1 && key_of(&m, PIM_LS_UP, 0x7E));
    assert(n_keys(&m, PIM_UP) == 0);                     /* Up left the D-pad, which the file does not mention */
    assert(n_keys(&m, PIM_CIRCLE) == 3 && key_of(&m, PIM_CIRCLE, 0x38) &&
           key_of(&m, PIM_CIRCLE, 0x3D) && key_of(&m, PIM_CIRCLE, 0x0A));
    assert(key_of(&m, PIM_SQUARE, 0x0E));                /* untouched controls keep their defaults */
    assert(m.mouse[PIM_R1] == 2 && m.mouse[PIM_R2] == 1 && m.mouse[PIM_L1] == 0);
    assert(m.mouse[PIM_UP] == PIM_MOUSE_WHEEL_UP);
    assert(m.mouse[PIM_DOWN] == (PIM_MOUSE_WHEEL_DOWN | 8u));
    assert(m.pad[PIM_CROSS] == 1u << 1 && m.pad[PIM_CIRCLE] == 1u << 0);
    assert(m.pad[PIM_SELECT] == ((1u << 20) | (1u << 4)));
    assert(m.pad[PIM_R1] == 1u << PIM_PAD_TRIGGER_RIGHT && m.pad[PIM_R2] == 1u << 10);
    assert(m.pad[PIM_RS_UP] == 1u << 16);
    assert(m.pad[PIM_SQUARE] == 1u << 2);
    assert(m.n_set == 18);
}

/* Bound here = gone from the controls the text does not mention; listed
 * twice = kept twice. */
static void test_steal_and_overlap(void)
{
    PadInputMap m;
    pim_defaults(&m);
    assert(pim_parse(&m, "[keyboard]\ncross = \"Q\"\n[mouse_buttons]\nr2 = \"Button4\"\n", NULL, "s") == 0);
    assert(key_of(&m, PIM_CROSS, 0x0C) && n_keys(&m, PIM_TRIANGLE) == 0);
    assert(m.mouse[PIM_R2] == 8 && m.mouse[PIM_L2] == 0 && m.mouse[PIM_L1] == 2);
    pim_defaults(&m);
    assert(pim_parse(&m, "[keyboard]\ncross = \"Q\"\ncircle = \"Q\"\n", NULL, "o") == 0);
    assert(key_of(&m, PIM_CROSS, 0x0C) && key_of(&m, PIM_CIRCLE, 0x0C));
}

static void test_mouse_settings(void)
{
    PadInputMap m;
    pim_defaults(&m);
    assert(pim_parse(&m, "[mouse]\ncamera = false\nsensitivity = 1.75\ninvert_x = yes\n"
                         "invert_y = true\ndeadzone = 500\ncapture = off\n", stderr, "mouse") == 0);
    assert(m.mouse_camera == 0 && m.mouse_invert_x == 1 && m.mouse_invert_y == 1);
    assert(m.mouse_sens > 1.74f && m.mouse_sens < 1.76f);
    assert(m.mouse_deadzone == 120);                     /* clamped */
    assert(m.mouse_capture == 0);
    /* bad values leave the setting alone */
    pim_defaults(&m);
    assert(pim_parse(&m, "[mouse]\nsensitivity = fast\ndeadzone = -4\ninvert_y = maybe\n", NULL, "m") == 2);
    assert(m.mouse_sens == 0.0f && m.mouse_deadzone == 0 && m.mouse_invert_y == -1);
}

static void test_unknown_names(void)
{
    PadInputMap m, d;
    pim_defaults(&m);
    pim_defaults(&d);
    FILE* err = tmpfile();
    assert(err);
    const int n = pim_parse(&m,
        "stray = 1\n"
        "[keyboard]\n"
        "cross = \"Space, Hyper\"\n"      /* Hyper unknown: Space still bound */
        "jump = \"J\"\n"                  /* not a PS3 control */
        "[joystick]\n"                    /* unknown section: its lines are skipped */
        "cross = \"q\"\n"
        "[mouse_buttons]\n"
        "r1 = \"Button9\"\n"
        "[controller]\n"
        "cross = \"south\"\n"
        "[mouse]\n"
        "speed = 3\n"
        "[keyboard]\n"
        "this line has no equals sign\n",
        err, "bad.toml");
    assert(n == 8);
    assert(m.n_warnings == 8);
    assert(n_keys(&m, PIM_CROSS) == 1 && key_of(&m, PIM_CROSS, 0x31));
    assert(m.mouse[PIM_R1] == 0);                        /* the line still rebinds, to nothing known */
    assert(m.pad[PIM_CROSS] == 0);
    assert(n_keys(&m, PIM_CIRCLE) == n_keys(&d, PIM_CIRCLE));
    rewind(err);
    char buf[2048]; size_t r = fread(buf, 1, sizeof buf - 1, err); buf[r] = 0;
    fclose(err);
    assert(strstr(buf, "bad.toml:1: unknown setting outside a section 'stray'"));
    assert(strstr(buf, "bad.toml:3: unknown key 'hyper'"));
    assert(strstr(buf, "bad.toml:4: unknown PS3 control 'jump'"));
    assert(strstr(buf, "bad.toml:5: unknown section 'joystick'"));
    assert(strstr(buf, "bad.toml:8: unknown mouse button 'button9'"));
    assert(strstr(buf, "bad.toml:10: unknown controller input 'south'"));
    assert(strstr(buf, "bad.toml:12: unknown mouse setting 'speed'"));
    assert(strstr(buf, "bad.toml:14: unknown line"));
}

static void test_load_file(void)
{
    PadInputMap m;
    assert(pim_load_file(&m, "/nonexistent/input.toml", NULL) == 0);
    PadInputMap d;
    pim_defaults(&d);
    assert(!memcmp(&m, &d, sizeof m));                   /* missing file = defaults */

    char path[] = "/tmp/pim_testXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE* f = fdopen(fd, "w");
    /* a long file: the reader grows its buffer past the first 4 KB */
    for (int i = 0; i < 300; i++) fprintf(f, "# padding line %03d ...........................\n", i);
    fprintf(f, "[keyboard]\ncross = \"Z\"\n");
    fclose(f);
    assert(pim_load_file(&m, path, stderr) == 1);
    remove(path);
    assert(n_keys(&m, PIM_CROSS) == 1 && key_of(&m, PIM_CROSS, 0x06));
}

static void test_keys_hit(void)
{
    PadInputMap m;
    pim_defaults(&m);
    uint32_t keys[4] = {0, 0, 0, 0};
    keys[0x4C >> 5] |= 1u << (0x4C & 31);                /* keypad Enter */
    assert(pim_keys_hit(&m, PIM_START, keys));
    assert(!pim_keys_hit(&m, PIM_SELECT, keys));
}

/* Writing (pim_save_file): every canonical name reads back as itself,
 * a written map reads back the same, the other sections stay, and saving
 * again changes nothing. */
static int maps_equal(const PadInputMap* a, const PadInputMap* b)
{
    return !memcmp(a->key, b->key, sizeof a->key) && !memcmp(a->mouse, b->mouse, sizeof a->mouse) &&
           !memcmp(a->pad, b->pad, sizeof a->pad);
}

static void test_names_round_trip(void)
{
    char tmp[8], low[32];
    for (unsigned vk = 0; vk < 128; vk++) {
        const char* n = pim_key_label(vk, tmp, sizeof tmp);
        snprintf(low, sizeof low, "%s", n);
        pim_lower(low);
        assert(pim_parse_key_name(low) == (int)vk);
    }
    for (int b = 0; b < 32; b++) {
        const char* n = pim_mouse_label(1u << b);
        if (!n) continue;
        snprintf(low, sizeof low, "%s", n); pim_lower(low);
        assert(pim_lookup(pim_mouse_buttons, PIM_COUNT(pim_mouse_buttons), low) == (int)(1u << b));
    }
    for (int b = 0; b <= PIM_PAD_TRIGGER_RIGHT; b++) {
        const char* n = pim_pad_label(b);
        assert(n);
        assert(pim_lookup(pim_pad_inputs, PIM_COUNT(pim_pad_inputs), n) == b);
    }
    /* the launcher's spelling */
    assert(!strcmp(pim_key_label(0x31, tmp, sizeof tmp), "Space"));
    assert(!strcmp(pim_key_label(0x4C, tmp, sizeof tmp), "KeypadEnter"));
    assert(!strcmp(pim_key_label(0x34, tmp, sizeof tmp), "0x34"));   /* no name */
    assert(!strcmp(pim_pad_label(PIM_PAD_TRIGGER_LEFT), "lefttrigger"));
}

static void test_render_round_trip(void)
{
    PadInputMap m;
    pim_defaults(&m);
    char list[256];
    pim_inputs_text(&m, PIM_SEC_KEYBOARD, PIM_START, list, sizeof list);
    assert(!strcmp(list, "P, Return, Escape, KeypadEnter"));
    pim_inputs_text(&m, PIM_SEC_MOUSE_BUTTONS, PIM_L1, list, sizeof list);
    assert(!strcmp(list, "Right"));
    pim_inputs_text(&m, PIM_SEC_MOUSE_BUTTONS, PIM_TRIANGLE, list, sizeof list);
    assert(!strcmp(list, "WheelUp, WheelDown"));
    pim_inputs_text(&m, PIM_SEC_CONTROLLER, PIM_CROSS, list, sizeof list);
    assert(!strcmp(list, "a"));
    pim_inputs_text(&m, PIM_SEC_CONTROLLER, PIM_LS_UP, list, sizeof list);
    assert(!strcmp(list, ""));

    /* a Controls screen moves Q from Triangle to Cross and puts R2 on the wheel */
    assert(pim_rebind(&m, PIM_SEC_KEYBOARD, PIM_CROSS, 0x0C) == 1u << PIM_TRIANGLE);
    assert(key_of(&m, PIM_CROSS, 0x0C) && n_keys(&m, PIM_CROSS) == 1 && n_keys(&m, PIM_TRIANGLE) == 0);
    assert(pim_rebind(&m, PIM_SEC_MOUSE_BUTTONS, PIM_R2, PIM_MOUSE_WHEEL_DOWN) == 1u << PIM_TRIANGLE);
    assert(m.mouse[PIM_R2] == PIM_MOUSE_WHEEL_DOWN);
    assert(pim_rebind(&m, PIM_SEC_CONTROLLER, PIM_R1, PIM_PAD_TRIGGER_RIGHT) == 1u << PIM_R2);
    pim_unbind(&m, PIM_SEC_CONTROLLER, PIM_SELECT);

    const char* old =
        "# hand-written\n"
        "[keyboard]\n"
        "cross = \"F\"\n"
        "\n"
        "[mouse]\n"
        "sensitivity = 1.5   # fast\n"
        "invert_y = true\n"
        "\n"
        "[controller]\n"
        "cross = \"b\"\n"
        "[future]\n"
        "x = 1\n";
    char* text = pim_render(&m, old);
    assert(text);
    assert(strstr(text, "\n[keyboard]\ncross    = \"Q\"\ncircle   = \"C, Backspace, Control\"\n"));
    assert(strstr(text, "triangle = \"\"\n"));
    assert(strstr(text, "start    = \"P, Return, Escape, KeypadEnter\"\n"));
    assert(strstr(text, "\n[mouse]\nsensitivity = 1.5   # fast\ninvert_y = true\n"));
    assert(strstr(text, "\n[future]\nx = 1\n"));
    assert(!strstr(text, "hand-written"));          /* the preamble is the writer's own */
    assert(!strstr(text, "cross = \"b\""));         /* the old bindings are gone */
    PadInputMap back;
    pim_defaults(&back);
    assert(pim_parse(&back, text, NULL, "rendered") == 1);   /* [future]: unknown to the game too */
    assert(maps_equal(&m, &back));
    assert(back.mouse_sens == 1.5f && back.mouse_invert_y == 1);
    /* saving again changes nothing */
    char* again = pim_render(&back, text);
    assert(again && !strcmp(again, text));
    free(again);
    free(text);

    /* through a file, into a folder that does not exist yet */
    char dir[] = "/tmp/pim_save_XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof path, "%s/sub/input.toml", dir);
    assert(pim_save_file(&m, path));
    PadInputMap loaded;
    assert(pim_load_file(&loaded, path, stderr));
    assert(maps_equal(&m, &loaded) && loaded.n_warnings == 0);
    remove(path);
    snprintf(path, sizeof path, "%s/sub", dir); rmdir(path);
    rmdir(dir);
}

int main(void)
{
    test_defaults();
    test_empty_and_comments();
    test_rebind();
    test_steal_and_overlap();
    test_mouse_settings();
    test_unknown_names();
    test_load_file();
    test_keys_hit();
    test_names_round_trip();
    test_render_round_trip();
    printf("test_pad_input_map: ok\n");
    return 0;
}
