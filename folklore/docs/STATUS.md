# Folklore recomp — status

BCUS98147 (US disc, v1.00). ps3recomp `39a36296511c` + `patches/0001`..`0009`.
Last updated 2026-10-18.

## Where it is

| Milestone | State |
|---|---|
| EBOOT analysed, imports mapped | ✅ |
| PPU lifted (13,572 functions, ~1.5M lines) | ✅ clean lift, no unknown instructions |
| SPURS job binaries lifted (9 jobs, 3,766 SPU functions) | ✅ registered by fingerprint, not yet exercised |
| Port compiles + links | ✅ Linux/clang test build. Windows/clang-cl is next |
| CRT startup (TLS, heaps, mutexes, static constructors) | ✅ |
| System modules, memory containers, cellPad, GCM init, video out | ✅ |
| Flip thread, display buffers, frames cleared and flipped | ✅ (Linux null renderer) |
| First-boot game-data install | ✅ completes on Windows; matches RPCS3 install |
| SPURS jobs dispatched to lifted SPU code | ✅ (verified locally) |
| Real Sony SPURS (your libsre, lifted): kernel, system service, jm2, all 9 jobs | ✅ locally: audio, movie and logo threads start, no SPU faults in 90 s |
| Logo movies (cellVdec + FFmpeg) | ⏳ decoder opens; nothing on screen yet |
| Title screen / menus / gameplay | — |

Reference: `docs/reference/linux-boot-2026-10-18.log` is the boot as far as
it gets without the 9 GB of archives, ending at "An HDD access error
occurred" because `model.bin_` etc. are missing.

## The game, by the numbers

- EBOOT: 6.2 MB, `.text` 0x1022C–0x4ED680, TOC 0x56EB00, entry OPD 0x54D820
- 12,894 OPD functions → 13,298 after boundary recovery → 13,572 lifted
- 167 firmware imports from 15 libraries; no game-side `.sprx` modules
- SPU use is light: 9 SPURS **job-chain** jobs (cellSpursCreateJobChain etc.),
  raw blobs in their own data sections at 0x10079E00–0x100B9180, ~250 KB total.
  No SPURS tasks or raw-SPU programs.
- Data: `USRDIR/bin/{model,mt,etc}.bin_` archives (2.5–3.7 GB each) with
  `.fht_` indexes, MPEG-2 `.m2v_` cutscenes (decoded via FFmpeg) and `.sgb_`
  audio streams.

## Imports without a ps3recomp handler (28) → `src/hle/folklore_hle.cpp`

Behaviour mirrors RPCS3's for each:

- SIXAXIS: `cellPadSetSensorMode`, `cellPadInfoSensorMode`
- Dialog: `cellMsgDialogOpenErrorCode` (auto-OK)
- Storage/browser: `cellStorageDataImport/Export`, 9× `cellWebBrowser*`
- `_cellGcmFunc15` (no-op)
- L10n: `UTF8toUTF16` (real), `SJISstoUTF8s` (ASCII pass-through), `SJIStoUTF8`, `UTF8stoSJISs`
- Online: `sceNpManagerGetNetworkTime`, `sceNpScore*GameDataAsync` → NOT_INITIALIZED;
  `connect/listen/accept/send` → -1; `inet_addr` (real); `sys_net_show_ifconfig`
- `cellRtcFormatRfc3339LocalTime` (real, host time zone)

## Fixes made to ps3recomp (`patches/0001`)

1. **lv2 350 `sys_memory_allocate_from_container`** was unimplemented. Folklore
   creates four containers with lv2 341 (72, 39, 8 and 48 MB) and takes every
   heap from them. The stub returned no address, so the CRT aborted with
   *"not enough memory to initialize mutex"*, and everything later saw NULL
   (GCM context, IO address). Now a first-fit sub-allocator runs inside the
   region 341 hands out, honouring `sys_memory_free` (Folklore frees and
   re-takes a container during the data install). Containers from lv2 324 are
   supported too.
2. **`PS3_HDD1_ROOT`**: `/dev_hdd1` (system cache) can be mapped away from the
   disc dump, like the existing `PS3_HDD0_ROOT`.
3. **POSIX only**: the boot harness allocated guest memory with `calloc`, which
   isn't page-aligned, so every `mprotect` failed. It now uses `mmap`. This
   doesn't affect Windows.

`patches/0002`:

4. **cellFs open flags had the wrong values.** The SDK's are octal
   (`O_CREAT 000100`, `O_TRUNC 001000`, `O_APPEND 002000`), but `ppu_fs.cpp`
   and `sys_fs.h` had them as hex-looking numbers, so `O_WRONLY|O_CREAT`
   (0x41) never created the file. Folklore's install opens
   `TMP_HDD_COPY.DAT` that way, so it failed with "An HDD access error
   occurred" (first Windows run, 2026-10-18). `O_CREAT|O_TRUNC` (0x241) only
   worked by accident. `PS3_FS_OPENFLAGS=1` logs each open's flags.

`patches/0003`:

5. **SPURS jobs above 0x10000000 were rejected as "implausible".** Folklore's
   nine job binaries live in the EBOOT data segment (0x10079E00..). The check
   now accepts any mapped guest range. Job fingerprints are registered for
   every 16-byte size from the last non-zero byte to the section end, since
   the job header's size (0xD190 for job 0) is smaller than the padded section.

`patches/0004`:

6. **`cellFsUnlink`/`cellFsRmdir` were no-op stubs, and `cellFsRename` bypassed
   the hdd0/hdd1 mapping.** Folklore deletes its `TMP_HDD_COPY.DAT`
   "install in progress" marker once the install finishes. The stub left it in
   place, so the next boot reported "Game data is corrupt". The finished
   install now matches RPCS3's file for file (no marker; same `~` files and
   sizes). `run.ps1 -ResetInstall` wipes the port's hdd0/hdd1 to re-run it.

`patches/0005` (opt-in, `PS3_SPURS_JOBCTX_SBUF70=1`, set by run.ps1):

7. **Job context: scratch base at +0x70.** Folklore's jobs read their scratch
   buffer pointer from `jobContext+0x70`, beyond the 48-byte
   `CellSpursJobContext2` ps3recomp builds. That offset landed in the
   descriptor copy (0xFFFFFFFF on one audio job), so the job DMA'd 84 KB to LS
   −1 and job 0x10087000 then spun forever. That was the black screen on run 4.
   This is inferred from the job code, not checked against Sony's jm2, so it's
   opt-in. Running the real jm2 from firmware (docs/FIRMWARE_LLE.md) would
   settle it.

## Real SPURS from your firmware (`firmware_lle/`, `patches/0006`)

`tools/lift_firmware.sh` lifts your own decrypted `libsre.prx` into
`firmware_lle/` (never share that folder). With it, the build runs Sony's
SPURS instead of the runtime's HLE:

- PPU: 384 functions, 269 exports, loaded at 0x30000000. The game's
  cellSpurs calls go to it through the NID bridge.
- SPU: the SPURS kernel (LS 0x100, entry 0x818), the system service and the
  job-chain policy module jm2 (both DMA'd to LS 0xA00, told apart by the
  source address of that DMA).
- The jobs: jm2 caches job binaries in local store, so a job is not always at
  the same address. `tools/relift_lle_jobs.sh` lifts each job at every address
  it has been seen at (so far 0x4C00 for all, 0x12000 for job 0x10087000) into
  `src/spu_gen_lle/`, with switch-table targets from `tools/spu_jump_tables.py`.
  An unseen placement still runs, through the SPU interpreter, and the log says
  `[spu-ovl] code from 0x... placed at LS 0x..., which has no lift`.

What 0006 fixes in ps3recomp to make that run:

8. lv2 14 (`sys_process_is_spu_lock_line_reservation_address`) rejected the
   SPURS instance in .bss (0x10A60C00), so `cellSpursInitialize` failed.
9. SPU `stop 0x101/0x102` (group/thread exit) did not end the thread; the
   system service then spun in its exit path forever.
10. SPU group events: the runtime invented a "group stopped" event that libsre's
    event helper reads as "terminate", so SPURS lost its helper the first time
    the kernel idled out. Now only the connected event types are sent, like lv2.
