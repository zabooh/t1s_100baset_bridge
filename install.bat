@echo off
rem ===========================================================================
rem  install.bat - check and set up the prerequisites for flash.bat
rem
rem  Usage:   install.bat            ... only check (changes nothing)
rem           install.bat --install  ... install missing pyOCD via pip
rem
rem  Checked: Python >= 3.9, pyOCD (verified with 0.43.0), visible EDBG probes
rem  on USB, and a locally present Microchip.SAME54_DFP pack. The pack itself
rem  is deliberately NOT loaded automatically - there is no official direct
rem  URL for it; it comes from an MPLAB X / MCC installation.
rem
rem  Exit code 0 = everything present, 1 = something is missing (details in
rem  the output).
rem ===========================================================================
setlocal

set "TOOL=%~dp0install_prereqs.py"

if not exist "%TOOL%" (
    echo ERROR: %TOOL% missing.
    exit /b 1
)

python "%TOOL%" %*
set "RC=%errorlevel%"

echo.
if "%RC%"=="0" (
    echo [ok   ] Prerequisites satisfied - build.bat and flash.bat are usable.
) else (
    echo [missing] See the messages above. "install.bat --install" fetches missing pyOCD.
)
exit /b %RC%
