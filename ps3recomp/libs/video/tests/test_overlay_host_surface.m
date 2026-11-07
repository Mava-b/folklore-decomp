/* The in-game menu (ps3_overlay.mm) in host-surface mode, against a launcher
 * fixture in this process (no window is opened): the menu draws into the
 * launcher's IOSurface, the keys the window counted in the control page
 * open and close it, the game's input is held off while it is open, and its
 * display options go to the launcher through the page.
 *
 *   T=/tmp/ovl_test; mkdir -p $T
 *   for f in third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
 *            third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp; do
 *     clang++ -std=c++17 -w -c -I third_party/imgui $f -o $T/$(basename $f).o; done
 *   clang++ -std=c++17 -fobjc-arc -w -c -I third_party/imgui third_party/imgui/backends/imgui_impl_metal.mm -o $T/impl.o
 *   clang++ -std=c++17 -fobjc-arc -c -I include -I third_party/imgui libs/video/ps3_overlay.mm -o $T/ovl.o
 *   clang -fobjc-arc -I include -I libs/video -c libs/video/tests/test_overlay_host_surface.m -o $T/t.o
 *   clang -std=gnu17 -I include -c libs/video/tests/test_overlay_host_surface_pad.c -o $T/pad.o
 *   clang++ $T/*.o libs/video/rsx_texture_layout.c libs/video/rsx_vertex_fetch.c libs/video/rsx_vp_decompiler.c \
 *     libs/video/rsx_fp_decompiler.c libs/video/rsx_shader_msl.cpp -I include -Wl,-dead_strip \
 *     -framework Metal -framework Cocoa -framework QuartzCore -framework IOSurface -o $T/test_overlay_host_surface
 *   $T/test_overlay_host_surface
 *
 * (The fixture is test_metal_host_surface.m's, cut down.) */
#include "../rsx_metal_backend.m"
#include <IOSurface/IOSurface.h>
#include <assert.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }
void rsx_set_backend(rsx_backend* b) { (void)b; }
void rsx_draw_engine_set_backend(const rsx_draw_backend* b) { (void)b; }
void rsx_draw_engine_set_default(int on) { (void)on; }
int  rsx_draw_engine_enabled(void) { return 1; }
int  rsx_draw_engine_init(u32 w, u32 h) { return eng_init(NULL, w, h); }
void rsx_draw_engine_shutdown(void) { eng_shutdown(NULL); }
void rsx_draw_engine_present(void) {}
u32  rsx_draw_engine_readback_center(void) { return 0; }
u32  rsx_draw_engine_guest_draws(void) { return 0; }
int  cellGcmOffsetIsDisplay(u32 off) { (void)off; return 1; }
u32  cellGcmResolveLocated(int location, u32 offset) { (void)location; return offset; }
u32  cellGcmResolveOffset(u32 offset) { return offset; }
u8*  vm_base;

/* What the menu drives: plain fixtures in test_overlay_host_surface_pad.c
 * (a translation unit of their own, so they win over the backend's weak
 * stand-ins at the link). */
extern volatile int      g_pad_overlay_open;
extern volatile uint32_t g_pad_overlay_toggle;

/* ---- the launcher fixture ------------------------------------------------ */
static mach_port_t       f_service, f_entry;
static rsx_host_control* f_ctl;
static IOSurfaceRef      f_surf[RSX_HOST_SLOTS];
static u32               f_w, f_h;

