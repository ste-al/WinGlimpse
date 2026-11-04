Glimpse - keep every window within reach
========================================

Every open window keeps a "glimpse": a corner or edge strip, 50 px by default, that no other
window covers. Every window stays one click away, also inside RDP and Citrix sessions where
Alt+Tab often reaches the local PC instead of the session.

Start
-----
  Glimpse.exe                        EXE build (win-x64 or win-arm64 package)
  powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File Glimpse.ps1
                                     PowerShell build (Glimpse.cs must sit next to it)

Glimpse lives in the notification area: left-click the icon for X-ray, right-click for the menu
(X-ray, Arrange now, Undo last fix, Pause, Settings, Exit). Nothing is installed; Settings has an
optional "Start with Windows" (off by default; per user, no admin rights).

What it does
------------
  * While you drag a window, labels show which windows keep their glimpse (green), get nudged
    on drop (amber) or would be hidden (red). Hold Shift while dropping to skip the fix.
  * After a drop or when a window opens, Glimpse restores the rule with the smallest move.
    Moved windows flash; every fix can be undone (toast or tray menu).
  * Minimum size: windows whose title matches a rule are grown back (200 x 200 px by default).
  * X-ray (Ctrl+Alt+Space, the top-left screen corner, or the tray icon) shows the outline and
    name of every window through whatever covers it. Click, press its number, or type to find.
  * After monitor, resolution, scaling changes or an RDP reconnect, windows are rescued and checked.

Files
-----
  Glimpse.ini   settings; reloaded when it changes, the settings window writes to it
  lang\*.txt    texts (en, de, cs, sk); a new language is one more file, no rebuild
  deploy\       logon scripts that copy Glimpse into each user's folder and start it
  build.cmd     (PowerShell package) compiles Glimpse.exe from Glimpse.cs on the server

Command line
------------
  /ini:<path>   use another settings file (environment variables are expanded; created if missing)
  /settings     open the settings window (also of a Glimpse that already runs)
  /exit         close the Glimpse that runs in this session
  /background   used at logon: exit quietly when Glimpse already runs
  /selftest     run the self-test (needs tests\geometry-cases.txt, or /selftest:<file>)
  Glimpse.ps1 takes the same as -Ini, -Settings, -Exit, -Background, -SelfTest.

Requirements
------------
  Windows 10 or 11, or Windows Server 2019 to 2025, with the .NET Framework 4.7.2 or later
  (built in). The ARM64 EXE needs .NET Framework 4.8.1 (Windows 11 22H2 or later).
  No admin rights, service, driver, DLL injection or keyboard hook. Log and size memory:
  %LOCALAPPDATA%\Glimpse

Source, documentation and license (GPL-3.0-or-later): https://github.com/ste-al/WinGlimpse
