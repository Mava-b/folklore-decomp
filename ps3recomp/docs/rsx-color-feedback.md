# Ordered colour-target feedback

A texture unit can name the same colour allocation the current draw writes.
The shared engine formerly excluded the live attachment from surface binding
and uploaded its guest-memory backing instead. GPU-only writes need not have
updated that backing. The old path therefore sampled stale pixels.

The optional backend `color_snapshot` callback now supplies a unique immutable
copy for each referenced owned colour attachment, shared by texture units in
that draw. Newer depth content retains priority. The copy receives the sampling unit's existing component-remap view; gamma/filter
state stays on the ordinary shader/sampler paths. No CPU readback is required.
Backends leaving the appended callback NULL retain their existing behaviour.

Metal records a full-size colour copy between existing draw records. Encoding
finishes earlier render work, blits the source storage without conversion, then
opens the next draw's pass with Load for colour, depth and stencil unless an
explicit intervening clear requests otherwise. Each copy has independent
storage, so subsequent copies cannot overwrite an earlier queued reader.
Retired numeric resource/view handles remain available until their records
have acquired retained Metal objects; resize and shutdown release the last
copy. Record/allocation failures are explicit failures, not successful fallback
textures.

The implemented aliases have the same location, offset, entire physical width
and height, compatible native storage format, and effective source/texture row
pitch. The original surface must declare pitch-linear raster and the matching
RSX colour format (8, 11, 12 or 13); the host RGBA8 fallback for packed
formats is not enough to prove storage compatibility. Eligible formats are A8R8G8B8, W16_Z16_Y16_X16_FLOAT,
W32_Z32_Y32_X32_FLOAT and X32_FLOAT. Only linear 2D, non-cube, one-level layouts
are copied. A zero pitch denotes packed rows; explicit matching padded pitches
are accepted. The copy preserves the complete dimensions supplied by the
surface decoder;
it neither resolves nor changes their interpretation. A separate native
integration including sample-resolution AA was tested with AA0/3/4/5, while
this direct-master fixture uses explicitly declared AA0 physical dimensions.
No hardware MSAA coverage is claimed.

For a backend providing the callback, incompatible owned aliases or failed
copy/view creation reject the consuming draw with a bounded diagnostic.
Subrectangles, swizzled reinterpretation, other formats,
mips, cubes and differing pitches require separate conversion contracts.
Non-owned colour/depth/guest binding paths are unchanged.

Synthetic validation drives the production engine and Metal translator with
queued producer and consumer draws, asymmetric component
remapping, alpha, EQUAL depth/stencil and a long queued retirement chain.
CPU controls exercise descriptor rejection, MRT, repeated units, allocation
location, depth priority, callback/view failures, resize and legacy NULL
behaviour. Ordinary engine regression stdout and stderr remain byte-identical
to the parent. These establish the generic feedback boundary; they do not
establish complete game rendering, visible object identity or hardware RSX
feedback hazard semantics within a single overlapping primitive.

The existing repository's `rsx_live_draw.c` own-target snapshot path documents
the compatibility behaviour (same-target GPU content, not stale guest VRAM).
This implementation is independently written. PR213 also describes snapshots
in a broad renderer change; this focused delta intentionally does not copy it.

Master has no checked-completion certificate API. Copy failures use the existing
Metal dropped-record diagnostic and reject the consuming draw when detected
at binding. The native validation uses its separately accepted completion
checks; this focused change does not import those unrelated interfaces.

The CPU fixture is included in Linux and macOS CI. It includes the engine
source to test the descriptor helper and uses only synthetic data. Build it
with the same translation/fetch dependencies as the existing draw-engine
test, without separately compiling `rsx_draw_engine.c`. The offscreen Metal
fixture includes the backend and engine in one translation unit; link the
existing HLSL-to-MSL translator plus the ordinary decompiler, dispatch, vertex
compact and texture-layout objects. It needs no window or title runtime.
Its additional native-format copy cases sample float values above one into
RGBA32Float, so integer saturation cannot hide a conversion error.
