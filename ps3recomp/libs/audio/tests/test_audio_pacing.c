/* The POSIX mixing thread runs the guest's audio clock in real time: one
 * 256-sample block every 5.33 ms, 187.5 a second, and one notify event per
 * block. Runs the real thread for a second with no host device (only the
 * clock paces it) and counts the events. The old loop slept a fixed 2 ms a
 * block there and mixed ~2.6x real time.
 *
 * Then audio_pace_wait against simulated devices that take their buffer in
 * chunks (256 and 1024 frames) and run 0.5% fast or slow: the queue must stay
 * bounded, the long-run rate must follow the device, and no two blocks may be
 * mixed closer than 0.75 periods -- the loop before this one read two blocks
 * back to back whenever the device took its buffer, before the guest's mixer
 * had been woken to fill the second, and half of every movie block was empty.
 * macOS: clang -std=gnu17 -I include -I /opt/homebrew/include
 *   libs/audio/tests/test_audio_pacing.c -L /opt/homebrew/lib -lSDL2
 *   -Wl,-dead_strip -o /tmp/test_audio_pacing && /tmp/test_audio_pacing
 */
#include "../cellAudio.c"
#include <assert.h>

#ifndef _WIN32
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

static volatile unsigned s_events;
uint32_t sys_event_find_queue_by_key(uint64_t key) { return key == 0x1234 ? 7u : 0u; }
int sys_event_queue_push_by_id(uint32_t id, uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3)
{
    (void)source; (void)d1; (void)d2; (void)d3;
    if (id == 7u) __atomic_add_fetch(&s_events, 1u, __ATOMIC_RELAXED);
    return 0;
}

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Simulate `secs` of a device that drains `chunk` frames every
 * chunk/48000*(1/speed) s. Returns 0 on success. */
static int simulate(unsigned chunk, double speed, double secs)
{
    AudioPace p = { 1000000000ull, 0, 0 };
    const unsigned long long period = 16000000ull / 3ull;
    unsigned long long now = 1000000000ull, next_pull = now, last_mix = 0;
    long queued = 0, max_q = 0, min_q_late = 1 << 30;
    unsigned long long min_gap = ~0ull, mixed = 0, underruns = 0;
    const unsigned long long end = now + (unsigned long long)(secs * 1e9);
    const double pull_ns = chunk / 48000.0 / speed * 1e9;
    double pull_acc = (double)next_pull;
    while (now < end) {
        while (now >= next_pull) {
            if (queued < (long)chunk) underruns++;
            queued = queued > (long)chunk ? queued - (long)chunk : 0;
            pull_acc += pull_ns;
            next_pull = (unsigned long long)pull_acc;
        }
        unsigned long long w = audio_pace_wait(&p, now, queued);
        if (w == 0) {
            if (last_mix && now - last_mix < min_gap) min_gap = now - last_mix;
            last_mix = now;
            queued += CELL_AUDIO_BLOCK_SAMPLES;
            mixed++;
            if (queued > max_q) max_q = queued;
            if (now > 1000000000ull + 2000000000ull && queued < min_q_late) min_q_late = queued;
            continue;
        }
        unsigned long long step = w < 50000ull ? w : 50000ull;   /* 50 us polling */
        now += step;
    }
    const double rate = mixed / secs, want = 187.5 * speed;
    printf("  chunk %4u speed %.3f: %.2f blocks/s (device %.2f), queue max %ld min %ld frames, "
           "min gap %.2f periods, %llu underruns\n", chunk, speed, rate, want, max_q,
           min_q_late, (double)min_gap / period, underruns);
    if (rate < want * 0.995 || rate > want * 1.005) return 1;
    if (max_q > (long)((AUDIO_PACE_HIGH + 2) * CELL_AUDIO_BLOCK_SAMPLES) + (long)chunk) return 1;
    if ((double)min_gap < 0.74 * period) return 1;
    if (underruns > 2) return 1;   /* the start, before the queue has filled */
    return 0;
}

int main(void)
{
    {
        int bad = 0;
        bad |= simulate(256, 1.0, 30.0);
        bad |= simulate(1024, 1.0, 30.0);
        bad |= simulate(256, 1.005, 60.0);
        bad |= simulate(1024, 0.995, 60.0);
        bad |= simulate(512, 1.005, 60.0);
        if (bad) { printf("test_audio_pacing: FAIL (pacing against a device)\n"); return 1; }
    }

    mutex_init(&s_audio_mutex);
    s_notify_queues[0].in_use = 1;
    s_notify_queues[0].key = 0x1234;
    s_mix_thread_running = 1;
    pthread_t th;
    const double t0 = now_s();
    assert(pthread_create(&th, NULL, audio_mix_thread_func, NULL) == 0);
    usleep(1000000);
    const unsigned n = __atomic_load_n(&s_events, __ATOMIC_RELAXED);
    const double el = now_s() - t0;
    s_mix_thread_running = 0;
    pthread_join(th, NULL);
    const double rate = n / el;
    printf("test_audio_pacing: %u blocks in %.3f s = %.1f/s (real time 187.5)\n", n, el, rate);
    /* Real time plus the ~4 blocks it may run ahead, with room for a loaded
     * host; the old loop's ~470/s is far outside. */
    if (rate < 160.0 || rate > 200.0) { printf("test_audio_pacing: FAIL\n"); return 1; }
    printf("test_audio_pacing: ok\n");
    return 0;
}
#else
int main(void) { return 0; }   /* the Windows loop paces on the device or QPC */
#endif
