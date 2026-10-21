/*
 * ps3recomp - sceNpUtil HLE implementation
 *
 * Reports production environment, validates NP IDs, and provides
 * no-restriction parental controls. Bandwidth test reports a fast
 * fake connection.
 */

#include "sceNpUtil.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static s32 s_np_env = SCE_NP_ENV_PRODUCTION;
static int s_bw_test_running = 0;

/* ---------------------------------------------------------------------------
 * Bandwidth test
 * -----------------------------------------------------------------------*/

s32 sceNpUtilBandwidthTestInitStart(u32 flags)
{
    (void)flags;
    printf("[sceNpUtil] BandwidthTestInitStart()\n");
    s_bw_test_running = 1;
    return CELL_OK;
}

s32 sceNpUtilBandwidthTestGetStatus(SceNpBandwidthTestResult* result)
{
    if (!result)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    /* Report a generous fake bandwidth (100 Mbps) */
    /* two doubles then an s32; vm_write64 takes the bit pattern. */
    u32 res_ea = GUEST_EA(result);
    double bps = 100000000.0;
    u64 bits;
    memcpy(&bits, &bps, sizeof(bits));
    vm_write64(res_ea + (u32)offsetof(SceNpBandwidthTestResult, uploadBps),   bits);
    vm_write64(res_ea + (u32)offsetof(SceNpBandwidthTestResult, downloadBps), bits);
    vm_write32(res_ea + (u32)offsetof(SceNpBandwidthTestResult, result), 0); /* success / done */

    s_bw_test_running = 0;
    return CELL_OK;
}

s32 sceNpUtilBandwidthTestShutdown(void)
{
    printf("[sceNpUtil] BandwidthTestShutdown()\n");
    s_bw_test_running = 0;
    return CELL_OK;
}

s32 sceNpUtilBandwidthTestAbort(void)
{
    printf("[sceNpUtil] BandwidthTestAbort()\n");
    s_bw_test_running = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP environment
 * -----------------------------------------------------------------------*/

s32 sceNpUtilGetNpEnv(s32* env)
{
    if (!env)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)env, (u32)s_np_env);
    return CELL_OK;
}

s32 sceNpUtilSetNpEnv(s32 env)
{
    printf("[sceNpUtil] SetNpEnv(%d)\n", env);
    s_np_env = env;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP ID validation
 * -----------------------------------------------------------------------*/

s32 sceNpUtilCheckOnlineId(const char* onlineId)
{
    if (!onlineId)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    /* PS3 online IDs: 3-16 chars, alphanumeric + dash + underscore,
       must start with a letter */
    onlineId = GUEST_PTR(onlineId, const char*);
    size_t len = strlen(onlineId);
    if (len < 3 || len > 16)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ONLINE_ID;

    if (!isalpha((unsigned char)onlineId[0]))
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ONLINE_ID;

    for (size_t i = 0; i < len; i++) {
        char c = onlineId[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_')
            return (s32)SCE_NP_UTIL_ERROR_INVALID_ONLINE_ID;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Parental control
 * -----------------------------------------------------------------------*/

s32 sceNpUtilGetParentalControlInfo(s32* age, s32* chatRestriction)
{
    if (age)
        vm_write32((u32)(uintptr_t)age, (u32)18); /* no restriction */
    if (chatRestriction)
        vm_write32((u32)(uintptr_t)chatRestriction, (u32)0); /* chat allowed */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP ID comparison
 * -----------------------------------------------------------------------*/

s32 sceNpUtilCmpNpId(const void* npId1, const void* npId2)
{
    if (!npId1 || !npId2)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    return memcmp(GUEST_PTR(npId1, const void*),
                  GUEST_PTR(npId2, const void*), 36) == 0 ? 0 : 1;
}

s32 sceNpUtilCmpNpIdInOrder(const void* npId1, const void* npId2, s32* order)
{
    if (!npId1 || !npId2 || !order)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)order, (u32)memcmp(npId1, npId2, 36));
    return CELL_OK;
}
