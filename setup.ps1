# setup.ps1 -- one-time setup for the Folklore recomp, all inside this folder.
#
#   From a PowerShell prompt in this folder:
#     powershell -ExecutionPolicy Bypass -File .\setup.ps1
#
# Downloads into .\tools: Git (portable), CMake, Ninja, Python, FFmpeg (LGPL,
# for the game's MPEG-2 cutscenes) and -- only if you have no Visual Studio
# with clang-cl already -- Visual Studio 2022 Build Tools. Then prepares
# ps3recomp (the included copy, or a clone with patches\ applied), lifts the
# game's code from your own dump, and builds the runtime library.
#
# Safe to re-run: every step skips itself when its result already exists.
#
# From a build kit (package.ps1 -Kind BuildKit) the game's code is not
# included: setup lifts it from your own dump. It asks for the dump folder at
# the start (a folder picker), or take it on the command line:
#     setup.ps1 -GameDir D:\Games\BCUS98147
# Either supported release works (BCUS98147 or BCES00050, v1.00). Giving
# -GameDir a dump of the other release than the one already set up lifts
# that one instead.
#
# The 1.10 update instead of the disc version: add the installed update
# (RPCS3: dev_hdd0\game\<title id>):
#     setup.ps1 -GameDir D:\Games\BCUS98147 -UpdateDir D:\RPCS3\dev_hdd0\game\BCUS98147
# -GameDir on its own (without -UpdateDir) goes back to the disc version.
#
# Folklore's SPU work runs on the port's own SPURS (folklore/docs/SPURS.md);
# no firmware file is needed. For comparing against Sony's library, setup can
# still lift your own decrypted libsre.prx into folklore\firmware_lle:
#     setup.ps1 -LibsrePrx D:\Firmware\libsre.prx     (or -WithLibsre for a file picker)
# and run.ps1 -Libsre then uses it.

param(
    # Your Folklore dump: the folder that contains PS3_GAME, with
    # PS3_GAME\USRDIR\EBOOT.BIN (setup decrypts it; an EBOOT.elf RPCS3 wrote
    # is used as it is). Only needed when folklore\src\recomp is empty.
    [string]$GameDir = '',
    # The installed 1.10 update, to build that instead of the disc version:
    # the folder holding its PARAM.SFO and USRDIR (its EBOOT.BIN is decrypted
    # here; the North American one needs its .rap, which RPCS3 installs with
    # the update). RPCS3 installs it in dev_hdd0\game\BCUS98147 (or BCES00050).
    [string]$UpdateDir = '',
    # Optional, for comparison runs (run.ps1 -Libsre): your decrypted
    # libsre.prx -- dev_flash\sys\external\libsre.sprx from RPCS3's firmware
    # install, decrypted the same way. A dev_flash folder works too.
    [string]$LibsrePrx = '',
    # The same, chosen with a file picker.
    [switch]$WithLibsre,
    # Accepted for old command lines; libsre is not lifted unless asked for.
    [switch]$SkipLibsre
)

. (Join-Path $PSScriptRoot 'common.ps1')

New-Item -ItemType Directory -Force -Path $Tools, $Downloads, $LogDir | Out-Null

# ---- What a build kit needs from you: asked first, so the rest runs alone ----
function Select-WithDialog([string]$kind, [string]$title, [string]$filter)
{
    # A Windows picker; a typed path if the picker cannot open (no desktop).
    try
    {
        Add-Type -AssemblyName System.Windows.Forms
        if ($kind -eq 'folder')
        {
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = $title
            $dialog.ShowNewFolderButton = $false
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK)
            {
                return $dialog.SelectedPath
            }
            return ''
        }
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = $title
        $dialog.Filter = $filter
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK)
        {
            return $dialog.FileName
        }
        return ''
    }
    catch
    {
        return (Read-Host $title)
    }
}

