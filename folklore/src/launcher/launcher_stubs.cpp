/*
 * launcher_stubs.cpp -- what the launcher's share of the engine code
 * (engine_settings.cpp, engine_portable.cpp, engine_dlc.cpp, engine_self.cpp)
 * expects from the game: the renderer and pad calls the settings hand their
 * values to. The launcher only edits folklore_settings.ini; folklore.exe
 * applies it when it starts, so these do nothing.
 */
#include <cstdint>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

extern "C"
{
    void rsx_live_draw_set_render_scale(uint32_t scale) { (void)scale; }
    void rsx_live_draw_set_post_aa(int mode) { (void)mode; }
    void rsx_live_draw_set_async_shaders(int enabled) { (void)enabled; }
    void rsx_live_draw_set_repeat_draws(int enabled) { (void)enabled; }
    void rsx_live_draw_set_anisotropy(uint32_t level) { (void)level; }
    void rsx_live_draw_set_sharpening(uint32_t percent) { (void)percent; }
    void rsx_live_draw_set_texture_lod_bias(int hundredths) { (void)hundredths; }
    void rsx_live_draw_set_texture_pack(const char* folder) { (void)folder; }
    void rsx_live_draw_set_hud_layout(int layout) { (void)layout; }
    void rsx_live_draw_set_hud_edge(int percent) { (void)percent; }
    void rsx_live_draw_set_shadow_softness(int softness) { (void)softness; }
    void rsx_live_draw_set_aspect(float aspect) { (void)aspect; }
    float rsx_live_draw_get_aspect(void) { return 16.0f / 9.0f; }
    void rsx_null_backend_set_fullscreen(int fullscreen) { (void)fullscreen; }
    void cellGcm_set_flip_per_present(int enabled) { (void)enabled; }
    int cellPad_set_motion_mode(int mode) { (void)mode; return 0; }
    void folklore_apply_aspect(float aspect) { (void)aspect; }
}

void engineDepthOfFieldApply()
{
}

void engineBloomApply()
{
}

void engineIdSoulsApply()
{
}
