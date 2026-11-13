/*
 * ps3recomp - where /dev_hdd1 (the system cache) lands on the host.
 *
 * On the console cellSysCacheMount answers getCachePath = "/dev_hdd1": the
 * title sees its own cache directory as the ROOT of /dev_hdd1, and the
 * firmware maps that root to the title's cache folder on the drive (RPCS3:
 * dev_hdd1/caches/<cacheId>). Titles build their cache paths from that answer,
 * and some only work with it: Resistance's file layer splits "/dev_hdd1/x"
 * into the device "/dev_hdd1" and "x", and finds the device only if the cache
 * path it was given is "/dev_hdd1" itself. Handed "/dev_hdd1/cache/<id>", its
 * movie stash ("<cache>/hdd1_movie_stash", written when an in-level cutscene
 * needs the memory) never opened, and the main thread retried the open for
 * ever (level 30, the conversion-centre switch).
 *
 * cellSysCacheMount (libs/system/cellSysutil.c) now answers "/dev_hdd1" and
 * publishes the mounted cache id in the environment (PS3_SYSCACHE_ID), and both
 * halves of the split filesystem (runtime/ppu/ppu_fs.cpp and
 * runtime/syscalls/sys_fs.c) map through ps3_hdd1_host_path:
 *
 *   /dev_hdd1/<rest>  ->  <PS3_HDD1_ROOT>/cache/<id>/<rest>
 *   /dev_hdd1         ->  <PS3_HDD1_ROOT>/cache/<id>
 *
 * which is the same host folder the old "/dev_hdd1/cache/<id>/<rest>" answer
 * resolved to, so caches written before keep working. A guest path that still
 * spells "/dev_hdd1/cache/<id>/..." (an older build's answer kept in a save)
 * is not mapped twice. With no cache mounted, /dev_hdd1/<rest> maps to
 * <PS3_HDD1_ROOT>/<rest> as before.
 *
 * The id travels through the environment rather than a shared symbol so each
 * file-system translation unit (also built alone into its own test harness)
 * stays free of a link dependency on cellSysutil.
 */
#ifndef PS3EMU_HDD1_CACHE_H
#define PS3EMU_HDD1_CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS3_SYSCACHE_ID_ENV "PS3_SYSCACHE_ID"

/* Map a guest /dev_hdd1 path under host root `root`. Returns 1 and writes
 * `out` when `guest` is on /dev_hdd1, 0 when it is not (or there is no root,
 * or the result would not fit: then `out` is left as an empty string). */
static inline int ps3_hdd1_host_path(const char* guest, const char* root,
                                     char* out, size_t cap)
{
    if (!guest || !root || !*root || !out || !cap) return 0;
    if (strncmp(guest, "/dev_hdd1", 9) != 0) return 0;
    if (guest[9] != '\0' && guest[9] != '/') return 0;     /* "/dev_hdd10" */
    const char* rest = guest[9] ? guest + 10 : "";
    const char* id = getenv(PS3_SYSCACHE_ID_ENV);
    int n;
    if (id && *id) {
        /* Already spelled with the cache folder: do not add it again. */
        size_t idn = strlen(id);
        int spelled = strncmp(rest, "cache/", 6) == 0 &&
                      strncmp(rest + 6, id, idn) == 0 &&
                      (rest[6 + idn] == '\0' || rest[6 + idn] == '/');
        if (spelled)
            n = snprintf(out, cap, "%s/%s", root, rest);
        else if (*rest)
            n = snprintf(out, cap, "%s/cache/%s/%s", root, id, rest);
        else
            n = snprintf(out, cap, "%s/cache/%s", root, id);
    } else {
        n = snprintf(out, cap, "%s/%s", root, rest);
    }
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
    return 1;
}

#endif /* PS3EMU_HDD1_CACHE_H */