function Get-LatestGitHubAsset([string]$repository, [string]$namePattern)
{
    $headers = @{ 'User-Agent' = 'folklore-recomp-setup' }
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repository/releases/latest" -Headers $headers
    $asset = $release.assets | Where-Object { $_.name -like $namePattern } | Select-Object -First 1
    if (-not $asset)
    {
        throw "No asset matching '$namePattern' in the latest $repository release"
    }
    return $asset
}

function Save-Download([string]$url, [string]$fileName)
{
    $target = Join-Path $Downloads $fileName
    if (-not (Test-Path $target))
    {
        Write-Host "    downloading $fileName"
        Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing
    }
    return $target
}

function Expand-Into([string]$archive, [string]$destination)
{
    # Extract to a temp folder, then move the single top-level folder (if the
    # archive has one) so tools\<name>\bin is predictable.
    $staging = "$destination._extract"
    if (Test-Path $staging)
    {
        Remove-Item -Recurse -Force $staging
    }
    Expand-Archive -Path $archive -DestinationPath $staging
    $children = @(Get-ChildItem $staging)
    if ($children.Count -eq 1 -and $children[0].PSIsContainer)
    {
        Move-Item $children[0].FullName $destination
        Remove-Item -Recurse -Force $staging
    }
    else
    {
        Move-Item $staging $destination
    }
}

function Install-Compiler
{
    # Visual Studio (or its Build Tools) with clang-cl: the one found, or
    # VS 2022 Build Tools installed into tools\VSBuildTools. Returns its path.
    $vsPath = Find-VsInstall
    if (-not $vsPath)
    {
        Write-Host '    No Visual Studio with clang-cl found -- installing VS 2022 Build Tools into tools\VSBuildTools.'
        Write-Host '    This is a ~6 GB download and asks for administrator rights (UAC prompt).'
        Write-Host '    (The Visual Studio Installer itself and some shared SDK files always go on C:.)'
        $bootstrapper = Save-Download 'https://aka.ms/vs/17/release/vs_BuildTools.exe' 'vs_BuildTools.exe'
        $installArguments = @(
            '--installPath', "`"$VsBuildToolsDir`"",
            '--add', 'Microsoft.VisualStudio.Workload.VCTools',
            '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
            '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
            '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset',
            '--passive', '--wait', '--norestart'
        )
        $process = Start-Process -Wait -PassThru -Verb RunAs -FilePath $bootstrapper -ArgumentList $installArguments
        # 3010 = success, reboot recommended
        if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 3010)
        {
            throw "Build Tools installer failed with exit code $($process.ExitCode)"
        }
        $vsPath = Find-VsInstall
        if (-not $vsPath)
        {
            throw 'Build Tools installed but clang-cl was not found -- see the README troubleshooting section.'
        }
    }
    return $vsPath
}

function Install-UnselfTool
{
    # tools\unself.exe: decrypts EBOOT.BIN to EBOOT.elf (folklore\tools\unself_main.cpp
    # with the engine's src\engine\engine_self.cpp), so no RPCS3 is needed for
    # it. Built with clang-cl the first time it is needed, and again when its
    # sources change.
    $sources = @((Join-Path $PortDir 'tools\unself_main.cpp'),
                 (Join-Path $PortDir 'src\engine\engine_self.cpp'),
                 (Join-Path $PortDir 'src\engine\engine_self.h'))
    if (Test-Path $UnselfExe)
    {
        $built = (Get-Item -LiteralPath $UnselfExe).LastWriteTimeUtc
        $newer = @($sources | Where-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc -gt $built })
        if ($newer.Count -eq 0)
        {
            return
        }
    }
    Write-Host '    building tools\unself.exe (decrypts EBOOT.BIN; a few seconds)'
    $null = Install-Compiler
    Import-BuildEnvironment
    $objects = Join-Path $Tools '_unself'
    New-Item -ItemType Directory -Force -Path $objects | Out-Null
    $unselfLog = Join-Path $LogDir 'setup-unself-build.log'
    Invoke-Native {
        & clang-cl /nologo /O2 /EHsc /std:c++17 /W0 "/I$(Join-Path $PortDir 'src\engine')" "/Fo$objects\\" "/Fe$UnselfExe" `
            (Join-Path $PortDir 'tools\unself_main.cpp') (Join-Path $PortDir 'src\engine\engine_self.cpp') 2>&1 |
            ForEach-Object { "$_" } | Tee-Object -FilePath $unselfLog
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $UnselfExe))
    {
        throw 'Building tools\unself.exe failed -- see logs\setup-unself-build.log'
    }
}

