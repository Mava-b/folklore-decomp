/*
 * ps3recomp - cellFontFT HLE implementation
 *
 * FreeType-based font rendering. Uses the same fallback metrics strategy as
 * cellFont — if no real font data is loaded, returns reasonable default metrics.
 * Glyph rendering produces empty bitmaps (no real rasterization without FreeType).
 */

#include "cellFontFT.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_read/vm_write: guest EA -> host */
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Guest struct access
 *
 * Same story as cellFont.c: these pointers are guest EAs, the fields are
 * big-endian, and CellFontFTGlyphImage leads with an 8-byte host pointer where
 * the guest has 4. Offsets are the guest ABI, spelled out so no host padding
 * can move them.
 * -----------------------------------------------------------------------*/
enum {
    FT_O_HANDLE = 0, FT_O_TYPE = 4, FT_O_SCALE = 8,
    FT_FONT_GUEST_SIZE = 28,          /* handle, type, scale, reserved[4] */
    FTIMG_O_BUFFER = 0, FTIMG_O_WIDTH = 4, FTIMG_O_HEIGHT = 8,
    FTIMG_O_PITCH  = 12, FTIMG_O_FORMAT = 16
};

#define FT_EA(p) ((u32)(uintptr_t)(p))

static u32 ft_handle(const CellFontFT* font)
{
    u32 ea = FT_EA(font);
    return ea ? vm_read32(ea + FT_O_HANDLE) : 0xFFFFFFFFu;
}

static void ft_font_store(CellFontFT* font, u32 handle, u32 type, float scale)
{
    u32 ea = FT_EA(font);
    if (!ea) return;
    for (u32 o = 0; o < FT_FONT_GUEST_SIZE; o += 4)
        vm_write32(ea + o, 0);
    vm_write32(ea + FT_O_HANDLE, handle);
    vm_write32(ea + FT_O_TYPE, type);
    vm_write_f32(ea + FT_O_SCALE, scale);
}

static void ft_font_clear(CellFontFT* font)
{
    u32 ea = FT_EA(font);
    if (!ea) return;
    for (u32 o = 0; o < FT_FONT_GUEST_SIZE; o += 4)
        vm_write32(ea + o, 0);
}

/* Internal state */

#define MAX_FT_FONTS 16

typedef struct {
    int in_use;
    float size;
    /* We don't have actual FreeType, so we store enough to return metrics */
    int from_file;
    char path[256];
} FTFontSlot;

static int s_initialized = 0;
static FTFontSlot s_fonts[MAX_FT_FONTS];

/* API */

s32 cellFontFTInit(const CellFontFTConfig* config, CellFontFTLibrary* lib)
{
    printf("[cellFontFT] Init()\n");
    (void)config;

    if (s_initialized)
        return (s32)CELL_FONTFT_ERROR_ALREADY_INITIALIZED;

    memset(s_fonts, 0, sizeof(s_fonts));
    s_initialized = 1;

    if (lib) vm_write32(FT_EA(lib), 1);
    return CELL_OK;
}

s32 cellFontFTEnd(CellFontFTLibrary lib)
{
    printf("[cellFontFT] End()\n");
    (void)lib;

    memset(s_fonts, 0, sizeof(s_fonts));
    s_initialized = 0;
    return CELL_OK;
}

static int ft_alloc_slot(void)
{
    for (int i = 0; i < MAX_FT_FONTS; i++) {
        if (!s_fonts[i].in_use) return i;
    }
    return -1;
}



