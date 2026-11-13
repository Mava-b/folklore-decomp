/* The window host's display options (host_display.h): PS3_WINDOW_MODE,
 * PS3_WINDOW_SIZE, PS3_VSYNC, PS3_UPSCALE, and the drawable fitted for
 * MetalFX upscaling.
 *
 *   cc -std=gnu17 -Wall -Wextra tools/host_window/test_host_display.c \
 *      -o /tmp/test_host_display && /tmp/test_host_display
 */
#include "host_display.h"
#include <assert.h>

int main(void)
{
    assert(hd_window_mode_from(NULL) == HD_WINDOWED);
    assert(hd_window_mode_from("") == HD_WINDOWED);
    assert(hd_window_mode_from("Windowed") == HD_WINDOWED);
    assert(hd_window_mode_from("fullscreen") == HD_FULLSCREEN);
    assert(hd_window_mode_from("native") == HD_FULLSCREEN);
    assert(hd_window_mode_from("BORDERLESS") == HD_BORDERLESS);
    assert(hd_window_mode_from("kiosk") == HD_WINDOWED);       /* unknown: windowed, one line */

    assert(hd_vsync_from(NULL) == 1 && hd_vsync_from("1") == 1 && hd_vsync_from("") == 1);
    assert(hd_vsync_from("0") == 0);
    assert(hd_vsync_from("00") == 1);                          /* only "0" turns it off */

    assert(hd_upscale_from(NULL) == 0 && hd_upscale_from("off") == 0 && hd_upscale_from("0") == 0);
    assert(hd_upscale_from("MetalFX") == 1);
    assert(hd_upscale_from("fsr") == 0);

    unsigned w = 1, h = 1;
    assert(hd_size_from("1920x1080", &w, &h) && w == 1920 && h == 1080);
    assert(!hd_size_from("1920x1080x2", &w, &h) && w == 1920);
    assert(!hd_size_from("10x10", &w, &h));
    assert(!hd_size_from("wide", &w, &h));
    assert(!hd_size_from(NULL, &w, &h));

    /* the drawable for MetalFX: the view's pixels fitted to 16:9 */
    hd_fit(2560, 1440, 1280, 720, &w, &h); assert(w == 2560 && h == 1440);
    hd_fit(3024, 1964, 1280, 720, &w, &h); assert(w == 3024 && h == 1701);   /* 16:10 screen: bars top and bottom */
    hd_fit(3440, 1440, 1280, 720, &w, &h); assert(w == 2560 && h == 1440);   /* ultrawide: bars at the sides */
    hd_fit(1280, 720, 1280, 720, &w, &h);  assert(w == 1280 && h == 720);    /* not larger: no upscale */
    hd_fit(1000, 2000, 1280, 720, &w, &h); assert(w == 1280 && h == 720);    /* narrower than the source */
    hd_fit(0, 0, 1280, 720, &w, &h);       assert(w == 1280 && h == 720);

    double cw = 2560, ch = 1440;
    hd_shrink_to(1512, 945, &cw, &ch); assert(cw == 1512 && ch == 850);
    cw = 1280; ch = 720;
    hd_shrink_to(1512, 945, &cw, &ch); assert(cw == 1280 && ch == 720);
    printf("test_host_display: ok\n");
    return 0;
}
