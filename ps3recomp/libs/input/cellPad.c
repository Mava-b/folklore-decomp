/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

/* One stick, both axes together.
 *
 * Done per axis (above), an XInput stick's round gate never reaches the
 * corners: pushed fully to a diagonal it reads about 0.62 on each axis after
 * the per-axis deadzone, where a DualShock 3 -- whose gate is close to
 * square -- reads nearly full on both. Folklore decides walk or run from the
 * stick's deflection, so Keats walked whenever the stick was in a corner.
 *
 * Here the deadzone is radial (the stick's distance from centre), the rest of
 * the travel is rescaled to 0..1, and the round gate is stretched onto a
 * square one: the direction is kept and the larger axis reaches full
 * deflection at the edge of the gate. Up is 0 on the PS3, so Y is flipped.
 * Centre stays exactly 128 on both axes. */
static u8 pad_axis_to_u8(float value)
{
    int converted = value < 0.0f ? 128 + (int)lroundf(value * 128.0f)
                                 : 128 + (int)lroundf(value * 127.0f);
    if (converted < 0)
    {
        converted = 0;
    }
    if (converted > 255)
    {
        converted = 255;
    }
    return (u8)converted;
}

/* stickX right, stickY up, both -1..1; the deadzone as a fraction of full
 * travel. Also used for the DualShock 4, DualSense and Switch Pro sticks,
 * whose gates are round as well. */
static void pad_stick_pair(float stickX, float stickY, float deadzoneFraction, u8* outX, u8* outY)
{
    const float magnitude = sqrtf(stickX * stickX + stickY * stickY);
    if (magnitude <= deadzoneFraction)
    {
        *outX = 128;
        *outY = 128;
        return;
    }
    float deflection = (magnitude - deadzoneFraction) / (1.0f - deadzoneFraction);
    if (deflection > 1.0f)
    {
        deflection = 1.0f;
    }
    const float directionX = stickX / magnitude;
    const float directionY = stickY / magnitude;
    const float largerAxis = fabsf(directionX) > fabsf(directionY) ? fabsf(directionX) : fabsf(directionY);
    const float squareX = directionX / largerAxis * deflection;
    const float squareY = directionY / largerAxis * deflection;
    *outX = pad_axis_to_u8(squareX);
    *outY = pad_axis_to_u8(-squareY);
}

static void pad_xinput_stick_pair(short rawX, short rawY, short deadzone, u8* outX, u8* outY)
{
    pad_stick_pair((float)rawX / 32767.0f, (float)rawY / 32767.0f, (float)deadzone / 32767.0f, outX, outY);
}

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;

    PadHostState* hs = &s_host_state[0];
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->connected = 1;
        return;
    }

    static const struct { int vk; u16 btn; } map[] = {
        { VK_UP,     CELL_PAD_CTRL_UP },      { VK_DOWN,  CELL_PAD_CTRL_DOWN },
        { VK_LEFT,   CELL_PAD_CTRL_LEFT },    { VK_RIGHT, CELL_PAD_CTRL_RIGHT },
        { 'Z',       CELL_PAD_CTRL_CROSS },   { 'X',      CELL_PAD_CTRL_CIRCLE },
        { 'A',       CELL_PAD_CTRL_SQUARE },  { 'S',      CELL_PAD_CTRL_TRIANGLE },
        { 'Q',       CELL_PAD_CTRL_L1 },      { 'W',      CELL_PAD_CTRL_R1 },
        { '1',       CELL_PAD_CTRL_L2 },      { '2',      CELL_PAD_CTRL_R2 },
        { VK_RETURN, CELL_PAD_CTRL_START },   { VK_TAB,   CELL_PAD_CTRL_SELECT },
    };

    u16 btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) btns |= map[i].btn;

    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)((btns & CELL_PAD_CTRL_LEFT) ? 0 :
                         (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 128);
    hs->analog_ly = (u8)((btns & CELL_PAD_CTRL_UP) ? 0 :
                         (btns & CELL_PAD_CTRL_DOWN) ? 255 : 128);
    hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no XInput device)\n");
          fflush(stdout); } }
}
#endif

/* ---------------------------------------------------------------------------
 * DualShock 3 / SIXAXIS over HID (Windows)
 *
 * XInput has no motion sensors, so a title that reads the SIXAXIS (Folklore
 * captures Folks with a pull gesture) needs the real controller. Nefarius'
 * DsHidMini driver in its SXS mode (the one RPCS3 uses) presents a DS3 the
 * way Sony's sixaxis.sys does: HID device 054C:0268 whose state -- buttons,
 * sticks, pressure and the four sensor words -- is read as a feature report.
 * This follows RPCS3's ds3_pad_handler (Windows path): probe feature report
 * 0xF2 (else 0x00) to learn the report id, then read that report; the
 * 49-byte state follows the id byte:
 *
 *   [2] SELECT L3 R3 START UP RIGHT DOWN LEFT  (= CELL_PAD DIGITAL1)
 *   [3] L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE (= CELL_PAD DIGITAL2)
 *   [4] PS   [6..9] LX LY RX RY   [14..25] pressure, up..square
 *   [41] accel X  [43] accel Z  [45] accel Y  [47] gyro   (u16 little endian,
 *        already in the PS3's 0..1023 scale, 512 = zero)
 *
 * A thread polls it every 2 ms so a slow USB control transfer never stalls
 * the game's pad read. The controller takes port 0 when present; XInput pads
 * keep the other ports (and port 0 when there is no DS3). PAD_NO_DS3=1 turns
 * this off.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <winioctl.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#ifndef IOCTL_HID_GET_FEATURE
#define IOCTL_HID_GET_FEATURE CTL_CODE(FILE_DEVICE_KEYBOARD, 100, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#endif

/* One controller read over HID: the DS3 here, or a DualShock 4, DualSense or
 * Switch Pro Controller (further down), in the PS3's own terms. */
typedef struct {
    int connected;
    u16 buttons;            /* CELL_PAD_CTRL_* packing, as PadHostState */
    int psButton;
    u8  stickLeftX, stickLeftY, stickRightX, stickRightY;
    u8  pressure[12];       /* up right down left L2 R2 L1 R1 tri circle cross square */
    u16 sensor[4];          /* X Y Z G, 0..1023 */
    int hasMotion;          /* sensor[] is live (0: a report without the sensors) */
    unsigned long long reports;
} PadHidState;

static SRWLOCK      s_ds3_lock = SRWLOCK_INIT;
static PadHidState  s_ds3_state;
static HANDLE       s_ds3_thread = NULL;
static volatile LONG s_ds3_stop = 0;
/* Sensor source for the guest (cellPad_set_motion_mode): */
static volatile LONG s_motion_mode = 0;   /* see cellPad_set_motion_mode */

static int pad_ds3_feature(HANDLE device, u8* buffer, DWORD length)
{
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_HID_GET_FEATURE, buffer, length, buffer, length, &returned, NULL))
    {
        return 0;
    }
    return 1;
}

