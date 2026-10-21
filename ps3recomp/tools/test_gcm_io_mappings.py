#!/usr/bin/env python3
"""cellGcm IO mapping table: many mappings, no leak on refusal, no slot reuse.

Builds the real find_free_mapping, gcm_io_alloc, cellGcmMapMainMemory and
cellGcm_syscall_iomap (pulled verbatim out of libs/video/cellGcmSys.c, with
CELL_GCM_MAX_IO_MAPPINGS from cellGcmSys.h) against small stubs for guest
memory and the offset table, and checks:

1. a title mapping separate 1 MB chunks gets at least 40 distinct IO offsets
   before the first refusal (Killzone 3 maps up to 38);
2. a refused cellGcmMapMainMemory does not consume IO address space;
3. a sys_rsx iomap after an unmap does not overwrite a live mapping.

Run: python tools/test_gcm_io_mappings.py
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
src = (root / 'libs/video/cellGcmSys.c').read_text()
hdr = (root / 'libs/video/cellGcmSys.h').read_text()


def function(signature):
    m = re.search(signature, src)
    assert m, f'{signature} not found'
    i, depth = src.index('{', m.start()) + 1, 1
    while depth:
        depth += {'{': 1, '}': -1}.get(src[i], 0)
        i += 1
    return src[m.start():i]


slots = re.search(r'^#define CELL_GCM_MAX_IO_MAPPINGS\s+(\d+)', hdr, re.M).group(1)
parts = [function(r'static IoMapping\* find_free_mapping\(void\)'),
         function(r'static u32 gcm_io_alloc\(u32 size\)\n\{'),
         function(r's32 cellGcmMapMainMemory\(u32 ea, u32 size, u32\* offset\)'),
         function(r'void cellGcm_syscall_iomap\(u32 ea, u32 io, u32 size\)')]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>
#ifndef CELL_OK
#define CELL_OK 0
#endif
#ifndef CELL_GCM_MAX_IO_MAPPINGS
#define CELL_GCM_MAX_IO_MAPPINGS %s
#endif
typedef struct { u32 ioAddress, ioSize; } CellGcmConfig;
static CellGcmConfig s_config = { 0x100000u, 0x200000u };
typedef struct IoMapping { u32 ea; u32 io; u32 size; int active; } IoMapping;
static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;
static unsigned char mem[0x1000];
static void vm_write32(u32 a, u32 v) { for (int i = 0; i < 4; i++) mem[a + i] = (unsigned char)(v >> (24 - 8 * i)); }
static u32 rd32(u32 a) { return (u32)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
static void populate_offset_table(u32 ea, u32 io, u32 size) { (void)ea; (void)io; (void)size; }
static u32 s_io_alloc_next = 0;
static u32 gcm_io_alloc(u32 size);
%s
int main(void)
{
    u32* out = (u32*)(uintptr_t)0x100;     /* guest out-param address */
    int ok = 1;

    /* 3: map A and B, unmap A (as cellGcmUnmapIoAddress does), then iomap C. */
    cellGcmMapMainMemory(0x20000000u, 0x100000u, out);
    cellGcmMapMainMemory(0x20100000u, 0x100000u, out);
    s_io_mappings[0].active = 0; s_io_mapping_count--;
    cellGcm_syscall_iomap(0x30000000u, 0x00A00000u, 0x100000u);
    int b_kept = 0, c_found = 0;
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        b_kept  |= s_io_mappings[i].active && s_io_mappings[i].ea == 0x20100000u;
        c_found |= s_io_mappings[i].active && s_io_mappings[i].ea == 0x30000000u;
    }
    printf("%%s live mapping kept after unmap + iomap: %%s\n",
           b_kept && c_found ? "ok  " : "FAIL", b_kept ? (c_found ? "yes" : "new one lost") : "overwritten");
    ok &= b_kept && c_found;

    /* 1 + 2: map 1 MB chunks until the first refusal. */
    static u32 offs[4096];
    int n = 0, distinct = 1, leaked = -1;
    for (int i = 0; i < 4096; i++) {
        u32 before = s_io_alloc_next;
        if (cellGcmMapMainMemory(0x40000000u + (u32)i * 0x100000u, 0x100000u, out) != CELL_OK) {
            leaked = s_io_alloc_next != before;
            break;
        }
        offs[n++] = rd32(0x100);
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++)
            if (offs[i] == offs[j]) distinct = 0;
    printf("%%s 1 MB chunks mapped before the first refusal: %%d (want >= 40, distinct offsets: %%s)\n",
           n >= 40 && distinct ? "ok  " : "FAIL", n, distinct ? "yes" : "no");
    ok &= n >= 40 && distinct;
    printf("%%s refused call leaves the IO allocator alone: %%s\n",
           leaked == 0 ? "ok  " : "FAIL", leaked < 0 ? "no refusal" : leaked ? "leaked" : "yes");
    ok &= leaked == 0;
    return ok ? 0 : 1;
}
'''

with tempfile.TemporaryDirectory(prefix='gcm-io-') as tmp:
    c = Path(tmp) / 'io.c'
    exe = Path(tmp) / 'io'
    c.write_text(HARNESS % (slots, '\n'.join(parts)))
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-w',
                    '-I' + str(root / 'include'), str(c), '-o', str(exe)], check=True)
    r = subprocess.run([str(exe)], timeout=10, stdout=subprocess.PIPE, text=True)
    print(''.join(l for l in r.stdout.splitlines(True) if not l.startswith('[cellGcmSys]')), end='')
    print('PASS' if r.returncode == 0 else 'FAIL')
    raise SystemExit(r.returncode)
