<#
.SYNOPSIS
    End-to-end smoke test of Glimpse on a real Windows desktop (CI runs it on GitHub's Windows runners).

.DESCRIPTION
    Starts Glimpse with a test Glimpse.ini, opens test windows and checks what Glimpse does:
      1. a new window opened exactly on top of another moves so both keep a glimpse;
      2. dragging a window over another one nudges the covered window on drop;
      3. the toast's Undo restores the nudged window's exact rectangle;
      4. Ctrl+Alt+Space opens X-ray, Esc closes it;
      5. a second start with /settings opens the settings window;
      6. /exit closes Glimpse, and the log has no errors.
    Screenshots and the log are written to -Out. The exit code is the number of failed checks.

.PARAMETER Exe
    Path of the Glimpse.exe to test.

.PARAMETER Out
    Folder for screenshots, the test Glimpse.ini and the log.

.NOTES
    SPDX-License-Identifier: GPL-3.0-or-later
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Out = 'e2e-out'
)

$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path -LiteralPath $Exe).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class E2E
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int size);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, IntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static RECT Frame(IntPtr h)
    {
        RECT r;
        if (DwmGetWindowAttribute(h, 9, out r, 16) != 0) GetWindowRect(h, out r);
        return r;
    }
}
'@
[void][E2E]::SetProcessDPIAware()

$failed = 0
function Check([bool]$ok, [string]$name, [string]$detail) {
    if ($ok) { Write-Host "ok - $name" }
    else { Write-Host "not ok - $name $detail"; $script:failed++ }
}

function Shot([string]$name) {
    try {
        $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
        $g.Dispose()
        $bmp.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
    } catch { Write-Host "screenshot $name failed: $_" }
}

function Rect($h) { $r = [E2E]::Frame($h); return @{ L = $r.L; T = $r.T; R = $r.R; B = $r.B } }
function Fmt($r) { return "$($r.L),$($r.T),$($r.R),$($r.B)" }
function Intersects($a, $b) { return $a.L -lt $b.R -and $b.L -lt $a.R -and $a.T -lt $b.B -and $b.T -lt $a.B }

# A window has a glimpse if a 50x50 square on one of its edges overlaps none of the others.
function HasGlimpse($w, $others) {
    $P = 50
    $spots = @()
    for ($x = $w.L; $x -le $w.R - $P; $x += 5) { $spots += @{ L = $x; T = $w.T; R = $x + $P; B = $w.T + $P }; $spots += @{ L = $x; T = $w.B - $P; R = $x + $P; B = $w.B } }
    for ($y = $w.T; $y -le $w.B - $P; $y += 5) { $spots += @{ L = $w.L; T = $y; R = $w.L + $P; B = $y + $P }; $spots += @{ L = $w.R - $P; T = $y; R = $w.R; B = $y + $P } }
    foreach ($s in $spots) {
        $free = $true
        foreach ($o in $others) { if (Intersects $s $o) { $free = $false; break } }
        if ($free) { return $true }
    }
    return $false
}

$testProcesses = @()
function Start-TestWindow([string]$title, [int]$x, [int]$y, [int]$w, [int]$h) {
    $code = "Add-Type -AssemblyName System.Windows.Forms; " +
        "`$f = New-Object System.Windows.Forms.Form; `$f.Text = '$title'; `$f.StartPosition = 'Manual'; " +
        "`$f.Location = New-Object System.Drawing.Point($x, $y); `$f.Size = New-Object System.Drawing.Size($w, $h); " +
        "[System.Windows.Forms.Application]::Run(`$f)"
    $p = Start-Process powershell.exe -ArgumentList '-NoProfile', '-Command', $code -PassThru
    $script:testProcesses += $p
    for ($i = 0; $i -lt 150; $i++) {
        Start-Sleep -Milliseconds 100
        $h = [E2E]::FindWindow($null, $title)
        if ($h -ne [IntPtr]::Zero -and [E2E]::IsWindowVisible($h)) { return $h }
    }
    throw "test window '$title' did not appear"
}