static u16 pad_ds3_sensor_word(const u8* bytes)
{
    u16 value = (u16)(bytes[0] | (bytes[1] << 8));
    if (value > 1023u)
    {
        /* Big-endian on some driver; take whichever reading is in range. */
        const u16 swapped = (u16)((bytes[0] << 8) | bytes[1]);
        if (swapped <= 1023u)
        {
            value = swapped;
        }
        else
        {
            value = 1023u;
        }
    }
    return value;
}

static HANDLE pad_ds3_open(u8* reportIdOut)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO deviceSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceSet == INVALID_HANDLE_VALUE)
    {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    interfaceData.cbSize = sizeof(interfaceData);
    for (DWORD interfaceIndex = 0;
         found == INVALID_HANDLE_VALUE &&
         SetupDiEnumDeviceInterfaces(deviceSet, NULL, &hidGuid, interfaceIndex, &interfaceData);
         interfaceIndex++)
    {
        DWORD detailSize = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceSet, &interfaceData, NULL, 0, &detailSize, NULL);
        if (!detailSize)
        {
            continue;
        }
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(detailSize);
        if (!detail)
        {
            continue;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(deviceSet, &interfaceData, detail, detailSize, NULL, NULL))
        {
            free(detail);
            continue;
        }
        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        free(detail);
        if (device == INVALID_HANDLE_VALUE)
        {
            continue;
        }
        HIDD_ATTRIBUTES attributes;
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(device, &attributes) ||
            attributes.VendorID != 0x054C || attributes.ProductID != 0x0268)
        {
            CloseHandle(device);
            continue;
        }
        u8 probe[0xFF];
        memset(probe, 0, sizeof(probe));
        probe[0] = 0xF2;
        if (!pad_ds3_feature(device, probe, sizeof(probe)) || probe[0] != 0xF2)
        {
            memset(probe, 0, sizeof(probe));
            probe[0] = 0x00;
            if (!pad_ds3_feature(device, probe, sizeof(probe)) || probe[0] != 0x00)
            {
                printf("[cellPad] DS3 054C:0268 found but its state report could not be read "
                       "(set DsHidMini to SXS mode)\n");
                fflush(stdout);
                CloseHandle(device);
                continue;
            }
        }
        *reportIdOut = probe[0];
        found = device;
    }
    SetupDiDestroyDeviceInfoList(deviceSet);
    return found;
}

static void pad_ds3_parse(const u8* report)
{
    PadHidState next;
    memset(&next, 0, sizeof(next));
    next.connected = 1;
    next.buttons = (u16)(report[2] | (report[3] << 8));
    next.psButton = (report[4] & 0x01) != 0;
    next.stickLeftX = report[6];
    next.stickLeftY = report[7];
    next.stickRightX = report[8];
    next.stickRightY = report[9];
    memcpy(next.pressure, report + 14, sizeof(next.pressure));
    next.sensor[0] = pad_ds3_sensor_word(report + 41);
    next.sensor[1] = pad_ds3_sensor_word(report + 45);
    next.sensor[2] = pad_ds3_sensor_word(report + 43);
    next.sensor[3] = pad_ds3_sensor_word(report + 47);
    next.hasMotion = 1;
    AcquireSRWLockExclusive(&s_ds3_lock);
    next.reports = s_ds3_state.reports + 1;
    s_ds3_state = next;
    ReleaseSRWLockExclusive(&s_ds3_lock);
}

static void pad_ds3_mark_disconnected(void)
{
    AcquireSRWLockExclusive(&s_ds3_lock);
    s_ds3_state.connected = 0;
    ReleaseSRWLockExclusive(&s_ds3_lock);
}

static DWORD WINAPI pad_ds3_thread(LPVOID unused)
{
    (void)unused;
    HANDLE device = INVALID_HANDLE_VALUE;
    u8 reportId = 0;
    unsigned long long nextScanMs = 0;
    while (!InterlockedCompareExchange(&s_ds3_stop, 0, 0))
    {
        if (device == INVALID_HANDLE_VALUE)
        {
            const unsigned long long nowMs = GetTickCount64();
            if (nowMs >= nextScanMs)
            {
                device = pad_ds3_open(&reportId);
                nextScanMs = nowMs + 2000ull;
                if (device != INVALID_HANDLE_VALUE)
                {
                    printf("[cellPad] DualShock 3 (SIXAXIS) connected over HID, report id 0x%02X\n", reportId);
                    fflush(stdout);
                }
            }
            if (device == INVALID_HANDLE_VALUE)
            {
                Sleep(100);
                continue;
            }
        }
        u8 buffer[64];
        memset(buffer, 0, sizeof(buffer));
        buffer[0] = reportId;
        if (!pad_ds3_feature(device, buffer, sizeof(buffer)))
        {
            printf("[cellPad] DualShock 3 disconnected\n");
            fflush(stdout);
            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
            pad_ds3_mark_disconnected();
            continue;
        }
        if (buffer[0] == reportId)
        {
            pad_ds3_parse(buffer + 1);
        }
        Sleep(2);
    }
    if (device != INVALID_HANDLE_VALUE)
    {
        CloseHandle(device);
    }
    pad_ds3_mark_disconnected();
    return 0;
}

static void pad_ds3_start(void)
{
    static int disabled = -1;
    if (disabled < 0)
    {
        disabled = getenv("PAD_NO_DS3") ? 1 : 0;
    }
    if (disabled || s_ds3_thread)
    {
        return;
    }
    InterlockedExchange(&s_ds3_stop, 0);
    s_ds3_thread = CreateThread(NULL, 0, pad_ds3_thread, NULL, 0, NULL);
}

static int pad_ds3_snapshot(PadHidState* out)
{
    AcquireSRWLockShared(&s_ds3_lock);
    *out = s_ds3_state;
    ReleaseSRWLockShared(&s_ds3_lock);
    return out->connected;
}

