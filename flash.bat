@echo off
rem ===========================================================================
rem  flash.bat - flashes the T1S<->100BASE-T bridge firmware onto the SAM E54
rem              board via pyOCD (EDBG probe, no MPLAB X needed at flash time)
rem
rem  Usage:   flash.bat                  ... flash the default build output
rem           flash.bat --dry-run        ... only show what would run
rem           flash.bat <file.hex> ...   ... flash a different image
rem           flash.bat --list           ... list connected probes
rem
rem  Which board gets programmed is decided once with "install.bat --select" and
rem  recorded in bench.json; flash.bat reads it from there. Without a recorded
rem  choice pyOCD picks the probe itself, which only works with one board on USB.
rem
rem  All further arguments are passed through to flash_same54.py.
rem ===========================================================================
setlocal

rem Serial number of the EDBG probe to program. Normally left empty: the board is
rem picked once with "install.bat --select", which records its serial in bench.json,
rem and the lookup below fills PROBE from there - so a plain "flash.bat" always hits
rem the same board even with several plugged in. Setting it here overrides bench.json
rem for this file; "flash.bat --probe <serial>" overrides it for a single run.
set "PROBE="

set "TOOL=%~dp0scripts\flash_same54.py"
set "HEX=%~dp0firmware\T1S_100BaseT_Bridge.X\dist\default\production\T1S_100BaseT_Bridge.X.production.hex"

if not exist "%TOOL%" (
    echo ERROR: %TOOL% missing.
    exit /b 1
)

rem --- board chosen with "install.bat --select" --------------------------
rem The serial lives in bench.json; flash_same54.py --show-probe is the one place
rem that reads it, so batch and Python cannot drift apart. Exit code 1 (nothing
rem recorded) leaves PROBE empty, which is the old behaviour: pyOCD picks the probe
rem itself, and that works as long as exactly one board is on USB.
if not defined PROBE (
    for /f "usebackq delims=" %%P in (`python "%TOOL%" --show-probe 2^>nul`) do set "PROBE=%%P"
)

rem --- first argument: image, unless it's an option -------------------
set "ARGS=%*"
set "FIRST=%~1"
if not "%FIRST%"=="" (
    echo %FIRST% | findstr /b /c:"-" >nul
    if errorlevel 1 (
        set "HEX=%~f1"
        shift
        set "ARGS=%1 %2 %3 %4 %5 %6 %7 %8 %9"
    )
)

rem --- special case --list: no image needed -------------------------------
echo %ARGS% | findstr /c:"--list" >nul
if not errorlevel 1 (
    python "%TOOL%" --list
    exit /b %errorlevel%
)

if not exist "%HEX%" (
    echo ERROR: %HEX% does not exist.
    echo         Run build.bat first ^(or open+build the project once in MPLAB X^).
    exit /b 1
)

set "PROBEARG="
if not "%PROBE%"=="" set "PROBEARG=--probe %PROBE%"

echo [flash] Image : %HEX%
if not "%PROBE%"=="" echo [flash] Probe : %PROBE%
echo.

python "%TOOL%" "%HEX%" %PROBEARG% %ARGS%
if errorlevel 1 (
    echo.
    echo ERROR: flashing failed.
    echo         Check prerequisites: python scripts\install_prereqs.py
    exit /b 1
)

echo.
echo %ARGS% | findstr /c:"--dry-run" >nul
if not errorlevel 1 (
    echo [ok   ] Dry run - the board was not touched.
    exit /b 0
)
echo [ok   ] flashed and reset.
echo         Console: EDBG COM port, 115200 8N1.
exit /b 0
