# package_all.ps1 -- build and package both releases into dist\.
#
#   powershell -ExecutionPolicy Bypass -File .\package_all.ps1
#   ... package_all.ps1 -EuGameDir G:\Games\PS3\folklore_eu_dump
#                                    the EU dump (needed the first time, when
#                                    the EU folder does not exist yet)
#   ... package_all.ps1 -EuDir D:\Folklore-EU
#                                    where the EU copy of the project lives
#                                    (default: Folklore-EU beside this folder)
#   ... package_all.ps1 -NoZip       leave the package folders, skip the zips
#
# One build is lifted from one release (folklore/docs/REGIONS.md), so the EU
# exe is built in a second copy of the project. This script:
#
#   1. builds this folder (the US release) with build.ps1;
#   2. brings the EU copy up to date with this folder: everything except
#      the lifted game code, build output, game data, logs and settings.
#      The first time, it creates the copy and runs its setup.ps1 with the EU
#      dump. Later, it runs setup.ps1 there again only when patches\ changed
#      (to reapply them to its ps3recomp) or it is not lifted from the EU
#      release;
#   3. builds the EU copy;
#   4. packages both into this folder's dist\, with the same date-time:
#      Folklore-<date-time> (US) and Folklore-<date-time>_eu (EU), each with a
#      .zip.

param(
    [string]$EuDir = '',
    [string]$EuGameDir = '',
    [switch]$NoZip
)

. (Join-Path $PSScriptRoot 'common.ps1')

if (-not $EuDir)
{
    $EuDir = Join-Path (Split-Path $Root) ((Split-Path -Leaf $Root) + '-EU')
}
$distDir = Join-Path $Root 'dist'
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'

function Get-LiftedRegionIn([string]$projectDir)
{
    # The release a project folder's lifted code is from: '' when it has
    # none, 'US' for lifted code from before folklore_region.h existed.
    $recompDir = Join-Path $projectDir 'folklore\src\recomp'
    $anyLifted = Get-ChildItem -Path $recompDir -Filter '*.cpp' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $anyLifted)
    {
        return ''
    }
    $header = Join-Path $recompDir 'folklore_region.h'
    if (Test-Path $header)
    {
        foreach ($line in Get-Content -LiteralPath $header)
        {
            if ($line -match '#define FOLKLORE_REGION_NAME "(\w+)"')
            {
                return $matches[1]
            }
        }
    }
    return 'US'
}

function Get-SavedGameDir([string]$projectDir)
{
    $settingsFile = Join-Path $projectDir 'folklore_settings.ini'
    if (Test-Path $settingsFile)
    {
        foreach ($line in Get-Content -LiteralPath $settingsFile)
        {
            if ($line -match '^\s*game_dir\s*=\s*(.*?)\s*$')
            {
                return $matches[1]
            }
        }
    }
    return ''
}

function Get-FolderFingerprint([string]$folder)
{
    # Names, sizes and times of the files in a folder, to see whether a sync
    # changed it.
    if (-not (Test-Path $folder))
    {
        return ''
    }
    $entries = Get-ChildItem -Path $folder -File -Recurse -ErrorAction SilentlyContinue | Sort-Object FullName |
               ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
    return ($entries -join "`n")
}

function Invoke-ProjectScript([string]$projectDir, [string]$script, [string[]]$arguments)
{
    # Run one of a project folder's scripts in its own PowerShell, so each
    # folder uses its own paths, and stop if it fails.
    $scriptPath = Join-Path $projectDir $script
    Write-Step "$script in $projectDir"
    Invoke-Native {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath @arguments
    }
    if ($LASTEXITCODE -ne 0)
    {
        throw "$script failed in $projectDir (exit code $LASTEXITCODE) -- see the logs\ folder there"
    }
}

# ---- 1. This folder: the US build --------------------------------------------
$usRegion = Get-LiftedRegionIn $Root
if ($usRegion -ne 'US')
{
    throw "This folder's game code is lifted from the '$usRegion' release, not the US one -- run setup.ps1 -GameDir <your BCUS98147 dump> here first"
}
Invoke-ProjectScript $Root 'build.ps1' @()

