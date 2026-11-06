@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem Logon script for Glimpse (EXE build, C# or native). Copies Glimpse into the user's own folder when
rem it is missing or differs from the share, then starts it. Nothing is installed into the system, no
rem admin rights needed.
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

rem Program and texts: copied when their content differs from the share. The packages give every file
rem the same date (the same sources make the same zips), so dates cannot tell versions apart. A program
rem that runs in another session of the same user cannot be replaced; it is updated at a later logon.
rem The user's Glimpse.ini is never overwritten.
call :sync "%SRC%Glimpse.exe" "%GLIMPSE_HOME%\Glimpse.exe"
for %%F in ("%SRC%lang\*.txt") do call :sync "%%~fF" "%GLIMPSE_HOME%\lang\%%~nxF"
if not exist "%GLIMPSE_HOME%\Glimpse.ini" if exist "%SRC%Glimpse.ini" copy /y "%SRC%Glimpse.ini" "%GLIMPSE_HOME%\Glimpse.ini" >nul

rem One instance per session: /background makes a second start exit quietly.
if defined GLIMPSE_INI (
  start "" "%GLIMPSE_HOME%\Glimpse.exe" /background "/ini:%GLIMPSE_INI%"
) else (
  start "" "%GLIMPSE_HOME%\Glimpse.exe" /background
)
endlocal
exit /b 0

:sync
if exist "%~2" fc /b "%~1" "%~2" >nul 2>&1 && exit /b 0
copy /y "%~1" "%~2" >nul 2>&1
exit /b 0
