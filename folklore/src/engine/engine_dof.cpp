/*
 * engine_dof.cpp -- a smoother depth of field for Folklore (engine setting
 * depth_of_field; patches/0059 adds the renderer's shader replacement hook).
 *
 * Folklore draws its depth of field in the pass that resolves the 2560x720
 * scene to the 1280x720 screen (pso key 4fb2ef46cbd180a0 in the 1.10 US
 * captures; one draw a frame, right after the scene). The pass reads the
 * depth buffer, works out a blur factor from it with the game's focus
 * constants, and takes two taps of the scene pushed apart along a diagonal by
 * that factor. In focus the two taps are the 2x resolve; out of focus they
 * become two offset copies of the picture -- the doubled, harsh look.
 *
 * kSmoothDepthOfField below keeps the game's factor, its arithmetic and its
 * constants (so the focus distance and strength are the game's), returns the
 * original two taps wherever the image is in focus, and out of focus takes a
 * 24-tap disc spanning the same distance as the two copies. Each tap's own
 * depth decides whether it may contribute, so a sharp object does not bleed
 * into the blurred background around it. kOriginalShaderHash is the 64-bit
 * FNV-1a of the shader as the renderer translates it (its LD_HLSL_DUMP text).
 */
#include "engine_settings.h"

#include <cstdint>

extern "C" void rsx_live_draw_set_ps_replacement(uint64_t sourceHash, const char* hlsl);

namespace
{
    constexpr uint64_t kOriginalShaderHash = 0xBF822F2AE9CE0473ull;

    const char* const kSmoothDepthOfField = R"HLSL(// Folklore's depth of field, smooth version (engine_dof.cpp).
//
// The game's own pass (one per frame) resolves the 2560x720 scene to the
// 1280x720 screen with two taps, pushed apart along a diagonal by a factor
// worked out from the depth: in focus they are the 2x resolve, out of focus
// they become two offset copies of the image. This keeps the game's factor
// and its constants, so the focus distance and strength are the artists',
// and replaces the two taps out of focus with a 24-tap disc of the same
// extent. Each tap's own depth decides whether it may contribute, so a
// sharp object does not smear into the blur around it.
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
cbuffer PSConstants : register(b1) {
    float4 fp_constants[10];
    float4 fp_alpha;
};
// The game's blur factor at one point (its own arithmetic, in order).
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
float4 main(PSInput input) : SV_TARGET {
    const float2 coordinate = input.tc0.xy;
    const float2 axisScale = float2(fp_constants[8].x, fp_constants[9].y);
    const float sharpFactor = fp_constants[7].x;
    const float factor = folkloreBlurFactor(coordinate);
    // The game's two taps: exactly the original image where it is sharp.
    const float2 pairOffset = factor * axisScale;
    float3 original = 0.5 * (rsx_tex[0].Sample(rsx_samp[0], coordinate - pairOffset).xyz +
                             rsx_tex[0].Sample(rsx_samp[0], coordinate + pairOffset).xyz);
    const float alpha = rsx_tex[0].Sample(rsx_samp[0], coordinate).w;
    const float blurAmount = factor - sharpFactor;
    if (!(blurAmount > 0.0)) {
        return float4(original, alpha);
    }
    // A disc reaching as far as the game's two copies (the pair's diagonal).
    const int tapCount = 24;
    const float goldenAngle = 2.39996323;
    const float2 discRadius = pairOffset * 1.41421356;
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
}

void engineDepthOfFieldApply()
{
    const bool smooth = engineSettings().depthOfField == EngineSettings::DepthOfField::Smooth;
    rsx_live_draw_set_ps_replacement(kOriginalShaderHash, smooth ? kSmoothDepthOfField : nullptr);
}
