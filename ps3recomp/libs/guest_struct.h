/*
 * ps3recomp - moving pointer-free structs across the guest boundary
 *
 * An HLE entry point receives pointer arguments as raw 32-bit GUEST addresses,
 * and everything in guest memory is big-endian. For a struct that holds NO
 * pointers and whose fields are all 4-byte scalars, that is the only
 * difference: the guest lays such a struct out exactly as the host does, field
 * for field, so the whole thing can move a word at a time with a byte swap.
 *
 * IMPORTANT -- when these do NOT apply:
 *
 *   - The struct holds a pointer. The guest's is 4 bytes and the host's is 8,
 *     so every field after it sits at a different offset. Read those by
 *     explicit guest offset (see cellFont.c, cellHttp.c).
 *
 *   - The struct holds a u64 or a double. A big-endian u64 is high word first,
 *     but on a little-endian host the low half comes first in memory, so a
 *     word-at-a-time copy swaps the two halves. Use vm_read64/vm_write64 for
 *     that field (see cellJpgDec.c's jpgdec_out_param_store).
 *
 * A struct of nothing but u32/s32/float is the common case, and that is what
 * these two are for.
 */

#ifndef PS3RECOMP_GUEST_STRUCT_H
#define PS3RECOMP_GUEST_STRUCT_H

#include "../runtime/ppu/ppu_memory.h"
#include <stddef.h>   /* offsetof, for the by-offset cases described above */

/* guest -> host. `size` must be a multiple of 4. */
static inline void guest_struct_load(void* host, uint32_t ea, uint32_t size)
{
    uint32_t* w = (uint32_t*)host;
    if (!ea) {
        for (uint32_t i = 0; i < size / 4; i++)
            w[i] = 0;
        return;
    }
    for (uint32_t i = 0; i < size / 4; i++)
        w[i] = vm_read32(ea + i * 4);
}

/* host -> guest. `size` must be a multiple of 4. */
static inline void guest_struct_store(uint32_t ea, const void* host, uint32_t size)
{
    if (!ea)
        return;
    const uint32_t* w = (const uint32_t*)host;
    for (uint32_t i = 0; i < size / 4; i++)
        vm_write32(ea + i * 4, w[i]);
}

#define GUEST_EA(p) ((uint32_t)(uintptr_t)(p))

#endif /* PS3RECOMP_GUEST_STRUCT_H */
