@echo off
setlocal EnableDelayedExpansion

:: ===========================================================================
:: build.bat - Shell build for the T1S<->100BASE-T bridge firmware.
::
:: Unlike the lan866x-tools sibling project, this is a PLAIN MPLAB X project
:: (no CMake scaffold). It drives MPLAB X's own generated NetBeans-style
:: Makefile (nbproject/Makefile-impl.mk etc.) via MPLAB X's bundled 'make',
:: the same mechanism the IDE itself uses under the hood.
::
:: A FRESH CLONE BUILDS WITH NO PREPARATION. The nbproject Makefile fragments
:: are gitignored (they hold absolute paths of the machine that generated them),
:: so a fresh checkout has none - this script notices and generates them via
:: genmk.bat, which drives MPLAB X's own prjMakefilesGenerator. The IDE must
:: therefore be INSTALLED, but it never has to be opened.
::
:: Tool setup (once per machine):
::     install_dependencies.bat       (pyserial for the python tools)
::     python scripts\setup_compiler.py   (pick the installed XC32 version)
::     install.bat --install          (pyOCD for flash.bat)
::     python scripts\setup_debug.py      (SAME54_DFP tool-pack fix for VS Code)
:: Then:
::     build.bat [incremental|clean|rebuild|help]
::     flash.bat
:: ===========================================================================

set "SCRIPT_DIR=%~dp0"
set "MPLAB_DIR=%SCRIPT_DIR%firmware\T1S_100BaseT_Bridge.X"
set "PROJ_NAME=T1S_100BaseT_Bridge"
set "CONF=default"
set "TYPE_IMAGE=PRODUCTION"
set "DIST_DIR=%MPLAB_DIR%\dist\%CONF%\production"
set "ELF_PATH=%DIST_DIR%\%PROJ_NAME%.X.production.elf"
set "HEX_PATH=%DIST_DIR%\%PROJ_NAME%.X.production.hex"
set "COMPILER_CONFIG=%SCRIPT_DIR%setup_compiler.config"
set "MPLABX_MAKE="
for /f "delims=" %%D in ('dir /b /ad /o-n "C:\Program Files\Microchip\MPLABX\v*" 2^>nul') do (
    if not defined MPLABX_MAKE if exist "C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe" (
        set "MPLABX_MAKE=C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe"
    )
)

rem Self-healing: the nbproject Makefile fragments are gitignored (they carry
rem absolute paths of the machine that made them), so a fresh clone has none.
rem Generate them here rather than sending the user into the IDE - genmk.bat
rem calls the generator MPLAB X ships for exactly this purpose. The IDE only
rem has to be INSTALLED, not opened.
if not exist "%MPLAB_DIR%\nbproject\Makefile-impl.mk" (
    echo nbproject Makefile fragments are missing - generating them now.
    call "%SCRIPT_DIR%genmk.bat" "%MPLAB_DIR%"
    if errorlevel 1 (
        echo.
        echo ERROR: could not generate the nbproject Makefiles ^(see above^).
        echo        Without them make has nothing to drive. If MPLAB X is not
        echo        installed, install it - opening it is not necessary.
        exit /b 1
    )
    echo.
)

if not defined MPLABX_MAKE (
    echo ERROR: Could not find MPLAB X's bundled make.exe under
    echo        C:\Program Files\Microchip\MPLABX\v*\gnuBins\GnuWin32\bin\
    echo        Install MPLAB X IDE.
    exit /b 1
)
echo Make      : %MPLABX_MAKE%

rem Parallel compile. Makefile-impl.mk invokes the per-configuration makefile
rem through ${MAKE}, so the sub-make inherits the jobserver and the 326 compile
rem rules run concurrently - measured 2m02s -> 35s for a full rebuild on 14
rem cores. NUMBER_OF_PROCESSORS is set by Windows in every cmd environment, so
rem nothing has to be detected at setup time and nothing goes stale when the
rem repo moves to another machine.
rem   -Otarget keeps each compiler's output together; without it the messages of
rem   14 concurrent compilers interleave and a failure cannot be traced back to
rem   a file. MPLAB X v6.25 ships GNU Make 4.4.1, which supports both on Windows.
rem Set BUILD_JOBS=1 to reproduce a failure serially.
if not defined BUILD_JOBS set "BUILD_JOBS=%NUMBER_OF_PROCESSORS%"
if not defined BUILD_JOBS set "BUILD_JOBS=1"
set "MAKE_PARALLEL=-j%BUILD_JOBS% -Otarget"
echo Jobs      : %BUILD_JOBS% parallel ^(override with BUILD_JOBS=n^)

