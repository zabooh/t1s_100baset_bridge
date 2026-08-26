@echo off
REM Launch the modern-theme (sv-ttk) build of the Bridge GUI (bridge_gui_modern.py)

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

python -c "import sv_ttk" >nul 2>&1
if errorlevel 1 (
    echo ERROR: sv-ttk not installed. Run: pip install -r requirements.txt
    pause
    exit /b 1
)

REM Run the GUI
echo Starting Bridge GUI (modern theme build)...
cd /d "%SCRIPT_DIR%"
python bridge_gui_modern.py %*

if errorlevel 1 (
    echo.
    echo ERROR: GUI failed to start
    pause
    exit /b 1
)
