/*
 * engine_input.h -- keyboard and mouse bindings, shared by the game's engine
 * menu and the launcher.
 *
 * The game only knows a PS3 controller, so the keyboard and mouse drive a
 * pad: each action below is a pad button, a direction of a stick, or one of
 * two extras (walk; the capture motion). Every action takes two bindings,
 * each a key or mouse button (a Windows virtual-key code) or a wheel
 * direction. The runtime (cellPad.c, cellPad_set_keyboard_mouse) combines
 * them with any controller, so both work at once; the mouse turns the camera.
 *
 * The actions are in cellPad.h's CELLPAD_KBM_* order: the settings pass the
 * table across as it is.
 */
#pragma once

#include <array>
#include <string>

constexpr int kInputActionCount = 26;
constexpr int kInputWheelUp = 0x101;      /* CELLPAD_KBM_WHEEL_UP */
constexpr int kInputWheelDown = 0x102;    /* CELLPAD_KBM_WHEEL_DOWN */

using InputBinding = std::array<int, 2>;
using InputBindings = std::array<InputBinding, kInputActionCount>;

struct InputActionInfo
{
    const char* key;       /* in the settings file: key_<key> */
    const char* label;     /* for people */
    int group;             /* index into the groups the tables are drawn in */
};

const InputActionInfo& engineInputAction(int action);
InputBindings engineInputDefaultBindings();

/* A code's name ("W", "Mouse Left", "Wheel Up"); "" for 0. */
std::string engineInputCodeName(int code);
/* The code named so (any case), or 0. */
int engineInputCodeFromName(const std::string& name);

/* "W, Up" and back; "" or "none" for nothing. */
std::string engineInputFormatBinding(const InputBinding& binding);
InputBinding engineInputParseBinding(const char* text);

/* The binding table (ImGui). Click a binding, then press the key, mouse
 * button or wheel direction for it; Delete while waiting empties it, and a
 * right click on a binding empties it too. A key bound here is taken off any
 * other action. `wheelNotches`: wheel movement this frame (up positive), for
 * binding the wheel. Returns true when a binding changed. */
struct InputBindingCapture
{
    int action = -1;
    int slot = 0;
    bool waitingForRelease = true;
    unsigned long long startedMs = 0;
};

bool engineInputDrawBindings(InputBindings& bindings, InputBindingCapture& capture, int wheelNotches,
                             float bindingWidth);
/* True while the table waits for a key (the caller should not let that key
 * do anything else). */
bool engineInputCaptureActive(const InputBindingCapture& capture);
