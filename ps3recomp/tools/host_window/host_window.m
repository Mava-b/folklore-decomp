/*
 * ps3recomp - host window: one window across an executable swap (macOS)
 *
 *   host_window [--title T] [--size WxH] [--] <program> [args...]
 *
 * Opens the game window, then runs <program> with PS3RECOMP_HOST_SURFACES set
 * in its environment. The Metal backend in <program> sees it and, instead of
 * opening a window of its own, renders into IOSurfaces this process shares
 * with it (rsx_host_surface.h); this process blits the newest finished one
 * into the window. When the game replaces its executable (exitspawn is an
 * execv, same pid) the new image attaches again by the same name, and the
 * window never goes away.
 *
 *   window closed / Cmd-Q / SIGINT, SIGTERM, SIGHUP here
 *       -> `closed` in the control page, SIGTERM to the game, SIGKILL 3 s later
 *   game exits -> exit with its status (0 after a close asked for here)
 *
 * The game process has no window and never touches NSApplication, so it gets
 * no Dock icon; this process is the app the user sees.
 *
 * Keyboard and mouse: their events arrive here, so this process records them
 * in the control page's input block (libs/input/host_input_cocoa.h) and the
 * game's cellPad reads it. A left click in the game captures the cursor when
 * the game asks for that (its mouse camera, PS3_MOUSE / PS3_MOUSE_CAPTURE on
 * the game's side); Esc, Cmd or leaving the window releases it.
 *
 * Environment:
 *   PS3_TITLE              window title (default: HOST_WINDOW_TITLE, built in)
 *   PS3_WINDOW_MODE, PS3_WINDOW_SIZE, PS3_DISPLAY, PS3_VSYNC, PS3_UPSCALE
 *                          how the window opens and presents (host_display.h)
 *   PS3_OVERLAY_INI        the game's in-game menu settings file: PS3_VSYNC,
 *                          PS3_WINDOW_MODE and PS3_UPSCALE are read from it
 *                          when they are not in the environment
 *                          (libs/video/ps3_overlay_settings.h)
 *   PS3RECOMP_METAL_FPS=1  FPS in the title and [RSX FPS] lines, as the backend's
 *                          own window does; guest FPS comes from the game.
 *                          A line ends in hidden=1 while the window is off
 *                          screen (below)
 *   PS3RECOMP_HOST_STATS=1 once a second: publish->screen latency, frames
 *                          copied/dropped, and this process's CPU time
 *
 * Off screen: macOS shows nothing of a window that is not on screen: another
 * app or Space in front (with PS3_WINDOW_MODE=fullscreen the window has a
 * Space of its own, so bringing any other app to the front hides it),
 * minimized, or fully covered. Presents go on and are dropped unseen, so the
 * display rate reads 0 while the guest's goes on, which in a log looks like a
 * frozen picture. The window logs when it leaves the screen and when it is
 * back, and shows the newest frame as soon as it is back.
 *
 * In-game menu (libs/video/ps3_overlay.mm): its keys (F1, Insert, Cmd+,)
 * are counted in the input block, and while it is open the pointer goes to
 * it (host_input_cocoa.h). Its display options -- vsync, the display mode,
 * MetalFX -- are this window's: the game asks in the control page and the
 * input tick applies them (rsx_host_surface.h, want_* / now_*).
 *
 * If the bootstrap service cannot be registered the program is exec'd
 * directly and opens its own window, as it would without this launcher.
 */
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#if __has_include(<MetalFX/MetalFX.h>)
#  import <MetalFX/MetalFX.h>
#  define HOST_HAVE_METALFX 1
#endif

#include <dlfcn.h>

#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_display.h"
#include "metal_frame_rate.h"
#include "rsx_host_surface.h"
#include "ps3_overlay_settings.h"
#include "../../libs/input/host_input_cocoa.h"

#ifndef HOST_WINDOW_TITLE
#define HOST_WINDOW_TITLE "ps3recomp"
#endif

extern char** environ;

static struct {
    /* the shared side */
    char                 name[128];
    mach_port_t          service;          /* receive right, bootstrap-registered */
    rsx_host_control*    ctl;
    mach_port_t          ctl_entry;        /* memory entry for the control page */
    IOSurfaceRef         surface[RSX_HOST_SLOTS];
    id<MTLTexture>       tex[RSX_HOST_SLOTS];
    u_int32_t            width, height;
    /* the window */
    id<MTLDevice>        dev;
    id<MTLCommandQueue>  queue;
    CAMetalLayer*        layer;
    NSWindow*            window;
    NSString*            title;
    int                  present_busy;
    uint64_t             shown_seq;
    int                  off_screen;       /* see "Off screen" above */
    double               off_since;
    HostInputCocoa       input;            /* keys and mouse -> ctl->input */
    /* display options (host_display.h) */
    int                  fs_style;         /* what View > Full Screen enters */
    int                  borderless;
    NSRect               saved_frame;
    NSWindowStyleMask    saved_style;
    int                  upscale_on;       /* PS3_UPSCALE=metalfx, still working */
    int                  upscale_dead;     /* MetalFX failed here: the menu shows it unavailable */
    int                  pending_borderless;   /* the menu asked for borderless while in macOS fullscreen */
    id                   fx_scaler;        /* id<MTLFXSpatialScaler> */
    id<MTLTexture>       fx_out;
    /* the game */
    pid_t                child;
    int                  closing;
    int                  child_status;
    /* diagnostics */
    double               tick_to_s;
    int                  fps_on, stats_on;
    MetalFrameRate       fps;
    pthread_mutex_t      fps_lock;
    uint64_t             last_guest;
    double               lat_sum, lat_max, copy_sum;
    unsigned             lat_n, copy_n, copies;
} g = { .fps_lock = PTHREAD_MUTEX_INITIALIZER };