/* ---------------------------------------------------------------------------
 * DualShock 4, DualSense and Switch Pro Controller over HID (Windows)
 *
 * None of these is an XInput device, so without this they are not seen at
 * all, and their motion sensors stand in for the SIXAXIS the same way RPCS3's
 * handlers use them. Over USB or Bluetooth, through Windows' own HID driver:
 *
 *   DualShock 4  054C:05C4, 054C:09CC, wireless adapter 054C:0BA0
 *     input report 0x01 (USB), 0x11-0x19 (Bluetooth, two more header bytes).
 *     Reading feature report 0x02 (and 0x05 over Bluetooth) is what switches
 *     a Bluetooth pad from its short 0x01 report to the full one.
 *   DualSense    054C:0CE6, Edge 054C:0DF2
 *     input report 0x01 (USB, 64 bytes), 0x31 (Bluetooth, one more header
 *     byte); over Bluetooth the full report also starts once feature reports
 *     0x05 / 0x09 / 0x20 have been read.
 *   Switch Pro   057E:2009
 *     needs to be told to send: over USB a handshake (output report 0x80:
 *     0x02, 0x03, 0x02, 0x04), then subcommands in output report 0x01 --
 *     0x40 01 (IMU on), 0x10 (read the sticks' factory calibration from SPI
 *     0x603D) and 0x03 30 (full input report 0x30, three IMU samples each).
 *
 * The layouts follow SDL's HIDAPI drivers (zlib licence), re-implemented
 * here. The sensors become the SIXAXIS's 0..1023 words as RPCS3 converts a
 * DualShock 4: acceleration in g times 113 about 512 (1 g down reads 399 on
 * Y, as a DS3 lying flat), yaw rate in degrees/s times 123/90 about 512,
 * every axis negated from the DualShock 4's own frame (X right, Y up, Z
 * toward the player). The Switch Pro's axes are turned into that frame first
 * (SDL's mapping). Sensor scales are the parts' nominal ones: DualShock 4 /
 * DualSense 8192 per g and 16 per degree/s, Switch Pro 4096 per g and 14.28
 * per degree/s.
 *
 * Like the DS3 it takes port 0; a DS3 wins if both are connected.
 * PAD_NO_HID_PADS=1 turns this off.
 * -----------------------------------------------------------------------*/
typedef enum {
    PAD_HID_NONE = 0,
    PAD_HID_DUALSHOCK4,
    PAD_HID_DUALSENSE,
    PAD_HID_SWITCH_PRO,
} PadHidKind;

typedef struct {
    u16 vendorId;
    u16 productId;
    PadHidKind kind;
} PadHidModel;

static const PadHidModel s_hid_models[] = {
    { 0x054C, 0x05C4, PAD_HID_DUALSHOCK4 },
    { 0x054C, 0x09CC, PAD_HID_DUALSHOCK4 },
    { 0x054C, 0x0BA0, PAD_HID_DUALSHOCK4 },   /* wireless adapter */
    { 0x054C, 0x0CE6, PAD_HID_DUALSENSE },
    { 0x054C, 0x0DF2, PAD_HID_DUALSENSE },    /* DualSense Edge */
    { 0x057E, 0x2009, PAD_HID_SWITCH_PRO },
};

static const char* pad_hid_kind_name(PadHidKind kind)
{
    switch (kind)
    {
    case PAD_HID_DUALSHOCK4:
        return "DualShock 4";
    case PAD_HID_DUALSENSE:
        return "DualSense";
    case PAD_HID_SWITCH_PRO:
        return "Switch Pro Controller";
    default:
        return NULL;
    }
}

typedef struct {
    HANDLE device;
    HANDLE ioEvent;
    PadHidKind kind;
    int bluetooth;
    USHORT inputLength;
    USHORT outputLength;
    USHORT featureLength;
    u8 switchPacketNumber;
    /* Switch Pro sticks: [stick][axis] centre and the travel either side */
    int stickCentre[2][2];
    int stickAbove[2][2];
    int stickBelow[2][2];
} PadHidDevice;

#define PAD_HID_MAX_REPORT 1024

static SRWLOCK       s_hidpad_lock = SRWLOCK_INIT;
static PadHidState   s_hidpad_state;
static volatile LONG s_hidpad_kind = PAD_HID_NONE;
static HANDLE        s_hidpad_thread = NULL;

static void pad_hid_close(PadHidDevice* pad)
{
    if (pad->device != INVALID_HANDLE_VALUE)
    {
        CancelIo(pad->device);
        CloseHandle(pad->device);
    }
    if (pad->ioEvent)
    {
        CloseHandle(pad->ioEvent);
    }
    memset(pad, 0, sizeof(*pad));
    pad->device = INVALID_HANDLE_VALUE;
}

/* The first supported controller that opens. */
static int pad_hid_open(PadHidDevice* pad)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO deviceSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceSet == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    int opened = 0;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    interfaceData.cbSize = sizeof(interfaceData);
    for (DWORD interfaceIndex = 0;
         !opened && SetupDiEnumDeviceInterfaces(deviceSet, NULL, &hidGuid, interfaceIndex, &interfaceData);
         interfaceIndex++)
    {
        DWORD detailSize = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceSet, &interfaceData, NULL, 0, &detailSize, NULL);
        if (!detailSize)
        {
            continue;
        }
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(detailSize);
        if (!detail)
        {
            continue;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(deviceSet, &interfaceData, detail, detailSize, NULL, NULL))
        {
            free(detail);
            continue;
        }
        /* Attributes first, through a handle that asks for no access, so
         * nothing else (keyboards, mice) is opened for reading. */
        HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
        if (probe == INVALID_HANDLE_VALUE)
        {
            free(detail);
            continue;
        }
        HIDD_ATTRIBUTES attributes;
        attributes.Size = sizeof(attributes);
        PadHidKind kind = PAD_HID_NONE;
        if (HidD_GetAttributes(probe, &attributes))
        {
            for (size_t modelIndex = 0; modelIndex < sizeof(s_hid_models) / sizeof(s_hid_models[0]); modelIndex++)
            {
                if (s_hid_models[modelIndex].vendorId == attributes.VendorID &&
                    s_hid_models[modelIndex].productId == attributes.ProductID)
                {
                    kind = s_hid_models[modelIndex].kind;
                }
            }
        }
        CloseHandle(probe);
        if (kind == PAD_HID_NONE)
        {
            free(detail);
            continue;
        }
        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, NULL);
        if (device == INVALID_HANDLE_VALUE && kind != PAD_HID_SWITCH_PRO)
        {
            /* Read-only is enough for the Sony pads over USB. */
            device = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        }
        free(detail);
        if (device == INVALID_HANDLE_VALUE)
        {
            printf("[cellPad] %s found but could not be opened (another program may have it exclusively)\n",
                   pad_hid_kind_name(kind));
            fflush(stdout);
            continue;
        }
        PHIDP_PREPARSED_DATA preparsed = NULL;
        HIDP_CAPS caps;
        memset(&caps, 0, sizeof(caps));
        if (!HidD_GetPreparsedData(device, &preparsed))
        {
            CloseHandle(device);
            continue;
        }
        HidP_GetCaps(preparsed, &caps);
        HidD_FreePreparsedData(preparsed);
        if (caps.InputReportByteLength == 0 || caps.InputReportByteLength > PAD_HID_MAX_REPORT)
        {
            CloseHandle(device);
            continue;
        }
        /* The fewest reports Windows will queue, so a read is never behind. */
        HidD_SetNumInputBuffers(device, 2);
        memset(pad, 0, sizeof(*pad));
        pad->device = device;
        pad->ioEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        pad->kind = kind;
        pad->inputLength = caps.InputReportByteLength;
        pad->outputLength = caps.OutputReportByteLength;
        pad->featureLength = caps.FeatureReportByteLength;
        /* Every one of them has a 64-byte input report over USB and a longer
         * one in its Bluetooth descriptor. */
        pad->bluetooth = caps.InputReportByteLength > 64;
        opened = pad->ioEvent != NULL;
        if (!opened)
        {
            pad_hid_close(pad);
        }
    }
    SetupDiDestroyDeviceInfoList(deviceSet);
    return opened;
}

