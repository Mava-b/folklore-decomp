/* The in-game menu's plumbing that needs no window or GPU:
 *   - its settings file (ps3_overlay_settings.h): lines, comments, quotes;
 *     the first image of a session exports the file's lines whose variable
 *     the player did not set; an exec (exitspawn) hands the next image what
 *     the menu changed, and the next image keeps knowing which variables
 *     were the player's; the file is written back with the changes, through
 *     a temporary file; the window host's lookup of one line;
 *   - the pointer the window gives the menu (ps3emu/host_input.h): the
 *     letterbox is undone, outside the picture is "none";
 *   - the display requests in the launcher's control page
 *     (rsx_host_surface.h): asked once, taken once.
 *
 *   clang -std=gnu17 -Wall -Wextra -I include -I libs/video \
 *         libs/video/tests/test_overlay_settings.c -o /tmp/test_overlay_settings && /tmp/test_overlay_settings
 */
#include "../ps3_overlay_settings.h"
#include "../../../include/ps3emu/host_input.h"
#include "../rsx_host_surface.h"
#include <assert.h>
#include <math.h>

static const char* const kNames[] = { "PS3_SHARPEN", "PS3_AA", "PS3_AUDIO_VOLUME", "PS3_VSYNC" };
enum { N = 4 };

static void write_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void test_split(void)
{
    char a[] = "  PS3_SHARPEN = \"0.40\"  \n", b[] = "# comment\n", c[] = "[section]\n",
         d[] = "novalue\n", e[] = "PS3_AA=\n", f[] = "=x\n";
    char *k, *v;
    assert(ps3os_split(a, &k, &v) && !strcmp(k, "PS3_SHARPEN") && !strcmp(v, "0.40"));
    assert(!ps3os_split(b, &k, &v) && !ps3os_split(c, &k, &v) && !ps3os_split(d, &k, &v));
    assert(ps3os_split(e, &k, &v) && !strcmp(k, "PS3_AA") && !*v);
    assert(!ps3os_split(f, &k, &v));
    assert(ps3os_list_has("PS3_AA,PS3_VSYNC", "PS3_VSYNC") && ps3os_list_has("PS3_AA", "PS3_AA"));
    assert(!ps3os_list_has("PS3_AA,PS3_VSYNC", "PS3_A") && !ps3os_list_has("", "PS3_AA"));
    puts("lines: ok");
}

static void test_session(const char* dir)
{
    char ini[512];
    snprintf(ini, sizeof ini, "%s/sub/overlay.ini", dir);
    unsigned char env_set[N];
    char saved[N][PS3OS_VAL], value[N][PS3OS_VAL];
    memset(value, 0, sizeof value);
    for (int i = 0; i < N; i++) unsetenv(kNames[i]);
    unsetenv(PS3OS_SESSION); unsetenv(PS3OS_USERENV);

    /* A file written by hand: comments, a section, quotes, a repeated name
     * (the last line wins), a name the menu does not take. */
    char hand[512];
    snprintf(hand, sizeof hand, "%s/hand.ini", dir);
    write_file(hand, "# menu\n[display]\nPS3_AA = \"smaa\"\n; old\nPS3_AA=fxaa\nPS3_OTHER=1\n");
    char got0[PS3OS_VAL];
    assert(ps3os_lookup_file(hand, "PS3_AA", got0, sizeof got0) && !strcmp(got0, "fxaa"));
    assert(ps3os_preload(kNames, N, env_set, saved, hand, 1) == 2 && !strcmp(getenv("PS3_AA"), "fxaa"));
    assert(!getenv("PS3_OTHER"));
    unlink(hand);
    unsetenv("PS3_AA"); unsetenv(PS3OS_SESSION); unsetenv(PS3OS_USERENV);

    /* No file yet: nothing applied; saving makes the folder. */
    assert(ps3os_preload(kNames, N, env_set, saved, ini, 1) == 0);
    assert(getenv(PS3OS_SESSION) && !strcmp(getenv(PS3OS_USERENV), ""));
    snprintf(value[0], PS3OS_VAL, "0.40");
    snprintf(value[1], PS3OS_VAL, "smaa");
    assert(ps3os_save(ini, kNames, N, saved, value, "# test\n") == 1);
    assert(!strcmp(saved[0], "0.40") && !strcmp(saved[1], "smaa") && !saved[2][0]);
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", ini);
    assert(access(tmp, F_OK) != 0);                          /* renamed into place */
    char got[PS3OS_VAL];
    assert(ps3os_lookup_file(ini, "PS3_AA", got, sizeof got) && !strcmp(got, "smaa"));
    assert(!ps3os_lookup_file(ini, "PS3_VSYNC", got, sizeof got));

    /* A new session: the player set PS3_AA, so the file's line for it is
     * kept but not applied; PS3_SHARPEN comes from the file. */
    unsetenv(PS3OS_SESSION); unsetenv(PS3OS_USERENV);
    setenv("PS3_AA", "fxaa", 1);
    assert(ps3os_preload(kNames, N, env_set, saved, ini, 1) == 1);
    assert(!env_set[0] && env_set[1] && !env_set[2]);
    assert(!strcmp(getenv("PS3_SHARPEN"), "0.40") && !strcmp(getenv("PS3_AA"), "fxaa"));
    assert(!strcmp(saved[1], "smaa"));
    assert(!strcmp(getenv(PS3OS_USERENV), "PS3_AA"));

    /* The menu changes the volume and the sharpening, not saved yet; an
     * exec follows. The next image starts from the environment. */
    memset(value, 0, sizeof value);
    snprintf(value[2], PS3OS_VAL, "55");
    snprintf(value[0], PS3OS_VAL, "0.70");
    ps3os_export_session(kNames, N, value);
    unsigned char env2[N];
    char saved2[N][PS3OS_VAL];
    assert(ps3os_preload(kNames, N, env2, saved2, ini, 1) == 0);      /* nothing re-applied */
    assert(!env2[0] && env2[1] && !env2[2]);                          /* still only PS3_AA is the player's */
    assert(!strcmp(getenv("PS3_SHARPEN"), "0.70") && !strcmp(getenv("PS3_AUDIO_VOLUME"), "55"));
    assert(!strcmp(saved2[0], "0.40"));                               /* the file as it is */

    /* read_file = 0 (a headless run nobody asked the menu of). */
    unsetenv(PS3OS_SESSION);
    assert(ps3os_preload(kNames, N, env2, saved2, ini, 0) == 0 && !saved2[0][0] && !saved2[1][0]);

    /* Written back: the old lines plus the change. */
    memset(value, 0, sizeof value);
    snprintf(value[3], PS3OS_VAL, "0");
    assert(ps3os_save(ini, kNames, N, saved, value, NULL) == 1);
    assert(ps3os_lookup_file(ini, "PS3_VSYNC", got, sizeof got) && !strcmp(got, "0"));
    assert(ps3os_lookup_file(ini, "PS3_SHARPEN", got, sizeof got) && !strcmp(got, "0.40"));
    /* An unwritable place: 0, the old file untouched. */
    assert(ps3os_save("/nonexistent-dir-for-test/x/overlay.ini", kNames, N, saved, value, NULL) == 0);
    unlink(ini);
    for (int i = 0; i < N; i++) unsetenv(kNames[i]);
    puts("settings file and session: ok");
}

