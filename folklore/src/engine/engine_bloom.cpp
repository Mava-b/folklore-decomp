/*
 * engine_bloom.cpp -- a steadier bloom for Folklore (engine setting bloom;
 * the renderer's shader replacement hook is patches/0059).
 *
 * The game's bloom (capture-20261031-115639): the scene copy (2560x720, times
 * the render scale) is reduced to 640x480 in ONE pass that takes a single
 * bilinear sample per pixel and keeps what is brighter than a threshold
 * (pso 46f31d99b7711f43); two 8-tap passes blur it at 640x480; and it is
 * added back over the screen with a 2x2 filter (181474cd2e468dbb). The
 * single sample reads one source pixel in every 4x1.5 at the PS3's
 * resolution -- one in 12x4.5 at 3x render scale -- so small bright details
 * (sparkles, edges, lights) fall between samples on one frame and on them
 * the next: the bloom flickers and crawls as the camera moves.
 *
 * The replacements: the reduction averages every source pixel under each
 * 640x480 pixel (thresholding each one first, as the game thresholds its
 * one sample), so the bloom is stable whatever the render scale; and the
 * composite, when it enlarges, uses a smooth bicubic (B-spline) filter
 * instead of the 2x2 one, so the 640x480 grid does not show. The game's
 * threshold, colours and blend are kept. Hashes as in engine_dof.cpp (the
 * renderer's normalised FNV-1a of the translated shader).
 */
#include "engine_settings.h"

#include <cstdint>

extern "C" void rsx_live_draw_set_ps_replacement(uint64_t sourceHash, const char* hlsl);

namespace
{
    constexpr uint64_t kBrightPassHash = 0xD5315C03A3656365ull;
    constexpr uint64_t kCompositeHash = 0x5619049F34E0928Full;

    const char* const kBrightPass = R"HLSL(
struct PSInput {
    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;
    float4 fog : FOG;
    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;
    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;
};
Texture2D    rsx_tex[16] : register(t0);
SamplerState rsx_samp[16] : register(s0);
cbuffer PSConstants : register(b1) {
    float4 fp_constants[1];
    float4 fp_alpha;
};
// The game: one sample, times the vertex colour, minus the threshold.
// Here: the same for every source pixel under this one, then the average.
float4 main(PSInput input) : SV_TARGET {
    const float2 coordinate = input.tc0.xy;
    const float4 tint = saturate(input.col0);
    const float threshold = fp_constants[0].x;
    uint sourceWidth, sourceHeight;
    rsx_tex[0].GetDimensions(sourceWidth, sourceHeight);
    const float2 sourceSize = float2(max(sourceWidth, 1u), max(sourceHeight, 1u));
    const float2 footprint = float2(abs(ddx(coordinate.x)), abs(ddy(coordinate.y)));
    // Each bilinear tap covers 2x2 source pixels.
    const int columns = clamp((int)ceil(footprint.x * sourceSize.x * 0.5), 1, 8);
    const int rows = clamp((int)ceil(footprint.y * sourceSize.y * 0.5), 1, 8);
    float3 brightSum = 0.0;
    float alphaSum = 0.0;
    [loop] for (int rowIndex = 0; rowIndex < rows; rowIndex++) {
        [loop] for (int columnIndex = 0; columnIndex < columns; columnIndex++) {
            const float2 position = float2(((float)columnIndex + 0.5) / (float)columns - 0.5,
                                           ((float)rowIndex + 0.5) / (float)rows - 0.5);
            const float4 tap = rsx_tex[0].SampleLevel(rsx_samp[0], coordinate + position * footprint, 0) * tint;
            brightSum += max(tap.xyz - threshold, 0.0);
            alphaSum += tap.w;
        }
    }
    const float tapCount = (float)(columns * rows);
    return float4(brightSum / tapCount, alphaSum / tapCount);
}
)HLSL";

    const char* const kComposite = R"HLSL(
float4 _rsx_conv_gauss(Texture2D t, SamplerState s, float2 uv) {
    uint w, h; t.GetDimensions(w, h);
    float2 d = 0.5 / float2(max(w, 1u), max(h, 1u));
    return 0.25 * (t.Sample(s, uv + float2(-d.x, -d.y)) + t.Sample(s, uv + float2(d.x, -d.y)) +
                   t.Sample(s, uv + float2(-d.x, d.y)) + t.Sample(s, uv + float2(d.x, d.y)));
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
    float4 fp_constants[1];
    float4 fp_alpha;
};
// Cubic B-spline through four bilinear taps (smooth, no ringing).
float4 bicubicBSpline(Texture2D sourceTexture, SamplerState sourceSampler, float2 coordinate, float2 sourceSize) {
    const float2 texel = coordinate * sourceSize - 0.5;
    const float2 base = floor(texel);
    const float2 fraction = texel - base;
    const float2 fraction2 = fraction * fraction;
    const float2 fraction3 = fraction2 * fraction;
    const float2 weight0 = (1.0 - 3.0 * fraction + 3.0 * fraction2 - fraction3) / 6.0;
    const float2 weight1 = (4.0 - 6.0 * fraction2 + 3.0 * fraction3) / 6.0;
    const float2 weight2 = (1.0 + 3.0 * fraction + 3.0 * fraction2 - 3.0 * fraction3) / 6.0;
    const float2 weight3 = fraction3 / 6.0;
    const float2 lowWeight = weight0 + weight1;
    const float2 highWeight = weight2 + weight3;
    const float2 lowPosition = (base - 0.5 + weight1 / lowWeight) / sourceSize;
    const float2 highPosition = (base + 1.5 + weight3 / highWeight) / sourceSize;
    return (sourceTexture.SampleLevel(sourceSampler, float2(lowPosition.x, lowPosition.y), 0) * lowWeight.x +
            sourceTexture.SampleLevel(sourceSampler, float2(highPosition.x, lowPosition.y), 0) * highWeight.x) * lowWeight.y +
           (sourceTexture.SampleLevel(sourceSampler, float2(lowPosition.x, highPosition.y), 0) * lowWeight.x +
            sourceTexture.SampleLevel(sourceSampler, float2(highPosition.x, highPosition.y), 0) * highWeight.x) * highWeight.y;
}
float4 main(PSInput input) : SV_TARGET {
    const float2 coordinate = input.tc0.xy;
    uint sourceWidth, sourceHeight;
    rsx_tex[0].GetDimensions(sourceWidth, sourceHeight);
    const float2 sourceSize = float2(max(sourceWidth, 1u), max(sourceHeight, 1u));
    const float2 footprint = float2(abs(ddx(coordinate.x)), abs(ddy(coordinate.y))) * sourceSize;
    // Only when this draw enlarges its texture (the bloom does); otherwise
    // the game's own 2x2 filter, as before.
    float4 filtered;
    if (max(footprint.x, footprint.y) < 0.75) {
        filtered = bicubicBSpline(rsx_tex[0], rsx_samp[0], coordinate, sourceSize);
    } else {
        filtered = _rsx_conv_gauss(rsx_tex[0], rsx_samp[0], coordinate);
    }
    return filtered * saturate(input.col0);
}
)HLSL";
}

void engineBloomApply()
{
    const bool smooth = engineSettings().bloom == EngineSettings::Bloom::Smooth;
    rsx_live_draw_set_ps_replacement(kBrightPassHash, smooth ? kBrightPass : nullptr);
    rsx_live_draw_set_ps_replacement(kCompositeHash, smooth ? kComposite : nullptr);
}