/* One input report: its length, 0 when none came within timeoutMs, -1 when
 * the controller has gone. */
static int pad_hid_read(PadHidDevice* pad, u8* buffer, DWORD timeoutMs)
{
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = pad->ioEvent;
    ResetEvent(pad->ioEvent);
    DWORD bytesRead = 0;
    if (!ReadFile(pad->device, buffer, pad->inputLength, &bytesRead, &overlapped))
    {
        if (GetLastError() != ERROR_IO_PENDING)
        {
            return -1;
        }
        if (WaitForSingleObject(pad->ioEvent, timeoutMs) != WAIT_OBJECT_0)
        {
            CancelIo(pad->device);
            GetOverlappedResult(pad->device, &overlapped, &bytesRead, TRUE);
            return 0;
        }
        if (!GetOverlappedResult(pad->device, &overlapped, &bytesRead, FALSE))
        {
            return GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
        }
    }
    return (int)bytesRead;
}

static int pad_hid_write(PadHidDevice* pad, const u8* data, DWORD length)
{
    u8 buffer[PAD_HID_MAX_REPORT];
    DWORD writeLength = pad->outputLength ? pad->outputLength : length;
    if (writeLength > sizeof(buffer) || length > writeLength)
    {
        return 0;
    }
    memset(buffer, 0, writeLength);
    memcpy(buffer, data, length);
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = pad->ioEvent;
    ResetEvent(pad->ioEvent);
    DWORD written = 0;
    if (!WriteFile(pad->device, buffer, writeLength, &written, &overlapped))
    {
        if (GetLastError() != ERROR_IO_PENDING)
        {
            return 0;
        }
        if (WaitForSingleObject(pad->ioEvent, 200) != WAIT_OBJECT_0)
        {
            CancelIo(pad->device);
            GetOverlappedResult(pad->device, &overlapped, &written, TRUE);
            return 0;
        }
        if (!GetOverlappedResult(pad->device, &overlapped, &written, FALSE))
        {
            return 0;
        }
    }
    return 1;
}

/* A feature report, read only for what reading it does to the controller. */
static void pad_hid_touch_feature(PadHidDevice* pad, u8 reportId)
{
    u8 buffer[PAD_HID_MAX_REPORT];
    DWORD length = pad->featureLength ? pad->featureLength : 64;
    if (length > sizeof(buffer))
    {
        length = sizeof(buffer);
    }
    memset(buffer, 0, length);
    buffer[0] = reportId;
    HidD_GetFeature(pad->device, buffer, length);
}

/* ---- Switch Pro Controller commands ------------------------------------- */

/* Output report 0x80 (USB only); waits for its 0x81 acknowledgement. */
static int pad_switch_proprietary(PadHidDevice* pad, u8 command, int waitForReply)
{
    u8 packet[2] = { 0x80, command };
    u8 reply[PAD_HID_MAX_REPORT];
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (!pad_hid_write(pad, packet, sizeof(packet)))
        {
            continue;
        }
        if (!waitForReply)
        {
            return 1;
        }
        const unsigned long long deadlineMs = GetTickCount64() + 100ull;
        while (GetTickCount64() < deadlineMs)
        {
            const int bytesRead = pad_hid_read(pad, reply, 20);
            if (bytesRead < 0)
            {
                return 0;
            }
            if (bytesRead >= 2 && reply[0] == 0x81 && reply[1] == command)
            {
                return 1;
            }
        }
    }
    return 0;
}

/* Output report 0x01: neutral rumble, then a subcommand. Waits for the 0x21
 * reply and leaves it in replyOut (when given). */
static int pad_switch_subcommand(PadHidDevice* pad, u8 subcommand, const u8* arguments, u8 argumentLength,
                                 u8* replyOut)
{
    u8 packet[49];
    u8 reply[PAD_HID_MAX_REPORT];
    for (int attempt = 0; attempt < 3; attempt++)
    {
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x01;
        packet[1] = pad->switchPacketNumber;
        pad->switchPacketNumber = (u8)((pad->switchPacketNumber + 1) & 0x0F);
        static const u8 neutralRumble[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };
        memcpy(packet + 2, neutralRumble, sizeof(neutralRumble));
        packet[10] = subcommand;
        if (arguments && argumentLength)
        {
            memcpy(packet + 11, arguments, argumentLength);
        }
        if (!pad_hid_write(pad, packet, sizeof(packet)))
        {
            continue;
        }
        const unsigned long long deadlineMs = GetTickCount64() + 150ull;
        while (GetTickCount64() < deadlineMs)
        {
            const int bytesRead = pad_hid_read(pad, reply, 20);
            if (bytesRead < 0)
            {
                return 0;
            }
            /* [13] ack (bit 7 set), [14] the subcommand it answers. */
            if (bytesRead >= 15 && reply[0] == 0x21 && reply[14] == subcommand && (reply[13] & 0x80))
            {
                if (replyOut)
                {
                    memcpy(replyOut, reply, (size_t)bytesRead < 64 ? (size_t)bytesRead : 64);
                }
                return 1;
            }
        }
    }
    return 0;
}

static int pad_switch_unpack12(const u8* bytes, int highHalf)
{
    /* Two 12-bit values packed in three bytes. */
    if (highHalf)
    {
        return (bytes[1] >> 4) | (bytes[2] << 4);
    }
    return bytes[0] | ((bytes[1] & 0x0F) << 8);
}

