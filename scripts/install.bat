@echo off
REM AirCube desktop app installer (Windows)
REM Double-click this file or run it from a command prompt.

setlocal
cd /d "%~dp0"

REM Prefer the Python launcher, fall back to python on PATH.
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 install.py %*
) else (
    python install.py %*
)

echo.
pause
