<#
.SYNOPSIS
    End-to-end smoke test of Glimpse on a real Windows desktop (CI runs it on GitHub's Windows runners).

.DESCRIPTION
    Starts Glimpse with a test Glimpse.ini, opens test windows and checks what Glimpse does:
      1. a new window opened exactly on top of another moves so both keep a glimpse;
      2. a window dropped so it covers all of another one but a 30 px strip nudges the covered
         window by the smallest move that gives it back a glimpse;
      3. the toast's Undo restores the nudged window's exact rectangle;
      4. Ctrl+Alt+Space opens X-ray with the keyboard focus, Esc closes it;
      5. a second start with /settings opens the settings window, and its controls have names,
         roles and states for screen readers (MSAA);
      6. Glimpse keeps running through all of it, /exit closes it, and the log has no errors.
    The test windows come from a small WinForms program compiled on the fly (no console windows), and
    every other window is minimized first, so only the test windows count.
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
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindow(string cls, string title);
    // by title only; PowerShell would pass $null for a string parameter as "", which matches no class
    public static IntPtr FindTitle(string title) { return FindWindow(null, title); }
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    // class and title, for messages
    public static string Describe(IntPtr h)
    {
        if (h == IntPtr.Zero) return "none";
        var c = new System.Text.StringBuilder(256);
        var t = new System.Text.StringBuilder(256);
        GetClassName(h, c, 256);
        GetWindowText(h, t, 256);
        return c + " \"" + t + "\"";
    }
    public static RECT Frame(IntPtr h)
    {
        RECT r;
        if (DwmGetWindowAttribute(h, 9, out r, 16) != 0) GetWindowRect(h, out r);
        return r;
    }
}
'@
[void][E2E]::SetProcessDPIAware()

# what a screen reader that uses MSAA (NVDA, JAWS) reads from the child windows of a window
Add-Type -ReferencedAssemblies Accessibility -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Msaa
{
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr l);
    [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object acc);
    public sealed class Item { public string Name; public int Role; public int State; }
    public static Item[] Children(IntPtr top)
    {
        var list = new List<Item>();
        EnumChildWindows(top, delegate(IntPtr h, IntPtr l)
        {
            Guid iid = new Guid("618736e0-3c3d-11cf-810c-00aa00389b71");   // IAccessible
            object o;
            if (AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref iid, out o) == 0)   // OBJID_CLIENT
            {
                var a = o as Accessibility.IAccessible;
                if (a != null)
                {
                    var it = new Item();
                    try { it.Name = a.get_accName(0); } catch { }
                    try { object r = a.get_accRole(0); if (r is int) it.Role = (int)r; } catch { }
                    try { object st = a.get_accState(0); if (st is int) it.State = (int)st; } catch { }
                    list.Add(it);
                }
            }
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }
}
'@

