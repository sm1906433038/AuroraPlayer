@echo off
REM Runs build.ps1 without requiring a relaxed machine ExecutionPolicy.
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