static void f_make_surfaces(u32 w, u32 h)
{
    for (int i = 0; i < RSX_HOST_SLOTS; i++) {
        if (f_surf[i]) CFRelease(f_surf[i]);
        NSDictionary* p = @{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
                             (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfacePixelFormat: @((u32)'BGRA') };
        f_surf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        assert(f_surf[i]);
        atomic_store(&f_ctl->state[i], RSX_HOST_FREE);
    }
    f_w = w; f_h = h;
    f_ctl->width = w; f_ctl->height = h;
    f_ctl->generation++;
}

static void* f_thread(void* unused)
{
    (void)unused;
    for (;;) {
        struct { rsx_host_attach_msg m; mach_msg_trailer_t t; } in;
        memset(&in, 0, sizeof in);
        if (mach_msg(&in.m.header, MACH_RCV_MSG, 0, sizeof in, f_service, 0, MACH_PORT_NULL)) continue;
        if (in.m.header.msgh_id == RSX_HOST_MSG_FRAME) continue;
        assert(in.m.header.msgh_id == RSX_HOST_MSG_ATTACH);
        if (in.m.width && (in.m.width != f_w || in.m.height != f_h)) f_make_surfaces(in.m.width, in.m.height);
        rsx_host_surfaces_msg r;
        memset(&r, 0, sizeof r);
        r.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
        r.header.msgh_size = sizeof r;
        r.header.msgh_remote_port = in.m.header.msgh_remote_port;
        r.header.msgh_id = RSX_HOST_MSG_SURFACES;
        r.body.msgh_descriptor_count = 1 + RSX_HOST_SLOTS;
        r.control.name = f_entry; r.control.disposition = MACH_MSG_TYPE_COPY_SEND;
        r.control.type = MACH_MSG_PORT_DESCRIPTOR;
        for (int i = 0; i < RSX_HOST_SLOTS; i++) {
            r.surface[i].name = IOSurfaceCreateMachPort(f_surf[i]);
            r.surface[i].disposition = MACH_MSG_TYPE_MOVE_SEND;
            r.surface[i].type = MACH_MSG_PORT_DESCRIPTOR;
        }
        r.width = f_w; r.height = f_h; r.generation = f_ctl->generation; r.launcher_pid = getpid();
        assert(mach_msg(&r.header, MACH_SEND_MSG, sizeof r, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) == KERN_SUCCESS);
    }
    return NULL;
}

static void f_start(void)
{
    char name[128];
    snprintf(name, sizeof name, "org.ps3recomp.test-overlay-host.%d", getpid());
    assert(bootstrap_check_in(bootstrap_port, name, &f_service) == KERN_SUCCESS);
    vm_address_t a = 0;
    assert(vm_allocate(mach_task_self(), &a, RSX_HOST_CONTROL_BYTES, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    memory_object_size_t sz = RSX_HOST_CONTROL_BYTES;
    assert(mach_make_memory_entry_64(mach_task_self(), &sz, a, VM_PROT_READ | VM_PROT_WRITE,
                                     &f_entry, MACH_PORT_NULL) == KERN_SUCCESS);
    f_ctl = (rsx_host_control*)a;
    f_ctl->magic = RSX_HOST_MAGIC; f_ctl->version = RSX_HOST_VERSION; f_ctl->launcher_pid = getpid();
    /* as tools/host_window makes it */
    atomic_store(&f_ctl->input.pointer, PS3_HOST_POINTER_NONE);
    rsx_host_display_init(f_ctl, RSX_HOST_DISPLAY_VSYNC | RSX_HOST_DISPLAY_WINDOW_MODE | RSX_HOST_DISPLAY_UPSCALE);
    atomic_store(&f_ctl->now_vsync, 1);
    atomic_store(&f_ctl->now_window_mode, 0);
    atomic_store(&f_ctl->now_upscale, -1);
    f_make_surfaces(1280, 720);
    pthread_t t;
    assert(pthread_create(&t, NULL, f_thread, NULL) == 0);
    setenv(RSX_HOST_SURFACE_ENV, name, 1);
}

/* The newest published frame: how many of its pixels differ from `bg`, and
 * its centre pixel. */
static int f_read(u32 bg, u32* centre)
{
    host_drain();
    static uint64_t shown;
    uint64_t seq = 0;
    const int slot = rsx_host_begin_read(f_ctl, shown, &seq);
    assert(slot >= 0);
    shown = seq;
    IOSurfaceRef s = f_surf[slot];
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    const u8* base = IOSurfaceGetBaseAddress(s);
    const size_t bpr = IOSurfaceGetBytesPerRow(s);
    int diff = 0;
    for (u32 y = 0; y < f_h; y += 4)
        for (u32 x = 0; x < f_w; x += 4) {
            u32 px; memcpy(&px, base + y * bpr + x * 4, 4);
            if (px != bg) diff++;
        }
    if (centre) memcpy(centre, base + (f_h / 2) * bpr + (f_w / 2) * 4, 4);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    rsx_host_end_read(f_ctl, slot);
    return diff;
}

static id<MTLTexture> solid(u32 w, u32 h, u32 bgra)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    u32* px = malloc((size_t)w * h * 4);
    for (u32 i = 0; i < w * h; i++) px[i] = bgra;
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4];
    free(px);
    return t;
}

static void frames(id<MTLTexture> t, int n) { for (int i = 0; i < n; i++) eng_encode_and_commit(t); }

int main(void)
{
    @autoreleasepool {
        unsetenv("PS3RECOMP_METAL_HEADLESS");
        unsetenv("PS3_OVERLAY"); unsetenv("PS3_OVERLAY_INI"); unsetenv("PS3_OVERLAY_FORCE_OPEN");
        f_start();
        assert(rsx_metal_backend_init(1280, 720, "Overlay test") == 0);
        assert(s_host && s_window == nil && NSApp == nil);
        /* cellPad gets the page's block (a mapping of its own) */
        atomic_store(&f_ctl->input.menu_clicks, 7);
        assert(ps3_pad_host_input() && atomic_load(&ps3_pad_host_input()->menu_clicks) == 7);
        atomic_store(&f_ctl->input.menu_clicks, 0);
        assert(g_ps3_overlay_active);                       /* the menu watches for its keys */
        const u32 bg = 0xFF204060u;
        id<MTLTexture> game = solid(1280, 720, bg);

        /* Closed: the launcher gets the game's frame untouched. */
        frames(game, 2);
        u32 c = 0;
        assert(f_read(bg, &c) == 0 && c == bg);
        puts("menu closed: frame untouched");

        /* F1 in the launcher's window: counted in the page; the menu opens,
         * says so to cellPad and to the window, and draws into the surface. */
        atomic_fetch_add(&f_ctl->input.menu_toggle, 1);
        frames(game, 3);
        assert(ps3_overlay_menu_open() && g_pad_overlay_open);
        assert(atomic_load(&f_ctl->input.game_flags) & PS3_HOST_INPUT_MENU);
        const int drawn = f_read(bg, &c);
        assert(drawn > 2000 && c != bg);
        printf("menu open: %d of %u sampled pixels drawn over the game\n", drawn, (f_w / 4) * (f_h / 4));

        /* Its display options go to the launcher. */
        assert(rsx_metal_vsync() == 1 && rsx_metal_window_mode() == 0 && rsx_metal_gfx_upscale() == -1);
        assert(rsx_metal_window_mode_caps() == 7 && rsx_metal_host_window());
        rsx_metal_set_vsync(0);
        rsx_metal_set_window_mode(2);
        assert(rsx_host_display_take(&f_ctl->want_vsync) == 0);
        assert(rsx_host_display_take(&f_ctl->want_window_mode) == 2);
        assert(rsx_host_display_take(&f_ctl->want_upscale) == -1);
        puts("display requests: ok");

        /* Esc (a key the window recorded) closes it; the frame is the game's again. */
        ps3_host_input_key(&f_ctl->input, 0x35, 1);
        frames(game, 2);
        ps3_host_input_key(&f_ctl->input, 0x35, 0);
        frames(game, 2);
        assert(!ps3_overlay_menu_open() && !g_pad_overlay_open);
        assert(!(atomic_load(&f_ctl->input.game_flags) & PS3_HOST_INPUT_MENU));
        /* (a "Settings kept for this session" notice may still be up in a corner) */
        assert(f_read(bg, &c) < drawn / 4 && c == bg);
        puts("Esc closes it: ok");

        /* L3+R3 (cellPad's count) opens it; Cmd+, twice in one frame is no change. */
        g_pad_overlay_toggle++;
        frames(game, 2);
        assert(ps3_overlay_menu_open());
        atomic_fetch_add(&f_ctl->input.menu_toggle, 2);
        frames(game, 2);
        assert(ps3_overlay_menu_open());
        atomic_fetch_add(&f_ctl->input.menu_toggle, 1);
        frames(game, 2);
        assert(!ps3_overlay_menu_open());
        puts("L3+R3 and the window's count: ok");

        rsx_metal_backend_shutdown();
        puts("test_overlay_host_surface: ok");
    }
    return 0;
}
