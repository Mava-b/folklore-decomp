/* AppKit events -> ps3_host_input (host_input_cocoa.h) -> cellPad: what the
 * window host records in the control page and the game's pad reads back.
 *   - keys are recorded and swallowed; Cmd shortcuts pass to AppKit;
 *   - Shift/Control/Option come from flag changes, left and right apart;
 *   - Esc releases a captured mouse and still reaches the pad (START);
 *   - motion, buttons and the wheel count only when the game asks for the
 *     mouse and the cursor is captured (no window here: nothing counts);
 *   - the window lets go when the game no longer wants the mouse, and of
 *     everything when focus goes elsewhere;
 *   - a block in a page shared like the control page reads the same in cellPad;
 *   - the in-game menu's keys (F1, Insert, Cmd+,) are counted, Cmd+, is
 *     swallowed; while the menu is open the mouse is released and its
 *     wheel goes to the menu, and the pointer is "none" again once it closes.
 *
 *   clang -fobjc-arc -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_host_input_cocoa.m libs/input/tests/test_host_input_cocoa_pad.c \
 *         $(sdl2-config --libs) -framework AppKit -o /tmp/test_host_input_cocoa && /tmp/test_host_input_cocoa
 * (No window is opened and the cursor is never captured or moved.)
 */
#include "../host_input_cocoa.h"
#include "../../video/rsx_host_surface.h"
#include <assert.h>
#include <unistd.h>

/* cellPad (test_host_input_cocoa_pad.c) */
void ps3_pad_host_input_attach(ps3_host_input* in);
void test_pad_poll(unsigned* buttons, int* lx, int* ly, int* rx, int* ry);
void test_pad_reload_mouse(void);
#define CTRL_L3     0x0002u
#define CTRL_START  0x0008u
#define CTRL_CROSS  0x4000u
static unsigned s_btn; static int s_lx, s_ly, s_rx, s_ry;
static void poll(void) { test_pad_poll(&s_btn, &s_lx, &s_ly, &s_rx, &s_ry); }

static NSEvent* key(NSEventType t, unsigned short vk, NSEventModifierFlags mods)
{
    return [NSEvent keyEventWithType:t location:NSZeroPoint modifierFlags:mods timestamp:0
                        windowNumber:0 context:nil characters:@"x" charactersIgnoringModifiers:@"x"
                           isARepeat:NO keyCode:vk];
}

static NSEvent* key_repeat(unsigned short vk)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
                        windowNumber:0 context:nil characters:@"x" charactersIgnoringModifiers:@"x"
                           isARepeat:YES keyCode:vk];
}

static NSEvent* scroll(int lines)
{
    CGEventRef e = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, lines);
    NSEvent* ev = [NSEvent eventWithCGEvent:e];
    CFRelease(e);
    return ev;
}

void test_pad_menu(int open);

static NSEvent* flags(unsigned short vk, NSEventModifierFlags mods)
{
    return [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:mods
                           timestamp:0 windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@""
                           isARepeat:NO keyCode:vk];
}

