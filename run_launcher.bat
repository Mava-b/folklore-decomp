@echo off
rem run_launcher.bat -- open FolkloreLauncher.exe: the game's folders, DLC
rem and settings in one window, then Play starts the game.
rem
rem In a package the launcher is beside this file. In the project folder it
rem is folklore\build\FolkloreLauncher.exe (build.ps1 makes it), and it is
rem pointed at this folder's settings, hdd0, saves and logs -- the ones
rem run.ps1 uses.
setlocal
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
if exist "%HERE%\FolkloreLauncher.exe" (
    start "" "%HERE%\FolkloreLauncher.exe"
    exit /b 0
)
if exist "%HERE%\folklore\build\FolkloreLauncher.exe" (
    set "FOLKLORE_SETTINGS=%HERE%\folklore_settings.ini"
    set "FOLKLORE_DATA_DIR=%HERE%"
    set "PS3_HDD0_ROOT=%HERE%\hdd0"
    set "PS3_HDD1_ROOT=%HERE%\hdd1"
    start "" /D "%HERE%" "%HERE%\folklore\build\FolkloreLauncher.exe"
    exit /b 0
)
echo FolkloreLauncher.exe was not found. Run build.ps1 first.
pause
exit /b 1
