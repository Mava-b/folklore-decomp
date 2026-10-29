# build_all.ps1 -- build both releases for testing, without packaging.
#
#   powershell -ExecutionPolicy Bypass -File .\build_all.ps1
#   ... build_all.ps1 -EuGameDir D:\Games\BCES00050   the EU dump (the first time)
#   ... build_all.ps1 -UsUpdateDir <RPCS3>\dev_hdd0\game\BCUS98147
#   ... build_all.ps1 -EuUpdateDir <RPCS3>\dev_hdd0\game\BCES00050
#                                                    also the 1.10 builds (once; see package_all.ps1)
#   ... build_all.ps1 -Clean                           full rebuild of both
#   ... build_all.ps1 -EuDir D:\Folklore-EU            the EU copy elsewhere
#
# Builds this folder (US), brings the EU copy beside it (Folklore-EU) up to
# date with this folder's changes, and builds that too -- package_all.ps1
# without the packaging step. Then run.bat in either folder runs that build.

param(
    [string]$EuDir = '',
    [string]$EuGameDir = '',
    [string]$UsUpdateDir = '',
    [string]$EuUpdateDir = '',
    [switch]$Clean
)

$forwarded = @{ BuildOnly = $true }
if ($EuDir)
{
    $forwarded['EuDir'] = $EuDir
}
if ($EuGameDir)
{
    $forwarded['EuGameDir'] = $EuGameDir
}
if ($UsUpdateDir)
{
    $forwarded['UsUpdateDir'] = $UsUpdateDir
}
if ($EuUpdateDir)
{
    $forwarded['EuUpdateDir'] = $EuUpdateDir
}
if ($Clean)
{
    $forwarded['Clean'] = $true
}
& (Join-Path $PSScriptRoot 'package_all.ps1') @forwarded
