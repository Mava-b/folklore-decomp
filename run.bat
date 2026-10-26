@echo off
rem run.bat -- start Folklore through run.ps1, which sets up the runtime
rem (environment, log, capture). Double-click it, or pass run.ps1 options:
rem     run.bat -ResetInstall
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*
if errorlevel 1 pause
