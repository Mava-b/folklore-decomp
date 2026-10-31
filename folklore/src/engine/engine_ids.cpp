/*
 * engine_ids.cpp -- a cleaner look for the Ids' souls (engine setting
 * id_souls; the renderer's shader replacement hook is patches/0059).
 *
 * The game's shader (capture-20261031-222947, the pink Id over the field:
 * draws with VS 3f0077e9, one 512x512 texture, alpha blended, no depth
 * write). It samples the texture once and adds the colours together
 * (shade = tex.rgb . col0.rgb), then puts that shade through a cosine:
 *
 *     wave = cos(shade * fp_constants[1].x + time * fp_constants[0].x) / 4
 *
 * fp_constants[1].x is large, so neighbouring texels land on unrelated
 * points of the cosine and the result is random from pixel to pixel: the
 * "static". At 720p each grain covered a PS3 pixel; at the PC's render scale
 * the grains are several times smaller than the screen can show, so after
 * the resolve they become a fine, crawling speckle instead of a texture.
 *
 * The replacements keep everything else exactly as the game does it (the
 * colour constants, the glow terms from TEXCOORD1 and TEXCOORD4, the alpha,
 * the alpha test, the second render target), and change only the wave:
 *
 *   Smooth: the shade the wave is worked out from is taken from a blurred
 *           copy of the texture, at a third of the game's frequency, and the
 *           wave is filtered to the screen: where it would change faster
 *           than a pixel can show, it fades to its average instead of
 *           aliasing (a box-filtered cosine: cos(x) * sin(w/2) / (w/2) for a
 *           phase change of w across the pixel). What is left is a soft
 *           shimmer that moves with the game's own timing.
 *   Clear:  no wave at all, just its average, so the soul is a clean
 *           translucent shape in the game's colours.
 *
 * Hash as in engine_dof.cpp (the renderer's normalised FNV-1a of the
 * translated shader).
 */
#include "engine_settings.h"

#include <cstdint>

extern "C" void rsx_live_draw_set_ps_replacement(uint64_t sourceHash, const char* hlsl);

namespace
{
    constexpr uint64_t kSoulHash = 0x19F0164E8EE06E0Eull;

#define FOLKLORE_SOUL_BODY R"HLSL(
struct PSInput {
    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;
    float4 fog : FOG;
    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;
    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;
};
Texture2D    rsx_tex[16] : register(t0);
SamplerState rsx_samp[16] : register(s0);
cbuffer PSConstants : register(b1) {
    float4 fp_constants[12];
    float4 fp_alpha;
};
struct PSOutput {
    float4 t0 : SV_TARGET0;
    float4 t1 : SV_TARGET1;
};
static const float kPi = 3.14159265;
// The average of |cos(x)| / 4 over a whole period.
static const float kAverageWaveSize = 0.25 * 2.0 / kPi;
// The game's shade: the texture times the vertex colour, channels added.
float soulShade(float4 texel, float4 tint)
{
    return dot(texel.rgb, tint.rgb);
}
// The same from a blurred copy: a disc of taps a few texels across, each
// one from a smaller mip where the texture has them.
float blurredShade(float2 coordinate, float4 tint)
{
    uint textureWidth, textureHeight;
    rsx_tex[0].GetDimensions(textureWidth, textureHeight);
    const float2 texelSize = 1.0 / float2(max(textureWidth, 1u), max(textureHeight, 1u));
    const float2 offsets[12] = {
        float2( 1.00,  0.00), float2( 0.50,  0.87), float2(-0.50,  0.87),
        float2(-1.00,  0.00), float2(-0.50, -0.87), float2( 0.50, -0.87),
        float2( 0.43,  0.25), float2( 0.00,  0.50), float2(-0.43,  0.25),
        float2(-0.43, -0.25), float2( 0.00, -0.50), float2( 0.43, -0.25)
    };
    const float radius = 3.0;
    float shadeSum = soulShade(rsx_tex[0].SampleBias(rsx_samp[0], coordinate, 1.5), tint);
    [unroll] for (int tapIndex = 0; tapIndex < 12; tapIndex++) {
        const float2 tapCoordinate = coordinate + offsets[tapIndex] * texelSize * radius;
        shadeSum += soulShade(rsx_tex[0].SampleBias(rsx_samp[0], tapCoordinate, 1.5), tint);
    }
    return shadeSum / 13.0;
}
PSOutput main(PSInput input) {
    const float2 coordinate = input.tc0.xy;
    const float4 texel = rsx_tex[0].Sample(rsx_samp[0], coordinate);
    const float4 tint = saturate(input.col0);
    const float shade = soulShade(texel, tint);
    const float coverage = texel.a * tint.a * tint.a;
    const float timePhase = input.tc3.x * fp_constants[0].x;
    // How much the wave is folded onto its size (darkens it), and the glow.
    const float waveFold = min(input.tc4.x * fp_constants[2].x, fp_constants[3].x);
    const float glow = min(input.tc1.x * fp_constants[4].x, fp_constants[5].x);
#if FOLKLORE_SOUL_SMOOTH
    const float phase = blurredShade(coordinate, tint) * fp_constants[1].x * (1.0 / 3.0) + timePhase;
    const float phaseWidth = max(fwidth(phase), 1e-4);
    const float halfWidth = 0.5 * phaseWidth;
    // How much of the wave the pixel can show: 1 when it is smooth here,
    // 0 once it changes by a whole period across the pixel.
    const float waveKept = phaseWidth < 2.0 * kPi ? saturate(sin(halfWidth) / halfWidth) : 0.0;
    const float wave = cos(phase) * 0.25;
    const float waveSize = lerp(kAverageWaveSize, abs(wave), waveKept);
    const float waveTerm = wave * waveKept - waveFold * waveSize;
#else
    const float waveTerm = -waveFold * kAverageWaveSize;
#endif
    const float brightness = shade * fp_constants[6].y + waveTerm;
    float3 colour = brightness * (fp_constants[7].x + fp_constants[7].y + fp_constants[7].z) + fp_constants[8].xyz;
    colour = min(glow * fp_constants[10].y + colour, fp_constants[11].x);
    float alpha = max(coverage, coverage * max(glow, waveFold));
    alpha = max(alpha, fp_constants[9].x);
    PSOutput result;
    const float4 colourOut = float4(colour, alpha);
    const float4 secondOut = float4(timePhase, 0.0, 0.0, 0.0);
    result.t0 = (colourOut == colourOut) ? colourOut : (float4)0;
    result.t1 = (secondOut == secondOut) ? secondOut : (float4)0;
    if (!(result.t0.a > fp_alpha.x)) {
        discard;
    }
    return result;
}
)HLSL"

    const char* const kSoulSmooth = "#define FOLKLORE_SOUL_SMOOTH 1\n" FOLKLORE_SOUL_BODY;
    const char* const kSoulClear = "#define FOLKLORE_SOUL_SMOOTH 0\n" FOLKLORE_SOUL_BODY;

#undef FOLKLORE_SOUL_BODY
}

void engineIdSoulsApply()
{
    const EngineSettings::IdSouls mode = engineSettings().idSouls;
    const char* replacement = nullptr;
    if (mode == EngineSettings::IdSouls::Smooth)
    {
        replacement = kSoulSmooth;
    }
    else if (mode == EngineSettings::IdSouls::Clear)
    {
        replacement = kSoulClear;
    }
    rsx_live_draw_set_ps_replacement(kSoulHash, replacement);
}
