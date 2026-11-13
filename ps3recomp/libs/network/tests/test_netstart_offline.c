/* The NetStart dialog's verdict follows the network state: offline (the
 * default) UnloadAsync reports CELL_NET_CTL_ERROR_DIALOG_CANCELED, big-endian
 * in the guest's result struct, so a title takes its "could not connect" path
 * instead of going on to PSN requests that never complete; with
 * PS3_NET_ONLINE it reports 0, connected.
 *
 *   cc -std=gnu17 -I include -I libs/system -o /tmp/test_netstart_offline \
 *      libs/network/tests/test_netstart_offline.c && /tmp/test_netstart_offline \
 *      && PS3_NET_ONLINE=1 /tmp/test_netstart_offline
 */
#include "../cellNetCtl.c"
#include <assert.h>

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
const char* np_psnr_identity(void) { return NULL; }

static unsigned s_events;
void cellSysutilQueueEvent(int slot, uint32_t status, uint32_t param)
{
    (void)slot; (void)param;
    if (status == CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED) s_events++;
}

#define RESULT 0x100u

static s32 run_dialog(void)
{
    vm_write32(RESULT, 8);              /* size */
    vm_write32(RESULT + 4, 0x12345678); /* result: overwritten */
    assert(cellNetCtlNetStartDialogLoadAsync(NULL) == CELL_OK);
    assert(cellNetCtlNetStartDialogUnloadAsync(
               (CellNetCtlNetStartDialogResult*)(uintptr_t)RESULT) == CELL_OK);
    assert(vm_read32(RESULT) == 8);
    return (s32)vm_read32(RESULT + 4);
}

int main(void)
{
    vm_base = calloc(1, 0x1000); assert(vm_base);

    if (getenv("PS3_NET_ONLINE")) {
        assert(run_dialog() == 0);                          /* connected */
    } else {
        assert(run_dialog() == (s32)CELL_NET_CTL_ERROR_DIALOG_CANCELED);
        assert(vm_base[RESULT + 4] == 0x80 && vm_base[RESULT + 7] == 0x90);   /* big-endian */
    }
    assert(s_events == CELL_SYSUTIL_MAX_CALLBACKS);   /* UNLOADED reaches every slot */

    free(vm_base);
    puts("test_netstart_offline: all passed");
    return 0;
}
