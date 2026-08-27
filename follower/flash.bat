@echo off
rem ===========================================================================
rem  follower\flash.bat - flashes BOTH followers (A and B).
rem
rem  Usage:   follower\flash.bat                ... both followers
rem           follower\flash.bat follower_a     ... only A
rem           follower\flash.bat follower_b     ... only B
rem           follower\flash.bat --list
rem           follower\flash.bat --dry-run
rem
rem  BEIDE, NICHT EINER - und das ist die eigentliche Aenderung vom 2026-08-19.
rem  Vorher stand hier PROJECT=follower, und dieser Name zeigt in boards.json auf
rem  Follower B. Wer "flash den Follower" las, erwartete A oder beide und bekam B;
rem  Follower A behielt seinen alten Stand, und der Aufruf meldete Erfolg. Ein
rem  Name, der eine Mehrzahl suggeriert und eine Einzahl trifft, ist genau der
rem  Fehler, der hier behoben ist.
rem
rem  Die Board-Zuordnung und die Wahl des Abbilds je Rolle liegen in
rem  ..\flash_boards.py, damit es nur EINE Quelle davon gibt.
rem ===========================================================================
setlocal

set "TOOL=%~dp0..\scripts\flash_boards.py"
if not exist "%TOOL%" (
    echo ERROR: %TOOL% missing.
    exit /b 1
)

rem Ohne Argument: beide Follower. Mit Argument wird es durchgereicht, damit
rem `follower\flash.bat follower_a` und `--list` weiter gehen.
if "%~1"=="" (
    python "%TOOL%" followers
) else (
    python "%TOOL%" %*
)
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    echo.
    echo         Voraussetzungen pruefen: python ..\scripts\install_prereqs.py
)
endlocal & exit /b %RC%