11. Code regions valid at one LS address (for jm2's binary cache).

`patches/0007`:

12. **cellFsAioRead completed inside the submit call.** Folklore's movie
    streamer submits a read, then marks it outstanding and waits for the
    completion callback to clear the mark. That callback had already run, so
    the logo movie never received any data: the black screen on the first
    real-libsre Windows run (2026-10-18 18:03). Reads now complete on a worker
    thread. Locally, with the real logo movie, the whole of movie_995 now
    decodes (StartSeq, about 290 pictures, EndSeq), matching RPCS3.

13. **Logo movie: a race in the game's own movie player** (second real-libsre
    Windows run, 2026-10-18 18:25: 16 pictures decoded, none ever taken).
    MovieInit sets a "hold" flag; the presentation thread (MoviePostThread)
    and the input thread each check it on a 10 ms poll, and a thread that sees
    it does a handshake. If only MoviePost sees it, it waits on a condition
    variable that is never signalled. On the PS3 the threads' low priority
    keeps them in step; on PC threads they race (locally 3 runs in 4 hung).
    `tools/game_patches.py` gives that one wait a 100 ms timeout; the game's
    loop after it already copes. Locally the movie now completes 14 runs out
    of 14.
14. `patches/0008`: guest callbacks run on a per-thread callback stack rather
    than one scratch stack shared by every host thread (a vdec callback and
    the vblank ticker's flip handler could overlap on it), and AIO
    completions arrive after 25 ms like a disc read.

15. Third Windows run (19:05): the Sony Computer Entertainment logo plays, then
    the picture corrupts (FFmpeg rejected AUs 76-80, 209, 217) and the game
    stops after the movie. With the sound running, jm2 placed three jobs at
    LS addresses with no lift (0x18400, 0x1AC00, 0x1E400), so they ran in the
    SPU interpreter; those are now lifted, and a local store can hold eight
    resident code regions instead of four (`patches/0009`). The next log
    prints each rejected AU's hash to show whether its data was corrupt.

16. Fourth Windows run (19:32): the game gets through the logo movies, a new
    game and the intro cutscene to the player-select screen. Subtitles flicker
    and the player-select screen is dark, with only its left half showing the
    scene. The scene targets are 2x antialiased (DIAGONAL_CENTERED_2) with a
    10240-byte pitch; some passes declare them with a 2560-pixel clip, which the
    renderer doubled to a 5120-wide surface at the same address as the
    2560-wide one. The resolve then sampled either the stale 2560 surface or
    the 5120 one (scene in its left half). `patches/0010` clamps an AA
    surface's width to what its pitch holds (PS3_RSX_NO_AA_PITCH_CLAMP=1 turns
    that off) and adds `run.ps1 -Capture` / `capture.bat`: a per-draw CSV,
    #flip rows, images of the target every 8th draw, and the first six
    presented frames of each capture.

17. Intermittent crash during the logo movie (5 of ~12 runs, frame ~100-150):
    an access violation in a DLL copying from a wild pointer, called from
    cellVdecDecodeAu. The movie player reads each AU's size from
    movie_995.dat_ (300 x 8-byte entries, at 0x45325B00 in a local run), and
    on Windows that table is sometimes overwritten with stream-like data, so
    an AU comes through as e.g. "addr 0x49B52031 size 1163982640". Earlier
    runs where AUs 46-80 "did not decode" are the same corruption without the
    crash. `patches/0011`: DecodeAu skips an AU that does not fit in guest
    memory and logs the player's registers (size table, ring); PS3_FS_AIO_LOG
    (on in run.ps1) logs every file read with its guest buffer; the crash
    report names the faulting DLL and every frame's module. Locally (Linux)
    nothing overlaps the table after it is read -- the culprit is in a
    Windows log.

18. Found the AU table overwrite (Windows log 19:53): the table and the main
    thread's model.bin_ read (LCm998, not in the hdd cache) both landed at
    0x45433600. The game has one work-area allocator (top at [toc-0x263C]);
    the movie takes its table from the top, and the main thread's loader
    reads archives into the space above the top as scratch. Locally the
    loader finished first; on Windows (real 4 MB reads from G:) the logo
    movie opened in the middle of it. Game patches 0x1A3BA4 / 0x1A3124 /
    0x1A33F4 (tools/game_patches.py) reserve 256 KB after the player struct
    at boot and put the table there. Local run: table at 0x45315A40, all
    271 AUs decode.

19. The other boot failure (first logo pulses forever, 19:52 log): the main
    thread sits in libsre 0x30018EBC -> 0x3000A304, waiting on the semaphore
    for a workload shutdown, while SpursHdlr0 polls lwcond and SpursHdlr1
    waits for events; the SPURS kernel keeps starting and idling (stop
    0x101). Watchdog now prints r28-r31 per thread and dumps the SPURS
    instance (PS3_WATCHDOG_DUMP in run.ps1) for the next occurrence.

20. Fifth Windows run (20:05): logo movies clean (no bad AUs) and the
    player-select screen is no longer covered (the AA clamp fired for
    0x2A40000, clip 2560 with aa=3). Still: camera stutter and stray
    polygons in the 3D scene. The FIFO drain treated NV406E semaphore
    ACQUIRE as a no-op, so the RSX never waited for labels -- PS3 titles hold
    the GPU on those until SPU jobs finish the data it reads. `patches/0012`:
    with GCM_SEMA_ACQUIRE=1 (run.ps1) ACQUIRE spins up to 2 ms, then ends the
    drain pass and retries; after 250 ms on one ACQUIRE it logs and goes on.
    [gcm-sema] lines count ACQUIREs seen / waited / timed out.

21. Sixth Windows run (20:12): no change. No [gcm-sema] line at all, so
    Folklore never issues NV406E ACQUIRE -- not the cause (0012 stays, inert).
    The run was without -Capture, so no capture. Camera stutter: the vblank
    ticker stepped its 16 ms ticks off GetTickCount64, which only advances in
    ~15.6 ms steps on Windows -- ticks (and with them flip completions and
    presents) came in bursts and ran 4% fast. `patches/0013` drives it from
    steady_clock at 59.94 Hz (PS3_VBLANK_US overrides) and adds each draw's
    vertex attribute layout to the capture CSV. run.ps1 now always arms the
    capture, so capture.bat works on any run.

22. First render capture (21:37, player select, logs\capture-20261018-213719):
    the frame is ~96 draws into 0x2A40000, 10 into 0x1B40000, a few at
    640x480, then ~552 into the display buffer -- but the presents do not
    line up with that. Two flips in a row showed the same buffer, one frame
    lacked its last HUD draws ("Player Select"), and a flip of buffer 1 came
    after the next frame had already drawn 44 times into it. Folklore flips
    with cellGcmSetFlip (NID 0xDC09357E), which the runtime executed at call
    time instead of queueing in the command buffer. `patches/0014` queues it
    as a FIFO flip marker, as _cellGcmSetFlipCommand already did
    (PS3_GCM_SETFLIP_IMMEDIATE=1 restores the old behaviour). The 0013 tick
    clock did not change the camera on its own.

23. With 0014 the player select is smooth and clean. Selecting Keats and
    entering gameplay froze (21:44): the first fault is "unresolved indirect
    call -> 0x34DDF8" from func_0034D268. That function dispatches a 25-entry
    switch (table 0x34D3CC, 19 distinct cases) through a table base it keeps in
    a stack slot (`ld r7,-0xD0(r1)`), which the lifter's jump-table scan does
    not follow -- so the lifted function returned half way. 400 calls through
    NULL function pointers and wild writes (guest address 0x10 holds floats,
    the sgx switch table at 0x2A3C1C read back 0) followed, then the main
    thread stopped and SpursHdlr0 spun ([HOTREAD] spam). `patches/0015` adds
    PPU_JT_HINTS to ppu_lifter.py; relift.sh passes 34D3C8:34D3CC:19. Relift
    is otherwise identical (only func_0034D268 changes in substance; chunk
    boundaries move, so all 48 files are reshipped).

24. Gameplay reached (21:55). Too dark: capture-20261018-215537 shows the
    scene in 0x2A40000 lit correctly up to draw 144, then draw 145 -- a
    full-screen blended quad with stencil GREATER ref 0x80 -- darkens the
    whole frame. That is Folklore's stencil shadow pass: lit geometry writes
    REPLACE 0x80, shadow volumes DECR_WRAP, the quad shades stencil < 0x80.
    Every stencil draw showed write mask 0: the game never sets
    STENCIL_MASK and relies on the NV40 reset value 0xFF, which the
    register file did not seed, so nothing was written and everything read
    as shadowed. `patches/0016` seeds the reset stencil state (masks 0xFF,
    ALWAYS, KEEP; PS3_RSX_NO_STENCIL_RESET=1 to skip). Same patch: since
    0014 every in-FIFO flip was presented twice (the count-change present
    did not consume the pending flag) -- [fps] read 120 at the 60 fps
    menus; now one present per flip.