static void pad_switch_load_stick_calibration(PadHidDevice* pad)
{
    for (int stick = 0; stick < 2; stick++)
    {
        for (int axis = 0; axis < 2; axis++)
        {
            pad->stickCentre[stick][axis] = 2048;
            pad->stickAbove[stick][axis] = 1433;
            pad->stickBelow[stick][axis] = 1433;
        }
    }
    /* SPI 0x603D, 18 bytes: left stick then right, 12-bit values in a
     * different order for each (SDL's LoadStickCalibration). */
    const u8 spiRead[5] = { 0x3D, 0x60, 0x00, 0x00, 18 };
    u8 reply[64];
    memset(reply, 0, sizeof(reply));
    if (!pad_switch_subcommand(pad, 0x10, spiRead, sizeof(spiRead), reply) ||
        reply[15] != 0x3D || reply[16] != 0x60)
    {
        printf("[cellPad] Switch Pro Controller: no stick calibration, using defaults\n");
        return;
    }
    const u8* left = reply + 20;
    const u8* right = reply + 29;
    pad->stickAbove[0][0] = pad_switch_unpack12(left + 0, 0);
    pad->stickAbove[0][1] = pad_switch_unpack12(left + 0, 1);
    pad->stickCentre[0][0] = pad_switch_unpack12(left + 3, 0);
    pad->stickCentre[0][1] = pad_switch_unpack12(left + 3, 1);
    pad->stickBelow[0][0] = pad_switch_unpack12(left + 6, 0);
    pad->stickBelow[0][1] = pad_switch_unpack12(left + 6, 1);
    pad->stickCentre[1][0] = pad_switch_unpack12(right + 0, 0);
    pad->stickCentre[1][1] = pad_switch_unpack12(right + 0, 1);
    pad->stickBelow[1][0] = pad_switch_unpack12(right + 3, 0);
    pad->stickBelow[1][1] = pad_switch_unpack12(right + 3, 1);
    pad->stickAbove[1][0] = pad_switch_unpack12(right + 6, 0);
    pad->stickAbove[1][1] = pad_switch_unpack12(right + 6, 1);
    for (int stick = 0; stick < 2; stick++)
    {
        for (int axis = 0; axis < 2; axis++)
        {
            /* 0xFFF: not written at the factory. */
            if (pad->stickCentre[stick][axis] == 0xFFF)
            {
                pad->stickCentre[stick][axis] = 2048;
            }
            if (pad->stickAbove[stick][axis] == 0xFFF || pad->stickAbove[stick][axis] < 256)
            {
                pad->stickAbove[stick][axis] = 1433;
            }
            if (pad->stickBelow[stick][axis] == 0xFFF || pad->stickBelow[stick][axis] < 256)
            {
                pad->stickBelow[stick][axis] = 1433;
            }
        }
    }
}

/* Get the controller sending its full reports. */
static void pad_hid_start(PadHidDevice* pad)
{
    switch (pad->kind)
    {
    case PAD_HID_DUALSHOCK4:
        pad_hid_touch_feature(pad, 0x02);
        if (pad->bluetooth)
        {
            pad_hid_touch_feature(pad, 0x05);
        }
        break;
    case PAD_HID_DUALSENSE:
        pad_hid_touch_feature(pad, 0x05);
        pad_hid_touch_feature(pad, 0x09);
        pad_hid_touch_feature(pad, 0x20);
        break;
    case PAD_HID_SWITCH_PRO:
    {
        if (!pad->bluetooth)
        {
            /* USB: a handshake before anything else; no answer means it is
             * really on Bluetooth after all. */
            if (pad_switch_proprietary(pad, 0x02, 1))
            {
                pad_switch_proprietary(pad, 0x03, 1);
                pad_switch_proprietary(pad, 0x02, 1);
                pad_switch_proprietary(pad, 0x04, 0);
            }
        }
        const u8 imuOn = 0x01;
        if (!pad_switch_subcommand(pad, 0x40, &imuOn, 1, NULL))
        {
            printf("[cellPad] Switch Pro Controller: no answer to 'IMU on'\n");
        }
        pad_switch_load_stick_calibration(pad);
        const u8 fullReports = 0x30;
        if (!pad_switch_subcommand(pad, 0x03, &fullReports, 1, NULL))
        {
            printf("[cellPad] Switch Pro Controller: no answer to 'full reports'\n");
        }
        fflush(stdout);
        break;
    }
    default:
        break;
    }
}

/* ---- Reports -> PS3 terms ----------------------------------------------- */

static s16 pad_hid_s16(const u8* bytes)
{
    return (s16)(u16)(bytes[0] | (bytes[1] << 8));
}

static u16 pad_hid_sensor_word(float value)
{
    if (value < 0.0f)
    {
        return 0;
    }
    if (value > 1023.0f)
    {
        return 1023;
    }
    return (u16)(value + 0.5f);
}

/* Acceleration in g and yaw rate in degrees/s, in the DualShock 4's frame:
 * X right, Y up out of the face, Z toward the player. */
static void pad_hid_set_motion(PadHidState* state, float accelRight, float accelUp, float accelToPlayer,
                               float yawDegreesPerSecond)
{
    state->sensor[0] = pad_hid_sensor_word(512.0f - 113.0f * accelRight);
    state->sensor[1] = pad_hid_sensor_word(512.0f - 113.0f * accelUp);
    state->sensor[2] = pad_hid_sensor_word(512.0f - 113.0f * accelToPlayer);
    state->sensor[3] = pad_hid_sensor_word(512.0f - yawDegreesPerSecond * (123.0f / 90.0f));
    state->hasMotion = 1;
}

/* The pressure words of a pad whose buttons are only on or off. */
static void pad_hid_digital_pressure(PadHidState* state, u8 leftTrigger, u8 rightTrigger)
{
    static const u16 pressureButtons[12] = {
        CELL_PAD_CTRL_UP, CELL_PAD_CTRL_RIGHT, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_LEFT,
        CELL_PAD_CTRL_L2, CELL_PAD_CTRL_R2, CELL_PAD_CTRL_L1, CELL_PAD_CTRL_R1,
        CELL_PAD_CTRL_TRIANGLE, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_SQUARE,
    };
    for (int pressureIndex = 0; pressureIndex < 12; pressureIndex++)
    {
        state->pressure[pressureIndex] = (state->buttons & pressureButtons[pressureIndex]) ? 255 : 0;
    }
    state->pressure[4] = leftTrigger;
    state->pressure[5] = rightTrigger;
}

static u16 pad_hid_hat_buttons(u8 hat)
{
    switch (hat & 0x0F)
    {
    case 0: return CELL_PAD_CTRL_UP;
    case 1: return CELL_PAD_CTRL_UP | CELL_PAD_CTRL_RIGHT;
    case 2: return CELL_PAD_CTRL_RIGHT;
    case 3: return CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_RIGHT;
    case 4: return CELL_PAD_CTRL_DOWN;
    case 5: return CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_LEFT;
    case 6: return CELL_PAD_CTRL_LEFT;
    case 7: return CELL_PAD_CTRL_UP | CELL_PAD_CTRL_LEFT;
    default: return 0;
    }
}

/* DualShock 4 and DualSense share the face/hat byte and the shoulder byte. */
static u16 pad_hid_sony_buttons(u8 hatAndFace, u8 shoulders)
{
    u16 buttons = pad_hid_hat_buttons(hatAndFace);
    if (hatAndFace & 0x10) { buttons |= CELL_PAD_CTRL_SQUARE; }
    if (hatAndFace & 0x20) { buttons |= CELL_PAD_CTRL_CROSS; }
    if (hatAndFace & 0x40) { buttons |= CELL_PAD_CTRL_CIRCLE; }
    if (hatAndFace & 0x80) { buttons |= CELL_PAD_CTRL_TRIANGLE; }
    if (shoulders & 0x01) { buttons |= CELL_PAD_CTRL_L1; }
    if (shoulders & 0x02) { buttons |= CELL_PAD_CTRL_R1; }
    if (shoulders & 0x04) { buttons |= CELL_PAD_CTRL_L2; }
    if (shoulders & 0x08) { buttons |= CELL_PAD_CTRL_R2; }
    if (shoulders & 0x10) { buttons |= CELL_PAD_CTRL_SELECT; }   /* Share / Create */
    if (shoulders & 0x20) { buttons |= CELL_PAD_CTRL_START; }    /* Options */
    if (shoulders & 0x40) { buttons |= CELL_PAD_CTRL_L3; }
    if (shoulders & 0x80) { buttons |= CELL_PAD_CTRL_R3; }
    return buttons;
}

