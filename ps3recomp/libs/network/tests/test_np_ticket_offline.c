/* sceNpManagerRequestTicket and friends with no PSN: offline the request fails
 * with SCE_NP_ERROR_OFFLINE (it used to fake CELL_OK as an unresolved import,
 * and Resistance then waited for a ticket for ever), arguments are checked
 * first, and the ticket getters report that there is no ticket.
 *
 *   cc -std=gnu17 -I include -o /tmp/test_np_ticket_offline \
 *      libs/network/tests/test_np_ticket_offline.c && /tmp/test_np_ticket_offline
 */
#include "../sceNp.c"
#include <assert.h>
#include <stdlib.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static int s_online;
int np_psnr_enabled(void) { return s_online; }
const char* np_psnr_online_id(void) { return "PS3Player"; }
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) { (void)opd; (void)args; return 0; }
void edat_add_klicensee(const uint8_t klic[16]) { (void)klic; }

#define NPID   0x1000u
#define SVC    0x2000u
#define SIZE   0x3000u
#define PARAM  0x3100u

int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    memcpy(vm_base + SVC, "IV0001-NPXS00001_00", 20);

    assert(sceNpManagerRequestTicket((const SceNpId*)(uintptr_t)NPID, (const char*)(uintptr_t)SVC,
                                     NULL, 0, NULL, 0) == (s32)SCE_NP_ERROR_NOT_INITIALIZED);
    assert(sceNpInit(0, NULL) == CELL_OK);

    /* Arguments before state, as on the console. */
    assert(sceNpManagerRequestTicket(NULL, (const char*)(uintptr_t)SVC, NULL, 0, NULL, 0)
           == (s32)SCE_NP_AUTH_EINVALID_ARGUMENT);
    assert(sceNpManagerRequestTicket((const SceNpId*)(uintptr_t)NPID, NULL, NULL, 0, NULL, 0)
           == (s32)SCE_NP_AUTH_EINVALID_ARGUMENT);
    assert(sceNpManagerRequestTicket((const SceNpId*)(uintptr_t)NPID, (const char*)(uintptr_t)SVC,
                                     NULL, SCE_NP_COOKIE_MAX_SIZE + 1, NULL, 0)
           == (s32)SCE_NP_AUTH_EINVALID_ARGUMENT);

    /* Offline: refused, and the status the title can read agrees. */
    assert(sceNpManagerRequestTicket((const SceNpId*)(uintptr_t)NPID, (const char*)(uintptr_t)SVC,
                                     NULL, 0, NULL, 0) == (s32)SCE_NP_ERROR_OFFLINE);
    assert(sceNpManagerRequestTicket2((const SceNpId*)(uintptr_t)NPID, NULL,
                                      (const char*)(uintptr_t)SVC, NULL, 0, NULL, 0)
           == (s32)SCE_NP_ERROR_OFFLINE);
    assert(sceNpManagerGetStatus((s32*)(uintptr_t)SIZE) == CELL_OK);
    assert((s32)vm_read32(SIZE) == SCE_NP_MANAGER_STATUS_OFFLINE);

    /* No ticket: zero bytes, and no parameter can be read from it. */
    vm_write32(SIZE, 1234);
    assert(sceNpManagerGetTicket(NULL, (u32*)(uintptr_t)SIZE) == CELL_OK);
    assert(vm_read32(SIZE) == 0);
    assert(sceNpManagerGetTicket(NULL, NULL) == (s32)SCE_NP_ERROR_INVALID_ARGUMENT);
    assert(sceNpManagerGetTicketParam(0, (void*)(uintptr_t)PARAM) == (s32)SCE_NP_ERROR_INVALID_STATE);
    assert(sceNpManagerGetTicketParam(12, (void*)(uintptr_t)PARAM) == (s32)SCE_NP_ERROR_INVALID_ARGUMENT);
    assert(sceNpManagerGetTicketParam(0, NULL) == (s32)SCE_NP_ERROR_INVALID_ARGUMENT);

    /* With a psnr server the request is accepted as before. */
    s_online = 1;
    assert(sceNpManagerRequestTicket((const SceNpId*)(uintptr_t)NPID, (const char*)(uintptr_t)SVC,
                                     NULL, 0, NULL, 0) == CELL_OK);

    free(vm_base);
    puts("test_np_ticket_offline: all passed");
    return 0;
}
