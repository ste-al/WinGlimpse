@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem Logon script for Glimpse (EXE build). Copies Glimpse into the user's own folder when it is
rem missing or older, then starts it. Nothing is installed into the system, no admin rights needed.
rem
rem Put the Glimpse files on a share (Glimpse.exe, Glimpse.ini, lang\, and this deploy\ folder) and run
rem this script at logon, for example as a Group Policy logon script.
rem   GLIMPSE_HOME   per-user folder (default %LOCALAPPDATA%\Glimpse)
rem   GLIMPSE_INI    optional settings file, passed as /ini: (for example %APPDATA%\Glimpse\Glimpse.ini)
setlocal
set "SRC=%~dp0"
if not exist "%SRC%Glimpse.exe" if exist "%SRC%..\Glimpse.exe" set "SRC=%SRC%..\"
if not exist "%SRC%Glimpse.exe" (
  echo Glimpse.exe was not found next to this script.
  exit /b 1
)
if not defined GLIMPSE_HOME set "GLIMPSE_HOME=%LOCALAPPDATA%\Glimpse"
if not exist "%GLIMPSE_HOME%\lang" mkdir "%GLIMPSE_HOME%\lang"

rem Program and texts: copied when newer (robocopy /XO). The user's Glimpse.ini is never overwritten.
robocopy "%SRC%." "%GLIMPSE_HOME%" Glimpse.exe /XO /R:1 /W:1 /NFL /NDL /NJH /NJS /NP >nul
robocopy "%SRC%lang" "%GLIMPSE_HOME%\lang" *.txt /XO /R:1 /W:1 /NFL /NDL /NJH /NJS /NP >nul
if not exist "%GLIMPSE_HOME%\Glimpse.ini" if exist "%SRC%Glimpse.ini" copy /y "%SRC%Glimpse.ini" "%GLIMPSE_HOME%\Glimpse.ini" >nul

rem One instance per session: /background makes a second start exit quietly.
if defined GLIMPSE_INI (
  start "" "%GLIMPSE_HOME%\Glimpse.exe" /background "/ini:%GLIMPSE_INI%"
) else (
  start "" "%GLIMPSE_HOME%\Glimpse.exe" /background
)
endlocal
exit /b 0
