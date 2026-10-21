/*
 * ps3recomp - cellCamera HLE: VIRTUAL PlayStation Eye.
 *
 * Reports an EyeToy2 and serves synthetic RAW8 Bayer frames (animated test
 * pattern) so camera-driven titles run without hardware -- the SDK
 * ImageProcessing samples (demosaic/wave/fire) are EyeToy image-effect demos
 * whose whole input is the camera; with the old "no device" stub they exit(1)
 * at boot.
 *
 * Contract implemented (from ImageProcessing/demosaic HqShow.cpp + SDK
 * cell/camera.h):
 *   - cellCameraOpenEx(dev, CellCameraInfoEx*): the app fills format/
 *     resolution/framerate/info_ver/container; the LIBRARY fills buffer
 *     (frame memory it owns), bytesize, width, height, dev_num, guid,
 *     read_mode, pbuf[].
 *   - cellCameraRead(dev, u32* frame, u32* bytesread): 3-arg form; frame data
 *     appears in info->buffer.
 *   - IsAttached/IsAvailable/GetType gate the app's attach-wait loop.
 *
 * The frame buffer lives at a fixed guest EA (like the GCM label window):
 * writes go through vm_base so lifted code reads them back byte-exact.
 */

#include "cellCamera.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

extern void vm_write32(u32 addr, u32 value);
extern u32  vm_read32(u32 addr);
extern unsigned char* vm_base;

/* Fixed guest EA for the virtual camera's frame memory (RAW8 VGA = 300KB).
 * 0x032xxxxx sits after the GCM label (0x03000000) / control (0x03002000)
 * windows and below typical title mappings. */
#define CAM_BUFFER_GUEST_EA  0x03200000u
#define CAM_W 640
#define CAM_H 480

/* SDK enum values (cell/camera.h): CellCameraType EYETOY2 = 2;
 * CellCameraFormat RAW8 = 2; CellCameraResolution VGA = 1. (This header's
 * legacy defines predate the SDK check -- use the SDK values.) */
#define SDK_CAMERA_EYETOY2 2

static int s_initialized = 0;
static int s_open        = 0;
static int s_started     = 0;
static u32 s_frame       = 0;

/* PS3_CAMERA=0 reports no camera attached: camera-optional apps (wave) then
 * fall back to their file-based input, matching RPCS3's no-camera behavior.
 * Default stays attached -- demosaic's whole input is the virtual camera. */
static int cam_present(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PS3_CAMERA");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v;
}

/* CellCameraInfoEx field offsets (all 4-byte fields, 32-bit guest ABI):
 * format@0 resolution@4 framerate@8 buffer@12 bytesize@16 width@20 height@24
 * dev_num@28 guid@32 info_ver@36 container@40 read_mode@44 pbuf0@48 pbuf1@52 */

s32 cellCameraInit(void)
{
    printf("[cellCamera] Init (virtual EyeToy2, RAW8 %ux%u @ guest 0x%08X)\n",
           CAM_W, CAM_H, CAM_BUFFER_GUEST_EA);
    s_initialized = 1;
    return CELL_OK;
}

s32 cellCameraEnd(void)
{
    s_initialized = 0;
    return CELL_OK;
}

s32 cellCameraOpenEx(s32 devNum, void* info)
{
    (void)devNum;
    u32 ea = (u32)(uintptr_t)info;
    if (!ea) return (s32)CELL_CAMERA_ERROR_INVALID_ARGUMENT;
    if (!cam_present()) return (s32)CELL_CAMERA_ERROR_DEVICE_NOT_FOUND;
    /* The app requested format/resolution; always serve RAW8 VGA frames. */
    vm_write32(ea + 12, CAM_BUFFER_GUEST_EA);       /* buffer   */
    vm_write32(ea + 16, CAM_W * CAM_H);             /* bytesize */
    vm_write32(ea + 20, CAM_W);                     /* width    */
    vm_write32(ea + 24, CAM_H);                     /* height   */
    vm_write32(ea + 28, 0);                         /* dev_num  */
    vm_write32(ea + 32, 0x00EE7031u);               /* guid     */
    vm_write32(ea + 44, 0);                         /* read_mode: direct buffer */
    vm_write32(ea + 48, CAM_BUFFER_GUEST_EA);       /* pbuf[0]  */
    vm_write32(ea + 52, CAM_BUFFER_GUEST_EA);       /* pbuf[1]  */
    s_open = 1;
    printf("[cellCamera] OpenEx -> virtual frames at 0x%08X\n", CAM_BUFFER_GUEST_EA);
    return CELL_OK;
}

s32 cellCameraOpen(s32 devNum, CellCameraInfo* info)
{
    /* Same library-fills-buffer contract; the Ex tail fields land in memory
     * past the caller's struct only if it really passed an Info (not InfoEx),
     * which no known caller does -- SDK apps use OpenEx. */
    return cellCameraOpenEx(devNum, (void*)info);
}

s32 cellCameraGetBufferSize(s32 devNum, void* info)
{
    (void)devNum; (void)info;
    return CAM_W * CAM_H;   /* RAW8 VGA */
}

s32 cellCameraClose(s32 devNum)
{
    (void)devNum;
    s_open = 0; s_started = 0;
    return CELL_OK;
}

s32 cellCameraReset(s32 devNum) { (void)devNum; return CELL_OK; }

