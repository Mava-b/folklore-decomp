/*
 * engine_capture_assist.cpp -- auto-capture for the boss Ids' capture
 * mechanics (Time, Beat, Balance, Stop & Go).
 *
 * With motion controls off (or no motion sensors reporting), the pad code
 * feeds every capture full-scale motion (cellPad_set_motion_mode 2). That
 * finishes the ordinary captures, but the boss captures want the right
 * motion at the right moment -- a flick only while the Id is red, a tilt
 * against its lean -- and constant motion loses as much as it gains.
 *
 * What the game keeps (found with engine_motion_watch.cpp and
 * engine_capture_finder.cpp; folklore/docs/STATUS.md 104): the player's
 * capture controller is a state machine whose capture states 1, 2, 3 and
 * 10 (func_00104D30, func_00103270, func_0010394C, func_00104308 in 1.10)
 * each count successes up to a target, and the capture is won when the
 * count reaches the target; the capture bar on screen is count / target.
 *
 * So while the controller is in one of those states and auto-capture is
 * in effect, the count is raised towards the target over about three
 * seconds, never lowered (a wrong move in the game can still take some off,
 * which the next step puts back), and the game finishes the capture itself
 * the moment the count reaches the target, the way it does for a player.
 *
 * Except state 2, the left / right swing (func_00103270 in 1.10,
 * func_00102B70 in 1.00; run-20261108-131244: the bar filled and stayed
 * full). It keeps a phase at +48 of the controller: 0 waiting for a swing,
 * 1 a swing landed -- wait for one the other way, 2 won -- after a short
 * wait it goes on to the state at +220 (the capture). The count is only
 * compared with the target as a swing lands, and constant motion never
 * swings back, so a full count was never noticed. Once the count is full,
 * the phase is set to 2 here, as the game does when the last swing lands.
 */
#include "engine_capture_assist.h"
#include "engine_region.h"
#include "engine_settings.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
static unsigned long long GetTickCount64()
{
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

#include <cstdint>
#include <cstdio>

extern "C"
{
    extern uint8_t* vm_base;
    int cellPad_motion_live(void);
}

namespace
{
    /* The capture controller (TOC slot of func_000FF864's first call), and
     * where its state and count live; 1.10 added 24 bytes before them. */
#if FOLKLORE_GAME_VERSION == 110
    constexpr uint32_t kController = 0x1014F0F0u;
    constexpr uint32_t kStateOffset = 208u;
    constexpr uint32_t kTargetOffset = 288u;
    constexpr uint32_t kCountOffset = 292u;
#else
    constexpr uint32_t kController = 0x1014F0D4u;
    constexpr uint32_t kStateOffset = 184u;
    constexpr uint32_t kTargetOffset = 264u;
    constexpr uint32_t kCountOffset = 268u;
#endif
    /* State 2's phase (see above); the same offset in both versions. */
    constexpr uint32_t kSwingState = 2u;
    constexpr uint32_t kSwingPhaseOffset = 48u;
    constexpr uint32_t kSwingPhaseWon = 2u;
    constexpr unsigned long long kStepMs = 100ull;
    constexpr int32_t kStepsToFill = 30;

    bool filling = false;
    uint32_t fillingState = 0;
    int32_t rampCount = 0;
    unsigned long long lastStepMs = 0;

    uint32_t readGuest32(uint32_t guestAddress)
    {
        const uint8_t* bytes = vm_base + guestAddress;
        return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
               (uint32_t)bytes[3];
    }

    void writeGuest32(uint32_t guestAddress, uint32_t value)
    {
        uint8_t* bytes = vm_base + guestAddress;
        bytes[0] = (uint8_t)(value >> 24);
        bytes[1] = (uint8_t)(value >> 16);
        bytes[2] = (uint8_t)(value >> 8);
        bytes[3] = (uint8_t)value;
    }

    bool isCaptureState(uint32_t state)
    {
        return state == 1u || state == 2u || state == 3u || state == 10u;
    }

    /* Auto-capture is in effect when motion controls are off, or on with no
     * controller whose motion sensors report (the pad code then finishes
     * captures by itself too). */
    bool autoCaptureInEffect()
    {
        return !engineSettings().motionControls || !cellPad_motion_live();
    }
}

void engineCaptureAssistUpdate()
{
    if (!vm_base)
    {
        return;
    }
    const uint32_t state = readGuest32(kController + kStateOffset);
    if (!isCaptureState(state) || !autoCaptureInEffect())
    {
        filling = false;
        return;
    }
    const int32_t target = (int32_t)readGuest32(kController + kTargetOffset);
    const int32_t count = (int32_t)readGuest32(kController + kCountOffset);
    if (target <= 0 || target > 100000)
    {
        filling = false;
        return;
    }
    const unsigned long long nowMs = GetTickCount64();
    if (!filling || fillingState != state)
    {
        filling = true;
        fillingState = state;
        rampCount = count > 0 ? count : 0;
        lastStepMs = nowMs;
        std::fprintf(stderr, "[capture-assist] capture state %u: filling the bar (%d of %d)\n", state, count,
                     target);
        std::fflush(stderr);
    }
    if (nowMs - lastStepMs >= kStepMs)
    {
        lastStepMs = nowMs;
        const int32_t step = target / kStepsToFill > 1 ? target / kStepsToFill : 1;
        rampCount = rampCount + step < target ? rampCount + step : target;
    }
    if (count < rampCount)
    {
        writeGuest32(kController + kCountOffset, (uint32_t)rampCount);
    }
    if (state == kSwingState && rampCount >= target)
    {
        const uint32_t phase = readGuest32(kController + kSwingPhaseOffset);
        if (phase < kSwingPhaseWon)
        {
            writeGuest32(kController + kSwingPhaseOffset, kSwingPhaseWon);
            std::fprintf(stderr, "[capture-assist] capture state 2: bar full, swing phase %u -> won\n", phase);
            std::fflush(stderr);
        }
    }
}

#ifndef _WIN32
/* Off Windows there is no engine menu, whose per-frame update drives this on
 * the present thread there; a thread of its own does the same about once a
 * frame, so boss captures still complete by themselves on macOS. */
#include <thread>
namespace
{
    struct CaptureAssistDriver
    {
        CaptureAssistDriver()
        {
            std::thread([] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    if (vm_base)
                    {
                        engineCaptureAssistUpdate();
                    }
                }
            }).detach();
        }
    } captureAssistDriver;
}
#endif
