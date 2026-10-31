/*
 * engine_dof.cpp -- a smoother depth of field for Folklore (engine setting
 * depth_of_field; patches/0059 adds the renderer's shader replacement hook).
 *
 * Folklore draws its depth of field in the pass that resolves the 2560x720
 * scene to the screen, one draw a frame. The pass reads the depth buffer,
 * works out a blur factor from it with the game's focus constants, and takes
 * two taps of the scene pushed apart by that factor. In focus the two taps
 * are the 2x resolve; out of focus they become two offset copies of the
 * picture -- the doubled, harsh look. Two versions of the pass turn up: one
 * that blurs only what is behind the focus (most of the game), and one that
 * blurs both sides of it. (A third, four-tap version seen once is left as it
 * is.)
 *
 * The replacements keep each version's factor, arithmetic and constants (so
 * focus distance and strength are the game's), return the original two taps
 * wherever the image is in focus, and out of focus take a round 24-tap disc
 * as wide as the two copies were apart. Each tap's own depth decides whether
 * it may contribute, so a sharp object does not bleed into the blur around
 * it. The hashes are the renderer's normalised 64-bit FNV-1a of each shader
 * as it translates it (rsx_live_draw_set_ps_replacement: the LD_HLSL_DUMP
 * text from "cbuffer PSConstants" on, rsx_tex[N] read as rsx_texN). The
 * first version hashed the whole text, which changes when the scene has a
 * cube map bound on another unit -- and then the pass was not replaced.
 */
#include "engine_settings.h"

#include <cstdint>
#include <string>

extern "C" void rsx_live_draw_set_ps_replacement(uint64_t sourceHash, const char* hlsl);

namespace
{
    /* Behind the focus only: pso 4fb2ef46cbd180a0 / 76bf6e099e2e0a4f. */
    constexpr uint64_t kFarOnlyHash = 0x7E1621B76F8AF72Eull;
    /* Both sides of the focus: pso 98dbb71efb61695f. */
    constexpr uint64_t kTwoSidedHash = 0x1F15BEB3C708B830ull;

    const char* const kCommon = R"HLSL(
float4 _rsx_zeta_argb(Texture2D depthTexture, SamplerState depthSampler, float2 coordinate) {
    float depth = saturate(depthTexture.SampleLevel(depthSampler, coordinate, 0).x);
    uint depthBits = (uint)round(depth * 16777215.0);
    return float4((float)((depthBits >> 8) & 255u), (float)(depthBits & 255u), 0.0,
                  (float)((depthBits >> 16) & 255u)) / 255.0;
}
struct PSInput {
    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;
    float4 fog : FOG;
    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;
    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;
};
Texture2D    rsx_tex[16] : register(t0);
SamplerState rsx_samp[16] : register(s0);
)HLSL";

    /* The game's blur factor at one point, its own arithmetic in order; the
     * in-focus value it is clamped to; and the pair's offset per unit. */
    const char* const kFarOnly = R"HLSL(
cbuffer PSConstants : register(b1) {
    float4 fp_constants[10];
    float4 fp_alpha;
};
float folkloreBlurFactor(float2 coordinate) {
    float4 depthBytes = _rsx_zeta_argb(rsx_tex[1], rsx_samp[1], coordinate);
    float factor = dot(depthBytes.wxy, fp_constants[0].xyz);
    float range = fp_constants[1].z - fp_constants[1].y;
    factor = factor - fp_constants[2].x;
    factor = factor - fp_constants[3].y;
    factor = max(factor, fp_constants[4].x);
    factor = factor / range;
    factor = min(factor, fp_constants[5].x);
    factor = factor * fp_constants[6].w;
    return max(factor, fp_constants[7].x);
}
float folkloreSharpFactor() { return fp_constants[7].x; }
float2 folkloreAxisScale() { return float2(fp_constants[8].x, fp_constants[9].y); }
)HLSL";

    const char* const kTwoSided = R"HLSL(
