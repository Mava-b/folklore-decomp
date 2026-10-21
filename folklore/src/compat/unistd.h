/* Minimal Windows <unistd.h> shim for the ps3recomp PPU boot scaffold built
 * with clang-cl/MSVC. Maps the POSIX low-level I/O the VFS uses onto MSVC's
 * <io.h>. */
#ifndef PS3RECOMP_COMPAT_UNISTD_H
#define PS3RECOMP_COMPAT_UNISTD_H

#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdlib.h>

/* MSVC spells these with a leading underscore. */
#ifndef open
#define open   _open
#endif
#ifndef close
#define close  _close
#endif
#ifndef read
#define read   _read
#endif
#ifndef write
#define write  _write
#endif
#ifndef lseek
#define lseek  _lseek
#endif
#ifndef dup
#define dup    _dup
#endif

#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

#endif /* PS3RECOMP_COMPAT_UNISTD_H */
