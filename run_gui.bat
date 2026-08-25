@echo off
REM Launch the Bridge GUI

setlocal enabledelayedexpansion

REM Get script directory
set SCRIPT_DIR=%~dp0

REM Check if Python is available
python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python not found. Please install Python 3.7+
    pause
    exit /b 1
)

REM Run the GUI
echo Starting Bridge GUI...
cd /d "%SCRIPT_DIR%"
python bridge_gui.py

if errorlevel 1 (
    echo.
    echo ERROR: GUI failed to start
    pause
    exit /b 1
)
