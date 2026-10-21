# package.ps1 -- make a folder (and .zip) to give someone else.
#
#   powershell -ExecutionPolicy Bypass -File .\package.ps1
#   ... package.ps1 -Kind Binaries     the built game, ready to run (default)
#   ... package.ps1 -Kind BuildKit     the project without the game's code:
#                                      the other person builds it from their
#                                      own dump (setup.ps1, build.ps1)
#   ... package.ps1 -NoZip             leave the folder, skip the .zip
#
# Output goes to dist\ beside this script.
#
# What each one contains, and why it matters:
#
#   Binaries: folklore.exe and the FFmpeg and Visual C++ runtime DLLs it
#   loads. The exe IS Folklore's own code, translated to C++ and compiled
#   (and, in a build that has folklore\firmware_lle, Sony's libsre lifted
#   from your firmware for comparison runs). Giving it to someone is
#   distributing that code; they still need their own dump to play.
#
#   BuildKit: the scripts, patches, engine code and tools -- none of the
#   game's code (src\recomp, src\spu_gen, src\spu_gen_lle) and none of the
#   firmware (firmware_lle, analysis). setup.ps1 lifts both again from the
#   other person's own dump and firmware file, reproducing exactly what this
#   project builds.

param(
    [ValidateSet('Binaries', 'BuildKit')]
    [string]$Kind = 'Binaries',
    [string]$OutDir = '',
    [switch]$NoZip
)

. (Join-Path $PSScriptRoot 'common.ps1')