static NSEvent* mouse(CGEventType type, int dx, int dy)
{
    CGEventRef e = CGEventCreateMouseEvent(NULL, type, CGPointMake(100, 100), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(e, kCGMouseEventDeltaX, dx);
    CGEventSetIntegerValueField(e, kCGMouseEventDeltaY, dy);
    NSEvent* ev = [NSEvent eventWithCGEvent:e];
    CFRelease(e);
    return ev;
}

static int held(const ps3_host_input* in, unsigned vk)
{
    return (atomic_load(&in->keys[vk >> 5]) >> (vk & 31)) & 1;
}

int main(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        unsetenv("PS3_INPUT_MAP");
        unsetenv("PS3_MOUSE");
        unsetenv("PS3_MOUSE_CAPTURE");
        unsetenv("PS3_INPUT_MODE");

        /* A control page as the window host makes one, shared with the game
         * through a memory entry: the game's mapping sees the window's writes. */
        vm_address_t win_addr = 0, game_addr = 0;
        assert(vm_allocate(mach_task_self(), &win_addr, RSX_HOST_CONTROL_BYTES, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
        memory_object_size_t size = RSX_HOST_CONTROL_BYTES;
        mach_port_t entry = MACH_PORT_NULL;
        assert(mach_make_memory_entry_64(mach_task_self(), &size, win_addr, VM_PROT_READ | VM_PROT_WRITE,
                                         &entry, MACH_PORT_NULL) == KERN_SUCCESS);
        assert(vm_map(mach_task_self(), &game_addr, RSX_HOST_CONTROL_BYTES, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                      VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE) == KERN_SUCCESS);
        assert(win_addr != game_addr);
        rsx_host_control* win_ctl = (rsx_host_control*)win_addr;
        rsx_host_control* game_ctl = (rsx_host_control*)game_addr;
        HostInputCocoa hic = { .in = &win_ctl->input };
        ps3_pad_host_input_attach(&game_ctl->input);

        /* keys: recorded, swallowed, and the game's pad sees them */
        assert(hic_event(&hic, key(NSEventTypeKeyDown, 0x0D, 0), nil) == 1);        /* W */
        assert(hic_event(&hic, key(NSEventTypeKeyDown, 0x31, 0), nil) == 1);        /* Space */
        assert(held(&win_ctl->input, 0x0D) && held(&game_ctl->input, 0x31));
        poll();
        assert(s_ly == 0 && s_btn == CTRL_CROSS);
        assert(hic_event(&hic, key(NSEventTypeKeyUp, 0x0D, 0), nil) == 1);
        assert(hic_event(&hic, key(NSEventTypeKeyUp, 0x31, 0), nil) == 1);
        poll();
        assert(s_ly == 128 && s_btn == 0);

        /* the game told the window what it wants of the mouse */
        assert(atomic_load(&win_ctl->input.game_flags) ==
               (PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MOUSE_CAMERA | PS3_HOST_INPUT_CAPTURE));

        /* Cmd+Q is the menu's: not swallowed, not recorded */
        assert(hic_event(&hic, key(NSEventTypeKeyDown, 0x0C, NSEventModifierFlagCommand), nil) == 0);
        assert(!held(&win_ctl->input, 0x0C));

        /* Shift and right Option from flag changes */
        assert(hic_event(&hic, flags(0x38, NSEventModifierFlagShift | 0x02), nil) == 0);
        assert(held(&win_ctl->input, 0x38) && !held(&win_ctl->input, 0x3C));
        poll();
        assert(s_btn == CTRL_L3);
        assert(hic_event(&hic, flags(0x3D, NSEventModifierFlagShift | 0x02 | NSEventModifierFlagOption | 0x40), nil) == 0);
        assert(held(&win_ctl->input, 0x3D) && held(&win_ctl->input, 0x38));
        assert(hic_event(&hic, flags(0x38, NSEventModifierFlagOption | 0x40), nil) == 0);
        assert(!held(&win_ctl->input, 0x38) && held(&win_ctl->input, 0x3D));
        assert(hic_event(&hic, flags(0x3D, 0), nil) == 0);
        assert(!held(&win_ctl->input, 0x3D));

        /* Esc: releases the cursor and pauses (START) */
        hic.captured = 1;                    /* as if a click had captured it */
        atomic_store(&win_ctl->input.captured, 1);
        assert(hic_event(&hic, key(NSEventTypeKeyDown, 0x35, 0), nil) == 1);
        assert(!hic.captured && atomic_load(&win_ctl->input.captured) == 0);
        poll();
        assert(s_btn == CTRL_START);
        hic_event(&hic, key(NSEventTypeKeyUp, 0x35, 0), nil);

        /* motion: not the game's while the cursor is free ... */
        assert(hic_event(&hic, mouse(kCGEventMouseMoved, 7, 3), nil) == 0);
        assert(atomic_load(&win_ctl->input.mouse_dx) == 0);
        /* ... and the game's once captured, in 1/16 px */
        hic.captured = 1; hic.cap_since = 0;
        assert(hic_event(&hic, mouse(kCGEventMouseMoved, 7, -3), nil) == 0);
        assert(atomic_load(&game_ctl->input.mouse_dx) == 7 * 16);
        assert(atomic_load(&game_ctl->input.mouse_dy) == -3 * 16);
        usleep(16 * 1000);
        poll();
        assert(s_rx > 128 && s_ry < 128);
        assert(atomic_load(&win_ctl->input.mouse_dx) == 0);    /* the game drained it */
        /* a button press while captured (no window: it is not ours) */
        assert(hic_event(&hic, mouse(kCGEventLeftMouseDown, 0, 0), nil) == 0);
        assert(atomic_load(&win_ctl->input.mouse_buttons) == 0);
        atomic_store(&win_ctl->input.mouse_buttons, 1);        /* as a capture-time press */
        assert(hic_event(&hic, mouse(kCGEventLeftMouseUp, 0, 0), nil) == 0);
        assert(atomic_load(&win_ctl->input.mouse_buttons) == 0);   /* a release always counts */

        /* the game switches its mouse camera off: the window lets go */
        setenv("PS3_MOUSE", "0", 1);
        test_pad_reload_mouse();
        poll();
        assert(atomic_load(&win_ctl->input.game_flags) == PS3_HOST_INPUT_ACTIVE);
        hic_tick(&hic);
        assert(!hic.captured);
        unsetenv("PS3_MOUSE");
        test_pad_reload_mouse();

        /* the in-game menu's keys: counted (a repeat does not count); F1 and
         * Insert are keys as well, Cmd+, is swallowed */
        const uint32_t t0 = atomic_load(&game_ctl->input.menu_toggle);
        assert(hic_event(&hic, key(NSEventTypeKeyDown, 0x7A, 0), nil) == 1);          /* F1 */
        assert(held(&win_ctl->input, 0x7A));
        assert(hic_event(&hic, key_repeat(0x7A), nil) == 1);
        hic_event(&hic, key(NSEventTypeKeyUp, 0x7A, 0), nil);
        hic_event(&hic, key(NSEventTypeKeyDown, 0x72, 0), nil);                       /* Insert / Help */
        hic_event(&hic, key(NSEventTypeKeyUp, 0x72, 0), nil);
        assert(hic_event(&hic, key(NSEventTypeKeyDown, 0x2B, NSEventModifierFlagCommand), nil) == 1);   /* Cmd+, */
        assert(!held(&win_ctl->input, 0x2B));
        assert(hic_event(&hic, key(NSEventTypeKeyDown, 0x0C, NSEventModifierFlagCommand), nil) == 0);   /* Cmd+Q still the app's */
        assert(atomic_load(&game_ctl->input.menu_toggle) == t0 + 3);

        /* the menu open: cellPad tells the window, which lets go of a captured
         * mouse at the next event and gives the wheel to the menu */
        test_pad_menu(1);
        poll();
        assert(atomic_load(&win_ctl->input.game_flags) == (PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MENU));
        assert(s_btn == 0);
        hic.captured = 1;
        atomic_store(&win_ctl->input.captured, 1);
        atomic_store(&win_ctl->input.wheel, 0);
        assert(hic_event(&hic, scroll(2), nil) == 0);                                 /* AppKit sees it too */
        assert(!hic.captured && atomic_load(&win_ctl->input.captured) == 0);
        assert(atomic_load(&game_ctl->input.wheel) != 0);
        atomic_store(&win_ctl->input.wheel, 0);
        /* a click outside any window of ours: not the menu's either */
        assert(hic_event(&hic, mouse(kCGEventLeftMouseDown, 0, 0), nil) == 0);
        assert(atomic_load(&win_ctl->input.mouse_buttons) == 0);
        hic_event(&hic, mouse(kCGEventLeftMouseUp, 0, 0), nil);
        /* motion is not the camera's while the menu is open */
        hic.captured = 1; hic.cap_since = 0;
        hic_event(&hic, mouse(kCGEventMouseMoved, 9, 9), nil);
        assert(atomic_load(&win_ctl->input.mouse_dx) == 0 && !hic.captured);
        /* the pointer stays while it is open, and is "none" once it closes */
        atomic_store(&win_ctl->input.pointer, 0x00400040u);
        hic_tick(&hic);
        assert(atomic_load(&win_ctl->input.pointer) == 0x00400040u);
        test_pad_menu(0);
        poll();
        assert(atomic_load(&win_ctl->input.game_flags) & PS3_HOST_INPUT_MOUSE_CAMERA);
        hic_tick(&hic);
        assert(atomic_load(&win_ctl->input.pointer) == PS3_HOST_POINTER_NONE);

        /* focus elsewhere: nothing stays held */
        hic_event(&hic, key(NSEventTypeKeyDown, 0x00, 0), nil);
        atomic_store(&win_ctl->input.mouse_buttons, 2);
        atomic_store(&win_ctl->input.wheel, 32);
        hic_focus_lost(&hic);
        for (int i = 0; i < 4; i++) assert(atomic_load(&game_ctl->input.keys[i]) == 0);
        assert(atomic_load(&game_ctl->input.mouse_buttons) == 0 && atomic_load(&game_ctl->input.wheel) == 0);
        /* (the app is not active here: a tick lets go of everything too) */
        hic_event(&hic, key(NSEventTypeKeyDown, 0x02, 0), nil);
        hic_tick(&hic);
        assert(!held(&game_ctl->input, 0x02));

        ps3_pad_host_input_attach(NULL);
        printf("test_host_input_cocoa: ok\n");
    }
    return 0;
}
