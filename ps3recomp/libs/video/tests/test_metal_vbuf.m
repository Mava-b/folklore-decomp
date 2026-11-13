/* clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_vbuf.m libs/video/rsx_texture_layout.c -Wl,-dead_strip
 * -framework Metal -framework Cocoa -framework QuartzCore -o /tmp/test_metal_vbuf
 *
 * The Metal backend's persistent vertex buffers (the draw engine's vertex
 * cache): a buffer holds the bytes it was made from, a draw records its
 * handle, and a released handle keeps naming the same MTLBuffer until no
 * recorded draw is left, so a draw recorded before the release never encodes
 * another buffer's vertices. */
#include "../rsx_metal_backend.m"
#include <assert.h>
/* The present path is linked in through the backend table checked below. */
float cellAvconfExt_display_gamma(void) { return 1.0f; }
int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        u8 a[64], b[64];
        for (u32 i = 0; i < 64; i++) { a[i] = (u8)i; b[i] = (u8)(0xFF - i); }

        u32 ha = eng_vbuf_create(NULL, a, sizeof a);
        assert(ha);
        id<MTLBuffer> ba = s_vbuf[ha - 1];
        assert(ba && [ba length] == sizeof a && !memcmp([ba contents], a, sizeof a));
        assert(!eng_vbuf_create(NULL, NULL, 16) && !eng_vbuf_create(NULL, a, 0));

        /* A draw from it records the handle, with its indices staged. */
        const u32 idx[3] = { 2, 1, 0 };
        eng_draw_vbuf(NULL, RSX_TOPOLOGY_TRIANGLES, ha, 4, 16, idx, 3);
        assert(s_eng_rec_count == 1);
        const EngRecord* r = &s_eng_rec[0];
        assert(r->kind == ENG_REC_DRAW && r->vb_buf == ha && r->vertex_count == 4 &&
               r->stride == 16 && r->index_count == 3);
        assert(!memcmp(s_eng_stage + r->ib_off, idx, sizeof idx));
        /* An ordinary draw still stages its vertices and names no buffer. */
        eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, a, 4, 16, NULL, 0);
        assert(s_eng_rec_count == 2 && s_eng_rec[1].vb_buf == 0);

        /* Released while recorded: the slot is not recycled yet. */
        eng_vbuf_release(NULL, ha);
        eng_vbuf_release(NULL, ha);            /* a second release is harmless */
        assert(s_vbuf[ha - 1] == ba);
        u32 hb = eng_vbuf_create(NULL, b, sizeof b);
        assert(hb && hb != ha && !memcmp([s_vbuf[hb - 1] contents], b, sizeof b));
        /* A draw naming a retired handle is not recorded. */
        eng_draw_vbuf(NULL, RSX_TOPOLOGY_TRIANGLES, ha, 4, 16, NULL, 0);
        assert(s_eng_rec_count == 2);

        /* Once everything recorded has been encoded, the slot is free. */
        s_eng_rec_count = 0;
        s_eng_stage_used = 0;
        eng_collect_retired_objects();
        assert(!s_vbuf[ha - 1] && s_vbuf[hb - 1]);
        u32 hc = eng_vbuf_create(NULL, a, 32);
        assert(hc == ha && [s_vbuf[hc - 1] length] == 32);

        eng_shutdown(NULL);
        assert(!s_vbuf_count && !s_vbuf_free_count && !s_vbuf[ha - 1] && !s_vbuf[hb - 1]);
        assert(s_engine_backend.vbuf_create && s_engine_backend.vbuf_release &&
               s_engine_backend.draw_vbuf);
        puts("Metal persistent vertex buffers keep their bytes until no draw names them");
    }
}
