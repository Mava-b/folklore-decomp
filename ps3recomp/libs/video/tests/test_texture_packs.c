/*
 * ps3recomp - texture packs (PS3_MODS, PS3_MODS_DIR, PS3_MODS_DUMP) in the
 * draw engine: pack file names, the content key, pack order, the
 * replacement's upload and mips, what is refused, the DXT decode the dump
 * uses, and a guest texture replaced and dumped end to end through
 * eng_texture_upload. A stub backend stands in for the image files.
 *
 *   clang -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I include -I libs/video \
 *     -o /tmp/test_texture_packs libs/video/tests/test_texture_packs.c libs/video/rsx_dispatch.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c libs/video/rsx_vp_decompiler.c \
 *     libs/video/rsx_fp_decompiler.c && /tmp/test_texture_packs
 */
#include "../rsx_draw_engine.c"
#include <assert.h>
#include <unistd.h>

u8* vm_base;
u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { return local ? (1u << 20) + offset : offset; }
u32 cellGcmResolveIO(u32 offset) { return offset; }
void cellGcm_replay_display_buffers_to_engine(void) {}

/* ---- the stub backend ---------------------------------------------------- */
static struct {
    u32 next;
    u32 created_w, created_h, created_mips, created_fmt, created_remap, created_rsx_fmt;
    int n_create, n_upload, n_release;
    u32 up_w[16], up_h[16], up_mip[16];
    u8  first_texel[4];
    int n_load, n_save;
    char saved_path[700];
    u32 saved_w, saved_h;
    u8  saved_first[4];
} st;

static u32 stub_create(void* u, rsx_be_format fmt, u32 w, u32 h, u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    st.n_create++;
    st.created_w = w; st.created_h = h; st.created_mips = mips; st.created_fmt = fmt;
    st.created_remap = remap; st.created_rsx_fmt = rsx_fmt;
    return ++st.next;
}
static void stub_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h, const void* src, u32 row, u32 rows)
{
    if (st.n_upload < 16) { st.up_w[st.n_upload] = w; st.up_h[st.n_upload] = h; st.up_mip[st.n_upload] = mip; }
    if (mip == 0) memcpy(st.first_texel, src, 4);
    st.n_upload++;
}
static void stub_release(void* u, u32 t) { st.n_release++; }
/* An "image": its size is in the file name after an 'x' (…_16x8.png), every
 * texel the same colour, 10 20 30 40. */
static int stub_load(void* u, const char* path, u8** rgba, u32* w, u32* h)
{
    st.n_load++;
    const char* p = strrchr(path, '_');
    unsigned a = 0, b = 0;
    if (!p || sscanf(p + 1, "%ux%u", &a, &b) != 2) return 0;
    *rgba = (u8*)malloc((size_t)a * b * 4);
    for (u32 i = 0; i < a * b; i++) { (*rgba)[i * 4] = 10; (*rgba)[i * 4 + 1] = 20; (*rgba)[i * 4 + 2] = 30; (*rgba)[i * 4 + 3] = 40; }
    *w = a; *h = b;
    return 1;
}
static int stub_save(void* u, const char* path, const u8* rgba, u32 w, u32 h, u32 row)
{
    st.n_save++;
    snprintf(st.saved_path, sizeof st.saved_path, "%s", path);
    st.saved_w = w; st.saved_h = h;
    memcpy(st.saved_first, rgba, 4);
    return 1;
}
static const rsx_draw_backend stub = {
    .texture_create = stub_create, .texture_upload = stub_upload, .texture_release = stub_release,
    .image_load = stub_load, .image_save = stub_save,
};

static void touch(const char* path)
{
    FILE* f = fopen(path, "wb");
    assert(f);
    fclose(f);
}

static void reset_mods(void)
{
    free(s_mods.e); free(s_mods.idx); free(s_mods.pool); free(s_mods.dumped);
    memset(&s_mods, 0, sizeof s_mods);
}