static void begin_close(const char* why);
static void keep_drawable(void);

/* ---- surfaces ------------------------------------------------------------ */

static IOSurfaceRef make_surface(u_int32_t w, u_int32_t h)
{
    const size_t bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)w * 4);
    NSDictionary* props = @{
        (id)kIOSurfaceWidth:           @(w),
        (id)kIOSurfaceHeight:          @(h),
        (id)kIOSurfaceBytesPerElement: @4,
        (id)kIOSurfaceBytesPerRow:     @(bpr),
        (id)kIOSurfaceAllocSize:       @(bpr * h),
        (id)kIOSurfacePixelFormat:     @((u_int32_t)'BGRA'),
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

/* (Re)create every surface at w x h. A surface the window is still copying
 * from stays READING and is freed by that copy's completion; the copy holds
 * its own reference to the old texture. */
static int make_surfaces(u_int32_t w, u_int32_t h)
{
    IOSurfaceRef sf[RSX_HOST_SLOTS] = { 0 };
    id<MTLTexture> tex[RSX_HOST_SLOTS] = { nil };
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:w height:h mipmapped:NO];
    td.usage       = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = g.dev.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    for (int i = 0; i < RSX_HOST_SLOTS; i++) {
        sf[i] = make_surface(w, h);
        if (sf[i]) tex[i] = [g.dev newTextureWithDescriptor:td iosurface:sf[i] plane:0];
        if (!tex[i]) {
            for (int j = 0; j <= i; j++) if (sf[j]) CFRelease(sf[j]);
            return -1;
        }
    }
    for (int i = 0; i < RSX_HOST_SLOTS; i++) {
        if (g.surface[i]) CFRelease(g.surface[i]);
        g.surface[i] = sf[i];
        g.tex[i] = tex[i];
        uint32_t s = atomic_load(&g.ctl->state[i]);
        if (s != RSX_HOST_READING) atomic_store(&g.ctl->state[i], RSX_HOST_FREE);
        atomic_store(&g.ctl->seq[i], 0);
    }
    g.width = w; g.height = h;
    g.ctl->width = w; g.ctl->height = h;
    g.input.aspect = (double)w / (double)h;     /* the menu's pointer, over the letterboxed picture */
    g.ctl->generation++;
    g.shown_seq = atomic_load(&g.ctl->next_seq);
    keep_drawable();
    return 0;
}

