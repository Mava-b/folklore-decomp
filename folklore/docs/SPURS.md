# What Folklore asks of SPURS

Folklore's SPU work runs on Sony's SPURS library (libsre). It is the one thing
this project cannot ship, so setup asks every player for their own decrypted
`libsre.prx`. This file collects what the game actually uses, from traces of
the real library (`run.ps1 -TraceSpurs`, `ps3recomp/runtime/spu/spurs_trace.c`),
as the specification for a replacement written here.

Source: the first golden trace, 2026-10-21. It is a full-mode recording of
boot through the first minutes of play, which hit its 4 GB limit after 242
seconds. The first 27 seconds and seconds 212–242 were examined.

## Summary

Folklore uses SPURS in the simplest way it can be used:

- **One SPU.**
- **One job chain**, recreated 13 times during the first 1.2 seconds of boot,
  then kept for the rest of the session.
- **Seven jobs in a fixed loop**, released one pass at a time by a guard that
  the game notifies about 190 times a second.

The jobs move their own data. The job manager only fetches each descriptor
and job binary, lays out local store, and calls the job. A replacement needs
13 PPU functions and a small job-chain runner on the SPU side. The lifted jobs
run unchanged.

## PPU calls

| Call | Arguments seen | Notes |
|---|---|---|
| `cellSpursInitialize` | spurs `0x10A60C00`, nSpu **1**, spuPriority 0x15, ppuPriority 0x5A, exitIfNoWork 1 | once, at boot |
| `cellSpursAttachLv2EventQueue` | queue 2, port from `*0x0FEFF720` (= 1, static) | once |
| `cellSpursAttachLv2EventQueue` | queue 3, dynamic port; libsre wrote **0x11** to `0x4501701C` | once; whether anything is ever sent on it is open |
| `cellSpursCreateJobChain` | chain `0x45016E00`, command list `0x4501B088`, sizeJobDescriptor 0x80, maxGrabbedJob 0x10, priorities `{1,1,1,1,1,1,1,1}`, maxContention 1, autoReadyCount 1 | 14 times in boot, then never again |
| `cellSpursJobGuardInitialize` | chain, guard `0x45016F80`, notifyCount 1, requestSpuCount 1, autoReset 1 | after every create |
| `cellSpursKickJobChain` | chain, numReadyCount 1 | after every create |
| `cellSpursJobGuardNotify` | guard `0x45016F80` | ~190/s; one per pass of the chain |
| `cellSpursShutdownJobChain` / `cellSpursJoinJobChain` | chain | end of each boot chain; Join returns at once |
| `cellSyncMutexInitialize` / `Lock` / `Unlock` (NIDs `A9072DEE`, `1BB675C2`, `91F2B7B0`) | various | libsre exports these too; ~45 lock/unlock pairs a second |

`cellSpursFinalize` and `cellSpursDetachLv2EventQueue` are imported but were
not called.

## The command lists

All lists live at `0x4501B088`. Each is a guard, then jobs separated by
SYNCs, then a jump back to the start:

```
boot, chains 1–13:  GUARD 45016F80 ; JOB 45017080 ; SYNC ; NEXT 4501B088
                    GUARD 45016F80 ; JOB 45017080 ; SYNC ; JOB 45017100 ; SYNC ; NEXT 4501B088
steady state:       GUARD 45016F80 ; JOB 45017080 ; SYNC ; JOB 45017100 ; SYNC ; JOB 45017180 ; SYNC ;
                    JOB 45017200 ; SYNC ; JOB 45017280 ; SYNC ; JOB 45017300 ; SYNC ; JOB 45017380 ; SYNC ;
                    NEXT 4501B088
```

The encoding is 64-bit big-endian words, opcode in the low 3 bits. A JOB word
is the descriptor's address. A SYNC word is 2. NEXT is the target address |
3. GUARD is the guard's address | 0xF.

## The guard

The guard sits at `0x45016F80`: u32 count, u32 reset value, u64 chain, then
words the game does not touch.

`cellSpursJobGuardNotify` decrements the count. At zero the chain may pass
the GUARD command. With autoReset, passing it puts the count back to the
reset value (1). In 5084 notifies the count was 1 every time before the call.
The game waits for each pass to finish before notifying again; it watches a
completion word that the last job writes. In 47 of those notifies the SPU had
already passed the guard and reset the count before the call returned.

Shutdown makes a chain that is waiting at its guard end, and Join waits for
that.

## A job run

For each JOB command, the job manager:

1. takes the chain's lock line (`0x45016E00`) and reads the command list line;
2. GETs the descriptor (0x80 bytes) into LS `0x3F400`–`0x3F700` (rotating);
3. GETs the job binary (`sizeBinary` × 16 bytes from `eaBinary`) to its LS
   address;
4. waits for all its DMA tags and calls the binary's first instruction with:
   - `r3` = the job context at LS `0x48F0`;
   - `r4` = the descriptor in LS;
   - `r0` = the return address in the manager (`0x24B0`);
   - `r1` = the job stack (below);
   - `r5` = `0xFFFFFFFF`; `r6` is left over from the manager.

The job context is all zero except:

| Offset | Value |
|---|---|
| +0x70 | the end of the binary's area: base + round-up(size, 1 KB) |
| +0x78 | the descriptor's EA (u64) |

