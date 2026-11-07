/*
 * ps3recomp - keyboard and mouse state handed to the host pad (cellPad.c)
 *
 * The keyboard/mouse pad reads one of these. Whoever owns the window fills
 * it in:
 *
 *   - the window host (tools/host_window, macOS): the window lives in the
 *     launcher process and the game has none, so the block sits in the
 *     control page both processes map (rsx_host_surface.h). The launcher
 *     writes key and mouse state, the game reads it -- no messages, no
 *     extra thread, and it survives the game's exec like the rest of the
 *     page;
 *   - the Metal backend's own window (no window host): a block in the game
 *     process that the backend's event pump fills.
 *
 * Every field is a lock-free 32-bit atomic, so the two processes can share
 * it through the mapped page.
 *
 *   keys[4]          macOS virtual key codes 0-127 held down, one bit each
 *   mouse_dx/dy      motion since the game last drained it, 1/16 px
 *                    (deltaY > 0 = down); the game exchanges it with 0
 *   wheel            wheel turns since the last drain, 1/16 lines, + = away
 *                    from the player whatever the scroll-direction setting
 *   mouse_buttons    bit n = NSEvent buttonNumber n (1 left, 2 right,
 *                    4 middle, 8 button 4, ...)
 *   game_flags       written by the game: what the window should do with
 *                    the mouse (PS3_HOST_INPUT_*)
 *   captured         written by the window: 1 while the cursor is captured
 *   pointer          written by the window while the game's in-game menu is
 *                    open (PS3_HOST_INPUT_MENU): where the cursor is over
 *                    the picture (ps3_host_input_pointer_pack), or
 *                    PS3_HOST_POINTER_NONE
 *   menu_toggle      counted up by the window at each press of the menu's
 *                    keys (F1, Insert/Help, Cmd+,), so a tap between two of
 *                    the game's frames is not lost (libs/video/ps3_overlay.mm)
 *   menu_clicks      counted up at each left click while the menu is open
 *                    (a click that goes down and up between two frames)
 *
 * Plain C, header only: cellPad.c, the Metal backend, the window host and the
 * tests share it.
 */
#ifndef PS3EMU_HOST_INPUT_H
#define PS3EMU_HOST_INPUT_H

#include <stdatomic.h>
#include <stdint.h>

/* game_flags */
#define PS3_HOST_INPUT_MOUSE_CAMERA  0x1u   /* the mouse turns the camera now */
#define PS3_HOST_INPUT_CAPTURE       0x2u   /* ... and wants the cursor captured on a click */
#define PS3_HOST_INPUT_ACTIVE        0x4u   /* a cellPad that reads this block is running */
#define PS3_HOST_INPUT_MENU          0x8u   /* the in-game menu is open: release the mouse, and
                                               record the pointer, buttons and wheel for it */

#define PS3_HOST_POINTER_NONE        0xFFFFFFFFu

typedef struct {
    _Atomic uint32_t keys[4];
    _Atomic int32_t  mouse_dx, mouse_dy, wheel;
    _Atomic uint32_t mouse_buttons;
    _Atomic uint32_t game_flags;
    _Atomic uint32_t captured;
    _Atomic uint32_t pointer;
    _Atomic uint32_t menu_toggle;
    _Atomic uint32_t menu_clicks;
    uint32_t         reserved[3];
} ps3_host_input;

_Static_assert(sizeof(ps3_host_input) == 64, "ps3_host_input layout");

/* A key went down (1) or up (0). */
static inline void ps3_host_input_key(ps3_host_input* in, unsigned vk, int down)
{
    vk &= 127u;
    if (down) atomic_fetch_or(&in->keys[vk >> 5], 1u << (vk & 31));
    else      atomic_fetch_and(&in->keys[vk >> 5], ~(1u << (vk & 31)));
}

/* The pointer for the menu: (px, py) in a view of vw x vh points, origin at
 * the top left, where the picture is letterboxed to `aspect` (width/height;
 * 0 = the whole view). Packed as x | y << 16, each 0..65534 across the
 * picture; PS3_HOST_POINTER_NONE when it is outside it. */
static inline uint32_t ps3_host_input_pointer_pack(double px, double py, double vw, double vh, double aspect)
{
    if (!(vw > 0) || !(vh > 0)) return PS3_HOST_POINTER_NONE;
    double fw = vw, fh = vh, x0 = 0, y0 = 0;
    if (aspect > 0) {
        if (vw / vh > aspect) { fw = vh * aspect; x0 = (vw - fw) / 2; }
        else                  { fh = vw / aspect; y0 = (vh - fh) / 2; }
    }
    const double nx = (px - x0) / fw, ny = (py - y0) / fh;
    if (!(nx >= 0.0 && nx <= 1.0 && ny >= 0.0 && ny <= 1.0)) return PS3_HOST_POINTER_NONE;
    const uint32_t x = (uint32_t)(nx * 65534.0 + 0.5), y = (uint32_t)(ny * 65534.0 + 0.5);
    return x | (y << 16);
}

/* 0..1 across the picture. */
static inline void ps3_host_input_pointer_unpack(uint32_t p, double* nx, double* ny)
{
    *nx = (double)(p & 0xFFFFu) / 65534.0;
    *ny = (double)(p >> 16) / 65534.0;
}

/* Nothing held: focus went elsewhere, so no key-up will arrive. */
static inline void ps3_host_input_release_all(ps3_host_input* in)
{
    for (int i = 0; i < 4; i++) atomic_store(&in->keys[i], 0);
    atomic_store(&in->mouse_buttons, 0);
    atomic_store(&in->wheel, 0);
}

#endif /* PS3EMU_HOST_INPUT_H */