25. Lighting fixed by 0016. Further into the first area (22:02): geometry
    "messed up" and Keats' shadow wrong. capture-20261018-220253 has one
    partial frame (the 64 per-draw readbacks outlast the 2 s gate, so no
    flips were recorded). Seen so far: the previous frame's final image has
    rock meshes with whole triangles missing (sky showing through) and a
    black fence; the next frame starts with a full-screen quad copying the
    previous 0x1B40000 into 0x2A40000 (depth test off, depth write on), then
    sky/terrain draws 113-136 with depth test OFF, then grass/characters with
    depth test on -- the earlier, correct capture had a full depth-tested
    world pass instead. Shadow volumes are drawn with culling off, so they
    need two-sided stencil. `patches/0017` adds the back-face stencil state to
    the capture CSV; run.ps1 dumps every 64th draw (was 8) and capture.ps1
    records 4 s, so a capture now reaches the flips.
26. capture-20261018-221210 (same spot, complete frames 3609-3632). The frame
    structure is right: depth prepass, colour pass (LEQUAL, stencil REPLACE
    0x80), sky/trees, two-sided shadow volumes (front DECR_WRAP, back
    INCR_WRAP), the darkening quad (stencil < 0x80), then post. The broken
    rock meshes (e.g. v0 half4 stride 40 @ local 0x7AAC500, 2037-vertex
    DRAW_ARRAYS triangle lists) lose the same triangles in the prepass and in
    the colour pass, and stray triangles stretch past the silhouette: the
    vertex data in VRAM itself is wrong, not the render state. No 2D-engine
    copies were logged, so PPU or SPU code writes it. Two tools for the next
    run: `patches/0018` adds `ar=`/`ix=`/`el=`/`pos=..bad=` to each CSV row
    and, with LD_VB_DUMP (run.ps1 sets it), the raw vertex/index bytes of
    each mesh drawn during a capture (vb_*.bin / ib_*.bin); and
    `run.ps1 -InterpJobs` (FOLKLORE_INTERP_JOBS=all, or a list of image ids
    50-63) leaves the lifted SPURS job placements unregistered so the jobs
    run in the SPU interpreter -- if the rocks come out whole that way, a job
    lift is at fault and the id list narrows it down.
27. Found it: capture-20261018-222651 (normal) and -222905 (-InterpJobs)
    are equally broken, so the job lifts are cleared. The new `ix=` column
    shows the big rock mesh (vertices @ 0x7946800, 22770 of them, indices
    @ 0x7A36D00) always drawing exactly 65536 indices: the renderer queued at
    most 256 DRAW_INDEX_ARRAY packets per primitive, each packet is at most
    256 indices, and the rest of the mesh (~25%) was silently dropped -- the
    missing triangles. `patches/0019` merges contiguous packets on arrival
    (exact: the primitive stream is their concatenation) and raises the
    queue to 4096 packets, with a one-time log line if that is ever exceeded.
    The vertex data itself was fine (`bad=0` everywhere).
28. Rocks confirmed whole. Next: grass cards and a wire fence drawn solid
    (capture-20261018-223430). Those draws have the alpha test OFF, no
    blending, culling off and the alpha ref still 0xBF, on 2x AA targets --
    the signature of alpha-to-coverage, which the renderer never read
    (NV4097_SET_ANTI_ALIASING_CONTROL 0x1D7C). AA surfaces are stored at
    sample resolution rather than as D3D12 MSAA, so `patches/0020` emulates
    it as an alpha test GEQUAL 0.5 when bits 0 and 4 are set on an AA
    surface and the title's own alpha test is off (PS3_RSX_NO_A2C=1 turns it
    off). The CSV's `aac=` shows the register, to confirm on the next capture.
29. Grass and fence confirmed fixed. Keats' shadow (capture-20261018-224131):
    his shadow volumes (stride-96 meshes @ 0xA0E4600.., two-sided stencil)
    are skinned per triangle in the vertex program -- each vertex carries
    the triangle's three positions (v0/v10/v13) with weights (v8/v11/v14)
    and bone indices (v9/v12/v15), loaded into BOTH address registers. The
    decompiled HLSL (from scratch\rsx_shader_cache_v2, V_D5438F37...) wrote
    every ARL to a0 while the reads used a1, so one vertex's bones came from
    another's indices and the volume was extruded from a wrong silhouette.
    The VP decompiler took the ARL destination from addr_reg_sel (the field
    that picks the register an indexed READ uses); RPCS3 and the hardware
    use D0.dst_tmp. `patches/0021` fixes it (checked with a two-instruction
    program: ARL dst_tmp=1 now writes a1).
30. Shadow confirmed. Performance (run-20261018-225055): 55-60 fps in the
    first area, 25-30 on the cliff path, 6-14 at the beach. The beach frames
    carry ~585k vertices in ~550 draws (the grass area ~258k in ~690), all
    from static meshes the renderer re-decodes every frame. The audio SPU
    group (image 40) is started and joined ~185 times a second, independent
    of the frame rate -- one group per mix block -- and each restart printed
    five flushed console lines; `patches/0022` logs the first 16 of each and
    then every 4096th (PS3_SPU_LOG_ALL=1 for all). `run.ps1 -Profile` turns
    on the runtime's sampler (PS3_SAMPLE=3): every 10 s it reports the
    hottest guest functions, host modules, threads and runtime/renderer RVAs
    (resolved offline against folklore\build\folklore.map). Next step is
    a profile taken at the beach.
