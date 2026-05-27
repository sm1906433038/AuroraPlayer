@echo off
REM Runs convert-chickenrice.ps1 without requiring a relaxed ExecutionPolicy.
REM Forwards all arguments through, so things like:
REM   convert-chickenrice.cmd -Force
REM   convert-chickenrice.cmd -CheckOnly
REM   convert-chickenrice.cmd -SourceDir "C:\..."
REM all work as expected.
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0convert-chickenrice.ps1" %*
