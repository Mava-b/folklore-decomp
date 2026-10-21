# Fragment export control in shader caches

The draw engine and legacy Metal guest-fragment cache previously keyed only
the colour export-width bit of SHADER_CONTROL. Depth-export bits therefore
collided: the same resident program with depth export enabled could reuse the
function or pipeline compiled with it disabled, or vice versa.

Include bits 1..3 (0xE) alongside colour export width (0x40) in both keys.
This changes only cache identity. It does not implement depth export, change
depth tests, or alter the decompiler. Existing upstream PR #279 independently
adds the fragment depth output and handles the D3D12 live-draw key.

`test_fp_export_cache.c` drives actual public draw-engine methods with resident
synthetic vertex and fragment programs, holding other state unchanged. Its
11 checks expect four distinct pipelines for controls 0, 0xE, 0x40, 0x4E,
reuse of each unchanged control, and reuse when switching back. The original
key fails eight checks. Build it like test_rsx_draw_engine.c.

`test_metal_fp_export_cache.m` includes the actual Metal backend and drives its
guest-fragment cache. Its 15 checks use real shader translation and Metal
compilation: four distinct functions for those controls, unchanged-control
reuse and switching back. The original key fails eight checks. This optional
fixture requires macOS, an accessible Metal device, glslang and spirv-cross;
compile with the project's shader translator, fragment decompiler, texture
layout and their existing dependencies. All inputs are hand-assembled. No
original game programs or assets are used.

Private combined validation with the exact existing PR #279 decompiler also
checks actual GPU depth: a fullscreen triangle has geometric depth 0.75 and
fp32 r1.z 0.25 while emitting half or full colour. With the old cache both
export modes incorrectly retain 0.75; with separate cache entries they write
0.25, and disabled export remains 0.75. This validates the integration boundary
without bundling or duplicating the independent depth-export implementation.