31. Beach profile (run-20261019-094217, PS3_SAMPLE=3): a flat 12.4 fps (five
    vblanks a frame). Only two threads are busy: the main guest thread (~40%
    lifted code, ~30% runtime code, ~30% waiting) and the renderer (~34%
    busy). Resolved against folklore.map, the main thread's runtime time is
    almost all the out-of-line guest STORE helpers (vm_write8/16/32/64 with
    their per-store diagnostics, null-page and coherence checks) -- a quarter
    of the thread. Loads were already inline; stores never were. The
    renderer's top costs are texture_content_hash (every bound texture fully
    re-hashed each frame) and per-draw vertex decode. `patches/0023`:
    inline guest stores (ppu_recomp.h / the lifter template; a store to a
    line an SPU has reserved, or with a store diagnostic armed, still goes
    out of line; PPU_NO_INLINE_STORES=1 turns it off), a four-lane texture
    hash, and a sampler addition that attributes each non-guest sample to
    the nearest lifted function on the stack ("[samp] wait/host tid N under
    func_X") so the main thread's waits are named. Checked locally: the
    Linux build with inline stores boots to the logo movie as before.
32. First Windows run with 0023 (run-20261019-100808, no profiler): 60 fps
    through most of the walk where it used to sag, ~28 on the cliff path,
    19.7 at the end. With -Profile the run died at boot: the new wait-site
    attribution scans every thread's stack, and a thread with no stack yet
    faulted -- the crash reporter is a vectored handler, so the __try never
    got to catch it. `patches/0024` bounds the scan with VirtualQuery.
33. Beach profile with 0023/0024 (run-20261019-101347): ~16 fps under the
    sampler (vs 12.4). The main thread's store helpers are gone from the
    profile (other=4 of 230 samples); it is now ~59% lifted code and ~40%
    waiting, the waits in cellGcmSys runtime code. (The "under
    func_004EEB64" lines are that runtime code: func_004EEB64 is the
    highest-addressed lifted function, and the sampler's extent guess for
    the last entry runs on into cellGcmSys.c.) Folklore calls
    cellGcmSetWaitFlip every frame, and the HLE blocked the calling thread
    until the next vblank -- the hardware call only queues a wait for the
    RSX. `patches/0025` adds GCM_WAITFLIP_NONBLOCKING=1 (run.ps1 sets it;
    -BlockingWaitFlip restores the old behaviour) and has the sampler also
    report the first runtime return address under each wait/host sample
    ("wait-rva"/"host-rva", for the link map).
34. Non-blocking was wrong for Folklore (run-20261019-102322): the logos
    ran at 164 fps -- the title paces on this wait -- and it bogged down at
    the Game Republic logo. Reworked: GCM_WAITFLIP_DEPTH=1 (what run.ps1
    now sets) keeps a count of flips issued and flips shown (drained to
    their marker and past a vblank, one per vblank in cellGcmTickFlip) and
    waits only while more than one flip is outstanding -- the next frame's
    CPU work overlaps the last frame's vblank wait, as on hardware, but the
    title can never run more than a frame ahead of the display.
35. With GCM_WAITFLIP_DEPTH=1 the logos run at normal speed and the beach is
    22 fps (21 under the sampler). Profile (run-20261019-102803): the main
    thread is now ~75% lifted code; the waits and store helpers are gone.
    The hot lifted code is VMX maths: ~10k vmaddfp sites, each a per-lane
    loop with a libm fmaf call per lane on baseline x86-64, and 1778 vperm
    sites emitted as a 16-iteration byte loop. `patches/0026`: SIMD paths in
    ppu_vmx.h for vaddfp/vsubfp/vmaddfp/vnmsubfp/vrefp/vrsqrtefp and a
    vmx_vperm helper (pshufb + blend), all bit-exact against the reference
    path (2M random cases incl. NaN/denormal/inf, both NJ settings; the
    lifter now emits vmx_vperm, and the 20 chunks with vperm were updated
    in place); FOLKLORE_CPU_LEVEL / PS3RECOMP_CPU_LEVEL (default
    x86-64-v3, set by CMakeLists/setup.ps1) so fmaf becomes one vfmadd and
    loads use movbe. Local Linux build boots to the same point as before.
36. First Windows run with 0026 (run-20261019-120051): froze on the
    install screen (the pulsing image), running at 60 fps with no SPU
    activity after the second SPURS kernel run. Workload 0 was left
    SHUTTING_DOWN with no SPU on it, the kernel had exited, SpursHdlr0
    waited for a runnable workload and the main thread waited in
    cellSpursWaitForWorkloadShutdown. Cause: PPU lwarx/stwcx. was emulated
    per word. libsre's cellSpursShutdownWorkload (0x3000A158) reserves the
    workload state, reads the SPU status byte 16 bytes away in the same
    128-byte line, then stores SHUTTING_DOWN or REMOVABLE; on hardware an
    SPU PUTLLC to that line in between fails the stwcx., here it committed
    against the stale status. A timing race, so occasional, and probably
    also the earlier "occasional SPURS boot hang". `patches/0027`: SPU
    PUTLLC/PUTLLUC/DMA PUT commits break every PPU reservation in the
    line (hook installed by the PPU runtime), and ppu_stwcx32/ppu_stdcx64
    re-check the reservation under the lock-line lock before committing.
    Locally the break fires on the SPURS line 0-7 times per boot (each one
    a commit that used to go through stale); 11 boots all reached the
    first movie. PPU_RESV_LINE_OFF=1 turns it off for comparison.
37. With 0027 the beach is ~25.5 fps (from ~22); the opening area ~50.
    `patches/0028`, two parts. (a) Every inline guest load and store in the
    lifted code checked four things against three globals (diagnostics
    armed, below 0x10000, at or above 0xE0000000, past the mapping, plus the
    gcm ref-poll word for loads and the SPU-reservation bit for stores).
    They are now one byte per 128-byte line, `g_vm_slow_lines`
    (spu_coherency.c): bit 0 sends a load out of line, bit 1 a store. The
    runtime marks the same regions (and the line before each, for accesses
    that run into one), and GETLLAR marks the reserved line for stores. The
    check becomes shift + test + branch; static instruction count of the
    hottest beach function (func_000116DC) drops 21%. ppu_recomp.h and the
    lifter template changed, so all lifted code rebuilds. (b) The [fps]
    heartbeat gains a second line, the average frame split into: guest
    blocked in cellGcmSetWaitFlip, present thread idle, CPU waiting on the
    GPU fence, and DXGI Present -- to tell whether the main thread or the
    render side is what holds the frame.
38. Run with 0028 (run-20261019-124954): beach 27 fps, 37 ms a frame;
    guest waitflip ~1 ms, present thread idle ~12 ms (busy ~25 ms, mostly
    vertex fetch/decode), gpu wait ~2 ms, swap 0.1 ms. -Profile run
    (run-20261019-125451) after ~85 s fell to 4.2 fps with the guest
    waiting ~194 ms per frame in cellGcmSetWaitFlip: the depth wait
    compared absolute counters (shown < flip request count), so once a
    flip went missing between issue and drain the shown count could never
    catch up and every frame ran into the 100 ms cap. `patches/0029`:
    shown advances by one per new drained request (relative), and a wait
    that hits the cap resynchronizes. The same profile shows the main
    thread at the beach spending most of its samples in sys_timer_usleep
    under func_00180E0C, the main loop: `while (elapsed() <= 4700) usleep
    (10000)`, elapsed in 10 us units (constant 1e-5 at TOC-0x494), i.e. a
    47 ms wait whose condition is r25 != 0. Whether that holds outside the
    sampler is the next question, so 0029 also adds a third [fps] line:
    the main thread's time per frame in usleep, other lv2 syscalls and HLE
    calls.
    Also in progress: folklore/tools/promote_regs.py, which keeps guest
    registers in C locals inside each lifted function (stores to
    ppu_context only before calls that take ctx and at returns, from a
    dirty-set dataflow over labels/gotos; reloads of the ABI-volatile set
    after calls). 12836 of 13563 functions convert; the converted build
    boots to the same point locally. Not delivered yet.
39. Run with 0029 (run-20261019-151715): beach 38.5 ms a frame; main
    thread usleep ~7 ms, HLE ~2.5 ms (waitflip ~1.5 of it), so ~29 ms of
    game work. One WaitFlip resync fired (issued 6688, requests 6685) and
    recovered. The usleep is the main loop's frame limiter in
    func_00180E0C: when [TOC-0x2B44] is non-zero, `while (elapsed() <=
    4700) usleep(10000)` (47 ms), else `while (elapsed() <= 3000)
    usleep(10000)` (30 ms), elapsed in 10 us units since the mark at the
    top of the frame. A 29 ms frame overshoots to ~39 ms. `patches/0030`:
    PS3_USLEEP_CLAMP=<lr>:<us>,... caps the sleep at named call sites;
    run.ps1 sets 0x00180F64:1000,0x00181344:1000 (-CoarseLimiter turns it
    off). The 3000 / 4700 constants are also the place a frame-rate unlock
    would start.
    Register promotion delivered: build.ps1 runs tools/promote_regs.py
    over src/recomp into build/recomp_promoted (only changed files) and
    builds that (FOLKLORE_RECOMP_DIR); -NoPromote builds as lifted.
    Callgrind, both builds side by side over the same 10 min of the local
    boot: PPU functions with 20+ calls run 14-19% fewer instructions with
    promotion (a few up to 15% more). Default mode reloads every promoted
    register after a call; --abi trusts the PowerPC ABI (volatile set
    only, CR fields 2-4 at returns).
40. With 0030 + promotion (run-20261019-152836): the beach holds the
    game's own 30 fps cap -- 31.6 ms a frame, ~8.6 ms of it the limiter
    sleeping, so ~21 ms of game work; the render thread is busy ~24 ms.
    Elsewhere 42-47 fps. Next limits: the 30 fps limiter itself, then the
    render thread.
    Engine menu (src/engine, Dear ImGui 1.91.9 in src/third_party/imgui):
    L3+R3 or F1 opens it; settings in folklore_settings.ini
    ($FOLKLORE_SETTINGS, set by run.ps1). Frame-rate cap (original 30 / 60
    / unlimited), precise limiter (1 ms wakes), performance overlay. The
    game's four main loops (func_0017AB20, func_0017D2D8, func_0017DC20,
    func_00180E0C) now call folklore_limiter_target() / _sleep_us() in place
    of their 3000 / 4700 and 10000 us immediates (tools/patch_limiter.py
    applied to src/recomp/ppu_recomp_013.cpp; re-run it after a relift), so
    the PS3_USLEEP_CLAMP of 0030 is no longer set by run.ps1.
    `patches/0031`: rsx_live_draw_set_overlay (a hook on the present thread
    with the frame in the backbuffer, RENDER_TARGET, before Present, both
    present paths); g_cellpad_block_guest_input (the game sees an idle pad
    while the menu is open); g_usleep_clamp_enabled; and
    rsx_vertex_fetch_many, the live renderer's vertex fetch done attribute by
    attribute in 128-vertex blocks with the format switch hoisted: 20000
    random plans byte-identical to rsx_vertex_fetch_one, 43 -> 9 ns per
    vertex for a 5-attribute layout locally. fetch/decode/remap was ~60% of
    the render thread's CPU samples at the beach.
