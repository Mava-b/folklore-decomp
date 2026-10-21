# Depth24 guest-memory rounding

The production depth-to-guest path previously evaluated
`d * 16777215.0f + 0.5f` in float32 before converting to an integer. At depth
1, float32 rounds that expression to 16777216. Storing its low 24 bits then
writes zero depth. Values near 1 and at 0.75 can also round one code too high.

The change evaluates that expression in double after the existing depth clamp.
For clamped `d` in [0,1], its value is in [0.5,16777215.5], so integer truncation
cannot exceed 16777215. Both interval endpoints are exactly representable in
double. Depth 1 therefore writes `FF FF FF 00`, including the existing zero
stencil byte. The clamp, Z16 expression, row addressing, texture selection,
cache invalidation and clear decoding are unchanged.

The regression drives the actual draw engine through its public methods with
a synthetic backend. It caches an alias before any depth write, writes depth,
then samples the alias to trigger production guest-memory storage and cache
refresh in the same frame. Two rows use opposite sample orders and an 80-byte
guest pitch, with sentinels before, between and after the used rectangle.
Expected quantized values are literal, not a duplicate of the implementation.

The 126 checks cover 1, its two adjacent float32 predecessors, 0.5, 0, NaN,
negative and above-one values, both infinities, 0.75 and 0.25. They check both
Z24S8 and unchanged Z16 bytes, zero stencil, decoded texture uploads, dirty
refresh, unchanged-depth reuse, clear decoding and untouched bytes outside the
used rectangle.

From the repository root:

```sh
clang -std=c11 -Wno-comment -I include -I libs/video \
  libs/video/tests/test_depth24_rounding.c libs/video/rsx_draw_engine.c \
  libs/video/rsx_dispatch.c libs/video/rsx_vertex_fetch.c \
  libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c \
  libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c \
  -lm -o /tmp/test_depth24_rounding
/tmp/test_depth24_rounding
```

The unchanged parent fails 26 checks; the change passes all 126 at both default
optimization and `-O2`. The existing complete draw-engine suite also passes.
This is independent of the Metal depth-readback change and requires no GPU or
original game data.
