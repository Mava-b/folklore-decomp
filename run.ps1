# run.ps1 -- run the recompiled Folklore against your own disc dump.
#
#   powershell -ExecutionPolicy Bypass -File .\run.ps1
#   ... run.ps1 -GameDir D:\somewhere\BCUS98147     your dump (the folder that holds
#                                                   PS3_GAME). Without it, the game_dir
#                                                   saved in folklore_settings.ini; with
#                                                   none saved, folklore.exe asks once.
#   ... run.ps1 -Verbose                            extra runtime logging
#   ... run.ps1 -NativeSpurs                        the port's own SPURS instead of
#                                                   Sony's libsre (folklore/docs/SPURS.md;
#                                                   -NoLibsre is the same)
#   ... run.ps1 -Profile                            record where the CPU time goes
#                                                   (a report every 10 s in the log)
#   ... run.ps1 -BlockingWaitFlip                   old frame pacing (A/B comparison)
#   ... run.ps1 -TraceSpurs                         record what the real libsre does to
#                                                   logs\spurs-<date-time>.trace (each new
#                                                   kind of event once, then counts; a few
#                                                   MB an hour). SPURS_GOLDEN_TRACE_MODE=full
#                                                   records every event (GBs in minutes).
#
# In game: L3 + R3 together (or F1) opens the engine menu -- frame-rate cap,
# frame limiter precision, performance overlay. Settings are kept in
# folklore_settings.ini next to this script.
#   ... run.ps1 -InterpJobs                         run the game's SPU jobs in the
#                                                   interpreter instead of their
#                                                   lifts (A/B comparison, slower)
#   capture.bat (double-click, any run)          record what the renderer draws
#                                                   while a screen looks wrong
#
# Reads the game straight from your dump folder (nothing is copied). Every run
# writes logs\run-<date-time>.log -- that log is what I need after each test.
# SPU jobs the port does not recognise are dumped to logs\spu_miss\ so they
# can be lifted (send me that folder too if it has files in it).

param(
    [string]$GameDir = '',
    # PS3 firmware files (fonts etc.) -- e.g. RPCS3's dev_flash, read-only.
    # Optional; without it, dev_flash from folklore_settings.ini.
    [string]$DevFlash = '',
    # Wipe the port's own hdd0/hdd1 (install data, cache, saves) first, so the
    # game re-runs its first-boot install. Only touches this folder.
    [switch]$ResetInstall,
    # Use the port's own SPURS (src/hle/spurs_native.cpp) even when
    # firmware_lle\ (Sony's libsre) was built in. -NoLibsre is the old name.
    [switch]$NativeSpurs,
    [switch]$NoLibsre,
    # Run every lifted SPURS job through the SPU interpreter instead (slower):
    # tells a fault in the lifted job code apart from anything else.
    [switch]$InterpJobs,
    # Sample every thread every few milliseconds and print which code the
    # time is spent in, every 10 s -- for finding what makes a scene slow.
    [switch]$Profile,
    # Make cellGcmSetWaitFlip block the game thread until the next vblank, as
    # the runtime used to (the hardware call does not block).
    [switch]$BlockingWaitFlip,
    # SPURS_GOLDEN_TRACE: record every libsre call, SPU DMA, lock-line
    # atomic and job entry/exit to logs\spurs-<date-time>.trace, the reference
    # for replacing libsre (ps3recomp runtime/spu/spurs_trace.c).
    [switch]$TraceSpurs,
    # Render capture is always armed; this only prints a reminder.
    [switch]$Capture,
    [switch]$Verbose
)

. (Join-Path $PSScriptRoot 'common.ps1')
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$exe = Join-Path $PortDir 'build\folklore.exe'
if (-not (Test-Path $exe))
{
    throw "folklore.exe not found -- run build.ps1 first"
}

# The engine menu's settings file also remembers the game and dev_flash
# folders (folklore.exe writes game_dir after asking for it once).
$settingsFile = Join-Path $Root 'folklore_settings.ini'
function Get-SavedSetting([string]$key)
{
    if (-not (Test-Path $settingsFile))
    {
        return ''
    }
    foreach ($line in Get-Content -LiteralPath $settingsFile)
    {
        if ($line -match "^\s*$key\s*=\s*(.*?)\s*$")
        {
            return $matches[1]
        }
    }
    return ''
}
$gameDirGiven = $PSBoundParameters.ContainsKey('GameDir')
if (-not $GameDir)
{
    $GameDir = Get-SavedSetting 'game_dir'
}
if (-not $DevFlash)
{
    $DevFlash = Get-SavedSetting 'dev_flash'
}
$exeArguments = @()
if ($GameDir)
{
    $eboot = Join-Path $GameDir 'PS3_GAME\USRDIR\EBOOT.elf'
    if (Test-Path $eboot)
    {
        $exeArguments = @($eboot)
    }
    elseif ($gameDirGiven)
    {
        throw "Decrypted EBOOT.elf not found at $eboot"
    }
}
if ($exeArguments.Count -eq 0)
{
    # folklore.exe asks for the folder and saves it (engine_portable.cpp).
    Write-Host "No game folder yet -- folklore.exe will ask for it (the dump folder that contains PS3_GAME)." -ForegroundColor Yellow
}
# Tells folklore.exe that this script captures its output (it writes its own
# log file only when started on its own).
$env:FOLKLORE_LAUNCHER = 'run.ps1'