s32 cellCameraStart(s32 devNum)
{
    (void)devNum;
    s_started = 1;
    return CELL_OK;
}

s32 cellCameraStop(s32 devNum)
{
    (void)devNum;
    s_started = 0;
    return CELL_OK;
}

/* Animated RAW8 Bayer test pattern (GRBG):
 *   even rows: G R G R ...
 *   odd  rows: B G B G ...
 * Scene: horizontal/vertical colour gradient + a bright bouncing square, so
 * the demosaiced output is an obviously colourful moving image. */
static void cam_fill_frame(void)
{
    if (!vm_base) return;
    unsigned char* dst = vm_base + CAM_BUFFER_GUEST_EA;
    u32 t = s_frame;
    int bx = (int)((t * 5u) % (CAM_W - 96));
    int by = (int)((t * 3u) % (CAM_H - 96));
    for (int y = 0; y < CAM_H; y++) {
        for (int x = 0; x < CAM_W; x++) {
            unsigned r = (unsigned)(x * 255 / CAM_W);
            unsigned g = (unsigned)(y * 255 / CAM_H);
            unsigned b = 255u - r;
            if (x >= bx && x < bx + 96 && y >= by && y < by + 96) r = g = b = 250;
            unsigned v;
            if ((y & 1) == 0) v = ((x & 1) == 0) ? g : r;   /* G R */
            else              v = ((x & 1) == 0) ? b : g;   /* B G */
            dst[y * CAM_W + x] = (unsigned char)v;
        }
    }
}

/* 3-arg read (libcamera's real signature):
 * cellCameraRead(dev, u32* frame_num, u32* bytes_read). */
s32 cellCameraRead(s32 devNum, u32* frame_num, u32* bytes_read)
{
    (void)devNum;
    if (!s_started) return (s32)CELL_CAMERA_ERROR_NOT_STARTED;
    s_frame++;
    { static int _once = 0;
      if (!_once++) printf("[cellCamera] frame reads begin\n"); }
    cam_fill_frame();
    u32 fea = (u32)(uintptr_t)frame_num;
    u32 bea = (u32)(uintptr_t)bytes_read;
    if (fea) vm_write32(fea, s_frame);
    if (bea) vm_write32(bea, CAM_W * CAM_H);
    return CELL_OK;
}

s32 cellCameraReadEx(s32 devNum, CellCameraReadInfo* info)
{
    (void)devNum;
    if (!s_started) return (s32)CELL_CAMERA_ERROR_NOT_STARTED;
    s_frame++;
    cam_fill_frame();
    u32 ea = (u32)(uintptr_t)info;
    if (ea) {
        vm_write32(ea + 0, s_frame);          /* frame     */
        vm_write32(ea + 4, CAM_W * CAM_H);    /* bytesread */
    }
    return CELL_OK;
}

s32 cellCameraIsAvailable(s32 devNum) { (void)devNum; return cam_present(); }
s32 cellCameraIsAttached(s32 devNum)  { (void)devNum; return cam_present(); }
s32 cellCameraIsOpen(s32 devNum)      { (void)devNum; return s_open; }
s32 cellCameraIsStarted(s32 devNum)   { (void)devNum; return s_started; }

s32 cellCameraGetType(s32 devNum, s32* type)
{
    (void)devNum;
    u32 ea = (u32)(uintptr_t)type;
    if (!ea) return (s32)CELL_CAMERA_ERROR_INVALID_ARGUMENT;
    /* TYPE_UNKNOWN (0) routes camera-optional apps (wave) to their
     * file-based input when PS3_CAMERA=0. */
    vm_write32(ea, cam_present() ? SDK_CAMERA_EYETOY2 : 0);
    return CELL_OK;
}

s32 cellCameraSetAttribute(s32 devNum, s32 attrib, u32 arg1, u32 arg2)
{
    (void)devNum; (void)attrib; (void)arg1; (void)arg2;
    return CELL_OK;
}

s32 cellCameraGetAttribute(s32 devNum, s32 attrib, u32* arg1, u32* arg2)
{
    (void)devNum; (void)attrib;
    u32 a1 = (u32)(uintptr_t)arg1, a2 = (u32)(uintptr_t)arg2;
    if (a1) vm_write32(a1, 128);   /* e.g. GAIN mid-scale */
    if (a2) vm_write32(a2, 0);
    return CELL_OK;
}

s32 cellCameraSetNotifyCallback(s32 devNum, CellCameraCallback cb, void* arg)
{
    (void)devNum; (void)cb; (void)arg;
    return CELL_OK;
}

s32 cellCameraRemoveNotifyCallback(s32 devNum)
{
    (void)devNum;
    return CELL_OK;
}

/* The lv2-event-queue notification forms: the title binds a queue it owns to
 * camera events (cellCameraSetNotifyEventQueue2) and later unbinds it. The
 * virtual camera raises no events, so there is nothing to bind; accept, so
 * the Move/camera init that asks (inFAMOUS 2's "Motion Controller Update")
 * carries on instead of hitting the unresolved stub. */
s32 cellCameraSetNotifyEventQueue2(u64 key, u64 source, u64 flag)
{
    (void)key; (void)source; (void)flag;
    if (!s_initialized) return (s32)CELL_CAMERA_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellCameraRemoveNotifyEventQueue2(u64 key)
{
    (void)key;
    if (!s_initialized) return (s32)CELL_CAMERA_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}