function Drag([int]$fromX, [int]$fromY, [int]$toX, [int]$toY) {
    [void][E2E]::SetCursorPos($fromX, $fromY)
    Start-Sleep -Milliseconds 200
    [E2E]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # left down
    Start-Sleep -Milliseconds 200
    $steps = 20
    for ($i = 1; $i -le $steps; $i++) {
        [void][E2E]::SetCursorPos([int]($fromX + ($toX - $fromX) * $i / $steps), [int]($fromY + ($toY - $fromY) * $i / $steps))
        Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 600
    Shot '2-during-drag'
    [E2E]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # left up
}

function Keys([byte[]]$down) {
    foreach ($k in $down) { [E2E]::keybd_event($k, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 30 }
    [array]::Reverse($down)
    foreach ($k in $down) { [E2E]::keybd_event($k, 0, 2, [IntPtr]::Zero); Start-Sleep -Milliseconds 30 }
}

$screen = [System.Windows.Forms.Screen]::PrimaryScreen
Write-Host "screen $($screen.Bounds) work area $($screen.WorkingArea)"

# a test Glimpse.ini: no Start with Windows, no hot corner, debug log
$ini = Join-Path $Out 'Glimpse.ini'
$defaults = Join-Path (Split-Path -Parent $Exe) 'Glimpse.ini'
$text = if (Test-Path $defaults) { Get-Content -Raw -LiteralPath $defaults } else { "[Glimpse]`r`n" }
$text = $text -replace 'StartWithWindows=1', 'StartWithWindows=0' -replace 'HotCorner=TopLeft', 'HotCorner=Off' -replace 'LogLevel=Info', 'LogLevel=Debug'
[System.IO.File]::WriteAllText($ini, $text)
$log = Join-Path $env:LOCALAPPDATA 'Glimpse\Glimpse.log'
if (Test-Path $log) { Remove-Item -LiteralPath $log -Force }

$glimpse = Start-Process -FilePath $Exe -ArgumentList "/ini:$ini" -PassThru
Start-Sleep -Seconds 4
Check (-not $glimpse.HasExited) 'Glimpse starts and keeps running' ''

try {
    # ---- 1. a new window on top of another one yields
    $a = Start-TestWindow 'Glimpse E2E A' 40 40 500 360
    Start-Sleep -Seconds 1
    $b = Start-TestWindow 'Glimpse E2E B' 40 40 500 360
    Start-Sleep -Seconds 2
    $ra = Rect $a; $rb = Rect $b
    Shot '1-new-window'
    Check ((Fmt $ra) -ne (Fmt $rb)) 'a new window opened on top of another is moved' "A $(Fmt $ra) B $(Fmt $rb)"
    Check (HasGlimpse $ra @($rb)) 'the older window keeps a glimpse' "A $(Fmt $ra) B $(Fmt $rb)"
    Check (HasGlimpse $rb @($ra)) 'the new window has a glimpse' "A $(Fmt $ra) B $(Fmt $rb)"

    # ---- 2. dragging B over C nudges C on drop
    $c = Start-TestWindow 'Glimpse E2E C' 620 380 380 320
    Start-Sleep -Seconds 1
    $rb = Rect $b; $rc = Rect $c
    [void][E2E]::SetForegroundWindow($b)
    Start-Sleep -Milliseconds 300
    $grabX = $rb.L + 150; $grabY = $rb.T + 12
    Drag $grabX $grabY ($grabX + (600 - $rb.L)) ($grabY + (350 - $rb.T))
    Start-Sleep -Seconds 2
    Shot '3-after-drop'
    $rb2 = Rect $b; $rc2 = Rect $c
    Write-Host "dragged B to $(Fmt $rb2); C $(Fmt $rc) -> $(Fmt $rc2)"
    Check ((Fmt $rb2) -ne (Fmt $rb)) 'the drag moved the window' "B $(Fmt $rb) -> $(Fmt $rb2)"
    Check ((Fmt $rc2) -ne (Fmt $rc)) 'the covered window is nudged on drop' "C stayed at $(Fmt $rc2)"
    Check (HasGlimpse $rc2 @($rb2, (Rect $a))) 'the nudged window has a glimpse' "C $(Fmt $rc2) B $(Fmt $rb2)"

    # ---- 3. the toast's Undo restores the exact previous rectangle
    $toast = [E2E]::FindWindow($null, 'Glimpse toast')
    Check ($toast -ne [IntPtr]::Zero) 'a toast offers Undo after the fix' ''
    if ($toast -ne [IntPtr]::Zero) {
        $rt = [E2E]::Frame($toast)
        # the Undo button sits at the right end of the toast
        [void][E2E]::SetCursorPos($rt.R - 45, [int](($rt.T + $rt.B) / 2))
        Start-Sleep -Milliseconds 200
        [E2E]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
        [E2E]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Seconds 1
        $rc3 = Rect $c
        Shot '3b-after-undo'
        Check ((Fmt $rc3) -eq (Fmt $rc)) 'Undo restores the exact previous rectangle' "C $(Fmt $rc3), expected $(Fmt $rc)"
    }

    # ---- 4. X-ray with Ctrl+Alt+Space, Esc closes
    Keys @(0x11, 0x12, 0x20)
    Start-Sleep -Seconds 2
    Shot '4-xray'
    $xray = [E2E]::FindWindow($null, 'Glimpse X-ray')
    Check ($xray -ne [IntPtr]::Zero) 'Ctrl+Alt+Space opens X-ray' ''
    Keys @(0x1B)
    Start-Sleep -Seconds 1
    $xray = [E2E]::FindWindow($null, 'Glimpse X-ray')
    Check ($xray -eq [IntPtr]::Zero) 'Esc closes X-ray' ''

    # ---- 5. a second start with /settings opens the settings window
    Start-Process -FilePath $Exe -ArgumentList '/settings' | Out-Null
    $settings = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $settings -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $settings = [E2E]::FindWindow($null, 'Glimpse settings') }
    Start-Sleep -Seconds 1
    Shot '5-settings'
    Check ($settings -ne [IntPtr]::Zero) 'a second start with /settings opens Settings' ''
    if ($settings -ne [IntPtr]::Zero) { [void][E2E]::PostMessage($settings, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    Start-Sleep -Seconds 1
}
catch {
    Check $false 'scenario' $_.Exception.Message
}
finally {
    # ---- 6. /exit ends Glimpse
    Start-Process -FilePath $Exe -ArgumentList '/exit' -Wait | Out-Null
    $glimpse.WaitForExit(10000) | Out-Null
    Check $glimpse.HasExited '/exit closes the running Glimpse' ''
    if (-not $glimpse.HasExited) { $glimpse.Kill() }
    foreach ($p in $testProcesses) { if (-not $p.HasExited) { $p.Kill() } }
}

if (Test-Path $log) {
    Copy-Item -LiteralPath $log -Destination (Join-Path $Out 'Glimpse.log') -Force
    $errors = @(Select-String -LiteralPath $log -Pattern ' ERROR ' -SimpleMatch)
    Check ($errors.Count -eq 0) 'the log has no errors' ($errors | Select-Object -First 3 | Out-String)
    Get-Content -LiteralPath $log | Select-Object -Last 40
} else {
    Check $false 'the log exists' $log
}

Write-Host ("e2e: " + $failed + " failed")
exit $failed