$failed = 0
$results = New-Object 'System.Collections.Generic.List[string]'
function Check([bool]$ok, [string]$name, [string]$detail) {
    if ($ok) { Write-Host "ok - $name"; $script:results.Add("| :white_check_mark: | $name | |") }
    else { Write-Host "not ok - $name $detail"; $script:failed++; $script:results.Add("| :x: | $name | $($detail -replace '\|', '/' -replace '[\r\n]+', ' ') |") }
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

# The test windows: a WinForms program without a console (a console helper would add windows of its
# own, and with Windows Terminal as the default terminal it cannot even be hidden).
$testExe = Join-Path $Out 'GlimpseTestWindow.exe'
if (Test-Path -LiteralPath $testExe) { Remove-Item -LiteralPath $testExe -Force }
Add-Type -OutputAssembly $testExe -OutputType WindowsApplication -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Windows.Forms;
public static class GlimpseTestWindow
{
    [STAThread]
    public static void Main(string[] a)
    {
        var f = new Form();
        f.Text = a[0];
        f.StartPosition = FormStartPosition.Manual;
        f.Location = new Point(int.Parse(a[1]), int.Parse(a[2]));
        f.Size = new Size(int.Parse(a[3]), int.Parse(a[4]));
        Application.Run(f);
    }
}
'@

$testProcesses = @()
function Start-TestWindow([string]$title, [int]$x, [int]$y, [int]$w, [int]$h) {
    $p = Start-Process -FilePath $testExe -ArgumentList "`"$title`"", $x, $y, $w, $h -PassThru
    $script:testProcesses += $p
    for ($i = 0; $i -lt 150; $i++) {
        Start-Sleep -Milliseconds 100
        $h = [E2E]::FindTitle($title)
        if ($h -ne [IntPtr]::Zero -and [E2E]::IsWindowVisible($h)) { return $h }
    }
    throw "test window '$title' did not appear"
}

function Click([int]$x, [int]$y) {
    [void][E2E]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 150
    [E2E]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # left down
    Start-Sleep -Milliseconds 60
    [E2E]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # left up
}

function Drag([int]$fromX, [int]$fromY, [int]$toX, [int]$toY) {
    [void][E2E]::SetCursorPos($fromX, $fromY)
    Start-Sleep -Milliseconds 200
    [E2E]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # left down
    Start-Sleep -Milliseconds 250
    $steps = 25
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
$work = $screen.WorkingArea
Write-Host "screen $($screen.Bounds) work area $work"

# a clean desktop: only the test windows count
try { (New-Object -ComObject Shell.Application).MinimizeAll(); Start-Sleep -Seconds 1 } catch { Write-Host "MinimizeAll is not available: $_" }

# a test Glimpse.ini: no Start with Windows, no hot corner, English texts, debug log
$ini = Join-Path $Out 'Glimpse.ini'
$defaults = Join-Path (Split-Path -Parent $Exe) 'Glimpse.ini'
$text = if (Test-Path $defaults) { Get-Content -Raw -LiteralPath $defaults } else { "[Glimpse]`r`n" }
$text = $text -replace 'StartWithWindows=1', 'StartWithWindows=0' -replace 'HotCorner=TopLeft', 'HotCorner=Off' -replace 'Language=auto', 'Language=en' -replace 'LogLevel=Info', 'LogLevel=Debug'
[System.IO.File]::WriteAllText($ini, $text)
$log = Join-Path $env:LOCALAPPDATA 'Glimpse\Glimpse.log'
if (Test-Path $log) { Remove-Item -LiteralPath $log -Force }

# when Glimpse is gone too early: its exit code and the crash report Windows logged, if any
function CrashDetail {
    if (-not $glimpse.HasExited) { return '' }
    $d = 'exit code 0x{0:X8}' -f $glimpse.ExitCode
    try {
        $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error'; StartTime = $started } -MaxEvents 10 -ErrorAction Stop |
            Where-Object { $_.Message -match 'Glimpse' } | Select-Object -First 1
        if ($ev) { $d += '; ' + (($ev.Message -split "`r?`n" | Where-Object { $_ -match 'Faulting|Exception code|Fault offset' }) -join '; ') }
    } catch { }
    return $d
}

$started = Get-Date
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

    # ---- 2. drop B over C so that only a 30 px strip of C stays free: C moves 20 px down
    $c = Start-TestWindow 'Glimpse E2E C' 600 300 380 320
    Start-Sleep -Seconds 1
    $rb = Rect $b; $rc = Rect $c; $ra = Rect $a
    Write-Host "before the drag: A $(Fmt $ra) B $(Fmt $rb) C $(Fmt $rc)"
    $bw = $rb.R - $rb.L; $bh = $rb.B - $rb.T
    $targetL = $rc.L - 40
    $targetT = $rc.B - 30 - $bh
    if ($rc.B + 60 -gt $work.Bottom -or $bw -lt ($rc.R - $rc.L) + 80) { Write-Host "warning: the layout leaves no room for the expected nudge" }
    # activate B with a click on its title bar, then drag it by the title bar
    $grabX = $rb.L + 150; $grabY = $rb.T + 12
    Click $grabX $grabY
    Start-Sleep -Milliseconds 400
    Drag $grabX $grabY ($grabX + $targetL - $rb.L) ($grabY + $targetT - $rb.T)
    Start-Sleep -Seconds 2
    Shot '3-after-drop'
    $rb2 = Rect $b; $rc2 = Rect $c
    Write-Host "dragged B to $(Fmt $rb2) (target $targetL,$targetT); C $(Fmt $rc) -> $(Fmt $rc2)"
    Check ($rb2.L -eq $targetL -and $rb2.T -eq $targetT) 'the drag moved the window to the target' "B $(Fmt $rb) -> $(Fmt $rb2), target $targetL,$targetT"
    Check ((Fmt $rc2) -ne (Fmt $rc)) 'the covered window is nudged on drop' "C stayed at $(Fmt $rc2)"
    Check ($rc2.T - $rc.T -eq 20 -and $rc2.L -eq $rc.L) 'the nudge is the smallest move: 20 px down' "C $(Fmt $rc) -> $(Fmt $rc2)"
    Check (HasGlimpse $rc2 @($rb2, (Rect $a))) 'the nudged window has a glimpse' "C $(Fmt $rc2) B $(Fmt $rb2)"

    # ---- 3. the toast's Undo restores the exact previous rectangle
    $toast = [E2E]::FindTitle('Glimpse toast')
    Check ($toast -ne [IntPtr]::Zero) 'a toast offers Undo after the fix' ''
    if ($toast -ne [IntPtr]::Zero) {
        $rt = [E2E]::Frame($toast)
        # the Undo button sits at the right end of the toast
        Click ($rt.R - 45) ([int](($rt.T + $rt.B) / 2))
        Start-Sleep -Seconds 1
        $rc3 = Rect $c
        Shot '3b-after-undo'
        Check ((Fmt $rc3) -eq (Fmt $rc)) 'Undo restores the exact previous rectangle' "C $(Fmt $rc3), expected $(Fmt $rc)"
    }

    # ---- 4. X-ray with Ctrl+Alt+Space, Esc closes
    Keys @(0x11, 0x12, 0x20)
    Start-Sleep -Seconds 2
    Shot '4-xray'
    $xray = [E2E]::FindTitle('Glimpse X-ray')
    Check ($xray -ne [IntPtr]::Zero) 'Ctrl+Alt+Space opens X-ray' ''
    $fgw = [E2E]::GetForegroundWindow()
    Check ($xray -ne [IntPtr]::Zero -and $fgw -eq $xray) 'X-ray has the keyboard focus' ('foreground: ' + [E2E]::Describe($fgw))
    Keys @(0x1B)
    Start-Sleep -Seconds 1
    $xray = [E2E]::FindTitle('Glimpse X-ray')
    Check ($xray -eq [IntPtr]::Zero -or -not [E2E]::IsWindowVisible($xray)) 'Esc closes X-ray' ''
    Check (-not $glimpse.HasExited) 'Glimpse is still running after X-ray' (CrashDetail)

    # ---- 5. a second start with /settings opens the settings window
    Start-Process -FilePath $Exe -ArgumentList '/settings' | Out-Null
    $settings = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $settings -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $settings = [E2E]::FindTitle('Glimpse settings') }
    Start-Sleep -Seconds 1
    Shot '5-settings'
    Check ($settings -ne [IntPtr]::Zero) 'a second start with /settings opens Settings' ''
    if ($settings -ne [IntPtr]::Zero) {
        # the custom-drawn controls tell screen readers what they are (MSAA: role 0x2C check button,
        # 0x2D radio button; state 0x10 checked)
        try {
            $items = @([Msaa]::Children($settings))
            $named = @($items | Where-Object { $_.Name } | ForEach-Object { '{0} [{1:X}/{2:X}]' -f $_.Name, $_.Role, $_.State })
            Write-Host ("MSAA: " + $items.Count + " windows; " + ($named -join '; '))
            $start = @($items | Where-Object { $_.Role -eq 0x2C -and $_.Name -eq 'Start with Windows' })
            $modes = @($items | Where-Object { $_.Role -eq 0x2D -and $_.Name -match '^(Guide|Assist|Strict)' })
            $chosen = @($modes | Where-Object { $_.State -band 0x10 })
            $detail = ($named | Select-Object -First 14) -join '; '
            Check ($start.Count -eq 1) 'Settings: screen readers see the Start with Windows toggle as a check box' $detail
            Check ($modes.Count -eq 3 -and $chosen.Count -eq 1) 'Settings: screen readers see the mode cards as radio buttons, one selected' $detail
        } catch {
            Check $false 'Settings: MSAA' $_.Exception.Message
        }
        # UI Automation (Narrator): reported for information
        try {
            Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
            $all = @([System.Windows.Automation.AutomationElement]::FromHandle($settings).FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition))
            $seen = @($all | ForEach-Object { $_.Current.ControlType.ProgrammaticName.Replace('ControlType.', '') + ' ' + $_.Current.Name } | Where-Object { $_ -notmatch ' $' })
            Write-Host ("UI Automation: " + $all.Count + " elements; " + (($seen | Select-Object -First 40) -join '; '))
        } catch {
            Write-Host ("UI Automation: " + $_.Exception.Message)
        }
        [void][E2E]::PostMessage($settings, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    Start-Sleep -Seconds 1
}
catch {
    Check $false 'scenario' $_.Exception.Message
}
finally {
    # ---- 6. /exit ends Glimpse (a Glimpse that is already gone does not count as closed by /exit)
    $alive = -not $glimpse.HasExited
    Check $alive 'Glimpse kept running through the scenario' (CrashDetail)
    if ($alive) {
        Start-Process -FilePath $Exe -ArgumentList '/exit' -Wait | Out-Null
        $glimpse.WaitForExit(10000) | Out-Null
        Check $glimpse.HasExited '/exit closes the running Glimpse' ''
    }
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
# on GitHub Actions: a table on the run page, so a failure is visible without opening the step log
if ($env:GITHUB_STEP_SUMMARY) {
    $lines = @("### End-to-end test: $([System.IO.Path]::GetFileName((Split-Path -Parent $Exe)))\$([System.IO.Path]::GetFileName($Exe))", '', '| | Check | Detail |', '|---|---|---|') + $results + @('', "$failed failed", '')
    try { Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value $lines -Encoding UTF8 } catch { }
}
exit $failed