41. 60 fps cap at the beach (run-20261019-181438, capture-20261019-181438):
    white triangle fans behind Keats, a blown-out sky, 47 -> 8 fps. The log
    shows the drain decoding floats as commands ("FIFO JUMP to unmapped IO
    0x1F000000 (word 0x3F000000)"), resyncing to `put`, and then the 100 ms
    WaitFlip resyncs. Cause: the game overwrote its 2 MB command ring ahead
    of the drain. Folklore writes a whole frame and publishes `put` once,
    just after its flip; nothing checked `get` in between. At 30 fps the
    game sleeps out each frame; uncapped it runs a frame ahead, and two beach
    frames do not fit. `patches/0032`: the runtime's ring shows the guest an
    `end` just short of the oldest unread command (gcm_ring_soft_end), so
    the game's own reserve check calls the buffer-full callback, which waits
    for the drain (cellGcm_fifo_reserve; now passed the word count). The
    ring wraps at the flip when a frame the size of the recent largest would
    not fit, `put` is published at the flip, and the drain publishes its
    position every 2048 words. The drain thread no longer recycles the
    runtime's ring (it rewound `current` under the guest). New [fps] line:
    largest recent frame and time the guest waited for the drain.
42. The pub (run-20261019-190707, capture-20261019-190707): 12-17 fps. The
    picture is right (409 draws a frame), but frames are 1.56 MB of commands
    (skinned characters upload their bone constants per draw), and the frame
    time is mostly waiting: 57 ms a frame with the game's render thread in
    cellGcmSetWaitFlip for 52 and the present thread idle for 33 (busy 24).
    No semaphore ACQUIREs at all, so the RSX is not waiting on SPU work.
    Six 100 ms WaitFlip resyncs, each of which let the game further ahead
    of the renderer for good. `patches/0033`: flip markers carry their flip
    number (bits 3-7), so a skipped marker is retired by the next one and the
    WaitFlip cap no longer forces the counters apart; flips are retired as
    soon as their frame is presented (cellGcm_note_frame_presented) rather
    than at the next vblank tick; publishing `put` at the flip and the ring
    back-pressure wake the present thread. New [fps] line: drain vs
    execute+present time on the present thread, flip latency game->drain and
    drain->shown, WaitFlip timeouts.
43. With 0033 (run-20261019-202714): the pub at ~40 fps. The present thread
    is now the limit: drain 22 ms a frame, execute+present 2 ms, idle 1 ms,
    and the game held for the drain ~20 ms a frame (which the overlay counted
    as "game" time). Profile (run-20261019-203308): of the present thread's
    samples, vertex conversion ~75% (rsx_vertex_fetch_many, remap_build, the
    index loop in fetch_batches_hoisted, rsx_live_guest_ptr),
    texture_content_hash ~10%, rsx_process_method ~5%. Logo movies and the
    opening cinematic ran too fast: 0033 retired every drained flip at
    present time, so flip-paced video was no longer held to one flip per
    vblank. `patches/0034`: flips retired at present only once per vblank
    (cellGcmTickFlip counts the ticks); rsx_geometry_cache.c, converted draws
    kept across frames, keyed by layout, formats, addresses, draw ranges and
    index contents and used only if the vertex spans they were decoded from
    still hash the same (LD_GEOM_CACHE=0 off, LD_GEOM_CACHE_MB budget, 384
    by default); [fps] line with its hits and misses. Overlay: the game's
    wait for the renderer (WaitFlip + ring) shown separately, render split
    into commands vs draw + present.
44. With 0034 (run-20261019-204951): 55-60 fps through the pub and on, drain
    9-11 ms (geometry cache ~95% hits). Reported: bloom blocky/stippled
    instead of soft; Keats walks with the stick in a corner; a freeze at the
    underworld portal (sound on, picture and game stopped, log silent after
    sound/work/w_040). Also in the log: the ring back-pressure let the game
    go twice after 150 ms while the present thread was compiling shaders,
    which can overwrite unread commands. `patches/0035`: RSX convolution
    filters (min 7, mag 4: quincunx/Gaussian, used for AA resolve and blur
    upscales) sampled bilinear instead of point, logged once; XInput sticks
    read as a pair (radial deadzone, round gate stretched to the DS3's
    square one); back-pressure gives up only when the drain completes 8+
    passes in 150 ms without moving (cap 5 s); hang watchdog dumps threads
    5 s and 20 s into any stall in presented frames.
45. 0035 never reached the game: setup.ps1 resets ps3recomp to the pinned
    commit, which leaves files a patch ADDED (0034's rsx_geometry_cache.c/h)
    in place as untracked files, so on the next setup `git apply` refused
    0034 and stopped; the old runtime library stayed. setup.ps1 now removes
    every file a patch adds before applying. Engine menu: save data
    snapshots (src/engine/engine_saves.cpp) -- copies of
    gamedata\dev_hdd0\home\00000001\savedata into save_snapshots\, restore
    with confirmation, the replaced saves kept as a snapshot.
46. With 0035 applied (run-20261019-211628): stick fixed. Bloom: the log
    confirms the bloom is magnified through the RSX convolution filter
    ("convolution filter on a 640x480 texture (min 7, mag 4, kernel 2)" --
    kernel 2 is Gaussian); bilinear softened it but left the texel grid.
    Freeze reproduced by opening the in-game menu (Select): the game reads
    menu_tex/LMenu.bi5_ (as it does at the title, where it then opens
    tk_batsu.dds_ and the rest) and stops; every host thread waits in
    ntdll, main thread polls usleep(10 ms), no WaitFlip timeouts, present
    thread idle. `patches/0036`: rsx_fp_apply_convolution -- the kernel in
    the shader for convolution-filtered units (Gaussian = four bilinear taps
    at half-texel diagonals = 3x3 [1 2 1]^2; quincunx = centre 1/2 + four
    diagonals at 1/8), part of the PSO key, LD_CONVOLUTION=0 to compare;
    the stall watchdog also prints every guest thread's call chain.
47. With 0036 (run-20261019-212756): bloom confirmed smooth. Froze entering
    the portal room, as movie_003 started. Chains: main (func_0021FC6C)
    calls EnvManager(1905, 0.25) and polls the voice's position every 1 ms
    until it moves; the voice is a disc stream whose FileOpen
    (func_0015C084, through the sound library's file table) never ran, so
    the position stays 0.0, the main loop never displays movie frames,
    movie_post waits for a free display buffer and cellVdec sits at 16
    queued pictures returning BUSY. Root cause: thread 64 was the last
    thread the game ever got -- here and in both earlier freeze logs.
    sys_ppu_thread_create ignored the flags and made every thread joinable,
    Folklore creates all of its threads (FadeOut, LoadThreadMain, stream_*,
    movie_lib) without SYS_PPU_THREAD_CREATE_JOINABLE and never joins them,
    so finished threads kept their slots and the 64-slot table filled after
    an hour of play; the next create returned EAGAIN and the game's error
    print is a no-op. `patches/0037`: non-joinable threads free their slot
    as their host procedure finishes (not in sys_ppu_thread_exit, where the
    thread is still running); ids carry a per-slot generation so an exited
    thread's id is never reused; guest stacks and TLS blocks of finished
    threads are reused (both were bump-allocated, ~2000 stacks / ~250 TLS
    blocks per session); join waits for the host thread before freeing;
    the reservation list dedupes reused contexts.
    PS3_THREAD_ALL_JOINABLE=1 restores the old behaviour. Unit test (Linux,
    3000 create/exit cycles, every tenth joined): all created, ids unique,
    no slots left, stack use bounded at ~15 stacks.
48. With 0037 (run-20261019-233331): through the portal cutscene, the game
    saved (cellSaveData SAVE BCUS98147-SAVEDATA-0000), Keats' prologue
    plays. Reported: a hitch the first time each effect plays, the beams
    when absorbing a defeated Folk missing, 52-58 fps in action, 30 fps in
    the story-event scenes. Found: (1) every new shader pair was compiled
    with fxc on the present thread inside the frame (32 pixel + 12 vertex
    misses that run); (2) the present waited for the GPU to finish each
    frame before decoding the next (ld_flush at present: CPU and GPU never
    overlapped); (3) the 60 fps limiter target of exactly 16.66 ms plus its
    wake slack missed vblanks, ~56 fps; (4) func_0017ADB0 (the event-scene
    loop) has two limiter sites with the sleep BEFORE the compare, which
    tools/patch_limiter.py missed, so those scenes stayed at 30; (5)
    BLEND_COLOR was never set (D3D12 blend factor stayed 0, so draws
    blending by CONSTANT_COLOR/ALPHA drew nothing) and polygon offset was
    ignored -- both candidates for the missing beams. `patches/0038`:
    background PSO builds (TrySubmitThreadpoolCallback; draws needing a
    pending PSO are skipped, shader caches under an SRW lock), hashed PSO
    lookup, frame pipelining (rings split into two banks, two allocators,
    per-bank retire lists, sampler tables fence-stamped; ld_submit_frame
    waits only for the frame before; LD_NO_PIPELINE=1 restores), render
    scale 1-4 (surfaces/zetas/snapshots/shared depth at scale x guest size,
    viewport/scissor/copies scaled, WPOS divided in the FP HLSL, slope bias
    scaled; LD_RENDER_SCALE overrides), a present pass (full-screen
    triangle: bilinear / FXAA / box downsample, letterboxed) with the swap
    chain following the window's client area, movies through it too,
    BLEND_COLOR(2) -> OMSetBlendFactor, POLY_OFFSET_FILL -> DepthBias /
    SlopeScaledDepthBias. Port: engine menu Graphics section (render
    resolution, FXAA, background shaders), 60 fps target 16.0 ms (the flip
    paces it), patch_limiter.py handles sleep-before-compare loops
    (ppu_recomp_013.cpp: 11 sites now), ImGui frames in flight 3.
49. Cloth and hair freaking out above 30 fps (RPCS3's wiki: "cloth physics
    break above 50 FPS"). Every Havok world is stepped through func_003C913C
    (from func_0004F188): setFrameTimeMarker(frame time), then
    stepDeltaTime(1/30) until the simulation reaches the marker. The cloth
    and hair objects (32 slots of 0x75B0 bytes, updated by func_0002BA64,
    callers pass 30 Hz, a few 60 Hz) drive their anchor bodies with hard
    keyframes computed for the frame time (func_0001FE14). At 30 fps that is
    one step per frame; above it some frames take no step and others a 1/30
    step with a velocity meant for a shorter frame, so the anchors overshoot
    and snap back and the cloth hanging off them whips around (a model of
    the loop: anchors up to 6 frames off at 60 fps with 0.8 ms of jitter,
    diverging at 90+). game_patches.py patch 0x3C913C now steps exactly the
    time to the marker, in equal steps of at most 1.15x the game's step
    (folklore_physics_plan_steps in engine_settings.cpp): one step of the
    frame time per frame, the original single 1/30 step at 30 fps, no step
    under 2 ms (carried to the next frame). Engine menu: Physics, "Step
    physics by the frame time" (physics_frame_step, default on).
50. Anisotropic filtering and sharpening (patches/0039). decode_sampler
    turns mip-mapped samplers that are linear for both min and mag into
    D3D12_FILTER_ANISOTROPIC with the chosen max anisotropy (render targets
    and point-sampled lookups unchanged); a change rebuilds the sampler
    cache and its descriptor tables at the next frame boundary after
    ld_wait_idle. The present pass gains contrast-adaptive sharpening
    (presentOptions.y, after FidelityFX CAS) in all three variants -- FXAA
    sharpens only the pixels it does not smooth -- and is used whenever
    sharpening is on; movies pass 0. Engine menu Graphics: anisotropic
    filtering (ini anisotropic_filtering, default 16) and sharpening (ini
    sharpening 0/30/60/100, default off). HLSL checked with glslang.
51. Bloom breaking up into stripes at 3-4x render scale (patches/0040).
    Folklore's surfaces (run-20261020-103039): 1280x720 scene and display,
    2560x720 AA, 2048x2048 shadow map, 640x480 x2 (bloom, Gaussian
    convolution) and 256x256 x2. The blur steps its taps by guest texels, so
    at 3x each tap covered a third of its footprint. Colour surfaces smaller
    than the canvas in either dimension now stay at 1x (surface_t.scale,
    ld_surface_scale_for); viewport/scissor, WPOS, slope bias
    (render_state_t.draw_scale, keyed), copies, readback and present use the
    surface's own scale. Blits between surfaces of different scales are not
    copied (logged); a depth-tested draw into a 1x target with a scaled zeta
    is logged. Zetas still all scale. LD_SCALE_ALL_SURFACES=1 restores the
    old behaviour.
52. Motion controls (patches/0041 + folklore_hle). Every capture finished
    by itself: Folklore's cellPadSetSensorMode HLE returned OK without
    recording SENSOR_ON, so cellPadGetData left button[20..23] 0, and the
    pad reader (func_001CCF30, 0x1CD550) stores word - 512 clamped to
    -512..511 -- full scale on every axis, a gesture completed at once.
    The runtime now implements cellPadSetSensorMode/InfoSensorMode (the HLE
    forwards to them) and reads a DualShock 3 over HID on its own thread:
    054C:0268 via SetupAPI, feature report 0xF2 (else 0x00) as in RPCS3's
    ds3_pad_handler Windows path (DsHidMini SXS), buttons/sticks/pressure
    into port 0, accel X/Y/Z + gyro (LE u16 at report +41/+45/+43/+47) into
    the sensor words. cellPad_set_motion_mode: 0 automatic (DS3 sensors,
    else the old all-zero words), 1 SIXAXIS only (else at rest), 2
    auto-capture. Engine menu Controls section (ini motion_controls); the
    menu itself reads the DS3 for L3+R3 and navigation. PAD_NO_DS3=1 turns
    the HID reader off.
53. Ultrawide (patches/0042 + folklore_apply_aspect). The game builds its
    perspective projections inline from a 16:9 constant in seven TOC slots
    (-28188, -16228, -13536, -13364, -6676, +316, +4360: m00 = cot / aspect,
    e.g. func_00145B20 at 0x145D70); at the first frame end they are set to
    the active aspect (Hor+). The renderer makes canvas-sized surfaces and
    zetas wider by aspect / (16:9) (g.widen, surface_t.scale_x; viewport,
    scissor, copies, WPOS per axis), so the horizontally squeezed guest frame
    is rendered at full resolution and presented at the target shape. 2D is
    stretched. Engine menu Graphics: aspect ratio (ini aspect_ratio
    16:9/display/21:9/32:9; LD_ASPECT overrides), next launch.
54. Capture beam (soul tether) still missing. Not dropped: no unsupported
    primitives, PSO drops only during shader warm-up. The capture shaders
    take fCapturePercentMovement / fCapturePercentColor, IN.position1/2,
    weight1/2, bone1/bone2 and boneMatrix/boneMatrixLast (parameter table
    at 0x1000D930, func_001A8268), i.e. a two-bone vertex program with
    indexed constants. Needs a capture during an absorb: F2 / PS button
    now starts one, and run.ps1 dumps every shader pair's HLSL to the
    capture's hlsl\ folder.
    Capture 20261020-122946 (F2 during "Flick the controller up!"): the
    tether IS drawn -- three tri-strip ribbons (pso c1c874d8753b813a, the
    generic VFX shader also used by the glow quads, 254 vertices each,
    world x 1288.8..1293.6 / y 7.1..8.4 / z -23.7..-22.3, i.e. from Keats'
    hand to the Id, colour FFFFFFBF, UV 0..1, texture 0x099FCF80 DXT5
    256x32, additive SRC_ALPHA/ONE, alpha test GEQUAL 5/255, depth LEQUAL
    no write, cull off) into the effects buffer 0x1B40000, plus 80 instances
    of the soul-particle mesh (53dbf9f98846bdda) after them. None of them
    changes a pixel (draw dumps i0192 vs i0256: 13 pixels), while the same
    shader's quads at either end do. Not dropped (no PSO drops in those
    frames), vertex data correct. patches/0043 adds LD_PROBE_PSO /
    LD_PROBE_AB (run.ps1 sets them for these two keys): probe.txt with the
    draws' constants, texture units and texel summary, and a four-frame A/B
    of the ribbons (as asked / no depth test / no alpha test / white
    texture) visible in the capture's flip images.
    Capture 20261020-170010 (three absorbs): the c1c874 ribbons are not the
    tether. The VS run offline on probe constants puts them on Keats' hand
    (world extent 2-5 units, rebuilt every few frames), and the flips show
    them there as the swirl around the glowing hand; none of the A/B
    variants adds a beam. The draws that follow an absorb and DO depend on
    something broken are the depth-faded ones: 58c818bae4441487 (additive,
    a 9.6-unit-long mesh into the scene) and 877f5ce684d6643d (the same
    mesh, refracting the scene into the effects buffer) bind the zeta
    0x22C0000 as an A8R8G8B8 texture (format 0xA5) and rebuild depth from
    its bytes (dot(tex.wxy, k)) for a soft fade. The renderer only took the
    depth snapshot for DEPTH24 formats, so those units read guest VRAM,
    which host depth never reaches: zeros fade the effect out entirely, and
    stale bytes turn it into speckle (the "TV static" on the soul).
    patches/0044 binds the depth snapshot for an ARGB8 read of a written
    zeta and rewrites those Sample() calls to rebuild the Z24S8 bytes
    (A = depth bits 23-16, R = 15-8, G = 7-0, B = stencil 0);
    LD_NO_ZETA_ALIAS=1 turns it off. Confirmed on Windows: the beam is
    back. run.ps1 no longer sets the probe.
55. Doolin at night drops to 27-49 fps (run 20261020-195739, capture at
    frame 9898). Not the GPU: the present thread's drain takes 19-32 ms a
    frame and holds the guest for up to 25 ms. The frame has 2,134 draws
    (1,545 of them the 48-vertex particle mesh 53dbf9f98846bdda) against
    ~800 elsewhere, at ~15 us of renderer CPU each. Every draw also had
    eight texture units enabled from stale state while its shader samples
    one or two, and each enabled unit cost a linear walk of the texture
    cache (630 entries by then) plus a content-hash check; and every draw
    rescanned and FNV-hashed its whole vertex program two or three times.
    patches/0045: texture units the fragment program does not sample are
    not bound (rsx_fp_texture_units, kept per PSO; LD_ALL_TEXTURE_UNITS=1
    restores binding them all); the texture cache has a hash index; the
    vertex program's size, TXL mask and hash are memoised per start slot
    and rsx.vp_generation (bumped on every program upload). The [fps]
    report gains a line splitting the renderer's per-draw time (vertex
    fetch / shader lookup / textures / constants+state+draw) so the next
    capture shows what is left. Same patch: borderless fullscreen
    (Alt+Enter, engine menu Graphics, ini fullscreen).
56. Running without run.ps1, and sharing (patches/0046, engine_portable.cpp,
    package.ps1). The boot harness calls ps3_host_bootstrap before loading
    the ELF: it fills in run.ps1's environment where unset, keeps hdd0/hdd1,
    gamedata (saves), the shader cache and logs in one data folder (beside
    the exe or data_dir; the working directory is set there), logs to
    logs\run-*.log and frees the console when nothing captures the output,
    and finds the EBOOT from the command line, game_dir, or a folder picker
    (checked: PS3_GAME, decrypted ELF, BCUS98147 in PARAM.SFO, 6,518,936
    bytes). run.ps1 sets FOLKLORE_LAUNCHER and falls back to the exe's own
    choice when its default dump path is absent. Escape no longer quits
    (PS3_ESCAPE_QUITS=1 restores it). package.ps1 -Kind Binaries copies the
    exe, FFmpeg and VC runtime DLLs and libsre_image.bin; -Kind BuildKit
    leaves out src\recomp, src\spu_gen*, analysis and firmware_lle, and
    setup.ps1 -GameDir/-LibsrePrx regenerates them through Git Bash
    (tools/relift.sh now also runs patch_limiter.py, and KEEP_GEN=1 keeps the
    shipped src/gen). Verified in the cloud: relift + patch_limiter
    reproduce src/recomp, src/gen, src/spu_gen and src/spu_gen_lle exactly;
    lift_firmware.sh reproduces firmware_lle except libsre_recomp.h, which
    the current lifter emits with the newer inline accessors.
57. Crunchy audio in loud moments (two captures at once). cellAudio summed
    the ports and folded Folklore's 7.1 port (nChannel 8: L/R + 0.707 C +
    0.5 LFE + 0.5 rear + 0.3 side) to stereo in float, then clipped flat at
    +-1.0 -- loud overlaps distorted. And the mixing thread, after any late
    wake-up, refilled its 20 ms WASAPI buffer in one burst, reading the
    game's blocks faster than its SPU mixer writes them (gaps). patches/0047:
    a peak limiter on the final mix (1 ms look-ahead, ~0.25 ms attack, 80 ms
    release, threshold 0.97; transparent below it; AUDIO_NO_LIMITER=1 turns
    it off); a 100 ms device buffer kept ~32 ms full (AUDIO_LATENCY_MS) and
    refilled at no more than 1.25x real time; the mixing thread registered
    with MMCSS "Pro Audio". The log gets a [cellAudio] line every 10 s when
    the device ran dry, the game was late with blocks, or the limiter acted,
    which tells the remaining causes apart.
58. Replacing libsre, step 1: a golden trace of the real one. Folklore
    needs Sony's libsre (lifted from the user's firmware) for its SPU work,
    which is the one thing the repository cannot ship. It uses only ten
    cellSpurs calls (Initialize/Finalize, Attach/DetachLv2EventQueue, the
    job-chain Create/Kick/Shutdown/Join, JobGuardInitialize/Notify) and nine
    job binaries from its own EBOOT, so a native job-chain runner can stand in
    for libsre. patches/0048 (runtime/spu/spurs_trace.c) records what the real
    one does, as the reference to write and check that runner against:
    SPURS_GOLDEN_TRACE=<file> (run.ps1 -TraceSpurs) logs every libsre call
    with its registers and the 512 bytes behind each pointer argument before
    and after; every SPU GET/PUT and lock-line atomic with who issued it (the
    SPURS kernel, the job-chain policy module, or a job), its address, size,
    a content hash and the bytes the first time each (address, size, content)
    appears; and every job entry and exit with the job context and descriptor
    in LS. Polling loops fold into REPEAT lines. Off, each hook is one load
    and a branch.
