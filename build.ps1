# build.ps1 -- configure and build folklore.exe (run setup.ps1 once first).
#
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#   ... build.ps1 -Clean      wipe folklore\build and start over
#   ... build.ps1 -NoPromote  compile the lifted code as lifted, without
#                             tools\promote_regs.py keeping guest registers
#                             in C locals (for comparison)
#
# The first build compiles ~1.5 million lines of lifted C++ and takes a while
# (roughly 10-30 minutes on a modern 8-core CPU). Later builds only recompile
# what changed. Output: folklore\build\folklore.exe and logs\build.log.

param([switch]$Clean, [switch]$NoPromote)

. (Join-Path $PSScriptRoot 'common.ps1')
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

Import-BuildEnvironment
$buildDir = Join-Path $PortDir 'build'
if ($Clean -and (Test-Path $buildDir))
{
    Remove-Item -Recurse -Force $buildDir
}

$buildLog = Join-Path $LogDir 'build.log'
Set-Content -Path $buildLog -Value ''

# The game fixes in tools\game_patches.py, on the lifted code. Setup applies
# them when it lifts; running them here as well brings code lifted before a
# fix was added up to date (a fix already in place is skipped, so this is a
# second or two).
$recompSourceDir = Join-Path $PortDir 'src\recomp'
if (Get-ChildItem -Path $recompSourceDir -Filter '*.cpp' -ErrorAction SilentlyContinue | Select-Object -First 1)
{
    $release = Get-BuildRelease
    $patchRegion = $release.Region
    if ($release.Version -eq '1.10')
    {
        $patchRegion = "$patchRegion-1.10"
    }
    Write-Step "Game fixes (tools\game_patches.py, $patchRegion)"
    Invoke-Native {
        & $PythonExe -I (Join-Path $PortDir 'tools\game_patches.py') $recompSourceDir --region $patchRegion 2>&1 |
            ForEach-Object { "$_" } | Tee-Object -FilePath $buildLog -Append
    }
    if ($LASTEXITCODE -ne 0)
    {
        throw 'the game fixes did not apply -- see logs\build.log'
    }
}

# Keep guest registers in C locals inside each lifted function (a copy of
# src\recomp in build\recomp_promoted; only files whose source changed are
# rewritten, so this is quick after the first time).
$recompDir = ''
if (-not $NoPromote)
{
    Write-Step 'Promoting guest registers (tools\promote_regs.py)'
    $recompDir = Join-Path $buildDir 'recomp_promoted'
    Invoke-Native {
        & $PythonExe (Join-Path $PortDir 'tools\promote_regs.py') --dir (Join-Path $PortDir 'src\recomp') $recompDir 2>&1 |
            ForEach-Object { "$_" } | Tee-Object -FilePath $buildLog -Append
    }
    if ($LASTEXITCODE -ne 0)
    {
        throw 'register promotion failed -- see logs\build.log'
    }
}
# Configure every time (a few seconds): a relift or an update can add source
# folders, and re-running CMake is what makes sure the build sees them.
Write-Step 'Configuring'
Invoke-Native {
    & $CMakeExe -S $PortDir -B $buildDir -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        "-DPS3RECOMP_DIR=$Ps3RecompDir" "-DFOLKLORE_RECOMP_DIR=$recompDir" 2>&1 |
        ForEach-Object { "$_" } | Tee-Object -FilePath $buildLog -Append
}
if ($LASTEXITCODE -ne 0)
{
    throw 'configure failed -- see logs\build.log'
}

Write-Step 'Building (first build takes a while)'
Invoke-Native {
    & $CMakeExe --build $buildDir 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $buildLog -Append
}
if ($LASTEXITCODE -ne 0) { throw 'build failed -- send me logs\build.log' }

Write-Step "Built $buildDir\folklore.exe -- next: run.ps1"
