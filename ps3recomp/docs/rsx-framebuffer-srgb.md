# Integer framebuffer sRGB output

The shared draw engine now honors NV4097_SET_SHADER_PACKER (0x1FEC). Any
nonzero value requests linear-to-sRGB conversion for integer color surfaces
when fragment color exports use the half register file. W16Z16Y16X16,
W32Z32Y32X32, X32 surfaces and 32-bit exports bypass it.

The fragment epilogue applies the standard piecewise transfer to RGB on each
emitted color target before target quantization/blending. Alpha, optional
fragment depth, texture inputs and clear values are preserved. Disabled and
ineligible states preserve the old shader source and pipeline key. Effective
state is pipeline identity; switching back reuses the existing variant.

The helper accepts generated source, fails atomically on insufficient space or
unrecognized output shape, and must run before alpha-test wrapping. It emits
no texture references. The correction serves all backends using the shared
draw engine; standalone legacy backend shader paths are outside this change.

Primary contracts are pinned to RPCS3 ffba2bf09ef3980638f9327b25240d81414034ab:
- [register decoder](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/rsx_decode.h)
- [integer/half eligibility](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/RSXThread.cpp)
- [ROP output ordering](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXROPEpilogue.glsl)
- [piecewise transfer](https://github.com/RPCS3/rpcs3/blob/ffba2bf09ef3980638f9327b25240d81414034ab/rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXFragmentPrologue.glsl)

Synthetic tests `test_rsx_framebuffer_srgb.c` exercise real engine dispatch,
eligibility, cache transitions, atomic failures, MRT output and alpha-test
composition. The companion Metal fixture consumes its generated synthetic
HLSL and observes float targets to prevent quantization hiding an error.
This verifies translated GPU execution, not original RSX hardware or an
application scene. Texture gamma is a separately controlled operation and
must be honored when enabled to preserve an encoded copy's original math.
