/* ps3_env_flag (ps3emu/env_cache.h): the debug switches on the RSX thread's
 * hot paths (cellGcmSys.c) read the environment once per call site.
 *
 *   clang -std=gnu17 -Wall -Wextra -I include libs/video/tests/test_env_cache.c \
 *         -o /tmp/test_env_cache && /tmp/test_env_cache
 */
#include "ps3emu/env_cache.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    unsetenv("PS3_TEST_ENV_FLAG");
    int a = -1;
    assert(ps3_env_flag(&a, "PS3_TEST_ENV_FLAG") == 0 && a == 0);
    setenv("PS3_TEST_ENV_FLAG", "1", 1);
    assert(ps3_env_flag(&a, "PS3_TEST_ENV_FLAG") == 0);      /* cached: not read again */
    int b = -1;
    assert(ps3_env_flag(&b, "PS3_TEST_ENV_FLAG") == 1 && b == 1);   /* another call site reads it */
    unsetenv("PS3_TEST_ENV_FLAG");
    assert(ps3_env_flag(&b, "PS3_TEST_ENV_FLAG") == 1);
    setenv("PS3_TEST_ENV_FLAG", "", 1);                      /* set to nothing still counts */
    int c = -1;
    assert(ps3_env_flag(&c, "PS3_TEST_ENV_FLAG") == 1);

    /* what it saves: a getenv over a typical environment vs the cached read */
    for (int i = 0; i < 60; i++) { char k[32]; snprintf(k, sizeof k, "PS3_TEST_PAD_%02d", i); setenv(k, "x", 1); }
    const int N = 2000000;
    volatile int sink = 0;
    struct timespec t0, t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) sink += getenv("GCM_RATE") != NULL;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int d = -1;
    for (int i = 0; i < N; i++) sink += ps3_env_flag(&d, "GCM_RATE");
    clock_gettime(CLOCK_MONOTONIC, &t2);
    const double g = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / N;
    const double f = ((t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec)) / N;
    printf("getenv %.1f ns/call, ps3_env_flag %.2f ns/call (%d)\n", g, f, sink & 1);
    printf("test_env_cache: ok\n");
    return 0;
}