static int make_control(void)
{
    vm_address_t addr = 0;
    if (vm_allocate(mach_task_self(), &addr, RSX_HOST_CONTROL_BYTES, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return -1;
    memory_object_size_t size = RSX_HOST_CONTROL_BYTES;
    if (mach_make_memory_entry_64(mach_task_self(), &size, addr, VM_PROT_READ | VM_PROT_WRITE,
                                  &g.ctl_entry, MACH_PORT_NULL) != KERN_SUCCESS ||
        size < RSX_HOST_CONTROL_BYTES)
        return -1;
    g.ctl = (rsx_host_control*)addr;
    g.ctl->magic        = RSX_HOST_MAGIC;
    g.ctl->version      = RSX_HOST_VERSION;
    g.ctl->launcher_pid = getpid();
    g.input.in          = &g.ctl->input;
    atomic_store(&g.ctl->input.pointer, PS3_HOST_POINTER_NONE);
    rsx_host_display_init(g.ctl, RSX_HOST_DISPLAY_VSYNC | RSX_HOST_DISPLAY_WINDOW_MODE | RSX_HOST_DISPLAY_UPSCALE);
    return 0;
}

/* ---- the game's messages ------------------------------------------------- */

static void reply_surfaces(mach_port_t reply)
{
    rsx_host_surfaces_msg m;
    memset(&m, 0, sizeof m);
    m.header.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    m.header.msgh_size        = sizeof m;
    m.header.msgh_remote_port = reply;
    m.header.msgh_id          = RSX_HOST_MSG_SURFACES;
    m.body.msgh_descriptor_count = 1 + RSX_HOST_SLOTS;
    m.control.name        = g.ctl_entry;
    m.control.disposition = MACH_MSG_TYPE_COPY_SEND;
    m.control.type        = MACH_MSG_PORT_DESCRIPTOR;
    for (int i = 0; i < RSX_HOST_SLOTS; i++) {
        m.surface[i].name        = IOSurfaceCreateMachPort(g.surface[i]);
        m.surface[i].disposition = MACH_MSG_TYPE_MOVE_SEND;
        m.surface[i].type        = MACH_MSG_PORT_DESCRIPTOR;
    }
    m.width = g.width; m.height = g.height;
    m.generation   = g.ctl->generation;
    m.launcher_pid = getpid();
    kern_return_t kr = mach_msg(&m.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0,
                                MACH_PORT_NULL, 1000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[host window] reply failed: %s\n", mach_error_string(kr));
        for (int i = 0; i < RSX_HOST_SLOTS; i++)
            mach_port_deallocate(mach_task_self(), m.surface[i].name);
        mach_port_deallocate(mach_task_self(), reply);
    }
}

static void on_attach(const rsx_host_attach_msg* a)
{
    const u_int32_t w = a->width, h = a->height;
    if (w && h && w <= RSX_HOST_MAX_DIM && h <= RSX_HOST_MAX_DIM && (w != g.width || h != g.height)) {
        if (make_surfaces(w, h) != 0)
            fprintf(stderr, "[host window] could not make %ux%u surfaces; keeping %ux%u\n",
                    w, h, g.width, g.height);
    } else {
        rsx_host_reset_rendering(g.ctl);       /* a new image: the old one's frames are gone */
    }
    fprintf(stderr, "[host window] pid %d attached: %ux%u, generation %u\n",
            a->pid, g.width, g.height, g.ctl->generation);
    reply_surfaces(a->header.msgh_remote_port);
}

static void try_present(void);

static void drain_messages(void)
{
    for (;;) {
        union { rsx_host_attach_msg attach; rsx_host_frame_msg frame; } u;
        struct { char buf[sizeof u]; mach_msg_trailer_t t; } in;
        memset(&in, 0, sizeof in);
        mach_msg_header_t* h = (mach_msg_header_t*)in.buf;
        kern_return_t kr = mach_msg(h, MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_LARGE, 0,
                                    sizeof in, g.service, 0, MACH_PORT_NULL);
        if (kr == MACH_RCV_TOO_LARGE) {             /* not ours: drop it */
            mach_msg(h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 0, g.service, 0, MACH_PORT_NULL);
            continue;
        }
        if (kr != KERN_SUCCESS) break;
        if (h->msgh_id == RSX_HOST_MSG_ATTACH && h->msgh_size >= sizeof(rsx_host_attach_msg) &&
            !(h->msgh_bits & MACH_MSGH_BITS_COMPLEX))
            on_attach((const rsx_host_attach_msg*)h);
        else if (h->msgh_id != RSX_HOST_MSG_FRAME)
            mach_msg_destroy(h);
    }
    try_present();
}

/* ---- presenting ---------------------------------------------------------- */

static void note_presented(double shown, uint64_t published)
{
    if (!(shown > 0)) return;
    pthread_mutex_lock(&g.fps_lock);
    metal_frame_rate_presented(&g.fps, shown);
    const double lat = shown - (double)published * g.tick_to_s;
    if (published && lat >= 0 && lat < 10) {
        g.lat_sum += lat; g.lat_n++;
        if (lat > g.lat_max) g.lat_max = lat;
    }
    pthread_mutex_unlock(&g.fps_lock);
}

/* The drawable's size: the surfaces' (the layer letterboxes them to the
 * window), or with PS3_UPSCALE the view's pixel size fitted to their aspect. */
static void keep_drawable(void)
{
    if (!g.layer || !g.width || !g.height) return;
    unsigned w = g.width, h = g.height;
    if (g.upscale_on && g.window) {
        NSView* v = g.window.contentView;
        const NSSize px = v ? [v convertSizeToBacking:v.bounds.size] : NSZeroSize;
        hd_fit(px.width, px.height, g.width, g.height, &w, &h);
    }
    const CGSize want = CGSizeMake(w, h);
    if (!CGSizeEqualToSize(g.layer.drawableSize, want)) g.layer.drawableSize = want;
}

/* MetalFX gave up: say why once and present at the surfaces' size. */
static void upscale_fail(const char* why)
{
    if (!g.upscale_on) return;
    g.upscale_on = 0;
    g.upscale_dead = 1;
    g.fx_scaler = nil; g.fx_out = nil;
    fprintf(stderr, "[host window] MetalFX upscaling unavailable (%s); presenting at %ux%u\n",
            why, g.width, g.height);
    keep_drawable();
}

/* src through a MetalFX spatial scaler into a private texture of ow x oh;
 * nil when it cannot. MetalFX is looked up when first needed, so the window
 * host links nothing new and runs where it is missing. */
static id<MTLTexture> upscale(id<MTLCommandBuffer> cb, id<MTLTexture> src, NSUInteger ow, NSUInteger oh)
{
#if HOST_HAVE_METALFX
    if (@available(macOS 13.0, *)) {
        static Class desc_class;
        if (!desc_class) {
            dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY | RTLD_LOCAL);
            desc_class = NSClassFromString(@"MTLFXSpatialScalerDescriptor");
            if (!desc_class) { upscale_fail("MetalFX is not on this system"); return nil; }
        }
        const MTLPixelFormat fmt = src.pixelFormat;
        id<MTLFXSpatialScaler> sc = (id<MTLFXSpatialScaler>)g.fx_scaler;
        if (!sc || sc.inputWidth != src.width || sc.inputHeight != src.height ||
            sc.outputWidth != ow || sc.outputHeight != oh) {
            g.fx_scaler = nil; g.fx_out = nil;
            if (![desc_class supportsDevice:g.dev]) { upscale_fail("not supported on this GPU"); return nil; }
            MTLFXSpatialScalerDescriptor* d = [desc_class new];
            d.inputWidth  = src.width;  d.inputHeight  = src.height;
            d.outputWidth = ow;         d.outputHeight = oh;
            d.colorTextureFormat  = fmt;
            d.outputTextureFormat = fmt;
            d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;   /* gamma-encoded */
            sc = [d newSpatialScalerWithDevice:g.dev];
            if (!sc) { upscale_fail("could not create the spatial scaler"); return nil; }
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                          width:ow height:oh
                                                                                      mipmapped:NO];
            td.usage       = sc.outputTextureUsage | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> out = [g.dev newTextureWithDescriptor:td];
            if (!out) { upscale_fail("no memory for its output"); return nil; }
            fprintf(stderr, "[host window] MetalFX spatial upscaling %lux%lu -> %lux%lu\n",
                    (unsigned long)src.width, (unsigned long)src.height,
                    (unsigned long)ow, (unsigned long)oh);
            g.fx_scaler = sc;
            g.fx_out = out;
        }
        if ((src.usage & sc.colorTextureUsage) != sc.colorTextureUsage) {
            upscale_fail("the surfaces' usage does not allow it");
            return nil;
        }
        sc.colorTexture       = src;
        sc.outputTexture      = g.fx_out;
        sc.inputContentWidth  = src.width;
        sc.inputContentHeight = src.height;
        [sc encodeToCommandBuffer:cb];
        return g.fx_out;
    }
    upscale_fail("needs macOS 13");
    return nil;
#else
    (void)cb; (void)src; (void)ow; (void)oh;
    upscale_fail("built without MetalFX");
    return nil;
#endif
}