if (-not $OutDir)
{
    $OutDir = Join-Path $Root 'dist'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'

function Copy-Tree([string]$source, [string]$destination, [string[]]$excludeRelative)
{
    # Copy $source into $destination, skipping the listed relative paths
    # (folders or files) and Python caches.
    $sourceFull = (Resolve-Path $source).Path.TrimEnd('\')
    Get-ChildItem -LiteralPath $sourceFull -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($sourceFull.Length + 1)
        $skip = $relative -match '(^|\\)__pycache__(\\|$)'
        foreach ($excluded in $excludeRelative)
        {
            if ($relative -eq $excluded -or $relative.StartsWith($excluded + '\'))
            {
                $skip = $true
                break
            }
        }
        if (-not $skip)
        {
            $target = Join-Path $destination $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        }
    }
}

function Complete-Package([string]$folder)
{
    if ($NoZip)
    {
        Write-Step "Done: $folder"
        return
    }
    $zipPath = "$folder.zip"
    if (Test-Path $zipPath)
    {
        Remove-Item -Force $zipPath
    }
    Write-Step "Zipping $(Split-Path -Leaf $zipPath)"
    Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zipPath -CompressionLevel Optimal
    $sizeMegabytes = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
    Write-Step "Done: $zipPath ($sizeMegabytes MB)"
}

if ($Kind -eq 'Binaries')
{
    $exe = Join-Path $PortDir 'build\folklore.exe'
    if (-not (Test-Path $exe))
    {
        throw 'folklore\build\folklore.exe not found -- run build.ps1 first'
    }
    $package = Join-Path $OutDir "Folklore-$stamp"
    if (Test-Path $package)
    {
        Remove-Item -Recurse -Force $package
    }
    New-Item -ItemType Directory -Force -Path $package | Out-Null

    Write-Step 'folklore.exe'
    Copy-Item $exe $package

    # FFmpeg (LGPL) for the cutscenes: the exe loads these at run time and
    # finds them beside itself. Its licence travels with it.
    Write-Step 'FFmpeg DLLs'
    $ffmpegBin = Join-Path $FfmpegDir 'bin'
    $ffmpegDlls = @(Get-ChildItem -Path $ffmpegBin -Filter '*.dll' -ErrorAction SilentlyContinue)
    if ($ffmpegDlls.Count -eq 0)
    {
        Write-Host '    none found in tools\ffmpeg\bin -- cutscenes will not play' -ForegroundColor Yellow
    }
    foreach ($dll in $ffmpegDlls)
    {
        Copy-Item $dll.FullName $package
        Write-Host "    $($dll.Name)"
    }
    $licenceFolder = Join-Path $package 'licences'
    New-Item -ItemType Directory -Force -Path $licenceFolder | Out-Null
    foreach ($licence in @(Get-ChildItem -Path $FfmpegDir -File -ErrorAction SilentlyContinue |
                           Where-Object { $_.Name -match '^(LICENSE|COPYING|README)' }))
    {
        Copy-Item $licence.FullName (Join-Path $licenceFolder "ffmpeg-$($licence.Name)")
    }

    # The Visual C++ runtime the exe was linked against, deployed beside it
    # (Microsoft allows this app-local copy), so nobody needs the
    # redistributable installed.
    Write-Step 'Visual C++ runtime DLLs'
    Import-BuildEnvironment
    $redistRoot = $env:VCToolsRedistDir
    $crtFolder = $null
    if ($redistRoot -and (Test-Path (Join-Path $redistRoot 'x64')))
    {
        $crtFolder = Get-ChildItem -Path (Join-Path $redistRoot 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
                     Select-Object -First 1
    }
    if ($crtFolder)
    {
        foreach ($dll in @(Get-ChildItem -Path $crtFolder.FullName -Filter '*.dll'))
        {
            Copy-Item $dll.FullName $package
            Write-Host "    $($dll.Name)"
        }
    }
    else
    {
        Write-Host '    not found -- the other PC needs the Visual C++ 2015-2022 x64 redistributable' -ForegroundColor Yellow
    }

    # The game runs on the port's own SPURS. Sony's libsre (firmware_lle,
    # only there after setup.ps1 -LibsrePrx) is for comparison runs and is
    # not packaged -- but a build that has it compiled it into the exe.
    if (Test-Path (Join-Path $PortDir 'firmware_lle\libsre_recomp.cpp'))
    {
        Write-Host ''
        Write-Host 'This build has folklore\firmware_lle, so folklore.exe also contains Sony''s libsre lifted' -ForegroundColor Yellow
        Write-Host 'from your firmware. For a package without it: delete folklore\firmware_lle, run build.ps1,' -ForegroundColor Yellow
        Write-Host 'then package.ps1 again.' -ForegroundColor Yellow
    }

    $readme = @"
Folklore (PS3) -- PC port, built $(Get-Date -Format 'yyyy-MM-dd')

You need your own copy of Folklore: the North American disc, BCUS98147, v1.00,
dumped to a folder. That folder contains PS3_GAME.

1. Decrypt the game's EBOOT once with RPCS3: Utilities > Decrypt PS3 Binaries,
   choose <your dump>\PS3_GAME\USRDIR\EBOOT.BIN. RPCS3 writes EBOOT.elf beside it.
2. Start folklore.exe. The first time it asks for the dump folder (the one that
   contains PS3_GAME) and remembers it in folklore_settings.ini.
3. The first start copies several GB of game data into hdd1 here; saves go in
   gamedata, logs in logs. Keep this folder somewhere you can write to (not
   Program Files) with enough free space, or set data_dir in
   folklore_settings.ini to put all of that elsewhere.

In the game
  F1 or L3+R3     engine menu: frame rate, resolution, aspect ratio, fullscreen,
                  motion controls, game folder, save backups
  Alt+Enter       fullscreen on/off
  Controller      any XInput pad; a DualShock 3 through DsHidMini (SXS mode)
                  gives the original motion controls

Optional: system fonts. Copy a dev_flash folder (RPCS3's, from its firmware
install) here, or set dev_flash in folklore_settings.ini.

If something goes wrong, send the newest file in logs\.
"@
    Set-Content -Path (Join-Path $package 'README.txt') -Value $readme -Encoding UTF8

    Write-Host ''
    Write-Host 'Note: folklore.exe contains the game''s own code, translated and compiled. Sharing it' -ForegroundColor Yellow
    Write-Host 'distributes that code. package.ps1 -Kind BuildKit makes a kit without it that rebuilds the' -ForegroundColor Yellow
    Write-Host 'same game from the other person''s own dump.' -ForegroundColor Yellow
    Complete-Package $package
}
else
{
    $package = Join-Path $OutDir "Folklore-buildkit-$stamp"
    if (Test-Path $package)
    {
        Remove-Item -Recurse -Force $package
    }
    New-Item -ItemType Directory -Force -Path $package | Out-Null

    Write-Step 'Scripts and patches'
    foreach ($file in @('setup.ps1', 'build.ps1', 'run.ps1', 'common.ps1', 'package.ps1',
                        'capture.bat', 'capture.ps1', 'slice_trace.ps1', 'README.md', 'LICENSE'))
    {
        $source = Join-Path $Root $file
        if (Test-Path $source)
        {
            Copy-Item $source $package
        }
    }
    Copy-Tree (Join-Path $Root 'patches') (Join-Path $package 'patches') @()
    # The included, already-patched ps3recomp (repository layout); a working
    # folder with a git clone instead lets the kit clone and patch upstream.
    if ((Test-Path (Join-Path $Ps3RecompDir 'CMakeLists.txt')) -and -not (Test-Path (Join-Path $Ps3RecompDir '.git')))
    {
        Write-Step 'ps3recomp\ (included copy)'
        Copy-Tree $Ps3RecompDir (Join-Path $package 'ps3recomp') @('build')
    }

    # The game's code (lifted from the EBOOT) and anything from the firmware
    # stay out; setup.ps1 regenerates them from the other person's files.
    Write-Step 'folklore\ (without the game''s code or firmware)'
    $excluded = @('build', 'analysis', 'firmware_lle', 'src\recomp', 'src\spu_gen', 'src\spu_gen_lle')
    Copy-Tree $PortDir (Join-Path $package 'folklore') $excluded
    foreach ($emptyFolder in @('src\recomp'))
    {
        New-Item -ItemType Directory -Force -Path (Join-Path $package "folklore\$emptyFolder") | Out-Null
    }

    $readme = @"
Folklore (PS3) recompilation -- build kit, $(Get-Date -Format 'yyyy-MM-dd')

This kit has no game code and no firmware in it. You need your own Folklore
dump: North American disc BCUS98147, v1.00 (a folder that contains PS3_GAME),
with PS3_GAME\USRDIR\EBOOT.BIN decrypted to EBOOT.elf (RPCS3: Utilities >
Decrypt PS3 Binaries). No PS3 firmware is needed.

Unzip somewhere with ~30 GB free (tools, build, installed game data), then in
PowerShell from that folder:

  powershell -ExecutionPolicy Bypass -File .\setup.ps1
  powershell -ExecutionPolicy Bypass -File .\build.ps1
  powershell -ExecutionPolicy Bypass -File .\run.ps1

setup.ps1 first asks for your dump folder (a folder picker; it checks it and
asks again if it is wrong), or take it on the command line: -GameDir <dump>.
Then it downloads the tools (Git, CMake, Ninja, Python, FFmpeg and, if you
have no Visual Studio with clang-cl, the VS 2022 Build Tools), sets up
ps3recomp with this project's patches, and lifts the game's code from your
EBOOT. The first build takes 10-30 minutes. After that, folklore\build\folklore.exe also runs on its own, and
package.ps1 makes a self-contained copy of it.

README.md has the details.
"@
    Set-Content -Path (Join-Path $package 'BUILD-KIT.txt') -Value $readme -Encoding UTF8
    Complete-Package $package
}
