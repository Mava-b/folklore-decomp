/*
 * ps3recomp - cellVdecDivx HLE implementation
 *
 * DivX video decoder.
 * Stub — decode produces a black frame (zero-filled YUV).
 */

#include "cellVdecDivx.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */

/* Internal state */

#define DIVX_MAX_HANDLES 4

typedef struct {
    int  in_use;
    u32  maxWidth;
    u32  maxHeight;
} DivxSlot;

static DivxSlot s_slots[DIVX_MAX_HANDLES];

static int divx_alloc(void)
{
    for (int i = 0; i < DIVX_MAX_HANDLES; i++) {
        if (!s_slots[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellVdecDivxOpen(const CellVdecDivxConfig* config, CellVdecDivxHandle* handle)
{
    printf("[cellVdecDivx] Open()\n");
    if (!handle) return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;

    int slot = divx_alloc();
    if (slot < 0) return (s32)CELL_VDEC_DIVX_ERROR_OUT_OF_MEMORY;

    s_slots[slot].in_use = 1;
    CellVdecDivxConfig host_cfg;
    guest_struct_load(&host_cfg, GUEST_EA(config), (u32)sizeof(host_cfg));
    s_slots[slot].maxWidth  = config ? host_cfg.maxWidth  : 1920;
    s_slots[slot].maxHeight = config ? host_cfg.maxHeight : 1080;

    vm_write32((u32)(uintptr_t)handle, (CellVdecDivxHandle)slot);
    return CELL_OK;
}

s32 cellVdecDivxClose(CellVdecDivxHandle handle)
{
    printf("[cellVdecDivx] Close(%u)\n", handle);
    if (handle >= DIVX_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;

    s_slots[handle].in_use = 0;
    return CELL_OK;
}

s32 cellVdecDivxDecode(CellVdecDivxHandle handle, const void* au, u32 auSize,
                        void* picOut, u32 picBufSize, CellVdecDivxFrameInfo* info)
{
    (void)au; (void)auSize;

    if (handle >= DIVX_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;

    /* Output black frame if buffer provided */
    if (picOut && picBufSize > 0)
        memset(GUEST_PTR(picOut, void*), 0, picBufSize);

    if (info) {
        CellVdecDivxFrameInfo host_info;
        memset(&host_info, 0, sizeof(host_info));
        host_info.width  = s_slots[handle].maxWidth;
        host_info.height = s_slots[handle].maxHeight;
        guest_struct_store(GUEST_EA(info), &host_info, (u32)sizeof(host_info));
    }
    return CELL_OK;
}

s32 cellVdecDivxReset(CellVdecDivxHandle handle)
{
    if (handle >= DIVX_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}
