/*
 * Binary PARAM.SFO (PSF) writer/reader for the savedata HLE. Header-only, no dependencies, so a host
 * test can include it alone.
 *
 * Layout [src:psdevwiki "PARAM.SFO"]: header (magic "\0PSF" and version 01 01 00 00 as they appear in the file, then
 * little-endian key_table_start, data_table_start, entry count), 16-byte index entries (key_offset u16, fmt u16,
 * len u32, max_len u32, data_offset u32, little-endian), key table (NUL-terminated names in alphabetical order,
 * padded to 4 bytes), data table (each value padded with zeros to its max_len, a multiple of 4).
 * fmt 0x0004 = utf8-S (not NUL-terminated), 0x0204 = utf8 (NUL counted in len), 0x0404 = int32.
 * The field set is that of a PS3 game save (CATEGORY "SD"); max lengths are the wiki's table values.
 */
#ifndef PS3_PSF_H
#define PS3_PSF_H
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define PS3_PSF_UTF8S 0x0004u
#define PS3_PSF_UTF8  0x0204u
#define PS3_PSF_INT32 0x0404u

static void ps3_psf_put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void ps3_psf_put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t ps3_psf_get16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t ps3_psf_get32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    const char*    key;
    uint16_t       fmt;
    uint32_t       max_len;   /* multiple of 4 */
    const uint8_t* data;      /* utf8: the C string; utf8-S: raw bytes; int32: unused */
    uint32_t       len;       /* used bytes (utf8-S); utf8 and int32 derive it */
    uint32_t       ival;
} ps3_psf_item;

/* Build a game-save PSF into out (cap bytes). Returns the size, or 0 if cap is too small. Text longer than its max
 * length is cut (utf8 keeps room for the NUL). dir is the savedata directory name. */
static size_t ps3_psf_build(uint8_t* out, size_t cap, const char* dir, const char* title, const char* subtitle,
                            const char* detail, uint32_t attribute, const char* list_param)
{
    /* ASSUMPTION: an unregistered account is 16 ASCII zeros (the wiki says "filled with zeroes"); PARAMS keeps only
     * its first byte (always 1 in the wiki's samples) and zeros for the counters, ids and reserved area. */
    static const uint8_t account[16] = {'0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0'};
    uint8_t params[1024]; memset(params, 0, sizeof params); params[0] = 1;
    uint8_t params2[12]; memset(params2, 0, sizeof params2);
    #define S(p) ((const uint8_t*)((p) ? (p) : ""))
    ps3_psf_item it[] = {   /* alphabetical, as in the real files */
        {"ACCOUNT_ID",          PS3_PSF_UTF8S, 16,   account,  16,   0},
        {"ATTRIBUTE",           PS3_PSF_INT32, 4,    NULL,     4,    attribute},
        {"CATEGORY",            PS3_PSF_UTF8,  4,    (const uint8_t*)"SD", 0, 0},
        {"DETAIL",              PS3_PSF_UTF8,  1024, S(detail),    0, 0},
        {"PARAMS",              PS3_PSF_UTF8S, 1024, params,   1024, 0},
        {"PARAMS2",             PS3_PSF_UTF8S, 12,   params2,  12,   0},
        {"PARENTAL_LEVEL",      PS3_PSF_INT32, 4,    NULL,     4,    0},
        {"SAVEDATA_DIRECTORY",  PS3_PSF_UTF8,  64,   S(dir),       0, 0},
        {"SAVEDATA_LIST_PARAM", PS3_PSF_UTF8,  8,    S(list_param), 0, 0},
        {"SUB_TITLE",           PS3_PSF_UTF8,  128,  S(subtitle),  0, 0},
        {"TITLE",               PS3_PSF_UTF8,  128,  S(title),     0, 0},
    };
    #undef S
    const uint32_t n = (uint32_t)(sizeof it / sizeof it[0]);
    uint32_t ktab = 0x14 + 16 * n, kbytes = 0, dbytes = 0;
    for (uint32_t i = 0; i < n; i++) kbytes += (uint32_t)strlen(it[i].key) + 1;
    uint32_t dtab = ktab + ((kbytes + 3) & ~3u);
    for (uint32_t i = 0; i < n; i++) dbytes += it[i].max_len;
    if (cap < (size_t)dtab + dbytes) return 0;
    memset(out, 0, (size_t)dtab + dbytes);
    out[1] = 'P'; out[2] = 'S'; out[3] = 'F'; out[4] = 1; out[5] = 1;
    ps3_psf_put32(out + 8, ktab); ps3_psf_put32(out + 12, dtab); ps3_psf_put32(out + 16, n);
    uint32_t ko = 0, dof = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* e = out + 0x14 + 16 * i;
        uint8_t* d = out + dtab + dof;
        uint32_t len = it[i].len;
        if (it[i].fmt == PS3_PSF_UTF8) {   /* text: cut to max_len-1, then the NUL counts in len */
            uint32_t l = (uint32_t)strlen((const char*)it[i].data);
            if (l > it[i].max_len - 1) l = it[i].max_len - 1;
            memcpy(d, it[i].data, l);
            len = l + 1;
        } else if (it[i].fmt == PS3_PSF_INT32) {
            ps3_psf_put32(d, it[i].ival);
        } else {
            memcpy(d, it[i].data, len);
        }
        ps3_psf_put16(e, ko); ps3_psf_put16(e + 2, it[i].fmt); ps3_psf_put32(e + 4, len);
        ps3_psf_put32(e + 8, it[i].max_len); ps3_psf_put32(e + 12, dof);
        memcpy(out + ktab + ko, it[i].key, strlen(it[i].key) + 1);
        ko += (uint32_t)strlen(it[i].key) + 1;
        dof += it[i].max_len;
    }
    return (size_t)dtab + dbytes;
}

