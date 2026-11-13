/*
 * ps3recomp - a diagnostic environment variable, read once per call site
 *
 * getenv walks the whole environment with a string compare per entry, and
 * a debug switch tested on a hot path (the RSX FIFO walker runs its checks
 * on every drain, a flip-status query can be polled in a tight loop) pays
 * that every time even when the switch is off. These read it once:
 *
 *     static int s_rate = -1;
 *     if (ps3_env_flag(&s_rate, "GCM_RATE")) { ... }
 *
 * The cache is per call site and never re-read, as the `static int x = -1`
 * pattern elsewhere in the tree: set the variable before the process starts.
 * Two threads that race on the first read both store the same value.
 */
#ifndef PS3EMU_ENV_CACHE_H
#define PS3EMU_ENV_CACHE_H

#include <stdlib.h>

/* 1 if `name` is set (to anything, even ""), else 0. *cache starts at -1. */
static inline int ps3_env_flag(int* cache, const char* name)
{
    int v = __atomic_load_n(cache, __ATOMIC_RELAXED);
    if (v < 0) {
        v = getenv(name) ? 1 : 0;
        __atomic_store_n(cache, v, __ATOMIC_RELAXED);
    }
    return v;
}

#endif /* PS3EMU_ENV_CACHE_H */