/* A small radial deadzone: these sticks rest much closer to centre than an
 * XInput pad is assumed to. */
#define PAD_HID_STICK_DEADZONE 0.08f

/* A Sony stick byte (128 at rest) as -1..1, reaching both ends. */
static float pad_hid_sony_axis(u8 raw)
{
    const float offset = (float)raw - 128.0f;
    return offset >= 0.0f ? offset / 127.0f : offset / 128.0f;
}

/* Sony sticks are bytes, 128 at rest, down = 255 (the PS3's own sense). */
static void pad_hid_sony_sticks(const u8* sticks, PadHidState* state)
{
    pad_stick_pair(pad_hid_sony_axis(sticks[0]), -pad_hid_sony_axis(sticks[1]),
                   PAD_HID_STICK_DEADZONE, &state->stickLeftX, &state->stickLeftY);
    pad_stick_pair(pad_hid_sony_axis(sticks[2]), -pad_hid_sony_axis(sticks[3]),
                   PAD_HID_STICK_DEADZONE, &state->stickRightX, &state->stickRightY);
}

/* DualShock 4 state, from just after the report's header: sticks [0..3],
 * hat/face [4], shoulders [5], PS [6], triggers [7..8], gyro [12..17],
 * accelerometer [18..23]. */
static void pad_parse_dualshock4(const u8* data, int withSensors, PadHidState* state)
{
    pad_hid_sony_sticks(data, state);
    state->buttons = pad_hid_sony_buttons(data[4], data[5]);
    state->psButton = (data[6] & 0x01) != 0;
    pad_hid_digital_pressure(state, data[7], data[8]);
    if (withSensors)
    {
        pad_hid_set_motion(state, pad_hid_s16(data + 18) / 8192.0f, pad_hid_s16(data + 20) / 8192.0f,
                           pad_hid_s16(data + 22) / 8192.0f, pad_hid_s16(data + 14) / 16.0f);
    }
}

/* DualSense full state: sticks [0..3], triggers [4..5], hat/face [7],
 * shoulders [8], PS [9], gyro [15..20], accelerometer [21..26]. */
static void pad_parse_dualsense(const u8* data, PadHidState* state)
{
    pad_hid_sony_sticks(data, state);
    state->buttons = pad_hid_sony_buttons(data[7], data[8]);
    state->psButton = (data[9] & 0x01) != 0;
    pad_hid_digital_pressure(state, data[4], data[5]);
    pad_hid_set_motion(state, pad_hid_s16(data + 21) / 8192.0f, pad_hid_s16(data + 23) / 8192.0f,
                       pad_hid_s16(data + 25) / 8192.0f, pad_hid_s16(data + 17) / 16.0f);
}

/* DualSense over Bluetooth before its full reports: sticks [0..3],
 * hat/face [4], shoulders [5], PS [6], triggers [7..8], no sensors. */
static void pad_parse_dualsense_simple(const u8* data, PadHidState* state)
{
    pad_hid_sony_sticks(data, state);
    state->buttons = pad_hid_sony_buttons(data[4], data[5]);
    state->psButton = (data[6] & 0x01) != 0;
    pad_hid_digital_pressure(state, data[7], data[8]);
}

/* One Switch Pro stick axis, -1..1 (up and right positive). */
static float pad_switch_stick(const PadHidDevice* pad, int stick, int axis, int raw)
{
    const int offset = raw - pad->stickCentre[stick][axis];
    /* Full deflection at 70% of the calibrated travel, as SDL does. */
    const float travel = 0.7f * (float)(offset >= 0 ? pad->stickAbove[stick][axis] : pad->stickBelow[stick][axis]);
    float position = travel > 0.0f ? (float)offset / travel : 0.0f;
    if (position > 1.0f)
    {
        position = 1.0f;
    }
    if (position < -1.0f)
    {
        position = -1.0f;
    }
    return position;
}

/* Switch Pro report 0x30: buttons [3..5], sticks [6..11], three IMU samples
 * from [13], 12 bytes each (accel X Y Z, gyro X Y Z); the last is newest.
 * Buttons by position: B = Cross, A = Circle, Y = Square, X = Triangle. */
static void pad_parse_switch_pro(const PadHidDevice* pad, const u8* report, PadHidState* state)
{
    const u8 right = report[3];
    const u8 shared = report[4];
    const u8 left = report[5];
    u16 buttons = 0;
    if (right & 0x01) { buttons |= CELL_PAD_CTRL_SQUARE; }     /* Y */
    if (right & 0x02) { buttons |= CELL_PAD_CTRL_TRIANGLE; }   /* X */
    if (right & 0x04) { buttons |= CELL_PAD_CTRL_CROSS; }      /* B */
    if (right & 0x08) { buttons |= CELL_PAD_CTRL_CIRCLE; }     /* A */
    if (right & 0x40) { buttons |= CELL_PAD_CTRL_R1; }         /* R */
    if (right & 0x80) { buttons |= CELL_PAD_CTRL_R2; }         /* ZR */
    if (shared & 0x01) { buttons |= CELL_PAD_CTRL_SELECT; }    /* - */
    if (shared & 0x02) { buttons |= CELL_PAD_CTRL_START; }     /* + */
    if (shared & 0x04) { buttons |= CELL_PAD_CTRL_R3; }
    if (shared & 0x08) { buttons |= CELL_PAD_CTRL_L3; }
    if (left & 0x01) { buttons |= CELL_PAD_CTRL_DOWN; }
    if (left & 0x02) { buttons |= CELL_PAD_CTRL_UP; }
    if (left & 0x04) { buttons |= CELL_PAD_CTRL_RIGHT; }
    if (left & 0x08) { buttons |= CELL_PAD_CTRL_LEFT; }
    if (left & 0x40) { buttons |= CELL_PAD_CTRL_L1; }          /* L */
    if (left & 0x80) { buttons |= CELL_PAD_CTRL_L2; }          /* ZL */
    state->buttons = buttons;
    state->psButton = (shared & 0x10) != 0;                    /* Home */
    pad_stick_pair(pad_switch_stick(pad, 0, 0, pad_switch_unpack12(report + 6, 0)),
                   pad_switch_stick(pad, 0, 1, pad_switch_unpack12(report + 6, 1)),
                   PAD_HID_STICK_DEADZONE, &state->stickLeftX, &state->stickLeftY);
    pad_stick_pair(pad_switch_stick(pad, 1, 0, pad_switch_unpack12(report + 9, 0)),
                   pad_switch_stick(pad, 1, 1, pad_switch_unpack12(report + 9, 1)),
                   PAD_HID_STICK_DEADZONE, &state->stickRightX, &state->stickRightY);
    pad_hid_digital_pressure(state, (buttons & CELL_PAD_CTRL_L2) ? 255 : 0, (buttons & CELL_PAD_CTRL_R2) ? 255 : 0);
    /* Its X points away from the player, Y left, Z up out of the face. */
    const u8* sample = report + 13 + 2 * 12;
    const float accelX = pad_hid_s16(sample + 0) / 4096.0f;
    const float accelY = pad_hid_s16(sample + 2) / 4096.0f;
    const float accelZ = pad_hid_s16(sample + 4) / 4096.0f;
    const float gyroZ = pad_hid_s16(sample + 10) / 14.2842f;
    pad_hid_set_motion(state, -accelY, accelZ, -accelX, gyroZ);
}

