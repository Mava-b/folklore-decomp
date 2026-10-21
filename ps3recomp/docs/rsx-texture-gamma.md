# Texture input gamma state

The shared draw engine now honors TEXTURE_ADDRESS bits 20..23 as a final RGBA
channel mask. Gamma decoding is the standard piecewise sRGB-to-linear
operation, applied to the filtered/remapped sample before instruction result
scaling and destination saturation. It leaves unselected lanes unchanged.

Normalized color format eligibility follows the RSX format feature table;
float, depth, X16/Y16_X16 and HILO formats bypass gamma. Signed source channels
take precedence after crossbar mapping. EXP_TEX overrides gamma per fetch, so
an ordinary and an expanded fetch using the same unit remain distinct.
Only units with an active referenced fetch contribute gamma cache identity
or cause a helper to be emitted. Gamma state does not mutate texture bytes or
request extra uploads. Disabling the mask reuses the original raw variant.

TEX, TXP, TXB and TXL use the existing translated fetches. Active gamma on
unsupported TXD/TEXBEM/TXPBEM rejects translation rather than silently omitting
the operation. Existing entry points pass NULL metadata and preserve their
source bytes. Unsupported format decoding, ADDRESS unsigned-remap bias,
source signed decoding and standalone legacy backend shader paths remain
existing limits; this correction does not claim to implement those paths.

Primary contracts at RPCS3 ffba2bf09ef3980638f9327b25240d81414034ab:
- [per-format hardware feature table](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/Common/TextureUtils.cpp)
- [channel mask and precedence](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/RSXTexture.cpp)
- [post-sample processing and EXP_TEX override](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXFragmentTextureOps.glsl)
- [piecewise transfer](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXFragmentPrologue.glsl)

Synthetic `test_rsx_texture_gamma.c` exercises real production dispatch/cache,
format and remap state, all 16 masks, supported fetch types, explicit failures
and source-byte compatibility. Its Metal companion consumes these generated
shaders on units 0 and 15 with asymmetric bilinear samples and a nonidentity
resource crossbar. Float observation targets expose arithmetic differences.
These are translated GPU checks, not original RSX hardware or scene claims.
Framebuffer output encoding is a separate original control: an encoded copy
requires both stages to preserve its original color math.
