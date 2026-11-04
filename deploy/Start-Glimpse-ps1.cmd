@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem Logon script for the PowerShell build of Glimpse (Glimpse.ps1 + Glimpse.cs), for servers where
rem EXE files are blocked but PowerShell runs in Full Language mode. Copies the files into the
rem user's own folder when they are missing or differ from the share, then starts Glimpse without a
rem window (Glimpse.ps1 -Background moves itself into a powershell.exe without a console window).
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

rem Program and texts: copied when their content differs from the share (the packages give every file
rem the same date, so dates cannot tell versions apart). The user's Glimpse.ini is never overwritten.
call :sync "%SRC%Glimpse.ps1" "%GLIMPSE_HOME%\Glimpse.ps1"
call :sync "%SRC%Glimpse.cs" "%GLIMPSE_HOME%\Glimpse.cs"
for %%F in ("%SRC%lang\*.txt") do call :sync "%%~fF" "%GLIMPSE_HOME%\lang\%%~nxF"
if not exist "%GLIMPSE_HOME%\Glimpse.ini" if exist "%SRC%Glimpse.ini" copy /y "%SRC%Glimpse.ini" "%GLIMPSE_HOME%\Glimpse.ini" >nul

rem Run in this script's console, which is hidden for a logon script: with -Background, Glimpse.ps1
rem starts the real Glimpse in a powershell.exe without a console window and returns at once.
set "PS=%WINDIR%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined GLIMPSE_INI (
  "%PS%" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%GLIMPSE_HOME%\Glimpse.ps1" -Background -Ini "%GLIMPSE_INI%"
) else (
  "%PS%" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%GLIMPSE_HOME%\Glimpse.ps1" -Background
)
endlocal
exit /b 0

:sync
if exist "%~2" fc /b "%~1" "%~2" >nul 2>&1 && exit /b 0
copy /y "%~1" "%~2" >nul 2>&1
exit /b 0
