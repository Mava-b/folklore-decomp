/*
 * ps3recomp - cellLicenseArea HLE implementation
 *
 * Stub. Reports Americas region, all areas valid.
 */

#include "cellLicenseArea.h"
#include <stdio.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */

/* API */

s32 cellLicenseAreaCheck(s32* areaCode)
{
    printf("[cellLicenseArea] Check()\n");
    if (!areaCode) return (s32)CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)areaCode, (u32)CELL_LICENSE_AREA_A); /* Americas */
    return CELL_OK;
}

s32 cellLicenseAreaGetAreaCode(s32* areaCode)
{
    if (!areaCode) return (s32)CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)areaCode, (u32)CELL_LICENSE_AREA_A);
    return CELL_OK;
}

s32 cellLicenseAreaIsValid(s32 areaCode, s32* isValid)
{
    (void)areaCode;
    if (!isValid) return (s32)CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)isValid, (u32)1); /* always valid */
    return CELL_OK;
}
