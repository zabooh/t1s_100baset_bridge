@echo off
REM Launch the Bridge GUI

setlocal enabledelayedexpansion

REM Get script directory
set SCRIPT_DIR=%~dp0

REM Runs bridge_gui.py through .venv (created by setup.bat), which is where
REM its hard sv-ttk dependency actually lives. Falls back to bare "python"
REM if .venv is missing.
set "PY=%SCRIPT_DIR%.venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"

REM Check if Python is available
"%PY%" --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python not found. Please install Python 3.7+
    pause
    exit /b 1
)

REM Run the GUI
echo Starting Bridge GUI...
cd /d "%SCRIPT_DIR%"
"%PY%" scripts\bridge_gui.py

if errorlevel 1 (
    echo.
    echo ERROR: GUI failed to start
    pause
    exit /b 1
)
