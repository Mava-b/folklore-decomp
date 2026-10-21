/*
 * ps3recomp - cellFont HLE implementation
 *
 * Font system with optional stb_truetype.h backend for actual TTF rendering.
 * Without stb_truetype, returns valid (but empty) glyph data so games
 * don't crash.
 */

#include "cellFont.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_read/vm_write: guest EA -> host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Try to include stb_truetype */
#if __has_include("stb_truetype.h")
  #define FONT_HAS_STB 1
  #ifndef STB_TRUETYPE_IMPLEMENTATION
    #define STB_TRUETYPE_IMPLEMENTATION
  #endif
  #include "stb_truetype.h"
#elif defined(PS3RECOMP_HAS_STB_TRUETYPE)
  #define FONT_HAS_STB 1
  #include "stb_truetype.h"
#else
  #define FONT_HAS_STB 0
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    int     in_use;
    u32     type;
    float   scale_x;
    float   scale_y;
    float   effect_weight;
    float   effect_slant;
    u32     renderer;       /* bound renderer index, or 0xFFFFFFFF */
    /* Font data */
    u8*     font_data;      /* TTF file data (owned if loaded from file) */
    u32     font_data_size;
    int     font_data_owned;
#if FONT_HAS_STB
    stbtt_fontinfo stb_info;
    int     stb_valid;
#endif
} FontSlot;

typedef struct {
    int in_use;
} RendererSlot;

static int          s_font_initialized = 0;
static u32          s_open_mode = CELL_FONT_OPEN_MODE_DEFAULT;
static FontSlot     s_fonts[CELL_FONT_MAX_OPENED];
static RendererSlot s_renderers[CELL_FONT_MAX_RENDERERS];

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static FontSlot* font_alloc_slot(void)
{
    for (int i = 0; i < CELL_FONT_MAX_OPENED; i++) {
        if (!s_fonts[i].in_use) {
            memset(&s_fonts[i], 0, sizeof(FontSlot));
            s_fonts[i].in_use = 1;
            s_fonts[i].scale_x = 1.0f;
            s_fonts[i].scale_y = 1.0f;
            s_fonts[i].renderer = 0xFFFFFFFF;
            return &s_fonts[i];
        }
    }
    return NULL;
}


/* ---------------------------------------------------------------------------
 * Guest struct marshalling
 *
 * The structs in cellFont.h are HOST layout and are not what the title has in
 * memory: CellFontRenderSurface and CellFontGlyphImage lead with an 8-byte host
 * pointer where the guest has 4, and every u32/float field is big-endian on the
 * guest side. So a pointer translation alone is not enough -- casting a guest EA
 * to CellFontGlyphMetrics* and storing floats through it writes little-endian
 * values at the wrong offsets, which is how a title reads a glyph advance of
 * 4.6e-41 and lays every string out on top of itself.
 *
 * The render/metrics bodies keep working on host structs; these move them in and
 * out. Offsets are the guest ABI, listed explicitly so they cannot drift with a
 * host compiler's padding.
 * -----------------------------------------------------------------------*/
enum {
    FONT_O_HANDLE = 0,  FONT_O_TYPE = 4,  FONT_O_SCALEX = 8, FONT_O_SCALEY = 12,
    SURF_O_BUFFER = 0,  SURF_O_WIDTHBYTE = 4, SURF_O_PIXELSIZE = 8,
    SURF_O_WIDTH  = 12, SURF_O_HEIGHT = 16,
    IMG_O_BUFFER  = 0,  IMG_O_WIDTHBYTE = 4,  IMG_O_PIXELSIZE = 8,
    IMG_O_WIDTH   = 12, IMG_O_HEIGHT = 16,
    LAY_O_BASELINEY = 0, LAY_O_LINEHEIGHT = 4, LAY_O_EFFECTHEIGHT = 8
};

#define FONT_EA(p) ((u32)(uintptr_t)(p))

/* CellFontGlyphMetrics is eight consecutive floats in guest order. */
static void font_metrics_store(CellFontGlyphMetrics* guest_ea,
                               const CellFontGlyphMetrics* h)
{
    u32 ea = FONT_EA(guest_ea);
    if (!ea) return;
    vm_write_f32(ea +  0, h->width);
    vm_write_f32(ea +  4, h->height);
    vm_write_f32(ea +  8, h->h_bearing_x);
    vm_write_f32(ea + 12, h->h_bearing_y);
    vm_write_f32(ea + 16, h->h_advance);
    vm_write_f32(ea + 20, h->v_bearing_x);
    vm_write_f32(ea + 24, h->v_bearing_y);
    vm_write_f32(ea + 28, h->v_advance);
}

