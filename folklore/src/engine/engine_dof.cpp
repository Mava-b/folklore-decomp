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
 *
 * The same pass also adds ambient occlusion (engine setting
 * ambient_occlusion): it runs before the HUD is drawn and already has the
 * depth buffer bound, so soft shading where surfaces meet can be worked out
 * from depth alone and multiplied in (kAmbient below). With the original
 * depth of field and occlusion on, the pass keeps the game's two taps.
 */
#include "engine_settings.h"

#include <cstdint>
#include <cstdio>
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

    /* Ambient occlusion from the depth buffer alone. The game's projection
     * is not known here, so positions are rebuilt up to a scale: with the far
     * plane much further than the near one, 1 / (1 - depth) is proportional
     * to the distance from the camera, and an assumed 60 degree field of view
     * spreads it across the screen. Everything the occlusion uses is a ratio
     * of those positions (directions, and distances relative to the depth),
     * so the unknown scale drops out. Sixteen taps in a disc of fixed size on
     * screen, turned per pixel by a 4x4 pattern so their gaps do not line up
     * into rings; a tap occludes by how far it rises above the surface's
     * plane, and fades out when it is much nearer or further than the pixel
     * (so a character does not cast a halo on the far background). The sky
     * (depth at the far plane) gets none. */
    const char* const kAmbient = R"HLSL(