59. Replacing libsre, step 2: what the trace says. folklore/docs/SPURS.md
    has the details. Folklore uses one SPU and, after 13 short-lived boot
    chains in the first 1.2 s, a single job chain for the whole session: a
    guard, then seven jobs (five binaries) with SYNCs between them, then a
    jump back. The game releases one pass with cellSpursJobGuardNotify, about
    190 times a second (~1300 jobs/s). The jobs do all their own DMA; the
    job manager only fetches the descriptor and the binary (packed in LS from
    0x4C00 in order of first use, which is where the lifted placements come
    from) and calls it with the context at 0x48F0 and the descriptor in r4.
    libsre also serves the game's cellSyncMutex calls. The full trace is far
    too big for a session (17 MB/s, 4 GB in four minutes), so patches/0049
    adds a shape mode, now the default: each kind of event is written once
    in full and then counted once a second. That includes a job run with its
    entry registers, context, descriptor, stack and LS map, and the manager's
    and kernel's operations before it. It also records SPU mailbox sends. A
    session's steady state comes to a few lines a second.
60. Replacing libsre, step 3: the second trace (shape mode) gave the job
    environment. Stack: 16 KB above a working-memory area sized by the
    descriptor's u16 at +0x1E. Nothing is cleared. The jobs throw events to
    queue 3 on port 0x11; the last job does it once a pass, which is likely
    how the game learns a pass is done. All in folklore/docs/SPURS.md. It
    still filled 1 GB in 2.5 minutes because libsre also serves the game's
    cellSyncMutexLock/Unlock (~260/s), whose argument dumps were not folded.
    patches/0050 folds every call except the job-chain setup ones into
    counts, and caps the shape table so a mistake can no longer flood the
    file.
