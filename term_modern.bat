@echo off
:: ===========================================================================
:: term_modern.bat - the modern-theme (sv-ttk) build of gui_term.py.
::
:: Same tool as term.bat/gui_term.py, restyled - see gui_term_modern.py's
:: module docstring. Port assignment and Display settings still come from
:: term_ports.json, shared with the plain build.
::
:: Started with pythonw so no second window (the console) sits behind the
:: GUI. If something goes wrong there is nothing to see there; run it by
:: hand instead:
::     python gui_term_modern.py
::
:: Addressed via %~dp0 so this also works from Git Bash and from Explorer.
:: ===========================================================================
setlocal

where pythonw >nul 2>&1
if errorlevel 1 (
    python "%~dp0gui_term_modern.py" %*
    set "RC=%ERRORLEVEL%"
    if not "%RC%"=="0" (
        echo.
        pause
    )
    exit /b %RC%
)

start "" pythonw "%~dp0gui_term_modern.py" %*
exit /b 0