static void font_surface_load(CellFontRenderSurface* h,
                              const CellFontRenderSurface* guest_ea)
{
    u32 ea = FONT_EA(guest_ea);
    memset(h, 0, sizeof(*h));
    if (!ea) return;
    u32 buf = vm_read32(ea + SURF_O_BUFFER);
    h->buffer        = buf ? (u8*)(vm_base + buf) : NULL;
    h->widthByte     = (s32)vm_read32(ea + SURF_O_WIDTHBYTE);
    h->pixelSizeByte = (s32)vm_read32(ea + SURF_O_PIXELSIZE);
    h->width         = (s32)vm_read32(ea + SURF_O_WIDTH);
    h->height        = (s32)vm_read32(ea + SURF_O_HEIGHT);
}

/* buffer is left as the title set it -- we render into the surface, never
 * hand back a buffer of our own. */
static void font_image_store(CellFontGlyphImage* guest_ea,
                             s32 widthByte, s32 pixelSizeByte, s32 w, s32 hgt)
{
    u32 ea = FONT_EA(guest_ea);
    if (!ea) return;
    vm_write32(ea + IMG_O_BUFFER,    0);
    vm_write32(ea + IMG_O_WIDTHBYTE, (u32)widthByte);
    vm_write32(ea + IMG_O_PIXELSIZE, (u32)pixelSizeByte);
    vm_write32(ea + IMG_O_WIDTH,     (u32)w);
    vm_write32(ea + IMG_O_HEIGHT,    (u32)hgt);
}

static void font_store(CellFont* guest_ea, u32 handle, u32 type,
                       float sx, float sy)
{
    u32 ea = FONT_EA(guest_ea);
    if (!ea) return;
    vm_write32(ea + FONT_O_HANDLE, handle);
    vm_write32(ea + FONT_O_TYPE,   type);
    vm_write_f32(ea + FONT_O_SCALEX, sx);
    vm_write_f32(ea + FONT_O_SCALEY, sy);
}

static FontSlot* font_get_slot(CellFont* font)
{
    /* font is a guest EA; the handle is the big-endian word at +0. */
    u32 ea = FONT_EA(font);
    if (!ea)
        return NULL;
    u32 handle = vm_read32(ea + FONT_O_HANDLE);
    if (handle >= CELL_FONT_MAX_OPENED)
        return NULL;
    FontSlot* slot = &s_fonts[handle];
    if (!slot->in_use)
        return NULL;
    return slot;
}

#if FONT_HAS_STB
static float font_get_stb_scale(FontSlot* slot)
{
    if (!slot->stb_valid) return 0.0f;
    return stbtt_ScaleForPixelHeight(&slot->stb_info, slot->scale_y);
}
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellFontInit(const CellFontConfig* config)
{
    (void)config;
    printf("[cellFont] Init()\n");

    if (s_font_initialized)
        return CELL_FONT_ERROR_ALREADY_INITIALIZED;

    memset(s_fonts, 0, sizeof(s_fonts));
    memset(s_renderers, 0, sizeof(s_renderers));
    s_font_initialized = 1;
    return CELL_OK;
}

/* cellFontInitializeWithRevision(revisionFlags, config): the entry point titles built with a newer SDK use instead of
 * cellFontInit (NID 0xF03DCC29). Without it every later cellFont call returns CELL_FONT_ERROR_UNINITIALIZED, which
 * a title that ignores the result never notices. */
s32 cellFontInitializeWithRevision(u64 revisionFlags, const CellFontConfig* config)
{
    (void)revisionFlags;
    return cellFontInit(config);
}

s32 cellFontEnd(void)
{
    printf("[cellFont] End()\n");

    if (!s_font_initialized)
        return CELL_FONT_ERROR_UNINITIALIZED;

    for (int i = 0; i < CELL_FONT_MAX_OPENED; i++) {
        if (s_fonts[i].in_use && s_fonts[i].font_data_owned && s_fonts[i].font_data)
            free(s_fonts[i].font_data);
        s_fonts[i].in_use = 0;
    }

    s_font_initialized = 0;
    return CELL_OK;
}

