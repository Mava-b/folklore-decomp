/*
 * A free (license type 3) EDAT under a title-specific klicensee decrypts once
 * the title has passed that klicensee to sceNpDrmIsAvailable[2], the way it
 * does on hardware before opening the file.
 *
 * Everything here is synthetic: the klicensee, the content id and the
 * plaintext are made up, and the EDAT is built by this file with edat.c's
 * own AES/CMAC primitives (NPD version 3, license 3, flags 0, three blocks,
 * the last one partial). No key or file from any title is used.
 *
 * Second file: test_edat_guest_klicensee_np.c (sceNp.c). Built and run by
 * tools/test_edat_guest_klicensee.py, which passes a scratch directory as
 * argv[1] and removes it afterwards.
 */
#include "../edat.c"
#include "ps3emu/ps3types.h"

s32 sceNpDrmIsAvailable(const u8* k_licensee, const char* drm_path);
s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path);
extern uint8_t* vm_base;

#define BLOCK  0x400u
#define FSIZE  2500u          /* 3 blocks, the last 452 bytes */

static const uint8_t KLIC[16] = {   /* synthetic title klicensee */
    0x5A,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0x01 };
static const uint8_t OTHER[16] = {  /* a different, equally synthetic key */
    0xA5,0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,0x90,0xA0,0xB0,0xC0,0xD0,0xE0,0xF0 };

static void put32(uint8_t* p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                        const uint8_t* in, uint8_t* out, size_t len)
{
    uint8_t rk[176], x[16];
    aes128_expand(key, rk);
    memcpy(x, iv, 16);
    for (size_t o = 0; o < len; o += 16) {
        for (int i = 0; i < 16; i++) x[i] ^= in[o + i];
        aes128_encrypt_block(rk, x, out + o);
        memcpy(x, out + o, 16);
    }
}

/* NPD v3, license 3, flags 0: meta table (one 16-byte CMAC per block) at
 * 0x100, then the blocks. Block key = AES(klic, dev_hash[0..11] || be32(i)),
 * data AES-CBC under it with the header digest as IV. */
static int build_edat(const char* path, const uint8_t* plain)
{
    const uint32_t nblk = (FSIZE + BLOCK - 1) / BLOCK;
    uint8_t hdr[0x100] = {0};
    memcpy(hdr, "NPD\0", 4);
    put32(hdr + 0x04, 3);                     /* version */
    put32(hdr + 0x08, 3);                     /* license: free */
    put32(hdr + 0x0C, 0);                     /* type */
    memcpy(hdr + 0x10, "XX0000-TEST00000_00-SYNTHETICEDAT0001", 36);
    for (int i = 0; i < 16; i++) hdr[0x40 + i] = (uint8_t)(0x30 + i);   /* digest */
    for (int i = 0; i < 16; i++) hdr[0x50 + i] = (uint8_t)(0x90 + i);   /* title hash */
    uint8_t ck[16];
    for (int i = 0; i < 16; i++) ck[i] = (uint8_t)(KLIC[i] ^ NP_OMAC_KEY_2[i]);
    aes128_cmac(ck, hdr, 0x60, hdr + 0x60);   /* dev_hash */
    put32(hdr + 0x80, 0);                     /* flags */
    put32(hdr + 0x84, BLOCK);
    put32(hdr + 0x88, 0); put32(hdr + 0x8C, FSIZE);

    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t rk[176];
    aes128_expand(KLIC, rk);
    static uint8_t enc[3][BLOCK];
    uint8_t meta[3][16];
    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t len = (b == nblk - 1 && FSIZE % BLOCK) ? FSIZE % BLOCK : BLOCK;
        uint32_t padded = (len + 15) & ~15u;
        uint8_t pt[BLOCK] = {0}, bk[16] = {0}, kres[16];
        memcpy(pt, plain + b * BLOCK, len);
        memcpy(bk, hdr + 0x60, 12);
        put32(bk + 12, b);
        aes128_encrypt_block(rk, bk, kres);
        cbc_encrypt(kres, hdr + 0x40, pt, enc[b], padded);
        aes128_cmac(kres, enc[b], padded, meta[b]);
    }
    for (uint32_t b = 0; b < nblk; b++) fwrite(meta[b], 1, 16, f);
    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t len = (b == nblk - 1 && FSIZE % BLOCK) ? FSIZE % BLOCK : BLOCK;
        fwrite(enc[b], 1, (len + 15) & ~15u, f);
    }
    fclose(f);
    return 0;
}

static int s_failed;
static void check(int ok, const char* what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) s_failed++;
}

int main(int argc, char** argv)
{
    if (argc < 2) { puts("FAIL: usage: test_edat_guest_klicensee SCRATCH_DIR"); return 1; }
    const char* dir = argv[1];
    char in[1024], out[1024], what[160];
    snprintf(in, sizeof in, "%s/data.edat", dir);
    snprintf(out, sizeof out, "%s/data.edat.dec", dir);

    static uint8_t plain[FSIZE];
    for (uint32_t i = 0; i < FSIZE; i++) plain[i] = (uint8_t)(i * 7 + (i >> 8));
    if (build_edat(in, plain) != 0) { puts("FAIL: could not write the fixture"); return 1; }

    vm_base = calloc(1, 0x10000);
    if (!vm_base) { puts("FAIL: calloc"); return 1; }
    const uint32_t klic_ea = 0x1000, other_ea = 0x1010, path_ea = 0x1100;
    memcpy(vm_base + klic_ea, KLIC, 16);
    memcpy(vm_base + other_ea, OTHER, 16);
    strcpy((char*)vm_base + path_ea, "/dev_hdd0/game/TEST00000/USRDIR/data.edat");

    int rc = edat_decrypt_file(in, out);
    snprintf(what, sizeof what, "no klicensee from the guest: -6 (got %d)", rc);
    check(rc == -6, what);

    s32 r = sceNpDrmIsAvailable2((const u8*)(uintptr_t)other_ea,
                                 (const char*)(uintptr_t)path_ea);
    snprintf(what, sizeof what, "sceNpDrmIsAvailable2(other key) = %d", r);
    check(r == 0, what);
    rc = edat_decrypt_file(in, out);
    snprintf(what, sizeof what, "a key that does not verify dev_hash is not used: -6 (got %d)", rc);
    check(rc == -6, what);

    r = sceNpDrmIsAvailable((const u8*)(uintptr_t)klic_ea,
                            (const char*)(uintptr_t)path_ea);
    snprintf(what, sizeof what, "sceNpDrmIsAvailable(title key) = %d", r);
    check(r == 0, what);
    rc = edat_decrypt_file(in, out);
    snprintf(what, sizeof what, "with the title's klicensee: 0, every block hash verified (got %d)", rc);
    check(rc == 0, what);
    if (rc == 0) {
        static uint8_t got[FSIZE + 16];
        FILE* f = fopen(out, "rb");
        size_t n = f ? fread(got, 1, sizeof got, f) : 0;
        if (f) fclose(f);
        snprintf(what, sizeof what, "plaintext: %zu bytes, matches", n);
        check(n == FSIZE && memcmp(got, plain, FSIZE) == 0, what);
    }

    remove(out); remove(in);
    free(vm_base);
    if (s_failed) { printf("FAIL (%d failed)\n", s_failed); return 1; }
    puts("PASS (0 failed)");
    return 0;
}
