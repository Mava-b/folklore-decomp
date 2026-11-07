/*
 * ps3recomp - the window host's display options (host_window.m)
 *
 *   PS3_WINDOW_MODE=windowed|fullscreen|borderless   how the window opens
 *                    (windowed). fullscreen is macOS's own (a Space of its
 *                    own); borderless covers the screen in place, menu bar
 *                    and Dock hidden, and switching apps is instant.
 *   PS3_WINDOW_SIZE=<w>x<h>   the window's content size when --size is not
 *                    given (1280x720); shrunk, keeping its shape, to fit
 *                    the screen
 *   PS3_DISPLAY=<n>|<name>    the screen it opens on (1 = the one with the
 *                    menu bar; a name as System Settings shows it, matched
 *                    exactly, then ignoring case, then as a part)
 *   PS3_VSYNC=0      present as soon as a frame is ready instead of at the
 *                    screen's refresh (lower latency, may tear). The game's
 *                    own pacing does not depend on it.
 *   PS3_UPSCALE=metalfx   the window's drawable follows its pixel size
 *                    (fitted to the game's aspect) and each frame goes
 *                    through a MetalFX spatial scaler to it; off when
 *                    MetalFX is missing or fails (one log line)
 *
 * The picture always keeps its aspect ratio (black bars). Plain C, so the
 * parsing and the fitting are tested on their own (test_host_display.c).
 */
#ifndef PS3RECOMP_HOST_DISPLAY_H
#define PS3RECOMP_HOST_DISPLAY_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { HD_WINDOWED, HD_FULLSCREEN, HD_BORDERLESS };

static inline int hd_window_mode_from(const char* e)
{
    if (!e || !*e || !strcasecmp(e, "windowed") || !strcasecmp(e, "window")) return HD_WINDOWED;
    if (!strcasecmp(e, "fullscreen") || !strcasecmp(e, "native")) return HD_FULLSCREEN;
    if (!strcasecmp(e, "borderless")) return HD_BORDERLESS;
    fprintf(stderr, "[host window] PS3_WINDOW_MODE=%s: expected windowed, fullscreen or borderless\n", e);
    return HD_WINDOWED;
}

/* 1 = wait for the screen's refresh (the default). */
static inline int hd_vsync_from(const char* e)
{
    return !(e && e[0] == '0' && e[1] == 0);
}

static inline int hd_upscale_from(const char* e)
{
    if (!e || !*e || !strcasecmp(e, "off") || !strcmp(e, "0")) return 0;
    if (!strcasecmp(e, "metalfx")) return 1;
    fprintf(stderr, "[host window] PS3_UPSCALE=%s unknown (metalfx); off\n", e);
    return 0;
}

/* "<w>x<h>" with both sides in range: 1, else 0 and *w, *h untouched. */
static inline int hd_size_from(const char* e, unsigned* w, unsigned* h)
{
    unsigned a = 0, b = 0;
    char tail = 0;
    if (!e || sscanf(e, "%ux%u%c", &a, &b, &tail) != 2) return 0;
    if (a < 160 || b < 90 || a > 8192 || b > 8192) return 0;
    *w = a; *h = b;
    return 1;
}

/* The drawable for an upscaled present: the view's pixel size (pw x ph)
 * fitted to the source's aspect, or the source's own size whenever that
 * would not be larger in both directions. */
static inline void hd_fit(double pw, double ph, unsigned sw, unsigned sh, unsigned* ow, unsigned* oh)
{
    *ow = sw; *oh = sh;
    if (!(pw > 0) || !(ph > 0) || !sw || !sh) return;
    double fw = floor(pw), fh = floor(ph);
    if (fw * sh > fh * sw) fw = floor(fh * sw / sh + 0.5);
    else                   fh = floor(fw * sh / sw + 0.5);
    if (fw > sw && fh > sh) { *ow = (unsigned)fw; *oh = (unsigned)fh; }
}

/* The content size that fits in a screen area of aw x ah, keeping w:h. */
static inline void hd_shrink_to(double aw, double ah, double* w, double* h)
{
    if (!(aw > 0) || !(ah > 0) || !(*w > 0) || !(*h > 0)) return;
    const double k = fmin(1.0, fmin(aw / *w, ah / *h));
    if (k < 1.0) { *w = floor(*w * k); *h = floor(*h * k); }
}

#endif /* PS3RECOMP_HOST_DISPLAY_H */