/* A report into state; 0 for reports that carry no controller state. */
static int pad_hid_parse(const PadHidDevice* pad, const u8* report, int length, PadHidState* state)
{
    memset(state, 0, sizeof(*state));
    state->connected = 1;
    state->sensor[0] = 512;
    state->sensor[1] = 399;
    state->sensor[2] = 512;
    state->sensor[3] = 512;
    switch (pad->kind)
    {
    case PAD_HID_DUALSHOCK4:
        if (report[0] == 0x01 && length >= 25)
        {
            /* Over Bluetooth, 0x01 is the short report: no sensors. */
            pad_parse_dualshock4(report + 1, !pad->bluetooth, state);
            return 1;
        }
        if (report[0] >= 0x11 && report[0] <= 0x19 && length >= 27)
        {
            pad_parse_dualshock4(report + 3, 1, state);
            return 1;
        }
        return 0;
    case PAD_HID_DUALSENSE:
        if (report[0] == 0x01 && !pad->bluetooth && length >= 28)
        {
            pad_parse_dualsense(report + 1, state);
            return 1;
        }
        if (report[0] == 0x01 && length >= 10)
        {
            pad_parse_dualsense_simple(report + 1, state);
            return 1;
        }
        if (report[0] == 0x31 && length >= 29)
        {
            pad_parse_dualsense(report + 2, state);
            return 1;
        }
        return 0;
    case PAD_HID_SWITCH_PRO:
        if ((report[0] == 0x30 || report[0] == 0x31) && length >= 49)
        {
            pad_parse_switch_pro(pad, report, state);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static void pad_hidpad_store(const PadHidState* next, PadHidKind kind)
{
    AcquireSRWLockExclusive(&s_hidpad_lock);
    const unsigned long long reports = s_hidpad_state.reports + 1;
    s_hidpad_state = *next;
    s_hidpad_state.reports = reports;
    ReleaseSRWLockExclusive(&s_hidpad_lock);
    InterlockedExchange(&s_hidpad_kind, (LONG)kind);
}

static void pad_hidpad_mark_disconnected(void)
{
    AcquireSRWLockExclusive(&s_hidpad_lock);
    s_hidpad_state.connected = 0;
    ReleaseSRWLockExclusive(&s_hidpad_lock);
    InterlockedExchange(&s_hidpad_kind, (LONG)PAD_HID_NONE);
}

static DWORD WINAPI pad_hidpad_thread(LPVOID unused)
{
    (void)unused;
    static u8 report[PAD_HID_MAX_REPORT];
    PadHidDevice pad;
    memset(&pad, 0, sizeof(pad));
    pad.device = INVALID_HANDLE_VALUE;
    unsigned long long nextScanMs = 0;
    int haveMotionLogged = 0;
    while (!InterlockedCompareExchange(&s_ds3_stop, 0, 0))
    {
        if (pad.device == INVALID_HANDLE_VALUE)
        {
            const unsigned long long nowMs = GetTickCount64();
            if (nowMs >= nextScanMs)
            {
                nextScanMs = nowMs + 2000ull;
                if (pad_hid_open(&pad))
                {
                    printf("[cellPad] %s connected over %s\n", pad_hid_kind_name(pad.kind),
                           pad.bluetooth ? "Bluetooth" : "USB");
                    fflush(stdout);
                    pad_hid_start(&pad);
                    haveMotionLogged = 0;
                }
            }
            if (pad.device == INVALID_HANDLE_VALUE)
            {
                Sleep(100);
                continue;
            }
        }
        const int bytesRead = pad_hid_read(&pad, report, 250);
        if (bytesRead < 0)
        {
            printf("[cellPad] %s disconnected\n", pad_hid_kind_name(pad.kind));
            fflush(stdout);
            pad_hid_close(&pad);
            pad_hidpad_mark_disconnected();
            continue;
        }
        if (bytesRead == 0)
        {
            continue;
        }
        PadHidState next;
        if (pad_hid_parse(&pad, report, bytesRead, &next))
        {
            if (!haveMotionLogged && next.hasMotion)
            {
                printf("[cellPad] %s: motion sensors live\n", pad_hid_kind_name(pad.kind));
                fflush(stdout);
                haveMotionLogged = 1;
            }
            pad_hidpad_store(&next, pad.kind);
        }
    }
    pad_hid_close(&pad);
    pad_hidpad_mark_disconnected();
    return 0;
}

static void pad_hidpad_start(void)
{
    static int disabled = -1;
    if (disabled < 0)
    {
        disabled = getenv("PAD_NO_HID_PADS") ? 1 : 0;
    }
    if (disabled || s_hidpad_thread)
    {
        return;
    }
    s_hidpad_thread = CreateThread(NULL, 0, pad_hidpad_thread, NULL, 0, NULL);
}

/* The controller read over HID that port 0 follows: a DS3 if one is
 * connected, else a DualShock 4 / DualSense / Switch Pro. Its name in
 * nameOut (may be NULL). Returns 0 when neither is connected. */
static int pad_motion_snapshot(PadHidState* out, const char** nameOut)
{
    if (pad_ds3_snapshot(out))
    {
        if (nameOut)
        {
            *nameOut = "DualShock 3";
        }
        return 1;
    }
    AcquireSRWLockShared(&s_hidpad_lock);
    *out = s_hidpad_state;
    ReleaseSRWLockShared(&s_hidpad_lock);
    if (nameOut)
    {
        *nameOut = out->connected ? pad_hid_kind_name((PadHidKind)InterlockedCompareExchange(&s_hidpad_kind, 0, 0))
                                  : NULL;
    }
    return out->connected;
}

/* Port 0 from the DS3 or another HID controller, when one is connected. */
static void pad_apply_hid(void)
{
    PadHidState hid;
    if (!pad_motion_snapshot(&hid, NULL))
    {
        return;
    }
    PadHostState* hostState = &s_host_state[0];
    hostState->connected = 1;
    hostState->buttons = hid.buttons;
    hostState->analog_lx = hid.stickLeftX;
    hostState->analog_ly = hid.stickLeftY;
    hostState->analog_rx = hid.stickRightX;
    hostState->analog_ry = hid.stickRightY;
    hostState->press_up = hid.pressure[0];
    hostState->press_right = hid.pressure[1];
    hostState->press_down = hid.pressure[2];
    hostState->press_left = hid.pressure[3];
    hostState->trigger_l2 = hid.pressure[4];
    hostState->trigger_r2 = hid.pressure[5];
    hostState->press_l1 = hid.pressure[6];
    hostState->press_r1 = hid.pressure[7];
    hostState->press_triangle = hid.pressure[8];
    hostState->press_circle = hid.pressure[9];
    hostState->press_cross = hid.pressure[10];
    hostState->press_square = hid.pressure[11];
}

/* For a port's own UI (engine menus): the HID controller's buttons and left
 * stick (a DS3, DualShock 4, DualSense or Switch Pro), unaffected by
 * g_cellpad_block_guest_input. Returns 0 with none. The name is kept from
 * when it was the DS3 only. */
int cellPad_host_ds3_state(u16* buttons, u8* stickLeftX, u8* stickLeftY, int* psButton)
{
    PadHidState hid;
    if (!pad_motion_snapshot(&hid, NULL))
    {
        return 0;
    }
    if (buttons)
    {
        *buttons = hid.buttons;
    }
    if (stickLeftX)
    {
        *stickLeftX = hid.stickLeftX;
    }
    if (stickLeftY)
    {
        *stickLeftY = hid.stickLeftY;
    }
    if (psButton)
    {
        *psButton = hid.psButton;
    }
    return 1;
}

/* The connected HID controller ("DualShock 3", "DualShock 4", "DualSense",
 * "Switch Pro Controller"), or NULL. */
const char* cellPad_host_pad_name(void)
{
    PadHidState hid;
    const char* name = NULL;
    pad_motion_snapshot(&hid, &name);
    return name;
}
#else
int cellPad_host_ds3_state(u16* buttons, u8* stickLeftX, u8* stickLeftY, int* psButton)
{
    (void)buttons; (void)stickLeftX; (void)stickLeftY; (void)psButton;
    return 0;
}
const char* cellPad_host_pad_name(void)
{
    return NULL;
}
static volatile long s_motion_mode = 0;
#endif

/* Where the guest's sensor words come from:
 *   0  automatic: the controller's motion sensors when it has them (DS3,
 *      DualShock 4, DualSense, Switch Pro), otherwise "automatic capture"
 *      (below), so a pad without motion sensors can still play
 *   1  motion only: the sensors, or a controller at rest without them
 *   2  automatic capture: every axis reads full scale, which a motion
 *      gesture accepts at once (what the port did before motion support:
 *      sensor mode was never recorded, so the words read 0)
 * While a port's menu blocks guest input the sensors read at rest.
 * Returns the previous mode. */
int cellPad_set_motion_mode(int mode)
{
    if (mode < 0 || mode > 2)
    {
        mode = 0;
    }
#ifdef _WIN32
    return (int)InterlockedExchange(&s_motion_mode, (LONG)mode);
#else
    const int previous = (int)s_motion_mode;
    s_motion_mode = mode;
    return previous;
#endif
}

/* 1 when the guest is getting live motion data right now. */
int cellPad_motion_live(void)
{
#ifdef _WIN32
    PadHidState hid;
    return pad_motion_snapshot(&hid, NULL) && hid.hasMotion &&
           InterlockedCompareExchange(&s_motion_mode, 0, 0) != 2;
#else
    return 0;
#endif
}

static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        pad_xinput_stick_pair(gp->sThumbLX, gp->sThumbLY, PAD_STICK_DEADZONE,
                              &s_host_state[i].analog_lx, &s_host_state[i].analog_ly);
        pad_xinput_stick_pair(gp->sThumbRX, gp->sThumbRY, PAD_STICK_DEADZONE,
                              &s_host_state[i].analog_rx, &s_host_state[i].analog_ry);

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init; the DS3 reader and the other HID
     * controllers' reader run on their own threads. */
    pad_ds3_start();
    pad_hidpad_start();
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* Set by a port's in-game menu while it is open: the game then sees every
 * port connected but idle, so navigating the menu does not also move the
 * character. */
volatile int g_cellpad_block_guest_input = 0;

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
    pad_apply_hid();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
#ifdef _WIN32
    if (!s_host_state[0].connected) pad_poll_keyboard();
#endif
    if (g_cellpad_block_guest_input)
    {
        for (int port = 0; port < PAD_MAX_HOST_PORTS; port++)
        {
            const int connected = s_host_state[port].connected;
            memset(&s_host_state[port], 0, sizeof(s_host_state[port]));
            s_host_state[port].connected = connected;
            s_host_state[port].analog_lx = 128;
            s_host_state[port].analog_ly = 128;
            s_host_state[port].analog_rx = 128;
            s_host_state[port].analog_ry = 128;
        }
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[32]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 32) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)\n", n_ev);
          }
      }
      if (s_sc && port_no == 0) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[32], held[32];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[32];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[64]; buf[0] = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, 0, 0);
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && (double)(GetTickCount64() - s_t0) / 1000.0 >= s_from && (double)(GetTickCount64() - s_t0) / 1000.0 < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        u16 sensorWords[4] = { 512, 399 /* gravity */, 512, 512 };
#if PAD_BACKEND_XINPUT
        const LONG motionMode = InterlockedCompareExchange(&s_motion_mode, 0, 0);
        PadHidState hid;
        const int haveMotion = port_no == 0 && pad_motion_snapshot(&hid, NULL) && hid.hasMotion;
        if (g_cellpad_block_guest_input)
        {
            /* menu open: at rest */
        }
        else if (motionMode == 2 || (motionMode == 0 && !haveMotion))
        {
            sensorWords[0] = sensorWords[1] = sensorWords[2] = sensorWords[3] = 0;
        }
        else if (haveMotion)
        {
            memcpy(sensorWords, hid.sensor, sizeof(sensorWords));
        }
#endif
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = sensorWords[0];
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = sensorWords[1];
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = sensorWords[2];
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = sensorWords[3];
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (_n++ < 2)
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

/* cellPadInfoSensorMode / cellPadSetSensorMode: the SIXAXIS counterpart of
 * press mode. Without Set recording CELL_PAD_SETTING_SENSOR_ON, GetData left
 * the four sensor words 0 -- full scale negative on every axis -- which a
 * motion-gesture title reads as the controller being yanked in every
 * direction at once. */
s32 cellPadInfoSensorMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetSensorMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_SENSOR_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_SENSOR_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257),
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257),
            100 /* duration ms */
        );
    }
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
