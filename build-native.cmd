@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem Builds the native Glimpse.exe (C++17, Win32, GDI+, DWM) with the Visual Studio C++ compiler,
rem statically linked (/MT): one EXE without runtime DLLs. Needs Visual Studio 2019 or later, or the
rem Build Tools, with the "Desktop development with C++" workload; for ARM64 also the ARM64 build tools.
rem
rem   build-native.cmd             for this PC (x64, or ARM64 on an ARM64 PC)
rem   build-native.cmd arm64       for ARM64 (cross-compiles on an x64 PC)
rem   build-native.cmd x64 test    build, then run the self-test
rem
rem Output: out\native-^<arch^>\Glimpse.exe. On Linux, build-native.sh builds the same sources with clang.
setlocal
cd /d "%~dp0"
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if "%~1"=="" if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=arm64"
if /i not "%ARCH%"=="x64" if /i not "%ARCH%"=="arm64" (
  echo usage: build-native.cmd [x64^|arm64] [test]
  exit /b 2
)

rem a Developer Command Prompt for the right target is used as it is
if /i "%VSCMD_ARG_TGT_ARCH%"=="%ARCH%" goto build
set "HOSTARCH=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOSTARCH=arm64"
set "VCARCH=%HOSTARCH%"
if /i not "%HOSTARCH%"=="%ARCH%" set "VCARCH=%HOSTARCH%_%ARCH%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio was not found: vswhere.exe is missing.
  exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio with the C++ tools was not found.
  exit /b 1
)
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (
  echo The C++ tools are not installed in %VSDIR%.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul
if errorlevel 1 (
  echo vcvarsall.bat %VCARCH% failed: are the %ARCH% build tools installed?
  exit /b 1
)

:build
set "OUT=out\native-%ARCH%"
if not exist "%OUT%" mkdir "%OUT%"
rc /nologo /I src /I . /fo "%OUT%\Glimpse.res" src\Glimpse.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /EHsc /GR /std:c++17 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Isrc /Fo"%OUT%\\" src\*.cpp "%OUT%\Glimpse.res" /Fe"%OUT%\Glimpse.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /INCREMENTAL:NO user32.lib gdi32.lib gdiplus.lib dwmapi.lib shcore.lib wtsapi32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib normaliz.lib
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
del /q "%OUT%\*.obj" "%OUT%\Glimpse.res" 2>nul
copy /y Glimpse.ini "%OUT%\" >nul
if not exist "%OUT%\lang" mkdir "%OUT%\lang"
copy /y lang\*.txt "%OUT%\lang\" >nul
echo Built %CD%\%OUT%\Glimpse.exe

if /i not "%~2"=="test" exit /b 0
rem in a batch file cmd waits for the GUI program and keeps the redirection
"%OUT%\Glimpse.exe" /selftest:tests\geometry-cases.txt > "%OUT%\selftest.txt"
set "RC=%ERRORLEVEL%"
type "%OUT%\selftest.txt"
exit /b %RC%