**Where binaries go in LS.** They are packed upward from `0x4C00` in the
order a chain first uses them, each rounded up to 1 KB. In steady state:

| Binary (EA) | Size | LS base | Lifted image |
|---|---|---|---|
| `10079E00` | 0xD190 | `04C00` | 50 |
| `10097B80` | 0x6100 | `12000` | 58 |
| `100A2800` | 0x24D0 | `18400` | 61 |
| `10094500` | 0x3600 | `1AC00` | 56 |
| `10087000` | 0x8E60 | `1E400` | 53 |

The boot chains run each of the nine binaries alone at `0x4C00` (images 50,
51, 54, 55, 57, 59, 60, 62, 63), and job `10087000` at `0x12000` (image 52)
in the two-job chain. The binary is fetched again for every run, because a
job's working memory past its binary can overwrite whatever is loaded above it.

**A job's local store**, from its base upward:

| Area | Size |
|---|---|
| binary | round-up(sizeBinary × 16, 1 KB); context +0x70 points past it |
| working memory | round-up(descriptor u16 at +0x1E × 16, 1 KB) |
| stack | 16 KB; `r1` = its top − 0x30 |

At `r1` the manager leaves four words:

- `r1 + 0x20` (the back chain);
- `0x4020`;
- (top − context +0x70) + 0x20;
- `0x20`.

Nothing is cleared: working memory and stack hold whatever was there before.
For example:

| Binary | Base | +0x70 | Working memory | Stack top | `r1` |
|---|---|---|---|---|---|
| `10079E00` | `04C00` | `12000` | 0x1FD2 → 0x20000 | `36000` | `35FD0` |
| `10097B80` | `12000` | `18400` | 0xE8C → 0xEC00 | `2B000` | `2AFD0` |
| `100A2800` | `18400` | `1AC00` | 0x600 → 0x6000 | `24C00` | `24BD0` |
| `10094500` | `1AC00` | `1E400` | 0x814 → 0x8400 | `2A800` | `2A7D0` |
| `10087000` | `1E400` | `27400` | 0x10A1 → 0x10C00 | `3C000` | `3BFD0` |

**Descriptors.** These are CellSpursJob128. Header: eaBinary, sizeBinary,
and nothing else non-zero up to +0x1C. At +0x1C: `0x04`, then at +0x1E the
working-memory size in 16-byte units (0 in the boot chains). User data
starts at +0x30:

| Offset | Contents |
|---|---|
| +0x44 | `0x0003000N` |
| +0x4C | a per-slot block at `0x4501B9xx` |
| +0x54 / +0x5C | buffer EAs |
| +0x64 | the EA of a completion word the job writes (4 bytes) when it is done |

No descriptor uses the manager's input/output/cache DMA lists (sizeDmaList 0,
no in/out buffers): **the jobs do all their own DMA.** Over 38,000 job runs
there were only 35 distinct descriptors.

**Inside a job.** The lifted jobs use only:

- DMA and tag waits;
- the decrementer;
- **events to the PPU**: `sys_spu_thread_throw_event`, i.e. channel 28 =
  data1, then channel 30 = `0x51000000 | data0`. That is port 0x11, the
  dynamic port `cellSpursAttachLv2EventQueue` gave queue 3.
  - In steady state, job `10087000` (the last in the loop) throws data0 6,
    data1 0 once a pass, about 188 a second. It is very likely how the game
    learns that a pass is done.
  - In boot, every binary throws data0 0 with a size-like data1.
  - Job `10087000` at `0x12000` throws data0 1.

The manager itself writes channel 28 = 0 (with no interrupt) about once a
pass, at LS `0x29B8`.

The jobs never call into the job manager. They return through `r0`.

## What a replacement needs

1. **PPU.** The 10 cellSpurs functions and 3 cellSync functions above, as
   native code serving those NIDs. ps3recomp already has HLE cellSync.
2. **SPU.** One worker with an SPU context that, per kicked chain, walks the
   command list:
   - GUARD: wait for a notify, or exit on shutdown;
   - JOB: GET the descriptor, place and GET the binary, set up the context,
     `r1` (and the frame there) and `r0`, and run the lifted job;
   - SYNC: nothing to wait for on one SPU;
   - NEXT: jump.

   The jobs' thrown events have to reach queue 3 as lv2 SPU-thread user
   events on port 0x11.
3. **Checks.** Fail loudly on anything outside this:
   - another opcode;
   - descriptor DMA lists;
   - a second chain;
   - a binary with no lift at the place it lands.

The shape trace (below) is the reference to compare against.

## Still open

- Whether later parts of the game use other chains, lists or binaries
  (menus, cutscenes, the Netherworld realms). The second trace (shape mode,
  2026-10-21) answered the stack, LS and event questions above, but filled
  its 1 GB in 2½ minutes: libsre also serves the game's cellSyncMutex calls,
  and their memory dumps were not folded. patches/0050 folds them.

## Recording

```powershell
.\run.ps1 -TraceSpurs                                  # shape mode: whole sessions, a few MB an hour
$env:SPURS_GOLDEN_TRACE_MODE = 'full'; .\run.ps1 -TraceSpurs   # every event: ~17 MB/s, minutes only
.\slice_trace.ps1                                      # cut a big trace into pieces small enough to send
```
