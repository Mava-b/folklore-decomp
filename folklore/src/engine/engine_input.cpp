/*
 * engine_input.cpp -- keyboard and mouse bindings (engine_input.h): the
 * actions, their defaults, key names, and the table that rebinds them.
 */
#include "engine_input.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <chrono>
static unsigned long long GetTickCount64()
{
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

namespace
{
    /* The groups the table is drawn in. */
    const char* const kGroupNames[] = {
        "Movement",
        "Camera (keys; the mouse turns it too)",
        "Buttons",
        "Shoulder buttons and sticks",
        "D-pad",
        "Menus and capturing",
    };
    constexpr int kGroupCount = (int)(sizeof(kGroupNames) / sizeof(kGroupNames[0]));

    /* cellPad.h's CELLPAD_KBM_* order. */
    const InputActionInfo kActions[kInputActionCount] = {
        {"move_up", "Move forward", 0},
        {"move_down", "Move back", 0},
        {"move_left", "Move left", 0},
        {"move_right", "Move right", 0},
        {"camera_up", "Camera up", 1},
        {"camera_down", "Camera down", 1},
        {"camera_left", "Camera left", 1},
        {"camera_right", "Camera right", 1},
        {"dpad_up", "D-pad up", 4},
        {"dpad_down", "D-pad down", 4},
        {"dpad_left", "D-pad left", 4},
        {"dpad_right", "D-pad right", 4},
        {"cross", "Cross", 2},
        {"circle", "Circle", 2},
        {"square", "Square", 2},
        {"triangle", "Triangle", 2},
        {"l1", "L1", 3},
        {"r1", "R1", 3},
        {"l2", "L2", 3},
        {"r2", "R2", 3},
        {"l3", "L3 (left stick press)", 3},
        {"r3", "R3 (right stick press)", 3},
        {"start", "Start", 5},
        {"select", "Select", 5},
        {"walk", "Walk (hold)", 0},
        {"capture_motion", "Capture motion (the controller's yank)", 5},
    };

    struct NamedCode
    {
        int code;
        const char* name;
    };

    const NamedCode kNamedCodes[] = {
        {0x01, "Mouse Left"},
        {0x02, "Mouse Right"},
        {0x04, "Mouse Middle"},
        {0x05, "Mouse 4"},
        {0x06, "Mouse 5"},
        {kInputWheelUp, "Wheel Up"},
        {kInputWheelDown, "Wheel Down"},
        {0x08, "Backspace"},
        {0x09, "Tab"},
        {0x0D, "Enter"},
        {0x13, "Pause"},
        {0x14, "Caps Lock"},
        {0x1B, "Escape"},
        {0x20, "Space"},
        {0x21, "Page Up"},
        {0x22, "Page Down"},
        {0x23, "End"},
        {0x24, "Home"},
        {0x25, "Left"},
        {0x26, "Up"},
        {0x27, "Right"},
        {0x28, "Down"},
        {0x2D, "Insert"},
        {0x2E, "Delete"},
        {0x6A, "Numpad *"},
        {0x6B, "Numpad +"},
        {0x6D, "Numpad -"},
        {0x6E, "Numpad ."},
        {0x6F, "Numpad /"},
        {0xA0, "Left Shift"},
        {0xA1, "Right Shift"},
        {0xA2, "Left Ctrl"},
        {0xA3, "Right Ctrl"},
        {0xA4, "Left Alt"},
        {0xA5, "Right Alt"},
        {0xBA, "Semicolon"},
        {0xBB, "Equals"},
        {0xBC, "Comma"},
        {0xBD, "Minus"},
        {0xBE, "Period"},
        {0xBF, "Slash"},
        {0xC0, "Backquote"},
        {0xDB, "Left Bracket"},
        {0xDC, "Backslash"},
        {0xDD, "Right Bracket"},
        {0xDE, "Quote"},
    };

    std::string lowerCase(const std::string& text)
    {
        std::string result = text;
        for (char& character : result)
        {
            character = (char)std::tolower((unsigned char)character);
        }
        return result;
    }

    std::string trim(const std::string& text)
    {
        size_t first = 0;
        size_t last = text.size();
        while (first < last && std::isspace((unsigned char)text[first]))
        {
            first++;
        }
        while (last > first && std::isspace((unsigned char)text[last - 1]))
        {
            last--;
        }
        return text.substr(first, last - first);
    }

    /* A code that can be bound by pressing it: keys and mouse buttons, but
     * not the either-side Shift/Ctrl/Alt codes (the sided ones are reported
     * too) or the Windows keys. */
    bool bindableByPress(int code)
    {
        if (code == 0x03 || code == 0x10 || code == 0x11 || code == 0x12 || code == 0x5B || code == 0x5C ||
            code == 0x5D)
        {
            return false;
        }
        return code > 0 && code < 0xFF;
    }

    bool keyDown(int code)
    {
#ifdef _WIN32
        return (GetAsyncKeyState(code) & 0x8000) != 0;
#else
        /* Off Windows the bindings are kept in the settings file, but the
         * runtime's keyboard-and-mouse play (cellPad.c) is Windows-only, so
         * nothing is captured by pressing it here. */
        (void)code;
        return false;
#endif
    }

    /* The first bindable key or mouse button held now, or 0. */
    int heldCode()
    {
        for (int code = 1; code < 0xFF; code++)
        {
            if (bindableByPress(code) && keyDown(code))
            {
                return code;
            }
        }
        return 0;
    }

    bool foregroundIsThisProcess()
    {
#ifndef _WIN32
        return false;
#else
        HWND foreground = GetForegroundWindow();
        if (!foreground)
        {
            return false;
        }
        DWORD processId = 0;
        GetWindowThreadProcessId(foreground, &processId);
        return processId == GetCurrentProcessId();
#endif
    }
}

const InputActionInfo& engineInputAction(int action)
{
    if (action < 0 || action >= kInputActionCount)
    {
        return kActions[0];
    }
    return kActions[action];
}

InputBindings engineInputDefaultBindings()
{
    InputBindings bindings = {};
    const auto set = [&bindings](int action, int first, int second)
    {
        bindings[action][0] = first;
        bindings[action][1] = second;
    };
    set(0, 'W', 0);                      /* move */
    set(1, 'S', 0);
    set(2, 'A', 0);
    set(3, 'D', 0);
    set(8, 0x26, 0);                     /* d-pad: the arrows */
    set(9, 0x28, 0);
    set(10, 0x25, 0);
    set(11, 0x27, 0);
    set(12, 0x20, 0x0D);                 /* cross: Space, Enter */
    set(13, 'E', 0x08);                  /* circle: E, Backspace */
    set(14, 0x01, 0);                    /* square: left mouse button */
    set(15, 0x02, 0);                    /* triangle: right mouse button */
    set(16, 'Q', 0);                     /* L1 */
    set(17, 'F', 0);                     /* R1 */
    set(18, '1', 0);                     /* L2 */
    set(19, '3', 0);                     /* R2 */
    set(20, 0xA0, 0);                    /* L3: Left Shift */
    set(21, 0x04, 0);                    /* R3: middle mouse button */
    set(22, 0x1B, 0);                    /* Start: Escape */
    set(23, 0x09, 'M');                  /* Select: Tab, M */
    set(24, 0xA2, 0);                    /* walk: Left Ctrl */
    set(25, kInputWheelUp, 'G');         /* capture motion: wheel up, G */
    return bindings;
}

std::string engineInputCodeName(int code)
{
    if (code == 0)
    {
        return "";
    }
    for (const NamedCode& named : kNamedCodes)
    {
        if (named.code == code)
        {
            return named.name;
        }
    }
    char text[24];
    if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z'))
    {
        std::snprintf(text, sizeof(text), "%c", (char)code);
    }
    else if (code >= 0x60 && code <= 0x69)
    {
        std::snprintf(text, sizeof(text), "Numpad %d", code - 0x60);
    }
    else if (code >= 0x70 && code <= 0x87)
    {
        std::snprintf(text, sizeof(text), "F%d", code - 0x70 + 1);
    }
    else
    {
        std::snprintf(text, sizeof(text), "Key 0x%02X", code);
    }
    return text;
}

int engineInputCodeFromName(const std::string& name)
{
    const std::string wanted = lowerCase(trim(name));
    if (wanted.empty() || wanted == "none")
    {
        return 0;
    }
    for (const NamedCode& named : kNamedCodes)
    {
        if (lowerCase(named.name) == wanted)
        {
            return named.code;
        }
    }
    if (wanted.size() == 1 && std::isalnum((unsigned char)wanted[0]))
    {
        return std::toupper((unsigned char)wanted[0]);
    }
    int number = 0;
    if (std::sscanf(wanted.c_str(), "numpad %d", &number) == 1 && number >= 0 && number <= 9)
    {
        return 0x60 + number;
    }
    if (wanted[0] == 'f' && std::sscanf(wanted.c_str() + 1, "%d", &number) == 1 && number >= 1 && number <= 24)
    {
        return 0x70 + number - 1;
    }
    unsigned hexadecimal = 0;
    if (std::sscanf(wanted.c_str(), "key 0x%x", &hexadecimal) == 1 && hexadecimal > 0 && hexadecimal < 0xFF)
    {
        return (int)hexadecimal;
    }
    return 0;
}

std::string engineInputFormatBinding(const InputBinding& binding)
{
    std::string text;
    for (int slot = 0; slot < 2; slot++)
    {
        if (binding[slot] == 0)
        {
            continue;
        }
        if (!text.empty())
        {
            text += ", ";
        }
        text += engineInputCodeName(binding[slot]);
    }
    return text.empty() ? "none" : text;
}

InputBinding engineInputParseBinding(const char* text)
{
    InputBinding binding = {0, 0};
    const std::string all = text ? text : "";
    size_t start = 0;
    int slot = 0;
    while (slot < 2 && start <= all.size())
    {
        size_t comma = all.find(',', start);
        if (comma == std::string::npos)
        {
            comma = all.size();
        }
        const int code = engineInputCodeFromName(all.substr(start, comma - start));
        if (code != 0)
        {
            binding[slot] = code;
            slot++;
        }
        start = comma + 1;
    }
    return binding;
}

bool engineInputCaptureActive(const InputBindingCapture& capture)
{
    return capture.action >= 0;
}

bool engineInputDrawBindings(InputBindings& bindings, InputBindingCapture& capture, int wheelNotches,
                             float bindingWidth)
{
    bool changed = false;
    const unsigned long long nowMs = GetTickCount64();

    /* Waiting for a key: first for everything to be let go (the click or key
     * that started it), then for the next press. */
    if (capture.action >= 0)
    {
        if (!foregroundIsThisProcess() || nowMs - capture.startedMs > 10000ull)
        {
            capture.action = -1;
        }
        else if (capture.waitingForRelease)
        {
            if (heldCode() == 0)
            {
                capture.waitingForRelease = false;
            }
        }
        else
        {
            int code = heldCode();
            if (code == 0 && wheelNotches > 0)
            {
                code = kInputWheelUp;
            }
            else if (code == 0 && wheelNotches < 0)
            {
                code = kInputWheelDown;
            }
            if (code == 0x2E)
            {
                bindings[capture.action][capture.slot] = 0;     /* Delete: empty */
                capture.action = -1;
                changed = true;
            }
            else if (code != 0)
            {
                for (InputBinding& other : bindings)
                {
                    for (int& otherCode : other)
                    {
                        if (otherCode == code)
                        {
                            otherCode = 0;
                        }
                    }
                }
                bindings[capture.action][capture.slot] = code;
                capture.action = -1;
                changed = true;
            }
        }
    }

    const float labelWidth = ImGui::GetFontSize() * 13.0f;
    for (int group = 0; group < kGroupCount; group++)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", kGroupNames[group]);
        for (int action = 0; action < kInputActionCount; action++)
        {
            if (kActions[action].group != group)
            {
                continue;
            }
            ImGui::PushID(action);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kActions[action].label);
            for (int slot = 0; slot < 2; slot++)
            {
                ImGui::SameLine(labelWidth + (float)slot * (bindingWidth + ImGui::GetStyle().ItemSpacing.x));
                const bool listening = capture.action == action && capture.slot == slot;
                std::string label;
                if (listening)
                {
                    label = "Press a key...";
                }
                else if (bindings[action][slot] == 0)
                {
                    label = "-";
                }
                else
                {
                    label = engineInputCodeName(bindings[action][slot]);
                }
                label += slot == 0 ? "##first" : "##second";
                if (ImGui::Button(label.c_str(), ImVec2(bindingWidth, 0.0f)))
                {
                    capture.action = action;
                    capture.slot = slot;
                    capture.waitingForRelease = true;
                    capture.startedMs = nowMs;
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && bindings[action][slot] != 0)
                {
                    bindings[action][slot] = 0;
                    if (listening)
                    {
                        capture.action = -1;
                    }
                    changed = true;
                }
            }
            ImGui::PopID();
        }
    }
    return changed;
}
