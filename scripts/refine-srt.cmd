@echo off
REM Runs refine-srt.py via the cached whisper-convert venv (already has Python).
REM Fallback to system python if the venv isn't present.
setlocal
set "VENV_PY=%~dp0..\.cache\whisper-convert-venv\Scripts\python.exe"
if exist "%VENV_PY%" (
    "%VENV_PY%" "%~dp0refine-srt.py" %*
) else (
    python "%~dp0refine-srt.py" %*
)
endlocal
