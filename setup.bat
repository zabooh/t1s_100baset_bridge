@echo off
setlocal
:: ===========================================================================
:: setup.bat - one-time, per-machine setup after cloning.
::
:: Adapts the project to the local machine: Python deps, the installed XC32
:: compiler version, pyOCD (for flashing), and the SAME54_DFP debug fix.
::
::   setup.bat            <-- this script
::   build.bat             (no IDE session needed - see step 5)
::   flash.bat
::
:: Connect the board via its USB debugger port BEFORE running this so the
:: probe check can detect it. Steps are independent: a failure in one is
:: reported but does not abort the rest.
:: ===========================================================================
set "SCRIPT_DIR=%~dp0"
set "RC=0"

echo ============================================================
echo   T1S 100BaseT Bridge - one-time machine setup
echo ============================================================

where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found in PATH. Install Python 3.9+ and re-run.
    exit /b 1
)

echo.
echo [1/5] Python dependencies (pyserial) ...
call "%SCRIPT_DIR%install_dependencies.bat"
if errorlevel 1 ( echo [WARN] pyserial install failed - check your network/pip. & set "RC=1" )

echo.
echo [2/5] Compiler selection (XC32) ...
python "%SCRIPT_DIR%setup_compiler.py"
if errorlevel 1 ( echo [WARN] setup_compiler.py failed - run it manually. & set "RC=1" )

echo.
echo [3/5] Flasher prerequisites (pyOCD, EDBG probe, SAME54_DFP pack) ...
call "%SCRIPT_DIR%install.bat" --install
if errorlevel 1 ( echo [WARN] install.bat reported missing prerequisites - see above. & set "RC=1" )

echo.
echo [4/5] VS Code debug fix (SAME54_DFP tool pack) ...
python "%SCRIPT_DIR%setup_debug.py"
if errorlevel 1 ( echo [WARN] setup_debug.py failed - only needed for VS Code debugging. & set "RC=1" )

rem The nbproject Makefile fragments are gitignored - they carry absolute paths
rem of the machine that generated them, so a fresh clone has none. Generating
rem them here means the first build.bat has nothing left to discover; build.bat
rem does it too, so this step is a convenience, not a prerequisite.
echo.
echo [5/5] MPLAB X project Makefiles (no IDE session needed) ...
call "%SCRIPT_DIR%genmk.bat" "%SCRIPT_DIR%firmware\T1S_100BaseT_Bridge.X"
if errorlevel 1 ( echo [WARN] genmk.bat failed - build.bat will try again. & set "RC=1" )

echo.
echo ============================================================
if "%RC%"=="0" (
    echo   Setup complete. Now build and flash:
) else (
    echo   Setup finished with warnings ^(see above^). You can still:
)
echo     build.bat
echo     flash.bat
echo ============================================================
endlocal & exit /b %RC%