/* The newest frame the game has finished, onto the window: one GPU copy from
 * its surface into a drawable of the same size, which the layer letterboxes
 * into the window (with PS3_UPSCALE, MetalFX to the window's pixel size
 * first). At most one copy is in flight; its completion looks for a newer
 * frame. */
static void try_present(void)
{
    if (g.present_busy || !g.ctl) return;
    uint64_t seq = 0;
    const int slot = rsx_host_begin_read(g.ctl, g.shown_seq, &seq);
    if (slot < 0) return;
    @autoreleasepool {
        id<MTLTexture> src = g.tex[slot];
        id<CAMetalDrawable> drawable = nil;
        const CGSize ds = g.layer.drawableSize;
        const int same  = src.width == (NSUInteger)ds.width && src.height == (NSUInteger)ds.height;
        const int scale = !same && g.upscale_on &&
                          (NSUInteger)ds.width > src.width && (NSUInteger)ds.height > src.height;
        if (same || scale) drawable = [g.layer nextDrawable];
        id<MTLCommandBuffer> cb = drawable ? [g.queue commandBuffer] : nil;
        id<MTLTexture> from = src;
        if (cb && scale) {
            from = upscale(cb, src, (NSUInteger)ds.width, (NSUInteger)ds.height);
            if (!from) cb = nil;                /* MetalFX is off now: plain next time */
        }
        if (!cb) {                              /* no drawable now: show it next time */
            uint32_t s = RSX_HOST_READING;
            atomic_compare_exchange_strong(&g.ctl->state[slot], &s, RSX_HOST_READY);
            return;
        }
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:from toTexture:drawable.texture];
        [blit endEncoding];
        const uint64_t published = atomic_load(&g.ctl->publish_time[slot]);
        [drawable addPresentedHandler:^(id<MTLDrawable> d) { note_presented(d.presentedTime, published); }];
        [cb presentDrawable:drawable];
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            (void)done;
            const double copied = (double)mach_absolute_time() * g.tick_to_s - (double)published * g.tick_to_s;
            dispatch_async(dispatch_get_main_queue(), ^{
                rsx_host_end_read(g.ctl, slot);
                g.present_busy = 0;
                g.copies++;
                if (published) { g.copy_sum += copied; g.copy_n++; }
                try_present();
            });
        }];
        g.present_busy = 1;
        g.shown_seq = seq;
        [cb commit];
    }
}

/* The window's occlusion changed: on screen or not (see "Off screen" at the
 * top). Logged both ways, so a run of display=0 reads as what it is. */
static void note_on_screen(int on, double now)
{
    const int off = !on;
    if (off == g.off_screen) return;
    g.off_screen = off;
    if (!on) {
        g.off_since = now;
        fprintf(stderr, "[host window] window off screen (another app or Space in front, minimized or "
                        "covered): its frames are not shown until it is back\n");
        return;
    }
    fprintf(stderr, "[host window] window on screen again after %.1f s\n", now - g.off_since);
    try_present();
}

/* Black until the game's first frame, rather than whatever the drawable held. */
static void present_black(void)
{
    @autoreleasepool {
        id<CAMetalDrawable> d = [g.layer nextDrawable];
        if (!d) return;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture     = d.texture;
        rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> cb = [g.queue commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb presentDrawable:d];
        [cb commit];
    }
}

/* ---- diagnostics --------------------------------------------------------- */

