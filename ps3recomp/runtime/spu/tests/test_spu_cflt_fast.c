/* test_spu_cflt_fast.c: the cflts/cfltu fast paths against the exact lanes (spu_cflt*_ref).
 * Build: cc -O2 -I.. -I../.. -I../../../include test_spu_cflt_fast.c -o /tmp/t && /tmp/t */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "spu_context.h"
#include "spu_helpers.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

int main(void)
{
    unsigned long long n = 0, bad = 0;
    static const uint32_t edge[] = { 0, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu,
        0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x3F800000u, 0xBF800000u, 0x4EFFFFFFu,
        0x4F000000u, 0xCF000000u, 0xCF000001u, 0x4F800000u, 0x3F7FFFFFu, 0xBF7FFFFFu, 0x3EFFFFFFu };
    for (int i8 = 0; i8 < 256; i8++) {
        for (unsigned k = 0; k < 200000; k++) {
            u128 a;
            for (int l = 0; l < 4; l++) {
                uint32_t v = rnd();
                if ((k & 7) == 0) v = edge[rnd() % (sizeof edge / sizeof edge[0])];
                else if ((k & 7) == 1) v = (v & 0x807FFFFFu) | ((uint32_t)(127 + (int)(rnd() % 64) - 32) << 23);
                a._u32[l] = v;
            }
            u128 f = spu_cflts(a, i8), e = spu_cflts_ref(a, i8);
            u128 fu = spu_cfltu(a, i8), eu = spu_cfltu_ref(a, i8);
            n += 2;
            if (memcmp(&f, &e, 16) || memcmp(&fu, &eu, 16)) {
                if (bad++ < 10)
                    printf("MISMATCH i8=%d in %08X %08X %08X %08X: s %08X/%08X u %08X/%08X\n", i8,
                           a._u32[0], a._u32[1], a._u32[2], a._u32[3], f._u32[0], e._u32[0], fu._u32[0], eu._u32[0]);
            }
        }
    }
    printf("%llu conversions, %llu mismatches\n", n, bad);
    return bad != 0;
}
