@echo off
REM ===========================================================================
REM AuroraPlayer (晨曦影音) launcher
REM   - Double-click this file to open AuroraPlayer.
REM   - Drag a video file onto this file to open it directly.
REM   - Right-click a video → "Open with..." → choose this .bat for a permanent
REM     "Open with AuroraPlayer" entry.
REM ===========================================================================

setlocal

set "EXE=%~dp0build\bin\AuroraPlayer.exe"

if not exist "%EXE%" (
    echo.
    echo  [!] AuroraPlayer.exe not found at:
    echo      %EXE%
    echo.
    echo  Build it first with:
    echo      powershell -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1"
    echo.
    pause
    exit /b 1
)

REM Forward any drag-dropped or cmd-line files to the exe.
start "" "%EXE%" %*
endlocal