int main(void)
{
    g.be = &stub;

    /* ---- file names ---- */
    assert(eng_mod_name_key("3f09a1c2b4d5e6f7.png") == 0x3f09a1c2b4d5e6f7ull);
    assert(eng_mod_name_key("3F09A1C2B4D5E6F7_logo.PNG") == 0x3f09a1c2b4d5e6f7ull);
    assert(eng_mod_name_key("3f09a1c2b4d5e6f.png") == 0);      /* 15 digits */
    assert(eng_mod_name_key("3f09a1c2b4d5e6f7a.png") == 0);    /* a 17th digit */
    assert(eng_mod_name_key("3f09a1c2b4d5e6f7.jpg") == 0);
    assert(eng_mod_name_key("3f09a1c2b4d5e6f7") == 0);
    assert(eng_mod_name_key("notes.txt") == 0);

    /* ---- the key: content, base format and size; not the address ---- */
    const u64 k1 = eng_texmod_key(0x1234, 0xA5, 64, 32);
    assert(k1 && k1 == eng_texmod_key(0x1234, 0xA5, 64, 32));
    assert(k1 != eng_texmod_key(0x1235, 0xA5, 64, 32));
    assert(k1 != eng_texmod_key(0x1234, 0xA5, 32, 64));
    assert(k1 != eng_texmod_key(0x1234, 0xA6, 64, 32));
    assert(k1 == eng_texmod_key(0x1234, 0x85, 64, 32));      /* the LN bit (0x20) does not count */

    /* ---- packs: order, nesting, a missing one ---- */
    char root[] = "/tmp/texpacks_XXXXXX";
    assert(mkdtemp(root));
    char p[700];
    snprintf(p, sizeof p, "%s/hd", root);              mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/hd/ui", root);           mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/base", root);            mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/hd/1111111111111111_16x8.png", root);     touch(p);
    snprintf(p, sizeof p, "%s/hd/ui/2222222222222222_4x4.png", root);   touch(p);
    snprintf(p, sizeof p, "%s/base/1111111111111111_8x8.png", root);    touch(p);   /* hd wins */
    snprintf(p, sizeof p, "%s/base/3333333333333333_8x8.png", root);    touch(p);
    snprintf(p, sizeof p, "%s/base/readme.txt", root);                  touch(p);
    setenv("PS3_MODS_DIR", root, 1);
    setenv("PS3_MODS", " hd, base ,nothere", 1);
    unsetenv("PS3_MODS_DUMP");
    reset_mods();
    assert(eng_mods_wanted() && s_mods.on && s_mods.npacks == 2 && s_mods.n == 3);
    assert(eng_mod_find(0x1111111111111111ull)->pack == 0);
    assert(eng_mod_find(0x2222222222222222ull)->pack == 0);
    assert(eng_mod_find(0x3333333333333333ull)->pack == 1);
    assert(!eng_mod_find(0x4444444444444444ull));

    /* ---- a replacement: RGBA8 at its own size, mips down to 1x1 ---- */
    memset(&st, 0, sizeof st);
    u32 h = eng_texmod_apply(0x1111111111111111ull, 0xA5, 64, 32, 7, 0xAAE4);
    assert(h && st.n_create == 1 && st.created_fmt == RSX_BE_FMT_R8G8B8A8);
    assert(st.created_w == 16 && st.created_h == 8 && st.created_mips == 5);   /* 16,8,4,2,1 */
    assert(st.created_remap == 0xAAE4 && st.created_rsx_fmt == 0xA5);          /* the original's sampling */
    assert(st.n_upload == 5 && st.up_w[4] == 1 && st.up_h[4] == 1 && st.up_mip[4] == 4);
    assert(st.first_texel[0] == 10 && st.first_texel[3] == 40);
    /* no mips in the original: none in the replacement */
    memset(&st, 0, sizeof st);
    assert(eng_texmod_apply(0x2222222222222222ull, 0xA5, 4, 4, 1, 0) && st.created_mips == 1 && st.n_upload == 1);
    /* unnormalised coordinates (0x40): the size must stay */
    memset(&st, 0, sizeof st);
    assert(eng_texmod_apply(0x3333333333333333ull, 0xA5 | 0x40, 4, 4, 1, 0) == 0 && st.n_create == 0);
    assert(eng_mod_find(0x3333333333333333ull)->bad);
    assert(eng_texmod_apply(0x3333333333333333ull, 0xA5, 8, 8, 1, 0) == 0);   /* bad stays bad: no retry */
    /* not in any pack */
    assert(eng_texmod_apply(0x4444444444444444ull, 0xA5, 4, 4, 1, 0) == 0);

    /* ---- the box filter ---- */
    { const u8 src[2 * 2 * 4] = { 0,0,0,0,  4,8,12,16,  8,16,24,32,  12,24,36,48 };
      u8 dst[4];
      eng_rgba_half(src, 2, 2, dst, 1, 1);
      assert(dst[0] == 6 && dst[1] == 12 && dst[2] == 18 && dst[3] == 24); }

    /* ---- DXT1 for the dump: white/black palette ---- */
    { const u8 blk[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00 };   /* texel 1 = index 1 */
      u8 rgba[4 * 4 * 4];
      eng_bc_decode(blk, 8, 4, 4, RSX_TEXFMT_BC1, rgba);
      assert(rgba[0] == 255 && rgba[3] == 255);                 /* texel 0: c0 white */
      assert(rgba[4] == 0 && rgba[5] == 0 && rgba[7] == 255); } /* texel 1: c1 black */

    /* ---- end to end: a guest texture, replaced by content ---- */
    vm_base = (u8*)calloc(2u << 20, 1);
    const u32 off = 0x1000, W = 16, H = 16, FMT = 0xA5;        /* A8R8G8B8, linear */
    for (u32 i = 0; i < W * H; i++) { vm_base[off + i * 4] = 0xFF; vm_base[off + i * 4 + 1] = (u8)i; }
    int readable = 0;
    const u64 content = eng_texture_content_hash(RSX_LOCATION_MAIN, off, W * H * 4, &readable);
    assert(readable);
    const u64 key = eng_texmod_key(content, FMT, W, H);
    snprintf(p, sizeof p, "%s/base/%016llx_32x32.png", root, (unsigned long long)key);
    touch(p);
    char dump[700];
    snprintf(dump, sizeof dump, "%s/dump", root);
    setenv("PS3_MODS_DUMP", dump, 1);
    reset_mods();
    memset(&st, 0, sizeof st);
    u32 hnd = eng_texture_upload(RSX_LOCATION_MAIN, off, FMT, W, H, 1, W * 4, 0, 0xAAE4, 1);
    assert(hnd && st.created_w == 32 && st.created_h == 32);   /* the pack's, not the guest's */
    assert(st.n_save == 1 && st.saved_w == W && st.saved_h == H);   /* and the original dumped */
    { char want[700]; snprintf(want, sizeof want, "%s/%016llx.png", dump, (unsigned long long)key);
      assert(!strcmp(st.saved_path, want)); }
    /* the game rewrites the texture: a new key, no replacement, the original
     * uploaded and dumped once */
    vm_base[off] ^= 0x5A;
    memset(&st, 0, sizeof st);
    hnd = eng_texture_upload(RSX_LOCATION_MAIN, off, FMT, W, H, 1, W * 4, 0, 0xAAE4, 1);
    assert(hnd && st.created_w == W && st.created_h == H && st.n_save == 1);
    memset(&st, 0, sizeof st);
    eng_texture_upload(RSX_LOCATION_MAIN, off, FMT, W, H, 1, W * 4, 0, 0xAAE4, 1);
    assert(st.n_save == 0);                                    /* once a key */

    /* ---- off: nothing runs ---- */
    unsetenv("PS3_MODS");
    unsetenv("PS3_MODS_DUMP");
    reset_mods();
    assert(!eng_mods_wanted());
    memset(&st, 0, sizeof st);
    hnd = eng_texture_upload(RSX_LOCATION_MAIN, off, FMT, W, H, 1, W * 4, 0, 0xAAE4, 1);
    assert(hnd && st.n_load == 0 && st.n_save == 0 && st.created_w == W);

    reset_mods();
    snprintf(p, sizeof p, "rm -rf '%s'", root);
    if (system(p) != 0) fprintf(stderr, "could not remove %s\n", root);
    printf("test_texture_packs: ok\n");
    return 0;
}