static void stats_tick(void)
{
    const uint64_t guest = atomic_load(&g.ctl->guest_frames);
    pthread_mutex_lock(&g.fps_lock);
    g.fps.guest += guest - g.last_guest;
    g.last_guest = guest;
    const MetalFrameRateSample r = metal_frame_rate_sample(&g.fps, CACurrentMediaTime());
    const double lat_avg = g.lat_n ? g.lat_sum / g.lat_n : 0, lat_max = g.lat_max;
    const unsigned lat_n = g.lat_n;
    g.lat_sum = g.lat_max = 0; g.lat_n = 0;
    pthread_mutex_unlock(&g.fps_lock);
    if (g.fps_on) {
        g.window.title = [NSString stringWithFormat:
            @"%@ | display %.1f FPS | guest %.1f FPS | %.1f ms avg / %.1f p95 / %.1f max | age %.1f s",
            g.title, r.display_fps, r.guest_fps, r.average_ms, r.p95_ms, r.worst_ms, r.age_seconds];
        fprintf(stderr, "[RSX FPS] display=%.2f guest=%.2f avg_ms=%.2f p95_ms=%.2f max_ms=%.2f age_s=%.2f%s\n",
                r.display_fps, r.guest_fps, r.average_ms, r.p95_ms, r.worst_ms, r.age_seconds,
                g.off_screen ? " hidden=1" : "");
    }
    if (g.stats_on) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        const double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                           ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        fprintf(stderr, "[host window] copies=%u published=%llu reclaimed=%llu skipped=%llu "
                        "publish->copied avg %.2f ms, publish->on-screen avg %.2f max %.2f ms (%u), cpu %.2f s\n",
                g.copies, (unsigned long long)atomic_load(&g.ctl->published),
                (unsigned long long)atomic_load(&g.ctl->reclaimed),
                (unsigned long long)atomic_load(&g.ctl->skipped),
                g.copy_n ? g.copy_sum / g.copy_n * 1e3 : 0.0, lat_avg * 1e3, lat_max * 1e3, lat_n, cpu);
        g.copy_sum = 0; g.copy_n = 0;
    }
}

/* ---- the game process ---------------------------------------------------- */

static void child_exited(void)
{
    /* DISPATCH_PROC_EXIT can fire a moment before the child becomes
     * reapable, so a WNOHANG reap may see nothing and this handler never
     * runs again: the host then stayed up with its window open after the
     * game had gone. The child has exited (an exec does not raise EXIT),
     * so a blocking reap returns at once. */
    int st = 0;
    pid_t r;
    do r = waitpid(g.child, &st, 0); while (r < 0 && errno == EINTR);
    if (r != g.child) {                     /* already reaped elsewhere: still done */
        fprintf(stderr, "[host window] pid %d gone (%s)\n", g.child, strerror(errno));
        exit(g.closing ? 0 : 1);
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
    fprintf(stderr, "[host window] pid %d %s %d\n", g.child,
            WIFSIGNALED(st) ? "killed by signal" : "exited with", WIFSIGNALED(st) ? WTERMSIG(st) : code);
    if (g.closing) code = 0;                /* the close was asked for here */
    g.child = 0;
    if (g.stats_on) stats_tick();
    exit(code);
}

static void begin_close(const char* why)
{
    if (g.closing) return;
    g.closing = 1;
    fprintf(stderr, "[host window] closing (%s)\n", why);
    if (g.input.in) hic_focus_lost(&g.input);
    if (g.ctl) atomic_store(&g.ctl->closed, 1);
    if (g.child <= 0) exit(0);
    kill(g.child, SIGTERM);
    const pid_t pid = g.child;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (g.child == pid) {
            fprintf(stderr, "[host window] pid %d still running; SIGKILL\n", pid);
            kill(pid, SIGKILL);
            /* Last resort if the exit notification never reaps it. */
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                if (g.child == pid) child_exited();
            });
        }
    });
}

static pid_t spawn_child(char** argv)
{
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_init(&fa);
    sigset_t def, none;
    sigemptyset(&def);
    sigaddset(&def, SIGINT); sigaddset(&def, SIGTERM); sigaddset(&def, SIGHUP); sigaddset(&def, SIGPIPE);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setsigmask(&attr, &none);
    /* stdin/stdout/stderr and nothing else of ours. */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT);
    for (int fd = 0; fd < 3; fd++) posix_spawn_file_actions_addinherit_np(&fa, fd);
    pid_t pid = 0;
    int rc = strchr(argv[0], '/') ? posix_spawn(&pid, argv[0], &fa, &attr, argv, environ)
                                  : posix_spawnp(&pid, argv[0], &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "[host window] cannot run %s: %s\n", argv[0], strerror(rc));
        return -1;
    }
    return pid;
}

/* ---- AppKit -------------------------------------------------------------- */

/* ---- window modes -------------------------------------------------------- */

