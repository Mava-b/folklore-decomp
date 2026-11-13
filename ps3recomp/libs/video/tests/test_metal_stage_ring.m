/* clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_stage_ring.m libs/video/rsx_texture_layout.c -Wl,-dead_strip
 * -framework Metal -framework Cocoa -framework QuartzCore -o /tmp/test_metal_stage_ring
 *
 * The Metal engine's staging arena is a ring of shared MTLBuffers written in
 * place: staged bytes are in the slot's buffer itself (nothing copied at
 * commit), each submission takes the next slot, growth keeps what was
 * already staged, a slot still marked in flight is waited for, and shutdown
 * lets every slot go. */
#include "../rsx_metal_backend.m"
#include <assert.h>
/* The present path is linked in through the backend table. */
float cellAvconfExt_display_gamma(void) { return 1.0f; }

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        s_queue = [s_dev newCommandQueue];
        assert(s_queue);

        u8 a[64], b[64];
        for (u32 i = 0; i < 64; i++) { a[i] = (u8)i; b[i] = (u8)(0xFF - i); }

        /* The first data of a submission takes a slot and lands in its buffer. */
        u32 off = 0;
        assert(s_eng_slot < 0);
        assert(eng_stage_copy(a, sizeof a, &off));
        const int k0 = s_eng_slot;
        assert(k0 >= 0 && s_eng_slot_buf[k0]);
        assert((u8*)[s_eng_slot_buf[k0] contents] == s_eng_stage);
        assert(!memcmp((u8*)[s_eng_slot_buf[k0] contents] + off, a, sizeof a));

        /* A commit hands that buffer over; the next submission uses the next slot. */
        eng_encode_and_commit(nil);
        assert(s_eng_slot < 0 && s_eng_stage_used == 0 && !s_eng_encoding_failed);
        id<MTLBuffer> first = s_eng_slot_buf[k0];
        assert(eng_stage_copy(b, sizeof b, &off));
        const int k1 = s_eng_slot;
        assert(k1 == (k0 + 1) % ENG_STAGE_SLOTS && s_eng_slot_buf[k1] != first);
        assert(!memcmp((u8*)[s_eng_slot_buf[k1] contents] + off, b, sizeof b));
        assert(!memcmp([first contents], a, sizeof a));   /* the committed slot is untouched */

        /* Growing a slot keeps what this submission staged before. */
        const u32 b_off = off;
        const u32 big = (u32)[s_eng_slot_buf[k1] length] + 4096u;
        u32 big_off = 0;
        assert(eng_stage_reserve(big, &big_off));
        assert([s_eng_slot_buf[k1] length] >= big_off + big);
        assert(!memcmp((u8*)[s_eng_slot_buf[k1] contents] + b_off, b, sizeof b));
        eng_encode_and_commit(nil);

        /* A slot still in flight is waited for, not overwritten. */
        const int k2 = (int)(s_eng_slot_next % ENG_STAGE_SLOTS);
        s_eng_slot_busy[k2] = 1;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC),
                       dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                           __atomic_store_n(&s_eng_slot_busy[k2], 0, __ATOMIC_RELEASE);
                       });
        const unsigned long waits = s_eng_slot_waits;
        assert(eng_stage_copy(a, sizeof a, &off));
        assert(s_eng_slot == k2 && s_eng_slot_waits == waits + 1 && !s_eng_slot_busy[k2]);
        eng_encode_and_commit(nil);

        eng_shutdown(NULL);
        for (int k = 0; k < ENG_STAGE_SLOTS; k++) assert(!s_eng_slot_buf[k]);
        assert(s_eng_slot < 0 && !s_eng_stage && !s_eng_stage_cap);
        puts("Metal staging ring: staged in place, one slot per submission, in-flight slots waited for");
    }
}