61. Replacing libsre, step 4: the port's own SPURS. A shape trace of
    Keats' first chapter showed nothing new; the job setups were identical
    across sessions. So src/hle/spurs_native.cpp and spurs_native_job.c now
    answer Folklore's ten cellSpurs calls:
    - a host thread per kicked chain walks the command list (GUARD / JOB /
      SYNC / NEXT);
    - each job is set up exactly as the traces show jm2 doing it, then the
      lifted job runs;
    - thrown events go through the runtime's SPURS port table.
    run.ps1 -NativeSpurs selects it. Builds without firmware_lle get it
    automatically, and src/spu_gen_lle (the jobs, lifted from the EBOOT) is
    now compiled in every build. Untested in play as of this entry.
62. The port now runs on its own SPURS by default, and needs no firmware.
    The first native run (entry 61) played through Ellen's chapter 1 at 60
    fps with no chain errors or dropped events. The main thread's time in
    library calls went from 4.6 to 0.6 ms per frame, since the lifted libsre
    PPU code no longer runs. So:
    - setup.ps1 no longer asks for libsre.prx; -LibsrePrx / -WithLibsre
      still lift it, for comparison;
    - libsre_load.cpp loads Sony's library only with FOLKLORE_SPURS=libsre
      (run.ps1 -Libsre);
    - package.ps1 no longer copies libsre_image.bin;
    - README's requirements are down to the disc and the decrypted EBOOT.
    The native handlers also go through SPURS_GOLDEN_TRACE now, so
    -TraceSpurs records a native run in the same form as a libsre one.