function Resolve-Eboot([string]$usrdir, [string[]]$licenceFolders)
{
    # The ELF to lift from a USRDIR: EBOOT.elf when there is one (RPCS3's),
    # otherwise EBOOT.BIN decrypted with tools\unself.exe into decrypted\. Empty
    # when the folder has neither.
    $elf = Join-Path $usrdir 'EBOOT.elf'
    if (Test-Path -LiteralPath $elf)
    {
        return $elf
    }
    $self = Join-Path $usrdir 'EBOOT.BIN'
    if (-not (Test-Path -LiteralPath $self))
    {
        return ''
    }
    Install-UnselfTool
    return (Get-DecryptedEboot $self $licenceFolders)
}

# The executables this project supports, by decrypted EBOOT.elf size
# (folklore/docs/REGIONS.md): each release's v1.00 disc and 1.10 update.
$SupportedEboots = @{ 6518936 = 'US'; 6518912 = 'EU'; 6520504 = 'US-1.10'; 6520480 = 'EU-1.10' }
$ReleaseNames = @{ 'US' = 'BCUS98147 (North America) v1.00'; 'EU' = 'BCES00050 (Europe) v1.00';
                   'US-1.10' = 'BCUS98147 (North America) 1.10 update';
                   'EU-1.10' = 'BCES00050 (Europe) 1.10 update' }

function Get-EbootRegion([string]$eboot)
{
    $size = (Get-Item -LiteralPath $eboot).Length
    if ($SupportedEboots.ContainsKey([int]$size))
    {
        return $SupportedEboots[[int]$size]
    }
    return ''
}

function Get-LiftedRegion
{
    # The executable folklore\src\recomp was lifted from, as a key of
    # $SupportedEboots (tools\relift.sh writes folklore_region.h; lifted code
    # from before that is the US disc).
    $release = Get-BuildRelease
    if ($release.Version -eq '1.10')
    {
        return "$($release.Region)-1.10"
    }
    return $release.Region
}

