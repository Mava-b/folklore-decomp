# capture.ps1 -- record about four seconds of rendering from a running
# "run.ps1 -Capture". capture.bat runs this; double-click it while the screen
# that looks wrong is showing. Each press is a separate capture.

. (Join-Path $PSScriptRoot 'common.ps1')
$gate = Join-Path $LogDir 'capture.go'
New-Item -ItemType File -Force -Path $gate | Out-Null
Write-Host 'Capturing for four seconds (the game will stutter)...'
Start-Sleep -Seconds 4
Remove-Item -Force $gate -ErrorAction SilentlyContinue
Write-Host 'Done. You can keep playing; the files are in logs\capture-<date-time>\.'
Start-Sleep -Seconds 2
