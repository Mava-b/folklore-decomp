# package_all.ps1 -- build and package every release into dist\.
#
#   powershell -ExecutionPolicy Bypass -File .\package_all.ps1
#   ... package_all.ps1 -EuGameDir D:\Games\BCES00050
#                                    the EU dump (needed the first time, when
#                                    the EU folder does not exist yet)
#   ... package_all.ps1 -UsUpdateDir D:\RPCS3\dev_hdd0\game\BCUS98147
#                                    also build the US 1.10 update (the
#                                    installed update, with USRDIR\EBOOT.BIN
#                                    decrypted; needed the first time)
#   ... package_all.ps1 -EuUpdateDir D:\RPCS3\dev_hdd0\game\BCES00050
#                                    the same for the EU 1.10 update
#   ... package_all.ps1 -EuDir D:\Folklore-EU
#                                    where the EU copy of the project lives
#                                    (default: Folklore-EU beside this folder;
#                                    the 1.10 copies: -Us110Dir / -Eu110Dir,
#                                    default Folklore-1.10 and Folklore-EU-1.10)
#   ... package_all.ps1 -NoZip       leave the package folders, skip the zips
#   ... package_all.ps1 -Clean       full rebuild of all (build.ps1 -Clean)
#   ... package_all.ps1 -BuildOnly   steps 1-3 only: update and build them,
#                                    no packages (build_all.ps1 runs this)
#
# One build is lifted from one executable (folklore/docs/REGIONS.md), so each
# other build is made in a copy of the project beside this one. This script:
#
#   1. builds this folder (the US v1.00 disc) with build.ps1;
#   2. brings each copy up to date with this folder: everything except the
#      lifted game code, build output, game data, logs and settings. The
#      first time, it creates the copy and runs its setup.ps1 with the dump
#      (and update) folders. Later, it runs setup.ps1 there again only when
#      patches\ changed (to reapply them to its ps3recomp) or it is not lifted
#      from the executable it is for. The copies: the EU disc, and -- once
#      their update folders have been given -- the US and EU 1.10 updates;
#   3. builds each copy;
#   4. packages them all into this folder's dist\, with the same date-time:
#      Folklore-<date-time> (US), Folklore-<date-time>_eu (EU),
#      Folklore-<date-time>_1.10 and Folklore-<date-time>_eu_1.10, each with
#      a .zip.

param(
    [string]$EuDir = '',
    [string]$EuGameDir = '',
    [string]$Us110Dir = '',
    [string]$UsUpdateDir = '',
    [string]$Eu110Dir = '',
    [string]$EuUpdateDir = '',
    [switch]$NoZip,
    [switch]$Clean,
    [switch]$BuildOnly
)

. (Join-Path $PSScriptRoot 'common.ps1')

$parentDir = Split-Path $Root
$rootName = Split-Path -Leaf $Root
if (-not $EuDir)
{
    $EuDir = Join-Path $parentDir ($rootName + '-EU')
}
if (-not $Us110Dir)
{
    $Us110Dir = Join-Path $parentDir ($rootName + '-1.10')
}
if (-not $Eu110Dir)
{
    $Eu110Dir = Join-Path $parentDir ($rootName + '-EU-1.10')
}
$distDir = Join-Path $Root 'dist'
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$buildArguments = @()
if ($Clean)
{
    $buildArguments += '-Clean'
}

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
    $region = 'US'
    $version = '1.00'
    if (Test-Path $header)
    {
        foreach ($line in Get-Content -LiteralPath $header)
        {
            if ($line -match '#define FOLKLORE_REGION_NAME "(\w+)"')
            {
                $region = $matches[1]
            }
            if ($line -match '#define FOLKLORE_VERSION_NAME "([\d.]+)"')
            {
                $version = $matches[1]
            }
        }
    }
    if ($version -eq '1.10')
    {
        return "$region-1.10"
    }
    return $region
}