cbuffer PSConstants : register(b1) {
    float4 fp_constants[11];
    float4 fp_alpha;
};
float folkloreBlurFactor(float2 coordinate) {
    float4 depthBytes = _rsx_zeta_argb(rsx_tex[1], rsx_samp[1], coordinate);
    float factor = dot(depthBytes.ywx, fp_constants[0].yzw);
    factor = factor * fp_constants[2].x - fp_constants[1].x;
    float range = fp_constants[3].z - fp_constants[3].y;
    factor = abs(factor) - fp_constants[4].y;
    factor = max(factor, fp_constants[5].x);
    factor = factor / range;
    factor = min(factor, fp_constants[6].x);
    factor = factor * fp_constants[7].w;
    return max(factor, fp_constants[8].x);
}
float folkloreSharpFactor() { return fp_constants[8].x; }
float2 folkloreAxisScale() { return float2(fp_constants[9].x, fp_constants[10].y); }
)HLSL";

    const char* const kBody = R"HLSL(
float4 main(PSInput input) : SV_TARGET {
    const float2 coordinate = input.tc0.xy;
    const float sharpFactor = folkloreSharpFactor();
    const float factor = folkloreBlurFactor(coordinate);
    // The game's two taps: exactly the original image where it is sharp.
    const float2 pairOffset = factor * folkloreAxisScale();
    float3 original = 0.5 * (rsx_tex[0].Sample(rsx_samp[0], coordinate - pairOffset).xyz +
                             rsx_tex[0].Sample(rsx_samp[0], coordinate + pairOffset).xyz);
    const float alpha = rsx_tex[0].Sample(rsx_samp[0], coordinate).w;
    const float blurAmount = factor - sharpFactor;
    if (!(blurAmount > 0.0)) {
        return float4(original, alpha);
    }
    // A round disc on screen (the scene is twice as wide as the screen),
    // reaching as far as each of the game's two copies was pushed.
    uint sceneWidth, sceneHeight;
    rsx_tex[0].GetDimensions(sceneWidth, sceneHeight);
    const float2 screenPixels = float2(0.5 * (float)max(sceneWidth, 1u), (float)max(sceneHeight, 1u));
    const float radiusPixels = length(pairOffset * screenPixels);
    const float2 discRadius = radiusPixels / screenPixels;
    const int tapCount = 24;
    const float goldenAngle = 2.39996323;
    float3 colourSum = original * 2.0;
    float weightSum = 2.0;
    [unroll] for (int tapIndex = 0; tapIndex < tapCount; tapIndex++) {
        const float distanceFraction = sqrt(((float)tapIndex + 0.5) / (float)tapCount);
        const float angle = (float)tapIndex * goldenAngle;
        const float2 tapCoordinate = coordinate + discRadius * distanceFraction * float2(cos(angle), sin(angle));
        // A tap counts if its own blur reaches back to this pixel: sharp
        // neighbours stay out of the blur, blurred ones spread as they should.
        const float tapBlur = folkloreBlurFactor(tapCoordinate) - sharpFactor;
        const float reach = max(tapBlur, 0.0) / blurAmount;
        const float weight = saturate((reach - distanceFraction) * 4.0 + 1.0);
        colourSum += rsx_tex[0].SampleLevel(rsx_samp[0], tapCoordinate, 0).xyz * weight;
        weightSum += weight;
    }
    return float4(colourSum / weightSum, alpha);
}
)HLSL";

    const std::string& farOnlyShader()
    {
        static const std::string text = std::string(kCommon) + kFarOnly + kBody;
        return text;
    }

    const std::string& twoSidedShader()
    {
        static const std::string text = std::string(kCommon) + kTwoSided + kBody;
        return text;
    }
}

void engineDepthOfFieldApply()
{
    const bool smooth = engineSettings().depthOfField == EngineSettings::DepthOfField::Smooth;
    rsx_live_draw_set_ps_replacement(kFarOnlyHash, smooth ? farOnlyShader().c_str() : nullptr);
    rsx_live_draw_set_ps_replacement(kTwoSidedHash, smooth ? twoSidedShader().c_str() : nullptr);
}