s32 cellFontOpenFontFile(CellFont* font, const char* fontPath, u32 subNum, s32 uniqueId)
{
    (void)subNum;
    (void)uniqueId;
    fontPath = GUEST_PTR(fontPath, const char*);
    printf("[cellFont] OpenFontFile(%s)\n", fontPath ? fontPath : "(null)");

    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!font || !fontPath)  return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_alloc_slot();
    if (!slot) return CELL_FONT_ERROR_ALLOCATION_FAILED;

    /* Load TTF file */
    FILE* f = fopen(fontPath, "rb");
    if (!f) {
        slot->in_use = 0;
        printf("[cellFont] Cannot open font file: %s\n", fontPath);
        return CELL_FONT_ERROR_OPEN_FAILED;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    slot->font_data = (u8*)malloc((size_t)size);
    if (!slot->font_data) {
        fclose(f);
        slot->in_use = 0;
        return CELL_FONT_ERROR_ALLOCATION_FAILED;
    }

    slot->font_data_size = (u32)fread(slot->font_data, 1, (size_t)size, f);
    slot->font_data_owned = 1;
    fclose(f);

#if FONT_HAS_STB
    if (stbtt_InitFont(&slot->stb_info, slot->font_data,
                       stbtt_GetFontOffsetForIndex(slot->font_data, 0))) {
        slot->stb_valid = 1;
    } else {
        printf("[cellFont] stbtt_InitFont failed for: %s\n", fontPath);
        slot->stb_valid = 0;
    }
#endif

    font_store(font, (u32)(slot - s_fonts), 0, 1.0f, 1.0f);

    return CELL_OK;
}

s32 cellFontOpenFontMemory(CellFont* font, const void* data, u32 dataSize, u32 subNum, s32 uniqueId)
{
    (void)subNum;
    (void)uniqueId;
    printf("[cellFont] OpenFontMemory(size=%u)\n", dataSize);

    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!font || !data || dataSize == 0) return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_alloc_slot();
    if (!slot) return CELL_FONT_ERROR_ALLOCATION_FAILED;

    slot->font_data = GUEST_PTR(data, u8*); /* borrowed guest buffer */
    slot->font_data_size = dataSize;
    slot->font_data_owned = 0;

#if FONT_HAS_STB
    if (stbtt_InitFont(&slot->stb_info, slot->font_data,
                       stbtt_GetFontOffsetForIndex(slot->font_data, 0))) {
        slot->stb_valid = 1;
    } else {
        slot->stb_valid = 0;
    }
#endif

    font_store(font, (u32)(slot - s_fonts), 0, 1.0f, 1.0f);

    return CELL_OK;
}

s32 cellFontOpenFontset(CellFontLibrary lib, CellFontType* fontType, CellFont* font)
{
    (void)lib;
    u32 want_type = fontType ? vm_read32(FONT_EA(fontType)) : 0;
    printf("[cellFont] OpenFontset(type=0x%X)\n", want_type);

    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!font) return CELL_FONT_ERROR_INVALID_PARAMETER;

    /* System fonts are not available in recomp. Allocate a slot anyway
     * so the game can proceed with empty glyphs. */
    FontSlot* slot = font_alloc_slot();
    if (!slot) return CELL_FONT_ERROR_ALLOCATION_FAILED;

    slot->type = want_type;

    font_store(font, (u32)(slot - s_fonts), slot->type, 1.0f, 1.0f);

    return CELL_OK;
}

s32 cellFontCloseFont(CellFont* font)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    if (slot->font_data_owned && slot->font_data)
        free(slot->font_data);

    slot->in_use = 0;
    return CELL_OK;
}

s32 cellFontSetFontOpenMode(u32 openMode)
{
    s_open_mode = openMode;
    return CELL_OK;
}

s32 cellFontCreateRenderer(CellFontLibrary lib, CellFontRenderer* renderer)
{
    (void)lib;
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!renderer) return CELL_FONT_ERROR_INVALID_PARAMETER;

    for (u32 i = 0; i < CELL_FONT_MAX_RENDERERS; i++) {
        if (!s_renderers[i].in_use) {
            s_renderers[i].in_use = 1;
            vm_write32(FONT_EA(renderer), i);
            return CELL_OK;
        }
    }

    return CELL_FONT_ERROR_ALLOCATION_FAILED;
}