static int native_fullscreen(void)
{
    return g.window && (g.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

static void enter_borderless(void)
{
    if (!g.window || g.borderless || native_fullscreen()) return;
    NSScreen* scr = g.window.screen ? g.window.screen : [NSScreen mainScreen];
    if (!scr) return;
    g.saved_frame = g.window.frame;
    g.saved_style = g.window.styleMask;
    g.borderless  = 1;
    [NSApp setPresentationOptions:(NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar)];
    g.window.styleMask = NSWindowStyleMaskBorderless;
    [g.window setFrame:scr.frame display:YES];
    [g.window makeKeyAndOrderFront:nil];
    keep_drawable();
}

static void leave_borderless(void)
{
    if (!g.window || !g.borderless) return;
    g.borderless = 0;
    g.window.styleMask = g.saved_style;
    [g.window setFrame:g.saved_frame display:YES];
    [NSApp setPresentationOptions:NSApplicationPresentationDefault];
    [g.window makeKeyAndOrderFront:nil];
    keep_drawable();
}

/* Leave whichever fullscreen is on, else enter the kind asked for. */
static void toggle_fullscreen(int style)
{
    if (!g.window) return;
    if (g.borderless)          { leave_borderless(); return; }
    if (native_fullscreen())   { [g.window toggleFullScreen:nil]; return; }
    if (style == HD_BORDERLESS) enter_borderless();
    else                        [g.window toggleFullScreen:nil];
}

/* The in-game menu's display mode: 0 window, 1 macOS fullscreen, 2 borderless.
 * Native fullscreen animates, so borderless from there waits for it to end
 * (windowDidExitFullScreen). */
static void set_window_mode(int mode)
{
    if (!g.window) return;
    g.pending_borderless = 0;
    if (mode == HD_BORDERLESS) {
        g.fs_style = HD_BORDERLESS;
        if (g.borderless) return;
        if (native_fullscreen()) { g.pending_borderless = 1; [g.window toggleFullScreen:nil]; return; }
        enter_borderless();
    } else if (mode == HD_FULLSCREEN) {
        g.fs_style = HD_FULLSCREEN;
        if (g.borderless) leave_borderless();
        if (!native_fullscreen()) [g.window toggleFullScreen:nil];
    } else {
        if (g.borderless) leave_borderless();
        if (native_fullscreen()) [g.window toggleFullScreen:nil];
    }
}

static int window_mode_now(void)
{
    return g.borderless ? HD_BORDERLESS : native_fullscreen() ? HD_FULLSCREEN : HD_WINDOWED;
}

/* The input tick: the in-game menu's requests (control page), and what is in
 * effect for it to show. */
static void display_tick(void)
{
    if (!g.ctl) return;
    int32_t v;
    if ((v = rsx_host_display_take(&g.ctl->want_vsync)) >= 0 && g.layer) {
        g.layer.displaySyncEnabled = v ? YES : NO;
        fprintf(stderr, "[host window] vsync %s (in-game menu)\n", v ? "on" : "off");
    }
    if ((v = rsx_host_display_take(&g.ctl->want_window_mode)) >= 0 && v <= HD_BORDERLESS) {
        fprintf(stderr, "[host window] display mode %d (in-game menu)\n", v);
        set_window_mode(v);
    }
    if ((v = rsx_host_display_take(&g.ctl->want_upscale)) >= 0 && !g.upscale_dead) {
#if HOST_HAVE_METALFX
        if (g.upscale_on != (v != 0)) {
            g.upscale_on = v != 0;
            if (!g.upscale_on) { g.fx_scaler = nil; g.fx_out = nil; }
            fprintf(stderr, "[host window] MetalFX upscaling %s (in-game menu)\n", g.upscale_on ? "on" : "off");
            keep_drawable();
        }
#endif
    }
    atomic_store(&g.ctl->now_vsync, g.layer && g.layer.displaySyncEnabled ? 1 : 0);
    atomic_store(&g.ctl->now_window_mode, window_mode_now());
#if HOST_HAVE_METALFX
    atomic_store(&g.ctl->now_upscale, g.upscale_dead ? -1 : g.upscale_on);
#else
    atomic_store(&g.ctl->now_upscale, -1);
#endif
}

/* PS3_VSYNC, PS3_WINDOW_MODE, PS3_UPSCALE: the environment, else the in-game
 * menu's file (the game reads the same file for its own settings). */
static const char* display_setting(const char* name)
{
    const char* e = getenv(name);
    if (e) return e;
    static char buf[3][PS3OS_VAL];
    static int k;
    char* out = buf[k++ % 3];
    const char* ini = getenv("PS3_OVERLAY_INI");
    if (ps3os_lookup_file(ini, name, out, PS3OS_VAL)) {
        fprintf(stderr, "[host window] %s=%s (from %s)\n", name, out, ini);
        return out;
    }
    return NULL;
}

/* PS3_DISPLAY: by number (1 = the menu bar's screen) or by name. */
static NSScreen* pick_screen(void)
{
    NSArray<NSScreen*>* screens = [NSScreen screens];
    NSScreen* fallback = [NSScreen mainScreen];
    const char* e = getenv("PS3_DISPLAY");
    if (!e || !*e || !screens.count) return fallback;
    char* end = NULL;
    const long k = strtol(e, &end, 10);
    if (end != e && *end == 0) {
        if (k >= 1 && k <= (long)screens.count) return screens[(NSUInteger)(k - 1)];
        fprintf(stderr, "[host window] PS3_DISPLAY=%s: there are %lu screen(s), using the main one\n",
                e, (unsigned long)screens.count);
        return fallback;
    }
    NSString* want = [NSString stringWithUTF8String:e];
    for (int pass = 0; want && pass < 3; pass++)
        for (NSScreen* sc in screens) {
            NSString* n = sc.localizedName;
            if (!n) continue;
            if ((pass == 0 && [n isEqualToString:want]) ||
                (pass == 1 && [n caseInsensitiveCompare:want] == NSOrderedSame) ||
                (pass == 2 && [n rangeOfString:want options:NSCaseInsensitiveSearch].location != NSNotFound))
                return sc;
        }
    fprintf(stderr, "[host window] PS3_DISPLAY=%s: no such screen, using the main one. Screens:\n", e);
    int i = 1;
    for (NSScreen* sc in screens)
        fprintf(stderr, "[host window]   %d: %s\n", i++, sc.localizedName ? sc.localizedName.UTF8String : "?");
    return fallback;
}

@interface HostWindowDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation HostWindowDelegate
- (void)toggleNativeFullScreen:(id)sender { (void)sender; toggle_fullscreen(HD_FULLSCREEN); }
- (void)toggleBorderless:(id)sender { (void)sender; toggle_fullscreen(HD_BORDERLESS); }
- (void)toggleFullScreenStyle:(id)sender { (void)sender; toggle_fullscreen(g.fs_style); }
- (void)windowDidResize:(NSNotification*)note { (void)note; keep_drawable(); }
- (void)windowDidChangeBackingProperties:(NSNotification*)note { (void)note; keep_drawable(); }
- (void)windowDidEnterFullScreen:(NSNotification*)note { (void)note; keep_drawable(); }
- (void)windowDidExitFullScreen:(NSNotification*)note
{
    (void)note;
    keep_drawable();
    if (g.pending_borderless) { g.pending_borderless = 0; enter_borderless(); }
}
- (void)windowDidChangeOcclusionState:(NSNotification*)note
{
    (void)note;
    note_on_screen((g.window.occlusionState & NSWindowOcclusionStateVisible) != 0, CACurrentMediaTime());
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app
{
    (void)app;
    begin_close("quit");
    return NSTerminateCancel;               /* exit follows the game's */
}
- (void)windowWillClose:(NSNotification*)note
{
    (void)note;
    if (g.borderless) [NSApp setPresentationOptions:NSApplicationPresentationDefault];
    g.borderless = 0;
    begin_close("window closed");
}
/* No key-up or mouse-up arrives once focus is elsewhere. */
- (void)windowDidResignKey:(NSNotification*)note { (void)note; hic_focus_lost(&g.input); }
- (void)applicationDidResignActive:(NSNotification*)note { (void)note; hic_focus_lost(&g.input); }
@end

static void build_menu(NSString* title, id target)
{
    NSMenu* bar = [NSMenu new];
    NSMenuItem* app_item = [NSMenuItem new];
    [bar addItem:app_item];
    NSMenu* app_menu = [NSMenu new];
    [app_menu addItemWithTitle:[@"Quit " stringByAppendingString:title]
                        action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;
    NSMenuItem* view_item = [NSMenuItem new];
    [bar addItem:view_item];
    NSMenu* view_menu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem* fs = [view_menu addItemWithTitle:@"Toggle Full Screen"
                                          action:@selector(toggleNativeFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    fs.target = target;
    NSMenuItem* bl = [view_menu addItemWithTitle:@"Toggle Borderless Full Screen"
                                          action:@selector(toggleBorderless:) keyEquivalent:@"f"];
    bl.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    bl.target = target;
    /* Cmd+F: the kind PS3_WINDOW_MODE asked for (macOS's own otherwise) */
    NSMenuItem* any = [view_menu addItemWithTitle:@"Full Screen"
                                           action:@selector(toggleFullScreenStyle:) keyEquivalent:@"f"];
    any.target = target;
    view_item.submenu = view_menu;
    NSApp.mainMenu = bar;
}

static void make_window(u_int32_t w, u_int32_t h, NSScreen* scr, int vsync)
{
    double cw = w, ch = h;
    if (scr) {
        const NSRect vis = [NSWindow contentRectForFrameRect:scr.visibleFrame styleMask:NSWindowStyleMaskTitled];
        hd_shrink_to(vis.size.width, vis.size.height, &cw, &ch);
    }
    g.window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, cw, ch)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    g.window.releasedWhenClosed = NO;
    g.window.title = g.title;
    g.window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    g.window.acceptsMouseMovedEvents = YES;    /* the mouse camera */
    g.window.contentMinSize = NSMakeSize(320, 180);
    g.window.backgroundColor = [NSColor blackColor];
    [g.window center];
    /* center goes by the main screen; the middle of the chosen one instead
     * (fullscreen and borderless then cover that screen). */
    if (scr && scr != g.window.screen) {
        const NSRect vf = scr.visibleFrame, wf = g.window.frame;
        [g.window setFrameOrigin:NSMakePoint(floor(NSMidX(vf) - wf.size.width / 2),
                                             floor(NSMidY(vf) - wf.size.height / 2))];
    }
    g.layer = [CAMetalLayer layer];
    g.layer.device          = g.dev;
    g.layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    g.layer.framebufferOnly = NO;           /* a blit destination */
    g.layer.drawableSize    = CGSizeMake(w, h);
    /* letterbox: the picture keeps its shape, black bars around it */
    g.layer.contentsGravity = kCAGravityResizeAspect;
    g.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    g.layer.displaySyncEnabled = vsync ? YES : NO;   /* PS3_VSYNC */
    NSView* view = g.window.contentView;
    view.wantsLayer = YES;
    view.layer = g.layer;
    view.layerContentsPlacement = NSViewLayerContentsPlacementScaleProportionallyToFit;
}

static int usage(const char* self)
{
    fprintf(stderr, "usage: %s [--title T] [--size WxH] [--] <program> [args...]\n", self);
    return 2;
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        const char* title = getenv("PS3_TITLE");
        if (!title || !*title) title = HOST_WINDOW_TITLE;
        u_int32_t w = 1280, h = 720;
        { const char* e = getenv("PS3_WINDOW_SIZE");
          if (e && *e && !hd_size_from(e, &w, &h))
              fprintf(stderr, "[host window] PS3_WINDOW_SIZE=%s: expected <width>x<height>\n", e); }
        int i = 1;
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "--")) { i++; break; }
            if (!strcmp(argv[i], "--title") && i + 1 < argc) { title = argv[++i]; continue; }
            if (!strcmp(argv[i], "--size") && i + 1 < argc) {
                if (sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h ||
                    w > RSX_HOST_MAX_DIM || h > RSX_HOST_MAX_DIM) return usage(argv[0]);
                continue;
            }
            if (argv[i][0] == '-' && argv[i][1] == '-') return usage(argv[0]);
            break;
        }
        if (i >= argc) return usage(argv[0]);
        char** child_argv = argv + i;

        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        g.tick_to_s = (double)tb.numer / tb.denom / 1e9;
        const char* e = getenv("PS3RECOMP_METAL_FPS");
        g.fps_on = e && !strcmp(e, "1");
        e = getenv("PS3RECOMP_HOST_STATS");
        g.stats_on = e && *e && *e != '0';
        g.title = [NSString stringWithUTF8String:title];

        snprintf(g.name, sizeof g.name, "org.ps3recomp.host-window.%d", getpid());
        kern_return_t kr = bootstrap_check_in(bootstrap_port, g.name, &g.service);
        g.dev = MTLCreateSystemDefaultDevice();
        g.queue = [g.dev newCommandQueue];
        if (kr != KERN_SUCCESS || !g.queue || make_control() != 0) {
            fprintf(stderr, "[host window] cannot share a window (%s); running %s on its own\n",
                    kr != KERN_SUCCESS ? bootstrap_strerror(kr) : "no Metal device or memory", child_argv[0]);
            execvp(child_argv[0], child_argv);
            fprintf(stderr, "[host window] cannot run %s: %s\n", child_argv[0], strerror(errno));
            return 127;
        }

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        HostWindowDelegate* delegate = [HostWindowDelegate new];
        NSApp.delegate = delegate;
        build_menu(g.title, delegate);
        const int mode = hd_window_mode_from(display_setting("PS3_WINDOW_MODE"));
        g.fs_style = mode == HD_BORDERLESS ? HD_BORDERLESS : HD_FULLSCREEN;
        g.upscale_on = hd_upscale_from(display_setting("PS3_UPSCALE"));
#if !HOST_HAVE_METALFX
        g.upscale_dead = 1;
#endif
        make_window(w, h, pick_screen(), hd_vsync_from(display_setting("PS3_VSYNC")));
        g.window.delegate = delegate;
        if (make_surfaces(w, h) != 0) {
            fprintf(stderr, "[host window] cannot make %ux%u surfaces\n", w, h);
            return 1;
        }
        /* Presenting is the whole job of this process: keep App Nap off it
         * even while the window is covered. */
        id activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                              reason:@"presenting the game"];
        (void)activity;

        dispatch_queue_t mq = dispatch_get_main_queue();
        dispatch_source_t rx = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, g.service, 0, mq);
        dispatch_source_set_event_handler(rx, ^{ drain_messages(); });
        dispatch_resume(rx);

        static dispatch_source_t sigs[3];
        const int signals[3] = { SIGINT, SIGTERM, SIGHUP };
        for (int s = 0; s < 3; s++) {
            signal(signals[s], SIG_IGN);
            sigs[s] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)signals[s], 0, mq);
            const int sig = signals[s];
            dispatch_source_set_event_handler(sigs[s], ^{ begin_close(strsignal(sig)); });
            dispatch_resume(sigs[s]);
        }

        setenv(RSX_HOST_SURFACE_ENV, g.name, 1);
        g.child = spawn_child(child_argv);
        if (g.child <= 0) return 127;
        dispatch_source_t proc = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)g.child,
                                                        DISPATCH_PROC_EXIT, mq);
        dispatch_source_set_event_handler(proc, ^{ child_exited(); });
        dispatch_resume(proc);

        /* Keys and mouse to the game (ctl->input); a key is swallowed, or
         * AppKit beeps at every press. */
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskAny handler:^NSEvent*(NSEvent* ev) {
            return hic_event(&g.input, ev, g.window) ? nil : ev;
        }];
        dispatch_source_t input_tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mq);
        dispatch_source_set_timer(input_tick, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                                  100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(input_tick, ^{ hic_tick(&g.input); display_tick(); });
        dispatch_resume(input_tick);

        metal_frame_rate_reset(&g.fps, CACurrentMediaTime());
        dispatch_source_t timer = nil;
        if (g.fps_on || g.stats_on) {
            timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mq);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                                      NSEC_PER_SEC, 50 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(timer, ^{ stats_tick(); });
            dispatch_resume(timer);
        }

        [g.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        if (mode == HD_BORDERLESS) enter_borderless();
        else if (mode == HD_FULLSCREEN)        /* it animates: the window has to be up first */
            dispatch_async(mq, ^{ if (!native_fullscreen()) [g.window toggleFullScreen:nil]; });
        present_black();
        display_tick();                         /* what the in-game menu shows from the start */
        fprintf(stderr, "[host window] '%s' pid %d window %ld, game pid %d (%s)\n",
                title, getpid(), (long)g.window.windowNumber, g.child, g.name);
        [NSApp run];
    }
    return 0;
}
