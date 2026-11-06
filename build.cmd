@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem Builds Glimpse.exe on Windows with the C# compiler that ships with the .NET Framework.
rem No SDK, Visual Studio or admin rights needed; run it in the folder that holds Glimpse.cs.
rem
rem   build.cmd              AnyCPU: runs as 64-bit on x64 and natively on ARM64 (.NET Framework 4.8.1)
rem   build.cmd x64          x64 only
rem   build.cmd anycpu test  build, then run the self-test
setlocal
cd /d "%~dp0"
set "PLATFORM=%~1"
if "%PLATFORM%"=="" set "PLATFORM=anycpu"

set "CSC="
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" if exist "%WINDIR%\Microsoft.NET\FrameworkArm64\v4.0.30319\csc.exe" set "CSC=%WINDIR%\Microsoft.NET\FrameworkArm64\v4.0.30319\csc.exe"
if not defined CSC if exist "%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not defined CSC if exist "%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not defined CSC (
  echo csc.exe from the .NET Framework 4.x was not found.
  exit /b 1
)
echo Compiler: %CSC%

"%CSC%" /nologo /target:winexe /optimize+ /platform:%PLATFORM% /win32manifest:Glimpse.manifest /win32icon:Glimpse.ico /out:Glimpse.exe Glimpse.cs
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
echo Built %CD%\Glimpse.exe

if /i not "%~2"=="test" exit /b 0
set "CASES=tests\geometry-cases.txt"
if not exist "%CASES%" set "CASES=geometry-cases.txt"
rem in a batch file cmd waits for the GUI program and keeps the redirection
Glimpse.exe /selftest:%CASES% > selftest.txt
set "RC=%ERRORLEVEL%"
type selftest.txt
exit /b %RC%
