/* The quit gate (include/ps3emu/quit_gate.h, in cellSaveData.c): a quit
 * waits for the saves and writes inside it, and once it has begun a new one
 * never starts -- its thread parks -- while one already inside may open
 * another (a save that opens a file must not park the thread the quit is
 * waiting for).
 *
 *   clang -std=gnu17 -I include libs/system/tests/test_quit_gate.c \
 *         -Wl,-dead_strip -o /tmp/test_quit_gate && /tmp/test_quit_gate */
#include "../cellSaveData.c"
#include <assert.h>
#include <pthread.h>
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
ps3_guest_caller_fn g_ps3_guest_caller;

static volatile int s_saving, s_done, s_late_started;

/* A save that takes 300 ms, with a file opened inside it at the end. */
static void* saver(void* arg)
{
    (void)arg;
    ps3_quit_gate_enter();
    s_saving = 1;
    usleep(300 * 1000);
    ps3_quit_gate_enter();            /* nested: never parks */
    ps3_quit_gate_leave();
    s_done = 1;
    ps3_quit_gate_leave();
    return NULL;
}

/* A save that would start after the quit began. */
static void* late(void* arg)
{
    (void)arg;
    ps3_quit_gate_enter();            /* parks for good */
    s_late_started = 1;
    return NULL;
}

int main(void)
{
    assert(ps3_quit_gate_busy() == 0 && !ps3_quit_gate_closed());
    ps3_quit_gate_enter();
    assert(ps3_quit_gate_busy() == 1);
    ps3_quit_gate_leave();
    assert(ps3_quit_gate_busy() == 0);

    pthread_t a, b;
    assert(pthread_create(&a, NULL, saver, NULL) == 0);
    while (!s_saving) usleep(1000);
    /* The quit: a short wait reports the save still inside ... */
    assert(ps3_quit_gate_wait(50) == 1 && ps3_quit_gate_closed() && !s_done);
    /* ... a longer one sees it finish (its nested open did not park). */
    assert(ps3_quit_gate_wait(5000) == 0);
    assert(s_done && ps3_quit_gate_busy() == 0);
    pthread_join(a, NULL);
    /* A save after the quit began never starts, and is not counted. */
    assert(pthread_create(&b, NULL, late, NULL) == 0);
    usleep(200 * 1000);
    assert(!s_late_started && ps3_quit_gate_busy() == 0);
    puts("test_quit_gate: ok");
    return 0;                        /* the parked thread ends with the process */
}