/* Parse a PSF. Returns 1 if buf is a PSF (the fields found are filled, the others left untouched), 0 if it is not
 * one (the caller then tries the old text format). Text is cut to the buffer size and NUL-terminated. */
static int ps3_psf_parse(const uint8_t* buf, size_t n, char* title, size_t title_sz, char* subtitle, size_t sub_sz,
                         char* detail, size_t detail_sz, uint32_t* attribute, char* list_param, size_t lp_sz)
{
    if (n < 0x14 || buf[0] != 0 || buf[1] != 'P' || buf[2] != 'S' || buf[3] != 'F') return 0;
    uint32_t ktab = ps3_psf_get32(buf + 8), dtab = ps3_psf_get32(buf + 12), cnt = ps3_psf_get32(buf + 16);
    if (cnt > 256 || ktab > n || dtab > n || 0x14 + 16ull * cnt > n) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* e = buf + 0x14 + 16 * i;
        uint32_t ko = ps3_psf_get16(e), fmt = ps3_psf_get16(e + 2), len = ps3_psf_get32(e + 4), dof = ps3_psf_get32(e + 12);
        if (ktab + (uint64_t)ko >= n || dtab + (uint64_t)dof + len > n) continue;
        /* the key must end in a NUL inside the buffer (a cut or corrupt file may not) */
        if (!memchr(buf + ktab + ko, 0, n - (ktab + (size_t)ko))) continue;
        const char* key = (const char*)buf + ktab + ko;
        const uint8_t* d = buf + dtab + dof;
        char* dst = NULL; size_t dsz = 0;
        if      (!strcmp(key, "TITLE"))               { dst = title;      dsz = title_sz; }
        else if (!strcmp(key, "SUB_TITLE"))           { dst = subtitle;   dsz = sub_sz; }
        else if (!strcmp(key, "DETAIL"))              { dst = detail;     dsz = detail_sz; }
        else if (!strcmp(key, "SAVEDATA_LIST_PARAM")) { dst = list_param; dsz = lp_sz; }
        else if (!strcmp(key, "ATTRIBUTE") && fmt == PS3_PSF_INT32 && len == 4 && attribute) *attribute = ps3_psf_get32(d);
        if (dst && dsz && fmt == PS3_PSF_UTF8) {
            size_t l = 0; while (l < len && d[l]) l++;
            if (l > dsz - 1) l = dsz - 1;
            memcpy(dst, d, l); dst[l] = 0;
        }
    }
    return 1;
}
#endif