function Resolve-GameEboot([string]$folder)
{
    # The decrypted EBOOT.elf of a supported v1.00 dump, from the dump folder
    # (or its PS3_GAME / USRDIR). Throws with what is wrong.
    $folder = $folder.Trim().Trim('"').TrimEnd('\')
    if ((Split-Path -Leaf $folder) -eq 'USRDIR') { $folder = Split-Path (Split-Path $folder) }
    if ((Split-Path -Leaf $folder) -eq 'PS3_GAME') { $folder = Split-Path $folder }
    $usrdir = Join-Path $folder 'PS3_GAME\USRDIR'
    if (-not (Test-Path (Join-Path $folder 'PS3_GAME')))
    {
        throw "$folder has no PS3_GAME folder -- choose the folder of your Folklore dump (the one that contains PS3_GAME)"
    }
    $eboot = Resolve-Eboot $usrdir @()
    if (-not $eboot)
    {
        throw "No EBOOT.BIN in $usrdir -- choose the folder of your Folklore dump (the one that contains PS3_GAME)"
    }
    $release = Get-EbootRegion $eboot
    if (-not $release -or $release.EndsWith('-1.10'))
    {
        throw "$eboot is not a v1.00 EBOOT of a supported release -- BCUS98147 (6,518,936 bytes decrypted) or BCES00050 (6,518,912 bytes)"
    }
    $script:ResolvedGameFolder = $folder
    return $eboot
}

function Resolve-UpdateEboot([string]$folder)
{
    # The decrypted EBOOT.elf of an installed 1.10 update, from the update
    # folder (or its USRDIR). Throws with what is wrong.
    $folder = $folder.Trim().Trim('"').TrimEnd('\')
    if ((Split-Path -Leaf $folder) -eq 'USRDIR') { $folder = Split-Path $folder }
    if (-not (Test-Path (Join-Path $folder 'PARAM.SFO')) -or -not (Test-Path (Join-Path $folder 'USRDIR')))
    {
        throw "$folder is not an installed game update (no PARAM.SFO and USRDIR) -- in RPCS3 the update is dev_hdd0\game\BCUS98147 or BCES00050"
    }
    $eboot = Resolve-Eboot (Join-Path $folder 'USRDIR') (Get-LicenceFolders $folder)
    if (-not $eboot)
    {
        throw "No EBOOT.BIN in $folder\USRDIR -- in RPCS3 the update is dev_hdd0\game\BCUS98147 or BCES00050"
    }
    $release = Get-EbootRegion $eboot
    if (-not $release -or -not $release.EndsWith('-1.10'))
    {
        throw "$eboot is not the 1.10 update of a supported release -- BCUS98147 (6,520,504 bytes decrypted) or BCES00050 (6,520,480 bytes)"
    }
    $script:ResolvedUpdateFolder = $folder
    return $eboot
}

function Resolve-LibsrePrx([string]$path)
{
    # Your decrypted libsre.prx: the file itself, or a folder holding it
    # (dev_flash, dev_flash\sys\external, or wherever RPCS3 wrote it).
    $path = $path.Trim().Trim('"').TrimEnd('\')
    $candidates = @($path)
    if (Test-Path -PathType Container $path)
    {
        $candidates = @((Join-Path $path 'libsre.prx'),
                        (Join-Path $path 'sys\external\libsre.prx'),
                        (Join-Path $path 'dev_flash\sys\external\libsre.prx'),
                        (Join-Path $path 'libsre.sprx'),
                        (Join-Path $path 'sys\external\libsre.sprx'),
                        (Join-Path $path 'dev_flash\sys\external\libsre.sprx'))
    }
    foreach ($candidate in $candidates)
    {
        if (-not (Test-Path -PathType Leaf $candidate))
        {
            continue
        }
        $stream = [System.IO.File]::OpenRead($candidate)
        try
        {
            $header = New-Object byte[] 18
            [void]$stream.Read($header, 0, 18)
        }
        finally
        {
            $stream.Close()
        }
        if ($header[0] -eq 0x7F -and $header[1] -eq 0x45 -and $header[2] -eq 0x4C -and $header[3] -eq 0x46)
        {
            # e_type 0xFFA4: a PS3 PRX (a module), not a game EBOOT.
            if ($header[16] -ne 0xFF -or $header[17] -ne 0xA4)
            {
                throw "$candidate is an ELF but not a PRX module -- choose libsre.prx"
            }
            return $candidate
        }
        if ($header[0] -eq 0x53 -and $header[1] -eq 0x43 -and $header[2] -eq 0x45)
        {
            throw "$candidate is still encrypted -- decrypt it with RPCS3 (Utilities > Decrypt PS3 Binaries), which writes libsre.prx beside it, and choose that"
        }
        throw "$candidate is not a PRX file"
    }
    throw "No libsre.prx at $path -- in RPCS3's firmware install it is dev_flash\sys\external\libsre.sprx; decrypt it with Utilities > Decrypt PS3 Binaries"
}

function Set-SavedSetting([string]$key, [string]$value)
{
    # key = value in folklore_settings.ini (the engine menu's file, which
    # run.ps1 and folklore.exe read the game folder from).
    $settingsFile = Join-Path $Root 'folklore_settings.ini'
    $lines = @()
    if (Test-Path $settingsFile)
    {
        $lines = @(Get-Content -LiteralPath $settingsFile)
    }
    $found = $false
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++)
    {
        if ($lines[$lineIndex] -match "^\s*$key\s*=")
        {
            $lines[$lineIndex] = "$key = $value"
            $found = $true
        }
    }
    if (-not $found)
    {
        $lines += "$key = $value"
    }
    Set-Content -LiteralPath $settingsFile -Value $lines -Encoding ASCII
}

$needGameCode = $null -eq (Get-ChildItem -Path (Join-Path $PortDir 'src\recomp') -Filter '*.cpp' -ErrorAction SilentlyContinue |
                           Select-Object -First 1)
$sourceEboot = ''
# The 1.10 update: lift its EBOOT (unless that is what is lifted already).
# The disc dump is still where the game's data comes from.
if ($UpdateDir)
{
    $givenEboot = Resolve-UpdateEboot $UpdateDir
    $givenRegion = Get-EbootRegion $givenEboot
    Write-Host "    1.10 update: $givenEboot -- $($ReleaseNames[$givenRegion])"
    Set-SavedSetting 'update_dir' $script:ResolvedUpdateFolder
    if ($GameDir)
    {
        $discFolder = $GameDir.Trim().Trim('"').TrimEnd('\')
        if ((Split-Path -Leaf $discFolder) -eq 'USRDIR') { $discFolder = Split-Path (Split-Path $discFolder) }
        if ((Split-Path -Leaf $discFolder) -eq 'PS3_GAME') { $discFolder = Split-Path $discFolder }
        if (-not (Test-Path (Join-Path $discFolder 'PS3_GAME')))
        {
            throw "$discFolder has no PS3_GAME folder -- -GameDir is your disc dump (the folder that contains PS3_GAME)"
        }
        Set-SavedSetting 'game_dir' $discFolder
    }
    $liftedRegion = Get-LiftedRegion
    if ($needGameCode -or $givenRegion -ne $liftedRegion)
    {
        if (-not $needGameCode)
        {
            Write-Step "Switching from $($ReleaseNames[$liftedRegion]) to $($ReleaseNames[$givenRegion])"
            Write-Host '    The game code will be lifted again from this EBOOT, and build.ps1 then rebuilds everything.'
        }
        $needGameCode = $true
        $sourceEboot = $givenEboot
    }
}
# A dump of the other release (or the disc version, after the 1.10 update)
# than the one already lifted: lift that one instead (a full rebuild follows).
elseif (-not $needGameCode -and $GameDir)
{
    try
    {
        $givenEboot = Resolve-GameEboot $GameDir
        $givenRegion = Get-EbootRegion $givenEboot
        $liftedRegion = Get-LiftedRegion
        if ($givenRegion -ne $liftedRegion)
        {
            Write-Step "Switching from $($ReleaseNames[$liftedRegion]) to $($ReleaseNames[$givenRegion])"
            Write-Host '    The game code will be lifted again from this dump, and build.ps1 then rebuilds everything.'
            $needGameCode = $true
            $sourceEboot = $givenEboot
            Set-SavedSetting 'game_dir' $script:ResolvedGameFolder
        }
    }
    catch
    {
        Write-Host "    $($_.Exception.Message)" -ForegroundColor Yellow
    }
}
$needLibsre = ($LibsrePrx -or $WithLibsre) -and -not (Test-Path (Join-Path $PortDir 'firmware_lle\libsre_recomp.cpp'))
if ($needGameCode -and -not $sourceEboot)
{
    Write-Step 'Your Folklore dump'
    Write-Host '    This build kit has none of the game''s code: setup translates it from your own copy.'
    Write-Host '    Needed: the folder of your Folklore dump (v1.00 disc: BCUS98147, North America, or'
    Write-Host '    BCES00050, Europe) -- the one that contains PS3_GAME. Its EBOOT.BIN is decrypted here'
    Write-Host '    (no RPCS3 needed); an EBOOT.elf RPCS3 already wrote beside it is used as it is.'
    while (-not $sourceEboot)
    {
        if (-not $GameDir)
        {
            $GameDir = Select-WithDialog 'folder' 'Folklore: choose your game dump folder (the one that contains PS3_GAME)' ''
            if (-not $GameDir)
            {
                throw 'No game folder chosen -- run setup.ps1 again, or pass -GameDir <your dump>'
            }
        }
        try
        {
            $sourceEboot = Resolve-GameEboot $GameDir
            Write-Host "    $sourceEboot -- $($ReleaseNames[(Get-EbootRegion $sourceEboot)])"
            # run.ps1 and folklore.exe start from this folder from now on.
            Set-SavedSetting 'game_dir' $script:ResolvedGameFolder
        }
        catch
        {
            Write-Host "    $($_.Exception.Message)" -ForegroundColor Yellow
            $GameDir = ''
        }
    }
}
$libsreFile = ''
if ($needLibsre)
{
    Write-Step 'Your PS3 firmware''s libsre (optional, for run.ps1 -Libsre)'
    Write-Host '    Your decrypted libsre.prx -- dev_flash\sys\external\libsre.sprx from RPCS3''s firmware'
    Write-Host '    install, decrypted with RPCS3 (Utilities > Decrypt PS3 Binaries).'
    while (-not $libsreFile)
    {
        if (-not $LibsrePrx)
        {
            $LibsrePrx = Select-WithDialog 'file' 'Folklore: choose your decrypted libsre.prx' 'libsre.prx|libsre.prx;libsre.sprx|PRX files (*.prx)|*.prx|All files (*.*)|*.*'
            if (-not $LibsrePrx)
            {
                throw 'No libsre.prx chosen -- run setup.ps1 again, or pass -LibsrePrx <your decrypted libsre.prx>'
            }
        }
        try
        {
            $libsreFile = Resolve-LibsrePrx $LibsrePrx
            Write-Host "    $libsreFile"
        }
        catch
        {
            Write-Host "    $($_.Exception.Message)" -ForegroundColor Yellow
            $LibsrePrx = ''
        }
    }
}

# ---- Git ------------------------------------------------------------------
Write-Step 'Git (portable)'
if (-not (Test-Path $GitExe))
{
    $asset = Get-LatestGitHubAsset 'git-for-windows/git' 'PortableGit-*-64-bit.7z.exe'
    $installer = Save-Download $asset.browser_download_url $asset.name
    $gitDir = Join-Path $Tools 'git'
    Start-Process -Wait -FilePath $installer -ArgumentList "-o`"$gitDir`"", '-y'
}
Write-Host "    $GitExe"

# ---- CMake ----------------------------------------------------------------
Write-Step 'CMake'
if (-not (Test-Path $CMakeExe))
{
    $asset = Get-LatestGitHubAsset 'Kitware/CMake' 'cmake-*-windows-x86_64.zip'
    $archive = Save-Download $asset.browser_download_url $asset.name
    Expand-Into $archive (Join-Path $Tools 'cmake')
}
Write-Host "    $CMakeExe"

# ---- Ninja ----------------------------------------------------------------
Write-Step 'Ninja'
if (-not (Test-Path (Join-Path $NinjaDir 'ninja.exe')))
{
    $asset = Get-LatestGitHubAsset 'ninja-build/ninja' 'ninja-win.zip'
    $archive = Save-Download $asset.browser_download_url $asset.name
    Expand-Archive -Path $archive -DestinationPath $NinjaDir
}
Write-Host "    $NinjaDir\ninja.exe"

# ---- Python (NuGet package: a full, self-contained CPython) -----------------
Write-Step 'Python'
if (-not (Test-Path $PythonExe))
{
    $index = Invoke-RestMethod -Uri 'https://api.nuget.org/v3-flatcontainer/python/index.json'
    $version = $index.versions | Where-Object { $_ -notmatch '-' } | Select-Object -Last 1
    $package = Save-Download "https://api.nuget.org/v3-flatcontainer/python/$version/python.$version.nupkg" "python.$version.zip"
    Expand-Archive -Path $package -DestinationPath (Join-Path $Tools 'python')
    & $PythonExe -m ensurepip --upgrade | Out-Null
}
Write-Host "    $PythonExe ($(& $PythonExe --version))"

# ---- FFmpeg (headers + DLLs; loaded at run time for cellVdec) --------------
Write-Step 'FFmpeg (LGPL shared build, for cutscenes)'
if (-not (Test-Path (Join-Path $FfmpegDir 'include\libavcodec\avcodec.h')))
{
    $asset = Get-LatestGitHubAsset 'BtbN/FFmpeg-Builds' 'ffmpeg-master-latest-win64-lgpl-shared.zip'
    $archive = Save-Download $asset.browser_download_url $asset.name
    Expand-Into $archive $FfmpegDir
}
Write-Host "    $FfmpegDir"

# ---- Compiler: Visual Studio Build Tools + clang-cl ------------------------
Write-Step 'C++ compiler (MSVC + clang-cl)'
$vsPath = Install-Compiler
Write-Host "    $vsPath"

# ---- ps3recomp ----------------------------------------------------------------
# Two layouts. In the GitHub repository (and kits made from it) ps3recomp\ is a
# plain folder: upstream at the pinned commit with patches\ already applied --
# it is used as it is. In a working folder made by an earlier setup it is a git
# clone of upstream, and every patch is applied fresh on top of the pinned
# commit (below).
$vendoredPs3Recomp = (Test-Path (Join-Path $Ps3RecompDir 'CMakeLists.txt')) -and
                     -not (Test-Path (Join-Path $Ps3RecompDir '.git'))
if ($vendoredPs3Recomp)
{
    Write-Step 'ps3recomp (included copy, patches already applied)'
    & $PythonExe -m pip install --quiet -r (Join-Path $Ps3RecompDir 'requirements.txt')
}
else
{
    # ---- ps3recomp at the pinned commit -----------------------------------------
    Write-Step "ps3recomp @ $($Ps3RecompCommit.Substring(0, 12))"
    if (-not (Test-Path (Join-Path $Ps3RecompDir '.git')))
    {
        & $GitExe clone https://github.com/sp00nznet/ps3recomp.git $Ps3RecompDir
    }
    & $GitExe -C $Ps3RecompDir fetch --quiet origin
    & $GitExe -C $Ps3RecompDir checkout --quiet $Ps3RecompCommit
    & $PythonExe -m pip install --quiet -r (Join-Path $Ps3RecompDir 'requirements.txt')

    # Folklore's fixes to ps3recomp itself (patches\*.patch, applied in order).
    # The checkout is put back to the pinned commit first and every patch is
    # applied fresh, so re-running setup always gives the same tree (testing each
    # patch on its own stops working once a later patch touches the same lines).
    # Only ps3recomp's tracked files are reset; its build folder is kept, so the
    # runtime rebuild below only recompiles what the patches touch.
    Write-Step 'Applying Folklore patches to ps3recomp'
    Invoke-Native { & $GitExe -C $Ps3RecompDir reset --quiet --hard $Ps3RecompCommit 2>&1 | ForEach-Object { "$_" } }
    if ($LASTEXITCODE -ne 0)
    {
        throw "Could not reset ps3recomp to $Ps3RecompCommit"
    }
    $patchFiles = Get-ChildItem (Join-Path $Root 'patches') -Filter '*.patch' | Sort-Object Name
    # A file a patch ADDS is untracked, so the reset above leaves the copy from
    # the last setup in place, and `git apply` then refuses the whole patch
    # ("already exists in working directory"). Remove those files first. (This
    # stopped 0034 from applying on its second setup, and 0035 with it.)
    foreach ($patchFile in $patchFiles)
    {
        $patchLines = Get-Content -LiteralPath $patchFile.FullName
        for ($lineIndex = 0; $lineIndex -lt $patchLines.Count - 1; $lineIndex++)
        {
            if ($patchLines[$lineIndex] -eq '--- /dev/null' -and $patchLines[$lineIndex + 1].StartsWith('+++ b/'))
            {
                $addedFile = Join-Path $Ps3RecompDir ($patchLines[$lineIndex + 1].Substring(6))
                if (Test-Path -LiteralPath $addedFile)
                {
                    Remove-Item -LiteralPath $addedFile -Force
                }
            }
        }
    }
    foreach ($patchFile in $patchFiles)
    {
        Invoke-Native { & $GitExe -C $Ps3RecompDir apply $patchFile.FullName 2>&1 | ForEach-Object { "$_" } }
        if ($LASTEXITCODE -ne 0)
        {
            throw "Could not apply $($patchFile.Name)"
        }
        Write-Host "    applied: $($patchFile.Name)"
    }
}

# ---- The game code, lifted from your own EBOOT (build kits) -----------------
# A build kit ships without folklore\src\recomp (it is the game's own code,
# translated). tools\relift.sh regenerates it -- byte for byte what the
# project was built from -- with the patched ps3recomp's lifter, run through
# the bundled Git Bash and Python.
$gitBash = Join-Path $Tools 'git\bin\bash.exe'
$recompSources = Join-Path $PortDir 'src\recomp'
$haveRecomp = -not $needGameCode
if (-not $haveRecomp)
{
    Write-Step 'Lifting the game code from your EBOOT (tools\relift.sh, about a minute)'
    $liftLog = Join-Path $LogDir 'setup-lift.log'
    $env:PS3RECOMP = $Ps3RecompDir -replace '\\', '/'
    $env:EBOOT = $sourceEboot -replace '\\', '/'
    $env:PYTHON = $PythonExe -replace '\\', '/'
    if (Test-Path (Join-Path $PortDir 'src\gen\ppu_hle_nids.cpp'))
    {
        $env:KEEP_GEN = '1'
    }
    Push-Location $PortDir
    try
    {
        Invoke-Native { & $gitBash tools/relift.sh 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $liftLog }
    }
    finally
    {
        Pop-Location
        Remove-Item Env:KEEP_GEN -ErrorAction SilentlyContinue
    }
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Lifting the game code failed -- see logs\setup-lift.log'
    }
}

# ---- Optional: Sony's SPURS library, lifted from your own firmware ----------
$firmwareLle = Join-Path $PortDir 'firmware_lle'
if (-not (Test-Path (Join-Path $firmwareLle 'libsre_recomp.cpp')))
{
    if ($libsreFile)
    {
        Write-Step 'Lifting libsre from your firmware (tools\lift_firmware.sh)'
        $firmwareLog = Join-Path $LogDir 'setup-firmware-lift.log'
        $env:PS3RECOMP = $Ps3RecompDir -replace '\\', '/'
        $env:PRX = $libsreFile -replace '\\', '/'
        $env:PYTHON = $PythonExe -replace '\\', '/'
        Push-Location $PortDir
        try
        {
            Invoke-Native { & $gitBash tools/lift_firmware.sh 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $firmwareLog }
        }
        finally
        {
            Pop-Location
        }
        if ($LASTEXITCODE -ne 0)
        {
            throw 'Lifting libsre failed -- see logs\setup-firmware-lift.log'
        }
    }
}

# ---- Build the ps3recomp runtime library -----------------------------------
Write-Step 'Building the ps3recomp runtime (ps3recomp_runtime.lib)'
Import-BuildEnvironment
$runtimeBuild = Join-Path $Ps3RecompDir 'build'
$setupLog = Join-Path $LogDir 'setup-runtime-build.log'
Invoke-Native {
    & $CMakeExe -S $Ps3RecompDir -B $runtimeBuild -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        -DPS3RECOMP_FFMPEG_RUNTIME=ON "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=$FfmpegDir\include" `
        -DPS3RECOMP_CPU_LEVEL=x86-64-v3 2>&1 |
        ForEach-Object { "$_" } | Tee-Object -FilePath $setupLog
}
if ($LASTEXITCODE -ne 0) { throw 'ps3recomp configure failed -- see logs\setup-runtime-build.log' }
Invoke-Native {
    & $CMakeExe --build $runtimeBuild --target ps3recomp_runtime 2>&1 |
        ForEach-Object { "$_" } | Tee-Object -FilePath $setupLog -Append
}
if ($LASTEXITCODE -ne 0) { throw 'ps3recomp runtime build failed -- send me logs\setup-runtime-build.log' }

Write-Step 'Setup complete. Next: build.ps1, then run.ps1'
