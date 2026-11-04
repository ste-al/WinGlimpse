@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem Logon script for the PowerShell build of Glimpse (Glimpse.ps1 + Glimpse.cs), for servers where
rem EXE files are blocked but PowerShell runs in Full Language mode. Copies the files into the
rem user's own folder when they are missing or older, then starts Glimpse hidden.
rem   GLIMPSE_HOME   per-user folder (default %LOCALAPPDATA%\Glimpse)
rem   GLIMPSE_INI    optional settings file, passed as -Ini
setlocal
set "SRC=%~dp0"
if not exist "%SRC%Glimpse.ps1" if exist "%SRC%..\Glimpse.ps1" set "SRC=%SRC%..\"
if not exist "%SRC%Glimpse.ps1" (
  echo Glimpse.ps1 was not found next to this script.
  exit /b 1
)
if not defined GLIMPSE_HOME set "GLIMPSE_HOME=%LOCALAPPDATA%\Glimpse"
if not exist "%GLIMPSE_HOME%\lang" mkdir "%GLIMPSE_HOME%\lang"

robocopy "%SRC%." "%GLIMPSE_HOME%" Glimpse.ps1 Glimpse.cs /XO /R:1 /W:1 /NFL /NDL /NJH /NJS /NP >nul
robocopy "%SRC%lang" "%GLIMPSE_HOME%\lang" *.txt /XO /R:1 /W:1 /NFL /NDL /NJH /NJS /NP >nul
if not exist "%GLIMPSE_HOME%\Glimpse.ini" if exist "%SRC%Glimpse.ini" copy /y "%SRC%Glimpse.ini" "%GLIMPSE_HOME%\Glimpse.ini" >nul

set "PS=%WINDIR%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined GLIMPSE_INI (
  start "" "%PS%" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%GLIMPSE_HOME%\Glimpse.ps1" -Background -Ini "%GLIMPSE_INI%"
) else (
  start "" "%PS%" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%GLIMPSE_HOME%\Glimpse.ps1" -Background
)
endlocal
exit /b 0