s32 cellFontFTOpenFontFile(CellFontFTLibrary lib, const char* path,
                           u32 index, CellFontFT* font)
{
    (void)lib; (void)index;
    path = GUEST_PTR(path, const char*);
    printf("[cellFontFT] OpenFontFile(%s)\n", path ? path : "(null)");

    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;
    if (!path || !font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    int slot = ft_alloc_slot();
    if (slot < 0) return (s32)CELL_FONTFT_ERROR_OUT_OF_MEMORY;

    s_fonts[slot].in_use = 1;
    s_fonts[slot].size = 16.0f; /* default size */
    s_fonts[slot].from_file = 1;
    strncpy(s_fonts[slot].path, path, sizeof(s_fonts[slot].path) - 1);
    s_fonts[slot].path[sizeof(s_fonts[slot].path) - 1] = '\0';

    ft_font_store(font, (u32)slot, 1, 1.0f);

    return CELL_OK;
}

s32 cellFontFTOpenFontMemory(CellFontFTLibrary lib, const void* data,
                             u32 dataSize, u32 index, CellFontFT* font)
{
    (void)lib; (void)data; (void)dataSize; (void)index;
    printf("[cellFontFT] OpenFontMemory(size=%u)\n", dataSize);

    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;
    if (!data || !font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    int slot = ft_alloc_slot();
    if (slot < 0) return (s32)CELL_FONTFT_ERROR_OUT_OF_MEMORY;

    s_fonts[slot].in_use = 1;
    s_fonts[slot].size = 16.0f;
    s_fonts[slot].from_file = 0;

    ft_font_store(font, (u32)slot, 2, 1.0f);

    return CELL_OK;
}

s32 cellFontFTCloseFont(CellFontFT* font)
{
    if (!font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = ft_handle(font);
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    s_fonts[h].in_use = 0;
    ft_font_clear(font);
    return CELL_OK;
}

s32 cellFontFTSetFontSize(CellFontFT* font, float size)
{
    if (!font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = ft_handle(font);
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    s_fonts[h].size = size;
    vm_write_f32(FT_EA(font) + FT_O_SCALE, size / 16.0f);
    return CELL_OK;
}

s32 cellFontFTGetFontMetrics(const CellFontFT* font, CellFontFTFontMetrics* metrics)
{
    if (!font || !metrics) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = ft_handle(font);
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    float size = s_fonts[h].size;

    /* Fallback metrics based on typical proportions */
    vm_write_f32(FT_EA(metrics) +  0, size * 0.8f);   /* ascender   */
    vm_write_f32(FT_EA(metrics) +  4, size * -0.2f);  /* descender  */
    vm_write_f32(FT_EA(metrics) +  8, size * 1.2f);   /* lineHeight */
    vm_write_f32(FT_EA(metrics) + 12, size * 0.6f);   /* maxAdvance */

    return CELL_OK;
}

s32 cellFontFTGetGlyphMetrics(const CellFontFT* font, u32 charCode,
                              CellFontFTGlyphMetrics* metrics)
{
    if (!font || !metrics) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = ft_handle(font);
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    (void)charCode;
    float size = s_fonts[h].size;

    /* Fallback glyph metrics */
    vm_write_f32(FT_EA(metrics) +  0, size * 0.5f);    /* width     */
    vm_write_f32(FT_EA(metrics) +  4, size * 0.8f);    /* height    */
    vm_write_f32(FT_EA(metrics) +  8, 0.0f);           /* hBearingX */
    vm_write_f32(FT_EA(metrics) + 12, size * 0.8f);    /* hBearingY */
    vm_write_f32(FT_EA(metrics) + 16, size * 0.6f);    /* hAdvance  */
    vm_write_f32(FT_EA(metrics) + 20, size * -0.25f);  /* vBearingX */
    vm_write_f32(FT_EA(metrics) + 24, size * 0.1f);    /* vBearingY */
    vm_write_f32(FT_EA(metrics) + 28, size * 1.2f);    /* vAdvance  */

    return CELL_OK;
}

s32 cellFontFTRenderGlyph(const CellFontFT* font, u32 charCode,
                          CellFontFTGlyphImage* image)
{
    if (!font || !image) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = ft_handle(font);
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    (void)charCode;
    float size = s_fonts[h].size;

    /* Produce an empty glyph bitmap (no real rasterization without FreeType) */
    u32 w = (u32)(size * 0.5f);
    u32 hh = (u32)(size * 0.8f);
    if (w < 1) w = 1;
    if (hh < 1) hh = 1;

    u32 img_ea = FT_EA(image);
    vm_write32(img_ea + FTIMG_O_WIDTH,  w);
    vm_write32(img_ea + FTIMG_O_HEIGHT, hh);
    vm_write32(img_ea + FTIMG_O_PITCH,  w);
    vm_write32(img_ea + FTIMG_O_FORMAT, 0); /* 8-bit alpha */

    /* If caller provided a buffer, zero it */
    u32 buf_ea = vm_read32(img_ea + FTIMG_O_BUFFER);
    if (buf_ea) {
        memset(vm_base + buf_ea, 0, (size_t)(w * hh));
    }

    return CELL_OK;
}

s32 cellFontFTGetCharGlyphCode(const CellFontFT* font, u32 charCode, u32* glyphCode)
{
    (void)font;
    if (!glyphCode) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    /* Direct mapping: char code = glyph code (no cmap lookup without FreeType) */
    vm_write32(FT_EA(glyphCode), charCode);
    return CELL_OK;
}

s32 cellFontFTGetKerning(const CellFontFT* font, u32 leftChar, u32 rightChar,
                         float* kernX, float* kernY)
{
    (void)font; (void)leftChar; (void)rightChar;
    /* No kerning data without FreeType */
    if (kernX) vm_write_f32(FT_EA(kernX), 0.0f);
    if (kernY) vm_write_f32(FT_EA(kernY), 0.0f);
    return CELL_OK;
}
