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
:: NO IDE SESSION NEEDED. The nbproject Makefile fragments do not exist in a
:: fresh checkout - they are gitignored because they hold absolute paths of the
:: machine that generated them. This script generates them when missing, from
:: the tracked nbproject\configurations.xml, via genmk.bat -> MPLAB X own
:: prjMakefilesGenerator.bat. MPLAB X must be installed, not opened.
::
:: Tool setup (once per machine):
::     install_dependencies.bat       (pyserial for the python tools)
::     python ..\scripts\setup_compiler.py       (pick the installed XC32 version)
::     install.bat --install          (pyOCD for flash.bat)
::     python ..\scripts\setup_debug.py          (SAME54_DFP tool-pack fix for VS Code)
:: Then:
::     build.bat [incremental|clean|rebuild|help]
::     flash.bat
:: ===========================================================================

set "SCRIPT_DIR=%~dp0"
set "MPLAB_DIR=%SCRIPT_DIR%firmware\T1S_Follower.X"
set "PROJ_NAME=T1S_Follower"
set "CONF=default"
set "TYPE_IMAGE=PRODUCTION"
set "DIST_DIR=%MPLAB_DIR%\dist\%CONF%\production"
set "ELF_PATH=%DIST_DIR%\%PROJ_NAME%.X.production.elf"
set "HEX_PATH=%DIST_DIR%\%PROJ_NAME%.X.production.hex"
set "COMPILER_CONFIG=%SCRIPT_DIR%..\setup_compiler.config"
set "MPLABX_MAKE="
for /f "delims=" %%D in ('dir /b /ad /o-n "C:\Program Files\Microchip\MPLABX\v*" 2^>nul') do (
    if not defined MPLABX_MAKE if exist "C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe" (
        set "MPLABX_MAKE=C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe"
    )
)

rem GEPRUEFT WERDEN ALLE VIER FRAGMENTE, nicht nur Makefile-impl.mk.  Fehlt eines
rem der anderen, lief der Waechter vorher nicht an, und make brach mit
rem "No rule to make target nbproject/Makefile-default.mk" ab - eine Meldung, die
rem nach einem kaputten Projekt klingt und nur eine fehlende erzeugte Datei ist.
rem Gemessen am 2026-08-21 beim Entfernen des MCC-Modells des Followers.
set "MK_MISSING="
for %%F in (Makefile-impl.mk Makefile-default.mk Makefile-variables.mk Makefile-local-default.mk) do (
    if not exist "%MPLAB_DIR%\nbproject\%%F" set "MK_MISSING=1"
)
if defined MK_MISSING (
    echo nbproject Makefile fragments missing - generating them ^(no IDE needed^)...
    call "%SCRIPT_DIR%..\genmk.bat" "%MPLAB_DIR%"
    if errorlevel 1 (
        echo.
        echo ERROR: could not generate the nbproject Makefile fragments.
        echo        They are derived from configurations.xml by MPLAB X and are
        echo        gitignored on purpose - they hold absolute paths of this machine.
        echo        Install MPLAB X IDE ^(opening it is NOT required^), then re-run.
        exit /b 1
    )
)

if not defined MPLABX_MAKE (
    echo ERROR: Could not find MPLAB X's bundled make.exe under
    echo        C:\Program Files\Microchip\MPLABX\v*\gnuBins\GnuWin32\bin\
    echo        Install MPLAB X IDE.
    exit /b 1
)
echo Make      : %MPLABX_MAKE%

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
    echo WARNING: No compiler configured ^(run "python ..\scripts\setup_compiler.py"^).
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
rem BUILD-STEMPEL WAHR HALTEN.  `app.c` traegt __DATE__/__TIME__, und die datieren
rem den letzten COMPILERLAUF DIESER Datei - nicht das Linken.  Ein inkrementeller
rem Build, der nur ptp_trigger.c anfasst, laesst app.o unveraendert, und das Geraet
rem meldet danach einen Stempel von Stunden zuvor.  Genau das ist am 2026-08-17
rem passiert: beide Follower meldeten 18:15:23, geflasht war der Stand von 20:38,
rem und ohne die Messung daneben (S2: 3 Flanken statt 1959) waere der Lauf dem
rem falschen Stand zugeschrieben worden.  Ein Stempel, der luegen kann, ist
rem schlimmer als keiner - also wird app.c vor jedem Build angefasst.
python -c "import os,sys; os.utime(os.path.join(sys.argv[1], 'src', 'app.c'), None)" "%~dp0firmware"

rem PARALLEL BAUEN - gemessen am 2026-08-19: seriell 139 s, -j14 38 s (Faktor 3,7),
rem und das Abbild ist bitgleich (von 15 907 Hex-Zeilen unterschied sich genau eine,
rem der Build-Stempel).  -Otarget haelt die Ausgabe je Ziel zusammen, sonst sind die
rem Meldungen von 14 gleichzeitigen Compilerlaeufen nicht mehr zuzuordnen.
rem RUECKFALL bei einem nur manchmal auftretenden Baufehler: set T1S_JOBS=1
set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=4"
if defined T1S_JOBS set "JOBS=%T1S_JOBS%"
set "JFLAG=-j%JOBS% -Otarget"
if "%JOBS%"=="1" set "JFLAG="
echo [1/1] Building (make %JFLAG%, CONF=%CONF%, TYPE_IMAGE=%TYPE_IMAGE%)...
pushd "%MPLAB_DIR%"
"%MPLABX_MAKE%" %JFLAG% -f Makefile CONF=%CONF% TYPE_IMAGE=%TYPE_IMAGE% build
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
    copy /Y "%HEX_PATH%" "%SCRIPT_DIR%release\T1S_Follower.hex" >nul
    echo Released: %SCRIPT_DIR%release\T1S_Follower.hex
) else (
    echo WARNING: expected HEX not found at %HEX_PATH%
)

rem Post-build memory / interrupt summary (flash/RAM, heap, IRQ handlers).
if exist "%ELF_PATH%" python "%SCRIPT_DIR%..\build_summary.py" "%DIST_DIR%" "%ELF_PATH%" "%XC32_BIN_DIR%"
rem CLI-Dokumentation gegen den Quelltext pruefen (CLI_KOMMANDOS.md, Anhang A).
rem NUR IM EINZELAUFRUF: wird dieses Skript vom uebergeordneten build.bat gerufen,
rem prueft das dort einmal fuer beide Projekte - das Skript sieht ohnehin beide an,
rem und zweimal 1,4 s fuer dasselbe Ergebnis ist verschenkt.  Der Aufrufer setzt
rem dafuer T1S_SKIP_DOCCHECK.
rem
rem UND DER EXITCODE: cli_doc_check.py gibt bei Funden 1 zurueck.  Als LETZTER Befehl
rem der Datei hat es damit einen erfolgreichen Build als Fehlschlag gemeldet, obwohl
rem der Kommentar hier immer das Gegenteil versprach - eine veraltete Doku ist kein
rem Baufehler.  Deshalb steht danach ein ausdrueckliches "exit /b 0".
if not defined T1S_SKIP_DOCCHECK python "%SCRIPT_DIR%..\cli_doc_check.py" --quiet

endlocal
exit /b 0