function Get-SavedSettingIn([string]$projectDir, [string]$key)
{
    $settingsFile = Join-Path $projectDir 'folklore_settings.ini'
    if (Test-Path $settingsFile)
    {
        foreach ($line in Get-Content -LiteralPath $settingsFile)
        {
            if ($line -match "^\s*$key\s*=\s*(.*?)\s*$")
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
    throw "This folder's game code is lifted from '$usRegion', not the US v1.00 disc -- run setup.ps1 -GameDir <your BCUS98147 dump> here first"
}
Invoke-ProjectScript $Root 'build.ps1' $buildArguments

# ---- 2. The copies: create them or bring them up to date ---------------------
function Update-ProjectCopy([string]$copyDir, [string]$release, [string]$label, [string]$gameDir,
                            [string]$updateDir)
{
    # Brings one copy up to date and runs its setup.ps1 when needed.
    # $gameDir / $updateDir: given on the command line, else what the copy
    # saved. Throws when the copy needs setting up and they are missing.
    $firstTime = -not (Test-Path (Join-Path $copyDir 'setup.ps1'))
    $usesUpdate = $release.EndsWith('-1.10')
    $patchesBefore = Get-FolderFingerprint (Join-Path $copyDir 'patches')

    Write-Step "Updating the $label copy at $copyDir"
    New-Item -ItemType Directory -Force -Path $copyDir | Out-Null
    # Not copied: the lifted game code (per executable), anything built, game
    # data, logs and settings (each copy keeps its own), Sony's libsre lift
    # (comparison runs only; it would end up in the copy's exe), and -- after
    # the first time -- a ps3recomp git clone (setup.ps1 applies patches\ to
    # the copy's own). Folders are named by full path so nothing else with the
    # same name is skipped.
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
    $robocopyArguments = @($Root, $copyDir, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP',
                           '/XF', 'folklore_settings.ini', 'folklore_settings.fresh.ini', '/XD') + $excludedFolders
    Invoke-Native { & robocopy.exe @robocopyArguments | Out-Null }
    if ($LASTEXITCODE -ge 8)
    {
        throw "Copying to $copyDir failed (robocopy exit code $LASTEXITCODE)"
    }
    $global:LASTEXITCODE = 0

    # setup.ps1 there: the first time, when it is not lifted from the right
    # executable, or when patches\ changed (its ps3recomp needs them
    # reapplied and rebuilt).
    $liftedRelease = Get-LiftedRegionIn $copyDir
    $patchesAfter = Get-FolderFingerprint (Join-Path $copyDir 'patches')
    if (-not $gameDir)
    {
        $gameDir = Get-SavedSettingIn $copyDir 'game_dir'
    }
    if ($usesUpdate -and -not $updateDir)
    {
        $updateDir = Get-SavedSettingIn $copyDir 'update_dir'
    }
    if ($firstTime -or $liftedRelease -ne $release -or $patchesBefore -ne $patchesAfter)
    {
        if (-not $gameDir -or ($usesUpdate -and -not $updateDir))
        {
            throw "The $label copy needs setting up and has no dump$(if ($usesUpdate) { ' or update' }) folder saved -- see the top of package_all.ps1"
        }
        $setupArguments = @('-GameDir', $gameDir)
        if ($usesUpdate)
        {
            $setupArguments += @('-UpdateDir', $updateDir)
        }
        Invoke-ProjectScript $copyDir 'setup.ps1' $setupArguments
        if ((Get-LiftedRegionIn $copyDir) -ne $release)
        {
            throw "The $label copy's game code is not $release after setup -- check the folders given for it"
        }
    }
}

# The copies to build: the EU disc always; a 1.10 copy once it exists or its
# update folder is given. The 1.10 copies read the disc dump of their own
# release: this folder's for the US one, the EU copy's for the EU one.
if (-not (Test-Path (Join-Path $EuDir 'setup.ps1')) -and -not $EuGameDir)
{
    throw "No EU copy at $EuDir yet -- give the EU dump once: package_all.ps1 -EuGameDir <folder that contains PS3_GAME>"
}
$copies = @(@{ Dir = $EuDir; Release = 'EU'; Label = 'EU'; GameDir = $EuGameDir; UpdateDir = ''; Suffix = '_eu' })
if ($UsUpdateDir -or (Test-Path (Join-Path $Us110Dir 'setup.ps1')))
{
    $usGameDir = Get-SavedSettingIn $Root 'game_dir'
    $copies += @{ Dir = $Us110Dir; Release = 'US-1.10'; Label = 'US 1.10'; GameDir = $usGameDir;
                  UpdateDir = $UsUpdateDir; Suffix = '_1.10' }
}
if ($EuUpdateDir -or (Test-Path (Join-Path $Eu110Dir 'setup.ps1')))
{
    $euDump = if ($EuGameDir) { $EuGameDir } else { Get-SavedSettingIn $EuDir 'game_dir' }
    $copies += @{ Dir = $Eu110Dir; Release = 'EU-1.10'; Label = 'EU 1.10'; GameDir = $euDump;
                  UpdateDir = $EuUpdateDir; Suffix = '_eu_1.10' }
}
foreach ($copy in $copies)
{
    Update-ProjectCopy $copy.Dir $copy.Release $copy.Label $copy.GameDir $copy.UpdateDir
}

# ---- 3. The copies' builds ---------------------------------------------------
foreach ($copy in $copies)
{
    Invoke-ProjectScript $copy.Dir 'build.ps1' $buildArguments
}
if ($BuildOnly)
{
    Write-Step 'Built:'
    Write-Host "    $Root\folklore\build\folklore.exe (US)"
    foreach ($copy in $copies)
    {
        Write-Host "    $($copy.Dir)\folklore\build\folklore.exe ($($copy.Label))"
    }
    Write-Host "    Run them with run.bat in each folder."
    return
}

# ---- 4. The packages, into this folder's dist\ ------------------------------
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$packageArguments = @('-OutDir', $distDir, '-Stamp', $stamp)
if ($NoZip)
{
    $packageArguments += '-NoZip'
}
Invoke-ProjectScript $Root 'package.ps1' $packageArguments
foreach ($copy in $copies)
{
    Invoke-ProjectScript $copy.Dir 'package.ps1' $packageArguments
}

Write-Step "Done, in $distDir\:"
Write-Host "    Folklore-$stamp (US)"
foreach ($copy in $copies)
{
    Write-Host "    Folklore-$stamp$($copy.Suffix) ($($copy.Label))"
}