static const float folkloreTanHalfFov = 0.57735;
float folkloreViewDistance(int2 texel, int2 depthSize) {
    float depth = rsx_tex[1].Load(int3(clamp(texel, int2(0, 0), depthSize - 1), 0)).x;
    return 1.0 / max(1.0 - saturate(depth), 1.0e-7);
}
float3 folkloreViewPosition(float2 coordinate, float viewDistance, float aspect) {
    float2 ndc = float2(coordinate.x * 2.0 - 1.0, 1.0 - coordinate.y * 2.0);
    return float3(ndc.x * aspect * folkloreTanHalfFov * viewDistance,
                  ndc.y * folkloreTanHalfFov * viewDistance, viewDistance);
}
float folkloreAmbient(float2 coordinate, float2 screenPosition) {
    uint depthWidth, depthHeight;
    rsx_tex[1].GetDimensions(depthWidth, depthHeight);
    const int2 depthSize = int2(max(depthWidth, 1u), max(depthHeight, 1u));
    // The scene is supersampled twice across, so its picture is half as wide.
    const float aspect = 0.5 * (float)depthSize.x / (float)depthSize.y;
    const int2 centreTexel = int2(coordinate * (float2)depthSize);
    const float centreDepth = saturate(rsx_tex[1].Load(int3(clamp(centreTexel, int2(0, 0), depthSize - 1), 0)).x);
    if (centreDepth >= folkloreAmbientSkyDepth) {
        return 1.0;
    }
    const float2 texelSize = 1.0 / (float2)depthSize;
    const float centreDistance = folkloreViewDistance(centreTexel, depthSize);
    const float3 centre = folkloreViewPosition(coordinate, centreDistance, aspect);
    // The surface normal from whichever neighbour on each axis is on the
    // same surface (the smaller step), so edges do not get a bent normal.
    const float3 right = folkloreViewPosition(coordinate + float2(texelSize.x, 0.0),
                                              folkloreViewDistance(centreTexel + int2(1, 0), depthSize), aspect);
    const float3 left = folkloreViewPosition(coordinate - float2(texelSize.x, 0.0),
                                             folkloreViewDistance(centreTexel - int2(1, 0), depthSize), aspect);
    const float3 below = folkloreViewPosition(coordinate + float2(0.0, texelSize.y),
                                              folkloreViewDistance(centreTexel + int2(0, 1), depthSize), aspect);
    const float3 above = folkloreViewPosition(coordinate - float2(0.0, texelSize.y),
                                              folkloreViewDistance(centreTexel - int2(0, 1), depthSize), aspect);
    const float3 acrossStep = abs(right.z - centre.z) < abs(centre.z - left.z) ? right - centre : centre - left;
    const float3 downStep = abs(below.z - centre.z) < abs(centre.z - above.z) ? below - centre : centre - above;
    float3 normal = normalize(cross(acrossStep, downStep));
    if (dot(normal, centre) > 0.0) {
        normal = -normal;
    }
    const float2 discRadius = float2(folkloreAmbientRadius / aspect, folkloreAmbientRadius);
    const float reach = folkloreAmbientRange * centreDistance;
    const uint2 patternCell = (uint2)screenPosition & 3u;
    const float patternTurn = (float)((patternCell.x * 4u + patternCell.y * 9u) & 15u) / 16.0;
    const int tapCount = 16;
    const float goldenAngle = 2.39996323;
    float occlusion = 0.0;
    [unroll] for (int tapIndex = 0; tapIndex < tapCount; tapIndex++) {
        const float distanceFraction = sqrt(((float)tapIndex + 0.5) / (float)tapCount);
        const float angle = ((float)tapIndex + patternTurn) * goldenAngle;
        const float2 tapCoordinate = coordinate + discRadius * distanceFraction * float2(cos(angle), sin(angle));
        const int2 tapTexel = int2(tapCoordinate * (float2)depthSize);
        const float3 tapPosition = folkloreViewPosition(tapCoordinate, folkloreViewDistance(tapTexel, depthSize), aspect);
        const float3 toTap = tapPosition - centre;
        const float tapDistance = length(toTap);
        const float rise = dot(normal, toTap) / max(tapDistance, 1.0e-6);
        const float nearness = saturate(1.0 - (tapDistance * tapDistance) / (reach * reach));
        occlusion += max(rise - folkloreAmbientBias, 0.0) * nearness;
    }
    const float ambient = saturate(1.0 - folkloreAmbientIntensity * occlusion / (float)tapCount);
    // Fade out into the distance, where the depth buffer has little precision.
    const float distanceFade = saturate((folkloreAmbientSkyDepth - centreDepth) / (folkloreAmbientSkyDepth - folkloreAmbientFadeDepth));
    return lerp(1.0, ambient, distanceFade);
}
)HLSL";

    /* The pass itself: the game's two taps; where something is out of focus
     * a round 24-tap disc instead when the smooth depth of field is on; and
     * the ambient occlusion multiplied in, fading out as the blur grows. */
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
    // A round disc on screen (the scene is twice as wide as the screen),
    // reaching as far as each of the game's two copies was pushed.
    uint sceneWidth, sceneHeight;
    rsx_tex[0].GetDimensions(sceneWidth, sceneHeight);
    const float2 screenPixels = float2(0.5 * (float)max(sceneWidth, 1u), (float)max(sceneHeight, 1u));
    const float radiusPixels = length(pairOffset * screenPixels);
    float ambient = 1.0;
    if (folkloreAmbientStrength > 0.0) {
        const float blurFade = blurAmount > 0.0 ? saturate(radiusPixels / 4.0) : 0.0;
        if (blurFade < 1.0) {
            ambient = lerp(1.0, folkloreAmbient(coordinate, input.position.xy), folkloreAmbientStrength * (1.0 - blurFade));
        }
    }
    if (!folkloreSmoothBlur || !(blurAmount > 0.0)) {
        return float4(original * ambient, alpha);
    }
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
    return float4(colourSum / weightSum * ambient, alpha);
}
)HLSL";

    /* The tuning, ahead of everything else in the text: whether the disc
     * blur is on, and the occlusion's strength (0 = off) and shape. */
    std::string optionsText(bool smoothBlur, float ambientStrength)
    {
        char options[640];
        std::snprintf(options, sizeof(options),
                      "\nstatic const bool folkloreSmoothBlur = %s;\n"
                      "static const float folkloreAmbientStrength = %.3f;\n"
                      "static const float folkloreAmbientRadius = 0.030;\n"
                      "static const float folkloreAmbientRange = 0.060;\n"
                      "static const float folkloreAmbientBias = 0.10;\n"
                      "static const float folkloreAmbientIntensity = 1.6;\n"
                      "static const float folkloreAmbientFadeDepth = 0.9990;\n"
                      "static const float folkloreAmbientSkyDepth = 0.99995;\n",
                      smoothBlur ? "true" : "false", ambientStrength);
        return options;
    }

    /* Every combination is built once and kept: the renderer holds on to
     * the pointer it is given. Index: version (2) x blur (2) x occlusion (3). */
    const char* passShader(int versionIndex, bool smoothBlur, int ambientIndex)
    {
        static std::string texts[2][2][3];
        static const float ambientStrengths[3] = {0.0f, 0.6f, 1.0f};
        std::string& text = texts[versionIndex][smoothBlur ? 1 : 0][ambientIndex];
        if (text.empty())
        {
            text = std::string(kCommon) + optionsText(smoothBlur, ambientStrengths[ambientIndex]) +
                   (versionIndex == 0 ? kFarOnly : kTwoSided) + kAmbient + kBody;
        }
        return text.c_str();
    }
}

void engineDepthOfFieldApply()
{
    const EngineSettings& settings = engineSettings();
    const bool smoothBlur = settings.depthOfField == EngineSettings::DepthOfField::Smooth;
    const int ambientIndex = (int)settings.ambientOcclusion;
    const bool replace = smoothBlur || ambientIndex != 0;
    rsx_live_draw_set_ps_replacement(kFarOnlyHash, replace ? passShader(0, smoothBlur, ambientIndex) : nullptr);
    rsx_live_draw_set_ps_replacement(kTwoSidedHash, replace ? passShader(1, smoothBlur, ambientIndex) : nullptr);
}
