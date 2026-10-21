/*
 * ps3recomp - cellFreeType HLE implementation
 *
 * Minimal wrapper around FreeType2 library.
 * Reports FreeType 2.4.12 (the version shipped with PS3 firmware 4.x).
 */

#include "cellFreeType.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */

/* Internal state */

static int s_initialized = 0;
static CellFreeTypeLibrary s_lib = 0;

/* API */

s32 cellFreeTypeInit(const CellFreeTypeConfig* config, CellFreeTypeLibrary* lib)
{
    printf("[cellFreeType] Init()\n");
    (void)config;

    if (s_initialized)
        return (s32)CELL_FREETYPE_ERROR_ALREADY_INITIALIZED;

    s_initialized = 1;
    s_lib = 1;
    if (lib) vm_write32((u32)(uintptr_t)lib, s_lib);
    return CELL_OK;
}

s32 cellFreeTypeEnd(CellFreeTypeLibrary lib)
{
    printf("[cellFreeType] End()\n");
    (void)lib;
    s_initialized = 0;
    s_lib = 0;
    return CELL_OK;
}

s32 cellFreeTypeGetVersion(CellFreeTypeVersion* version)
{
    if (!version) return (s32)CELL_FREETYPE_ERROR_INVALID_ARGUMENT;

    /* PS3 firmware 4.x ships FreeType 2.4.12 */
    vm_write32((u32)(uintptr_t)version + 0, 2);    /* major */
    vm_write32((u32)(uintptr_t)version + 4, 4);    /* minor */
    vm_write32((u32)(uintptr_t)version + 8, 12);   /* patch */
    return CELL_OK;
}

s32 cellFreeTypeGetLibrary(CellFreeTypeLibrary* lib)
{
    if (!lib) return (s32)CELL_FREETYPE_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FREETYPE_ERROR_NOT_INITIALIZED;
    vm_write32((u32)(uintptr_t)lib, s_lib);
    return CELL_OK;
}
