# common.ps1 -- shared paths and helpers for setup.ps1 / build.ps1 / run.ps1.
# Everything lives under the folder this file is in.

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar on

$Root          = $PSScriptRoot
$Tools         = Join-Path $Root 'tools'
$Downloads     = Join-Path $Tools '_downloads'
$Ps3RecompDir  = Join-Path $Root 'ps3recomp'
$PortDir       = Join-Path $Root 'folklore'
$LogDir        = Join-Path $Root 'logs'

# The ps3recomp revision the lifted code in folklore\src was generated and
# test-built against. Change it only together with a relift (tools\relift.sh).
$Ps3RecompCommit = '39a36296511c445707adabcd312f0b740cfbd694'

$GitExe    = Join-Path $Tools 'git\cmd\git.exe'
$CMakeExe  = Join-Path $Tools 'cmake\bin\cmake.exe'
$NinjaDir  = Join-Path $Tools 'ninja'
$PythonExe = Join-Path $Tools 'python\tools\python.exe'
$FfmpegDir = Join-Path $Tools 'ffmpeg'
$VsBuildToolsDir = Join-Path $Tools 'VSBuildTools'
# tools\unself.exe (setup.ps1 builds it): EBOOT.BIN -> EBOOT.elf, and where
# the ELFs it writes go (one folder per EBOOT.BIN, named by its hash).
$UnselfExe     = Join-Path $Tools 'unself.exe'
$DecryptedDir  = Join-Path $Root 'decrypted'

function Get-LicenceFolders([string]$updateFolder)
{
    # Where a .rap is looked for when decrypting an update's EBOOT.BIN:
    # RPCS3's dev_hdd0\home\<user>\exdata (two folders above
    # dev_hdd0\game\<title id>), a licences folder here, and the folder itself.
    $folders = @()
    $homeFolder = Join-Path (Split-Path (Split-Path $updateFolder)) 'home'
    if (Test-Path -LiteralPath $homeFolder)
    {
        foreach ($user in Get-ChildItem -LiteralPath $homeFolder -Directory -ErrorAction SilentlyContinue)
        {
            $folders += (Join-Path $user.FullName 'exdata')
        }
    }
    $folders += (Join-Path $Root 'licences')
    $folders += $updateFolder
    return $folders
}

function Get-DecryptedEboot([string]$self, [string[]]$licenceFolders)
{
    # EBOOT.BIN decrypted with tools\unself.exe into decrypted\<hash>\EBOOT.elf
    # (once; the copy is reused). Throws with the tool's sentence when it
    # cannot. Nothing is written beside the EBOOT.BIN.
    $hash = (Get-FileHash -LiteralPath $self -Algorithm SHA256).Hash.Substring(0, 16)
    $elf = Join-Path (Join-Path $DecryptedDir $hash) 'EBOOT.elf'
    if (Test-Path -LiteralPath $elf)
    {
        return $elf
    }
    if (-not (Test-Path -LiteralPath $UnselfExe))
    {
        throw "$self is encrypted and tools\unself.exe is not built yet -- run setup.ps1"
    }
    $output = Invoke-Native { & $UnselfExe $self $elf @licenceFolders 2>&1 | ForEach-Object { "$_" } }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $elf))
    {
        throw "Could not decrypt $self -- $($output -join ' ')"
    }
    Write-Host "    decrypted $self"
    return $elf
}

function Invoke-Native([scriptblock]$command)
{
    # Windows PowerShell 5.1 turns every stderr line of a native program into an
    # error record when stderr is redirected, and with 'Stop' the first one
    # (a compiler warning, a log line from the game) aborts the script.
    # Run native tools with 'Continue' and judge them by $LASTEXITCODE instead.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try
    {
        & $command
    }
    finally
    {
        $ErrorActionPreference = $previousPreference
    }
}

function Write-Step([string]$message)
{
    Write-Host ''
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Find-VsInstall
{
    # 1) Build Tools installed by setup.ps1 into tools\VSBuildTools.
    $localClang = Join-Path $VsBuildToolsDir 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
    if (Test-Path $localClang)
    {
        return $VsBuildToolsDir
    }

    # 2) An existing Visual Studio that already has the C++ tools AND clang-cl.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere)
    {
        $candidates = & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        foreach ($candidate in $candidates)
        {
            if (Test-Path (Join-Path $candidate 'VC\Tools\Llvm\x64\bin\clang-cl.exe'))
            {
                return $candidate
            }
        }
    }
    return $null
}

function Import-BuildEnvironment
{
    $vsPath = Find-VsInstall
    if (-not $vsPath)
    {
        throw 'No Visual Studio / Build Tools with clang-cl found. Run setup.ps1 first.'
    }
    # Once per PowerShell session: vcvars64 adds to PATH every time it runs,
    # so running build.ps1 / package.ps1 again and again in one window grows
    # PATH each time. A session whose PATH had lost System32 failed here with
    # "'cmd.exe' is not recognized" (2026-10-26), so cmd.exe is named by its
    # full path, and System32 is put back below if it is missing.
    if (-not $env:VSCMD_VER)
    {
        $cmdExe = $env:ComSpec
        if (-not $cmdExe -or -not (Test-Path $cmdExe))
        {
            $cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe'
        }
        $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
        $lines = & $cmdExe /c "`"$vcvars`" >nul 2>&1 && set"
        foreach ($line in $lines)
        {
            if ($line -match '^([^=]+)=(.*)$')
            {
                Set-Item -Path "env:$($matches[1])" -Value $matches[2]
            }
        }
    }
    # The project's own tools first, each only once.
    $clangBin = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin'
    $extraPaths = @((Split-Path $CMakeExe), $NinjaDir, (Split-Path $PythonExe), (Split-Path $GitExe), $clangBin, (Join-Path $FfmpegDir 'bin'))
    $currentPaths = @($env:PATH -split ';' | Where-Object { $_ -and ($extraPaths -notcontains $_) })
    $systemPaths = @((Join-Path $env:SystemRoot 'System32'), $env:SystemRoot,
                     (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0'))
    foreach ($systemPath in $systemPaths)
    {
        if ($currentPaths -notcontains $systemPath)
        {
            $currentPaths += $systemPath
        }
    }
    $env:PATH = (@($extraPaths) + $currentPaths) -join ';'
}

# Which executable this build was made from: Region 'US' or 'EU', Version
# '1.00' (the disc) or '1.10' (the update's EBOOT). The project folder reads
# what tools\relift.sh wrote beside the lifted code; a binaries package reads
# release.txt, which package.ps1 writes from the same header. Lifted code from
# before the header existed is the US disc; headers from before the version
# was recorded are 1.00.
function Get-BuildRelease
{
    $release = @{ Region = 'US'; Version = '1.00' }
    $sources = @((Join-Path $PortDir 'src\recomp\folklore_region.h'), (Join-Path $Root 'release.txt'))
    foreach ($source in $sources)
    {
        if (-not (Test-Path -LiteralPath $source))
        {
            continue
        }
        foreach ($line in Get-Content -LiteralPath $source)
        {
            if ($line -match '(#define FOLKLORE_REGION_NAME|^region\s*=)\s*"?(\w+)"?')
            {
                $release.Region = $matches[2]
            }
            if ($line -match '(#define FOLKLORE_VERSION_NAME|^version\s*=)\s*"?([\d.]+)"?')
            {
                $release.Version = $matches[2]
            }
        }
        break
    }
    return $release
}