# FFmpeg DLLs for the cutscenes are found through PATH.
$env:PATH = (Join-Path $FfmpegDir 'bin') + ';' + $env:PATH

if ($exeArguments.Count -gt 0)
{
    $env:PS3_VFS_ROOT = $GameDir       # folder that contains PS3_GAME (read only)
}
else
{
    Remove-Item Env:PS3_VFS_ROOT -ErrorAction SilentlyContinue
}
# The game's install data, saves and system cache go here, never into the dump.
# First boot copies game data to the "HDD": allow several GB free on I:.
$hdd0Dir = Join-Path $Root 'hdd0'
$hdd1Dir = Join-Path $Root 'hdd1'
if ($ResetInstall)
{
    foreach ($hddDir in @($hdd0Dir, $hdd1Dir))
    {
        if (Test-Path $hddDir)
        {
            Write-Host "Removing $hddDir (fresh install)"
            Remove-Item -Recurse -Force $hddDir
        }
    }
}
New-Item -ItemType Directory -Force -Path $hdd0Dir, $hdd1Dir | Out-Null
$env:PS3_HDD0_ROOT = $hdd0Dir
$env:PS3_HDD1_ROOT = $hdd1Dir
if ($DevFlash -and (Test-Path $DevFlash))
{
    $env:PS3_DEV_FLASH = $DevFlash
}
else
{
    Remove-Item Env:PS3_DEV_FLASH -ErrorAction SilentlyContinue
    Write-Host "Note: no dev_flash set -- pass -DevFlash <RPCS3 folder>\dev_flash (or set dev_flash in folklore_settings.ini) if fonts are missing" -ForegroundColor Yellow
}
$env:PS3_TITLE     = 'Folklore'
$env:RSX_LIVE_DRAW = '1'               # run the game's real shaders through D3D12
# Folklore's SPURS jobs read their scratch-buffer base from jobContext+0x70
# (patches/0005); without this one audio job DMAs to an invalid address.
$env:PS3_SPURS_JOBCTX_SBUF70 = '1'
# Real libsre (firmware_lle\): SPU code its job manager places somewhere the
# port has no lift for is run by the SPU interpreter instead of ending the job.
# The log then says "[spu-ovl] code from 0x... placed at LS 0x..., which has
# no lift" -- send me those lines and I will lift that placement too.
$env:SPU_INTERP_UNLIFTED = '1'
# One line per file read (AIO and plain), with the guest buffer it lands in:
# the logo movie's AU size table is sometimes overwritten mid-movie, and this
# shows which read it shares memory with.
$env:PS3_FS_AIO_LOG = '1'
# Boot sometimes stops on the first logo with the main thread waiting for a
# SPURS workload to shut down: the watchdog dumps the SPURS instance with
# each sample so the stuck state is in the log.
$env:PS3_WATCHDOG_DUMP = '0x10A60C00:0x1000'
# The RSX waits at semaphore ACQUIREs until the label is written, as the
# hardware does (patches/0012) -- the game holds the GPU there until SPU jobs
# have finished writing what it draws. Bounded, and logged as [gcm-sema].
$env:GCM_SEMA_ACQUIRE = '1'
Remove-Item Env:PS3_NO_LIBSRE -ErrorAction SilentlyContinue
if ($NativeSpurs -or $NoLibsre)
{
    $env:FOLKLORE_SPURS = 'native'
    Write-Host "SPURS: the port's own (-NativeSpurs), not libsre." -ForegroundColor Yellow
}
else
{
    Remove-Item Env:FOLKLORE_SPURS -ErrorAction SilentlyContinue
}
if ($InterpJobs)
{
    $env:FOLKLORE_INTERP_JOBS = 'all'
    Write-Host "SPU jobs run in the interpreter (-InterpJobs): expect a lower frame rate." -ForegroundColor Yellow
}
else
{
    Remove-Item Env:FOLKLORE_INTERP_JOBS -ErrorAction SilentlyContinue
}
if ($Profile)
{
    $env:PS3_SAMPLE = '3'
    Write-Host "Profiling: stay in the slow spot for 30 seconds or more, then quit." -ForegroundColor Yellow
}
else
{
    Remove-Item Env:PS3_SAMPLE -ErrorAction SilentlyContinue
}
# cellGcmSetWaitFlip lets the game build the next frame while the last one
# waits for its vblank, as on the PS3, holding it back only when it gets more
# than one frame ahead (patches/0025). Before, the game thread sat out the
# rest of each vblank after every flip.
Remove-Item Env:GCM_WAITFLIP_NONBLOCKING -ErrorAction SilentlyContinue
if ($BlockingWaitFlip)
{
    Remove-Item Env:GCM_WAITFLIP_DEPTH -ErrorAction SilentlyContinue
}
else
{
    $env:GCM_WAITFLIP_DEPTH = '1'
}
# The engine menu's settings (frame-rate cap, limiter precision, overlay).
# The game's frame limiter now reads them directly (tools/patch_limiter.py),
# so the per-call-site usleep clamp of patches/0030 is no longer set here.
$env:FOLKLORE_SETTINGS = Join-Path $Root 'folklore_settings.ini'
Remove-Item Env:PS3_USLEEP_CLAMP -ErrorAction SilentlyContinue
$spuMissDir = Join-Path $LogDir 'spu_miss'
New-Item -ItemType Directory -Force -Path $spuMissDir | Out-Null
$env:SPU_DUMP_MISS = $spuMissDir
$env:PS3_VERBOSE = '0'               # per-call logging slows the game; -Verbose turns it on
if ($Verbose)
{
    $env:PS3_VERBOSE = '1'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runLog = Join-Path $LogDir "run-$stamp.log"
$captureGate = Join-Path $LogDir 'capture.go'
Remove-Item -Force $captureGate -ErrorAction SilentlyContinue
# Render capture is always armed (it costs nothing until capture.bat runs):
# every draw of the frames around a capture.bat press goes to
# logs\capture-<date-time>\ -- draws.csv plus images of the screen as it is
# built up. -Capture is still accepted; it only prints this reminder.
$captureDir = Join-Path $LogDir "capture-$stamp"
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
$env:RSX_DRAW_CSV = Join-Path $captureDir 'draws.csv'
$env:RSX_DRAW_CSV_GATE = $captureGate
$env:LD_DRAW_DUMP = $captureDir
# Per-draw images are off (they slow a capture to two frames, which hides
# performance problems); set to '64' to get every 64th draw's target again.
$env:LD_DRAW_DUMP_EVERY = '100000'
# ...and the raw vertex/index data of each mesh drawn during a capture
# (vb_*.bin / ib_*.bin), so broken geometry can be checked byte by byte.
$env:LD_VB_DUMP = $captureDir
# ...and the HLSL of every shader pair the renderer builds, named by the
# pso_key column of draws.csv, so a draw that comes out wrong can be read.
$hlslDir = Join-Path $captureDir 'hlsl'
New-Item -ItemType Directory -Force -Path $hlslDir | Out-Null
$env:LD_HLSL_DUMP = $hlslDir
# Draw probe (LD_PROBE_PSO=<pso keys> / LD_PROBE_AB, patches/0043): off now
# that the capture beam draws (STATUS.md #54) -- it hashes every draw's
# shaders an extra time. Set LD_PROBE_PSO here to follow a broken effect.
Remove-Item Env:LD_PROBE_PSO -ErrorAction SilentlyContinue
Remove-Item Env:LD_PROBE_AB -ErrorAction SilentlyContinue
$spursTrace = Join-Path $LogDir "spurs-$stamp.trace"
if ($TraceSpurs)
{
    $env:SPURS_GOLDEN_TRACE = $spursTrace
    Write-Host "Recording SPURS to $spursTrace" -ForegroundColor Yellow
}
else
{
    Remove-Item Env:SPURS_GOLDEN_TRACE -ErrorAction SilentlyContinue
}
if ($Capture)
{
    Write-Host "Render capture armed -- press F2 (or the PS button on a DualShock 3), or double-click capture.bat, while the screen looks wrong." -ForegroundColor Yellow
    Write-Host "Images and draws.csv go to $captureDir"
}
Write-Step "Running Folklore -- log: $runLog"
Invoke-Native {
    & $exe @exeArguments 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $runLog
}
Write-Host ''
Write-Host "Exit code $LASTEXITCODE. Log saved to $runLog"