static void test_pointer(void)
{
    double x, y;
    /* A 16:9 picture in a 4:3 view of 800x600: bars of 75 above and below. */
    const double a = 16.0 / 9.0;
    assert(ps3_host_input_pointer_pack(400, 50, 800, 600, a) == PS3_HOST_POINTER_NONE);
    uint32_t p = ps3_host_input_pointer_pack(400, 300, 800, 600, a);
    ps3_host_input_pointer_unpack(p, &x, &y);
    assert(fabs(x - 0.5) < 1e-4 && fabs(y - 0.5) < 1e-4);
    p = ps3_host_input_pointer_pack(0, 75, 800, 600, a);
    ps3_host_input_pointer_unpack(p, &x, &y);
    assert(x == 0.0 && y == 0.0);
    p = ps3_host_input_pointer_pack(800, 525, 800, 600, a);
    ps3_host_input_pointer_unpack(p, &x, &y);
    assert(x == 1.0 && y == 1.0 && p != PS3_HOST_POINTER_NONE);
    /* Pillarbox: a 16:9 picture in a 21:9 view. */
    p = ps3_host_input_pointer_pack(2100 / 2.0 - 450, 450, 2100, 900, a);
    ps3_host_input_pointer_unpack(p, &x, &y);
    assert(fabs(x - (1050 - 450 - (2100 - 1600) / 2.0) / 1600.0) < 1e-4);
    /* No aspect: the whole view; no view: none. */
    p = ps3_host_input_pointer_pack(200, 150, 800, 600, 0);
    ps3_host_input_pointer_unpack(p, &x, &y);
    assert(fabs(x - 0.25) < 1e-4 && fabs(y - 0.25) < 1e-4);
    assert(ps3_host_input_pointer_pack(1, 1, 0, 0, a) == PS3_HOST_POINTER_NONE);
    puts("pointer: ok");
}

static void test_display_requests(void)
{
    static rsx_host_control c;
    memset(&c, 0, sizeof c);
    rsx_host_display_init(&c, RSX_HOST_DISPLAY_VSYNC | RSX_HOST_DISPLAY_UPSCALE);
    assert(atomic_load(&c.display_caps) == (RSX_HOST_DISPLAY_VSYNC | RSX_HOST_DISPLAY_UPSCALE));
    assert(rsx_host_display_take(&c.want_vsync) == -1);              /* nothing asked */
    rsx_host_display_ask(&c.want_vsync, 0);                          /* 0 is a request, not "none" */
    rsx_host_display_ask(&c.want_upscale, 1);
    assert(rsx_host_display_take(&c.want_vsync) == 0);
    assert(rsx_host_display_take(&c.want_vsync) == -1);              /* taken once */
    assert(rsx_host_display_take(&c.want_upscale) == 1);
    assert(rsx_host_display_take(&c.want_window_mode) == -1);
    rsx_host_display_ask(&c.want_window_mode, 1);
    rsx_host_display_ask(&c.want_window_mode, 2);                    /* the last one asked wins */
    assert(rsx_host_display_take(&c.want_window_mode) == 2);
    puts("display requests: ok");
}

int main(void)
{
    char dir[512];
    const char* tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/test_overlay_settings.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(mkdtemp(dir));
    test_split();
    test_session(dir);
    test_pointer();
    test_display_requests();
    char sub[600];
    snprintf(sub, sizeof sub, "%s/sub", dir);
    rmdir(sub);
    rmdir(dir);
    puts("test_overlay_settings: ok");
    return 0;
}