s32 cellFontDestroyRenderer(CellFontRenderer renderer)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (renderer >= CELL_FONT_MAX_RENDERERS) return CELL_FONT_ERROR_INVALID_PARAMETER;

    s_renderers[renderer].in_use = 0;
    return CELL_OK;
}

s32 cellFontBindRenderer(CellFont* font, CellFontRenderer renderer)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    if (renderer >= CELL_FONT_MAX_RENDERERS || !s_renderers[renderer].in_use)
        return CELL_FONT_ERROR_INVALID_PARAMETER;

    if (slot->renderer != 0xFFFFFFFF)
        return CELL_FONT_ERROR_RENDERER_ALREADY_BIND;

    slot->renderer = renderer;
    return CELL_OK;
}

s32 cellFontUnbindRenderer(CellFont* font)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    slot->renderer = 0xFFFFFFFF;
    return CELL_OK;
}

s32 cellFontSetScalePixel(CellFont* font, float w, float h)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    slot->scale_x = w;
    slot->scale_y = h;
    vm_write_f32(FONT_EA(font) + FONT_O_SCALEX, w);
    vm_write_f32(FONT_EA(font) + FONT_O_SCALEY, h);

    return CELL_OK;
}

s32 cellFontSetEffectWeight(CellFont* font, float weight)
{
    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    slot->effect_weight = weight;
    return CELL_OK;
}

s32 cellFontSetEffectSlant(CellFont* font, float slant)
{
    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    slot->effect_slant = slant;
    return CELL_OK;
}

s32 cellFontGetHorizontalLayout(CellFont* font, CellFontHorizontalLayout* layout)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!layout) return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

#if FONT_HAS_STB
    if (slot->stb_valid) {
        float scale = font_get_stb_scale(slot);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&slot->stb_info, &ascent, &descent, &lineGap);
        vm_write_f32(FONT_EA(layout) + LAY_O_BASELINEY, (float)ascent * scale);
        vm_write_f32(FONT_EA(layout) + LAY_O_LINEHEIGHT,
                     (float)(ascent - descent + lineGap) * scale);
        vm_write_f32(FONT_EA(layout) + LAY_O_EFFECTHEIGHT,
                     (float)(ascent - descent) * scale);
        return CELL_OK;
    }
#endif

    /* Fallback: reasonable defaults based on scale */
    vm_write_f32(FONT_EA(layout) + LAY_O_BASELINEY,    slot->scale_y * 0.8f);
    vm_write_f32(FONT_EA(layout) + LAY_O_LINEHEIGHT,   slot->scale_y * 1.2f);
    vm_write_f32(FONT_EA(layout) + LAY_O_EFFECTHEIGHT, slot->scale_y);
    return CELL_OK;
}

/* Fills a HOST metrics struct. cellFontGetRenderCharGlyphMetrics is the
 * marshalling wrapper; cellFontRenderCharGlyphImage wants the host copy
 * anyway and would otherwise write the guest struct then read it back. */
static s32 font_metrics_host(CellFont* font, u32 code,
                             CellFontGlyphMetrics* metrics)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    memset(metrics, 0, sizeof(CellFontGlyphMetrics));

#if FONT_HAS_STB
    if (slot->stb_valid) {
        float scale = font_get_stb_scale(slot);
        int glyph = stbtt_FindGlyphIndex(&slot->stb_info, (int)code);
        if (glyph == 0) {
            /* Glyph not found -- return space-like metrics */
            metrics->h_advance = slot->scale_x * 0.5f;
            return CELL_OK;
        }

        int advanceWidth, leftSideBearing;
        stbtt_GetGlyphHMetrics(&slot->stb_info, glyph, &advanceWidth, &leftSideBearing);

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&slot->stb_info, glyph, scale, scale, &x0, &y0, &x1, &y1);

        metrics->width      = (float)(x1 - x0);
        metrics->height     = (float)(y1 - y0);
        metrics->h_bearing_x = (float)leftSideBearing * scale;
        metrics->h_bearing_y = (float)(-y0);
        metrics->h_advance  = (float)advanceWidth * scale;
        metrics->v_advance  = metrics->height;

        return CELL_OK;
    }