63. Native SPURS checked against libsre, job by job. Keats' chapter 1 was
    traced on the native SPURS and compared with the earlier libsre trace of
    the same chapter. All 19 kinds of job entry match: binary, LS
    placement, r1, context, descriptor and stack frame. Two cosmetic
    differences were fixed:
    - the dynamic event port is now 0x11, as libsre's;
    - the two-job load chain no longer puts its first job's working memory
      above the previous job's binary after a wait at the guard, which
      libsre only did once.
64. folklore.exe on its own, fixed. Double-clicked, it drew the title
    screen wrongly and stopped at a black screen, while the same exe in the
    same fresh folder played normally under run.ps1. The exact cause in the
    single-process start was not found. Now the double-clicked exe acts as
    its own launcher (engine_portable.cpp): it sets run.ps1's whole
    environment and starts itself again as a child, with the data folder as
    its working folder and output to logs\run-<stamp>.log. The child is in a
    kill-on-close job. That plays normally. Closing the window used to only
    stop presenting, so a game with no visible console kept running and
    playing audio. patches/0051 ends the process when the window closes.
65. The European release (BCES00050 v1.00) is supported, from a dump the
    user provided. Aligning the two executables function by function
    (folklore/docs/REGIONS.md) showed the same code with small shifts:
    13,284 of 13,298 functions match. Ten places differ in content, one of
    them the movie set-up the AU-table fix patches. The TOC is laid out a few
    entries differently, and the nine SPU job binaries are byte-identical.
    - tools/relift.sh recognises the release by EBOOT size, uses its code
      end and switch-table hint, and writes src/recomp/folklore_region.h.
    - game_patches.py takes --region, with each fix's function, anchors and
      TOC offsets per release. US output is byte-identical to before.
    - The aspect-ratio camera constants are per release (folklore_hle.cpp).
    - The exe's title and size check follows the build's release.
    - setup.ps1 accepts both, and relifts when -GameDir is the other
      release.
    - Binaries packages are named by release.
    Lifted in the cloud: all five fixes and the ten frame-limiter sites
    apply to the EU code. Untested in play as of this entry.
66. A language setting for the European release. The game asks for the
    console language once, as it starts (cellSysutilGetSystemParamInt,
    CELL_SYSUTIL_SYSTEMPARAM_ID_LANG), and the runtime always answered
    English. The EU executable keeps CELL_SYSUTIL_LANG 1-5 (English, French,
    Spanish, German, Italian) and turns anything else into English. Its
    text, subtitle, movie and sound loaders pick their files by that value,
    from a suffix table of _jp/_uk/_fr/_sp/_ge/_it (folklore/docs/REGIONS.md).
    - folklore_hle.cpp answers the language query from the engine menu's
      new Language setting (`language =` in folklore_settings.ini) and hands
      every other setting to the runtime.
    - The game reads it once, so a change applies on the next launch. The
      menu says so when the setting and the running language differ.
    - The US build is always told English. Its code accepts the same five,
      but its disc has only the English subtitle files.
    Untested in play as of this entry.
67. Motion controls on any controller that has them, and one switch for
    them. The menu's three-way choice (Automatic / SIXAXIS / Auto-capture)
    tied motion to the DualShock 3. It is now one setting, Motion controls,
    on or off (`motion_controls = on|off`; the old values still load):
    - on: the connected controller's motion sensors drive the gestures; a
      controller without them captures by itself (cellPad motion mode 0);
    - off: every capture completes by itself (mode 2).
    patches/0052 reads a DualShock 4, DualSense / Edge or Switch Pro
    Controller over HID, USB or Bluetooth. None of them is an XInput device,
    so before this they were not seen at all. Each gives buttons, sticks and
    motion on port 0, as the DS3 does (a DS3 wins if both are connected).
    - Report layouts and set-up follow SDL's HIDAPI drivers. A Bluetooth
      DualShock 4 / DualSense is switched to its full report by reading its
      calibration feature reports. A Switch Pro gets the USB handshake, IMU
      on, its stick calibration read from SPI, and full report mode 0x30.
    - Sensors become the SIXAXIS words the way RPCS3 converts a DualShock 4:
      113 per g and 123/90 per degree/s about 512, axes negated. The Switch
      Pro's axes are turned into the DualShock 4's frame first. A test
      against built reports checks that the same pose gives the same words on
      both.
    - The sticks go through the XInput path's round-to-square gate mapping
      (entry on Keats walking at diagonals), with a smaller deadzone.
    Untested on real controllers as of this entry; the sign of each axis is
    the one thing most likely to need a flip after a real capture.
68. No sound on some PCs (user report, EU release, logs 20261027-103446/
    -103736): `[cellAudio] WASAPI Initialize failed: 0x88890008`, then the
    game ran on silently. 0x88890008 is AUDCLNT_E_UNSUPPORTED_FORMAT: the
    port asked for 48 kHz stereo float in shared mode, which Windows only
    accepts when it is the device's own mix format -- a device at 44.1 or
    96 kHz, or set up as 5.1/7.1, refused it. patches/0053 asks for
    AUTOCONVERTPCM | SRC_DEFAULT_QUALITY so Windows converts the rate and
    channel count (the old requests stay as last fallbacks), logs the
    conversion, and on failure logs the device's mix format.
69. The 1.10 update, both regions (folklore/docs/REGIONS.md, "The 1.10
    update"). A third and fourth build target: relift.sh knows the two
    update EBOOTs by size (US 6,520,504, EU 6,520,480 bytes decrypted),
    game_patches.py has their addresses ("US-1.10", "EU-1.10"), and
    folklore_region.h now records the version. All five game patches and
    the 10 limiter sites apply to both lifts; the touched functions are
    unchanged from 1.00, only moved. A 1.10 build loads the update folder's
    EBOOT (update_dir / run.ps1 -UpdateDir / a second picker), reads the
    rest from the disc dump, and on each start copies the update's three
    data files (LMonS2.bi5_, the Spanish fix, their list) into hdd0 and
    records them in install.dat_, as the game's own installer would.
    setup.ps1 -UpdateDir lifts it; package_all.ps1 -UsUpdateDir/-EuUpdateDir
    builds the two extra copies. Rumble, which 1.10 adds (its Vibration
    option), goes to XInput pads, DualShock 4 and DualSense
    (patches/0054); the DS3's on/off small motor had been sent to XInput as
    257 of 65535. Lifted and patched here; not yet built or run on Windows.

`run.ps1 -Libsre` runs on Sony's libsre instead, for comparison (entry 62). Locally (stand-in
data), HLE crashes in `sgx-audio-thr` within 25 s; the real libsre runs on to
the logo movie open, which needs the real movie files.

Still open (HLE path): the `sgx-audio-thr` thread (tid 8) calls through garbage function
descriptors in the sound library's heap (r2 ends up 0x4503xxxx). It happens on
Windows with real data too, so it isn't caused by the stand-in data.

All seven are general fixes, worth sending upstream once confirmed on Windows.

## Next steps

1. Windows run with the real libsre: logo movies, then the title screen.
2. Lift any job placements the log reports as interpreted.
3. Logos → title screen. Watch for unlifted SPU job misses (`logs\spu_miss`),
   cutscene decode (cellVdec/FFmpeg) and audio streaming.
4. Menus → gameplay → SIXAXIS gesture mapping.
