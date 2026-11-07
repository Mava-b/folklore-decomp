/* /dev_hdd1 -> host mapping of the mounted system cache (ps3emu/hdd1_cache.h).
 *
 *   clang -std=gnu17 -I include tests/test_hdd1_cache.c -o /tmp/test_hdd1_cache && /tmp/test_hdd1_cache
 */
#include "ps3emu/hdd1_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int maps(const char* guest, const char* root, const char* want)
{
    char out[256];
    memset(out, 'x', sizeof out);
    if (!ps3_hdd1_host_path(guest, root, out, sizeof out)) return want == NULL;
    if (!want) { fprintf(stderr, "  '%s' mapped to '%s', expected no mapping\n", guest, out); return 0; }
    if (strcmp(out, want)) { fprintf(stderr, "  '%s' -> '%s', expected '%s'\n", guest, out, want); return 0; }
    return 1;
}

int main(void)
{
    /* No cache mounted: /dev_hdd1/x -> <root>/x (the old mapping). */
    unsetenv(PS3_SYSCACHE_ID_ENV);
    CHECK(maps("/dev_hdd1/cache/BCUS98107/PROFILE.SAV", "/h1", "/h1/cache/BCUS98107/PROFILE.SAV"));
    CHECK(maps("/dev_hdd1/x", "/h1", "/h1/x"));
    CHECK(maps("/dev_hdd1", "/h1", "/h1/"));

    /* Mounted: the cache folder is the root of /dev_hdd1. */
    setenv(PS3_SYSCACHE_ID_ENV, "BCUS98107", 1);
    CHECK(maps("/dev_hdd1/hdd1_movie_stash", "/h1", "/h1/cache/BCUS98107/hdd1_movie_stash"));
    CHECK(maps("/dev_hdd1/PROFILE.SAV", "/h1", "/h1/cache/BCUS98107/PROFILE.SAV"));
    CHECK(maps("/dev_hdd1/a/b.dat", "/h1", "/h1/cache/BCUS98107/a/b.dat"));
    CHECK(maps("/dev_hdd1", "/h1", "/h1/cache/BCUS98107"));
    CHECK(maps("/dev_hdd1/", "/h1", "/h1/cache/BCUS98107"));
    /* The old answer's spelling lands in the same folder, not cache/<id>/cache/<id>. */
    CHECK(maps("/dev_hdd1/cache/BCUS98107/PROFILE.SAV", "/h1", "/h1/cache/BCUS98107/PROFILE.SAV"));
    CHECK(maps("/dev_hdd1/cache/BCUS98107", "/h1", "/h1/cache/BCUS98107"));
    /* Another title's folder name is just a path inside this cache. */
    CHECK(maps("/dev_hdd1/cache/BCUS98107X/f", "/h1", "/h1/cache/BCUS98107/cache/BCUS98107X/f"));
    /* Not /dev_hdd1. */
    CHECK(maps("/dev_hdd10/x", "/h1", NULL));
    CHECK(maps("/dev_hdd0/game/x", "/h1", NULL));
    CHECK(maps("/dev_bdvd/PS3_GAME", "/h1", NULL));
    /* No root, no mapping (the caller falls back to its mount table). */
    CHECK(maps("/dev_hdd1/x", "", NULL));
    CHECK(maps("/dev_hdd1/x", NULL, NULL));
    /* Windows separators come out as '/'. */
    CHECK(maps("/dev_hdd1/a\\b", "C:\\h1", "C:/h1/cache/BCUS98107/a/b"));

    /* Too small: no mapping, and an empty string rather than a truncated path. */
    char small[12];
    memset(small, 'x', sizeof small);
    CHECK(ps3_hdd1_host_path("/dev_hdd1/hdd1_movie_stash", "/h1", small, sizeof small) == 0);
    CHECK(small[0] == '\0');
    CHECK(ps3_hdd1_host_path("/dev_hdd1/x", "/h1", small, 0) == 0);

    printf("%u hdd1 cache mapping checks PASS\n", checks);
    return 0;
}
