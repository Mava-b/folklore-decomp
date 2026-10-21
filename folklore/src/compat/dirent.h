/* Minimal Windows <dirent.h> shim (FindFirstFile-based) for building the
 * ps3recomp PPU boot scaffold (runtime/ppu/ppu_fs.cpp) with clang-cl/MSVC.
 * Covers exactly what the VFS uses: opendir/readdir/closedir + d_name/d_type. */
#ifndef PS3RECOMP_COMPAT_DIRENT_H
#define PS3RECOMP_COMPAT_DIRENT_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    unsigned char d_type;
    char          d_name[260];
};

typedef struct DIR {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              first;
    struct dirent    ent;
} DIR;

static inline DIR* opendir(const char* path)
{
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", path);
    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) return NULL;
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}

static inline struct dirent* readdir(DIR* d)
{
    if (!d) return NULL;
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = 0;
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    strncpy(d->ent.d_name, d->fd.cFileName, sizeof(d->ent.d_name) - 1);
    d->ent.d_name[sizeof(d->ent.d_name) - 1] = 0;
    return &d->ent;
}

static inline int closedir(DIR* d)
{
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

#endif /* PS3RECOMP_COMPAT_DIRENT_H */
