@echo off
:: ===========================================================================
:: term.bat - double-click: three serial consoles in ONE window, ready to go.
::
:: Replaces "open three PuTTY/Tera Term windows, set the port on each, drag
:: them into place". Which board is on which COM port comes from
:: term_ports.json (gitignored, machine-specific) - use "Setup > Configure
:: Ports" inside the tool to set it up, not by hand.
::
:: The window comes up DISCONNECTED - a port only opens once you click
:: "Connect All" (or the button in a single pane's header). Force it to
:: connect on startup with:
::     term.bat --connect
::
:: Everything else is passed through, e.g.:
::     term.bat --columns           ... side by side instead of stacked
::     term.bat --font-size 9
::
:: Started with pythonw so no second window (the console) sits behind the
:: GUI. If something goes wrong there is nothing to see there; run it by
:: hand instead:
::     python scripts\gui_term.py --selftest
::     python scripts\gui_term.py
::
:: Addressed via %~dp0 so this also works from Git Bash and from Explorer.
:: ===========================================================================
setlocal

where pythonw >nul 2>&1
if errorlevel 1 (
    python "%~dp0scripts\gui_term.py" %*
    set "RC=%ERRORLEVEL%"
    if not "%RC%"=="0" (
        echo.
        pause
    )
    exit /b %RC%
)

start "" pythonw "%~dp0scripts\gui_term.py" %*
exit /b 0