rem NOTE: deliberately NOT passing MP_CC_DIR on the make command line here.
rem nbproject\Makefile-local-default.mk (written by MPLAB X itself) already
rem defines it with the correct absolute path from the last IDE build. Any
rem command-line MP_CC_DIR - even blank, e.g. from a flaky read below - takes
rem precedence over that file's assignment and silently breaks xc32-bin2hex
rem ("undefined reference"-free link, then "\xc32-bin2hex: file not found").
rem setup_compiler.config is only used here for build_summary.py's xc32-nm.
if exist "%COMPILER_CONFIG%" (
    for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "(Get-Content '%COMPILER_CONFIG%' | ConvertFrom-Json).bin_dir"`) do set "XC32_BIN_DIR=%%D"
    if defined XC32_BIN_DIR echo Compiler  : %XC32_BIN_DIR%
) else (
    echo WARNING: No compiler configured ^(run "python scripts\setup_compiler.py"^).
    echo          build_summary.py's interrupt-handler listing will be empty.
)

set "MODE=incremental"
if not "%~1"=="" set "MODE=%~1"
if /i "%MODE%"=="help"        goto :help
if /i "%MODE%"=="clean"       goto :clean
if /i "%MODE%"=="rebuild"     goto :rebuild
if /i "%MODE%"=="incremental" goto :incremental
echo ERROR: Unknown parameter "%~1"
goto :help

:help
echo Usage: build.bat [incremental^|clean^|rebuild^|help]
echo   (no argument)  Incremental build (default)
echo   clean          Delete all build artifacts for this configuration
echo   rebuild        Clean then perform a full build
echo.
echo Environment:
echo   BUILD_JOBS=n   Parallel compile jobs (default: NUMBER_OF_PROCESSORS = %NUMBER_OF_PROCESSORS%).
echo                  Use BUILD_JOBS=1 to reproduce a build failure serially.
exit /b 0

:clean
echo Cleaning (make clean, CONF=%CONF%)...
pushd "%MPLAB_DIR%"
"%MPLABX_MAKE%" -f Makefile CONF=%CONF% TYPE_IMAGE=%TYPE_IMAGE% clean
popd
exit /b 0

:rebuild
call :clean
goto :build

:incremental
:build
echo [1/1] Building (make, CONF=%CONF%, TYPE_IMAGE=%TYPE_IMAGE%, %BUILD_JOBS% jobs)...
pushd "%MPLAB_DIR%"
"%MPLABX_MAKE%" -f Makefile CONF=%CONF% TYPE_IMAGE=%TYPE_IMAGE% %MAKE_PARALLEL% build
set "BUILD_RC=%errorlevel%"
popd
if not "%BUILD_RC%"=="0" ( echo ERROR: Build failed. & exit /b 1 )

echo.
echo BUILD SUCCESSFUL.
if exist "%HEX_PATH%" (
    echo HEX: %HEX_PATH%
    rem Copy the HEX into release\ so a fresh clone can flash without building.
    rem NOTE: only this script does that copy - a build from inside the MPLAB X
    rem IDE leaves release\ stale. flash.bat programs the dist\ HEX, not this one,
    rem so release\ is not a record of what is on the target. The guard below is
    rem "if exist", not a freshness check: a HEX that survived from an earlier
    rem build gets copied as-is.
    if not exist "%SCRIPT_DIR%release" mkdir "%SCRIPT_DIR%release"
    copy /Y "%HEX_PATH%" "%SCRIPT_DIR%release\T1S_100BaseT_Bridge.hex" >nul
    echo Released: %SCRIPT_DIR%release\T1S_100BaseT_Bridge.hex
) else (
    echo WARNING: expected HEX not found at %HEX_PATH%
)

rem Post-build memory / interrupt summary (flash/RAM, heap, IRQ handlers).
if exist "%ELF_PATH%" python "%SCRIPT_DIR%scripts\build_summary.py" "%DIST_DIR%" "%ELF_PATH%" "%XC32_BIN_DIR%"
endlocal