# ---- 2. The EU copy: create it or bring it up to date ------------------------
$firstTime = -not (Test-Path (Join-Path $EuDir 'setup.ps1'))
if ($firstTime -and -not $EuGameDir)
{
    throw "No EU copy at $EuDir yet -- give the EU dump once: package_all.ps1 -EuGameDir <folder that contains PS3_GAME>"
}
$patchesBefore = Get-FolderFingerprint (Join-Path $EuDir 'patches')

Write-Step "Updating the EU copy at $EuDir"
New-Item -ItemType Directory -Force -Path $EuDir | Out-Null
# Not copied: the lifted game code (per release), anything built, game data,
# logs and settings (each copy keeps its own), Sony's libsre lift (comparison
# runs only; it would end up in the EU exe), and -- after the first time -- a
# ps3recomp git clone (setup.ps1 applies patches\ to the EU copy's own).
# Folders are named by full path so nothing else with the same name is
# skipped.
$excludedFolders = @(
    (Join-Path $Root 'folklore\src\recomp'),
    (Join-Path $Root 'folklore\analysis'),
    (Join-Path $Root 'folklore\firmware_lle'),
    (Join-Path $Root 'folklore\build'),
    (Join-Path $Root 'ps3recomp\build'),
    (Join-Path $Root 'dist'),
    (Join-Path $Root 'hdd0'),
    (Join-Path $Root 'hdd1'),
    (Join-Path $Root 'logs'),
    (Join-Path $Root 'gamedata'),
    (Join-Path $Root 'scratch'),
    (Join-Path $Root 'save_snapshots'),
    (Join-Path $Root '.git'))
if ($firstTime)
{
    Write-Host '    first time: copying the tools as well (several GB)'
}
else
{
    $excludedFolders += (Join-Path $Root 'tools')
}
if (-not $firstTime -and (Test-Path (Join-Path $Ps3RecompDir '.git')))
{
    $excludedFolders += $Ps3RecompDir
}
$robocopyArguments = @($Root, $EuDir, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP',
                       '/XF', 'folklore_settings.ini', 'folklore_settings.fresh.ini', '/XD') + $excludedFolders
Invoke-Native { & robocopy.exe @robocopyArguments | Out-Null }
if ($LASTEXITCODE -ge 8)
{
    throw "Copying to $EuDir failed (robocopy exit code $LASTEXITCODE)"
}
$global:LASTEXITCODE = 0

# setup.ps1 there: the first time, when it is not lifted from the EU dump, or
# when patches\ changed (its ps3recomp needs them reapplied and rebuilt).
$euRegion = Get-LiftedRegionIn $EuDir
$patchesAfter = Get-FolderFingerprint (Join-Path $EuDir 'patches')
if (-not $EuGameDir)
{
    $EuGameDir = Get-SavedGameDir $EuDir
}
if ($firstTime -or $euRegion -ne 'EU' -or $patchesBefore -ne $patchesAfter)
{
    if (-not $EuGameDir)
    {
        throw "The EU copy needs setting up and has no EU dump saved -- run package_all.ps1 -EuGameDir <folder that contains PS3_GAME>"
    }
    Invoke-ProjectScript $EuDir 'setup.ps1' @('-GameDir', $EuGameDir)
    if ((Get-LiftedRegionIn $EuDir) -ne 'EU')
    {
        throw "$EuGameDir is not a BCES00050 v1.00 dump (the EU copy's game code is not the EU release after setup)"
    }
}

# ---- 3. The EU build ---------------------------------------------------------
Invoke-ProjectScript $EuDir 'build.ps1' @()

# ---- 4. Both packages, into this folder's dist\ ------------------------------
$packageArguments = @('-OutDir', $distDir, '-Stamp', $stamp)
if ($NoZip)
{
    $packageArguments += '-NoZip'
}
Invoke-ProjectScript $Root 'package.ps1' $packageArguments
Invoke-ProjectScript $EuDir 'package.ps1' $packageArguments

Write-Step "Done: $distDir\Folklore-$stamp (US) and $distDir\Folklore-${stamp}_eu (EU)"