#endif

    /* Fallback: monospace-like metrics */
    metrics->width       = slot->scale_x * 0.6f;
    metrics->height      = slot->scale_y * 0.8f;
    metrics->h_bearing_x = 0.0f;
    metrics->h_bearing_y = slot->scale_y * 0.7f;
    metrics->h_advance   = slot->scale_x * 0.6f;
    metrics->v_advance   = slot->scale_y;

    return CELL_OK;
}

s32 cellFontGetRenderCharGlyphMetrics(CellFont* font, u32 code,
                                       CellFontGlyphMetrics* metrics)
{
    if (!metrics) return CELL_FONT_ERROR_INVALID_PARAMETER;
    CellFontGlyphMetrics h;
    s32 rc = font_metrics_host(font, code, &h);
    if (rc == (s32)CELL_OK) font_metrics_store(metrics, &h);
    return rc;
}

s32 cellFontGetRenderCharGlyphMetricsVertical(CellFont* font, u32 code,
                                               CellFontGlyphMetrics* metrics)
{
    /* Vertical metrics: use horizontal metrics as base, swap axes */
    if (!metrics) return CELL_FONT_ERROR_INVALID_PARAMETER;
    CellFontGlyphMetrics h;
    s32 rc = font_metrics_host(font, code, &h);
    if (rc != (s32)CELL_OK) return rc;

    /* Swap horizontal/vertical bearings for vertical layout */
    float tmp;
    tmp = h.h_bearing_x; h.h_bearing_x = h.v_bearing_x; h.v_bearing_x = tmp;
    tmp = h.h_bearing_y; h.h_bearing_y = h.v_bearing_y; h.v_bearing_y = tmp;
    tmp = h.h_advance;   h.h_advance   = h.v_advance;   h.v_advance   = tmp;

    font_metrics_store(metrics, &h);
    return CELL_OK;
}

s32 cellFontRenderCharGlyphImage(CellFont* font, u32 code,
                                  CellFontRenderSurface* surface,
                                  float x, float y,
                                  CellFontGlyphMetrics* metrics,
                                  CellFontGlyphImage* image)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    /* Work from a host copy; hand the title its own if it asked. */
    CellFontGlyphMetrics hm;
    font_metrics_host(font, code, &hm);
    if (metrics) font_metrics_store(metrics, &hm);

    CellFontRenderSurface hs;
    font_surface_load(&hs, surface);

#if FONT_HAS_STB
    if (slot->stb_valid && hs.buffer) {
        float scale = font_get_stb_scale(slot);
        int glyph = stbtt_FindGlyphIndex(&slot->stb_info, (int)code);
        if (glyph == 0) goto done;

        int bw, bh, bx, by;
        u8* bitmap = stbtt_GetGlyphBitmap(&slot->stb_info, scale, scale,
                                           glyph, &bw, &bh, &bx, &by);
        if (!bitmap) goto done;

        /* Blit glyph bitmap to surface */
        int dx = (int)(x + hm.h_bearing_x);
        int dy = (int)(y - hm.h_bearing_y);

        for (int row = 0; row < bh; row++) {
            int sy = dy + row;
            if (sy < 0 || sy >= hs.height) continue;
            for (int col = 0; col < bw; col++) {
                int sx = dx + col;
                if (sx < 0 || sx >= hs.width) continue;
                u8 alpha = bitmap[row * bw + col];
                if (alpha > 0) {
                    int offset = sy * hs.widthByte + sx * hs.pixelSizeByte;
                    /* Alpha-blend (simple overwrite for alpha-only surface) */
                    hs.buffer[offset] = alpha;
                }
            }
        }

        stbtt_FreeBitmap(bitmap, NULL);

        /* Fill image output if requested */
        if (image)
            font_image_store(image, bw, 1, bw, bh);

        return CELL_OK;
    }
#endif

done:
    /* Fallback: empty glyph */
    if (image) {
        font_image_store(image, (s32)hm.width, 1,
                         (s32)hm.width, (s32)hm.height);
    }

    return CELL_OK;
}

s32 cellFontSetupRenderScalePixel(CellFont* font, float w, float h)
{
    return cellFontSetScalePixel(font, w, h);
}
