<#
.SYNOPSIS
    Glimpse: keep every window within reach (PowerShell build).

.DESCRIPTION
    Compiles Glimpse.cs (next to this script) with Add-Type and runs it inside powershell.exe.
    It is the same program as Glimpse.exe: same source, same behaviour, same Glimpse.ini.
    Use it where EXE files are blocked but PowerShell runs in Full Language mode.

    Start it in the background, as the logon script or Start with Windows does:
        powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File Glimpse.ps1 -Background
    With -Background it starts a second powershell.exe without any console window and exits, so no
    window stays open, also where Windows Terminal is the default terminal (-WindowStyle Hidden alone
    cannot hide a Windows Terminal window). With a settings file elsewhere (variables are expanded):
        powershell.exe -NoProfile -WindowStyle Hidden -File Glimpse.ps1 -Background -Ini "%APPDATA%\Glimpse\Glimpse.ini"

.PARAMETER Ini
    Settings file to use instead of Glimpse.ini next to this script. Created if missing.

.PARAMETER Settings
    Open the settings window (also asks an already running Glimpse to show it).

.PARAMETER Exit
    Ask the Glimpse that runs in this session to exit.

.PARAMETER Background
    Started at logon: when Glimpse already runs in this session, exit quietly; otherwise run Glimpse
    in a powershell.exe without a console window.

.PARAMETER Detached
    Used by -Background for the process it starts; not needed otherwise.

.PARAMETER SelfTest
    Run the self-test: unit tests and the shared geometry cases (tests\geometry-cases.txt or -Cases).

.PARAMETER CheckLang
    List missing, obsolete and mismatched keys in the lang\*.txt files.

.PARAMETER Render
    Write preview images of the overlays, X-ray, settings and tray menu to this folder.

.NOTES
    SPDX-License-Identifier: GPL-3.0-or-later
#>
[CmdletBinding()]
param(
    [string]$Ini,
    [switch]$Settings,
    [switch]$Exit,
    [switch]$Background,
    [switch]$Detached,
    [switch]$SelfTest,
    [string]$Cases,
    [switch]$CheckLang,
    [string]$Render,
    [string]$RenderLang
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$scriptPath = $MyInvocation.MyCommand.Path
$here = Split-Path -Parent $scriptPath

# PowerShell 7 runs on .NET, not the .NET Framework that Glimpse.cs targets: hand over to Windows PowerShell.
if ($PSVersionTable.PSEdition -eq 'Core') {
    $winPs = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $winPs)) { throw 'Glimpse.ps1 needs Windows PowerShell 5.1 (powershell.exe).' }
    $pass = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath)
    foreach ($p in $PSBoundParameters.GetEnumerator()) {
        if ($p.Value -is [System.Management.Automation.SwitchParameter]) { if ($p.Value.IsPresent) { $pass += '-' + $p.Key } }
        else { $pass += '-' + $p.Key; $pass += [string]$p.Value }
    }
    & $winPs @pass
    exit $LASTEXITCODE
}

function Write-GlimpseLog([string]$message) {
    try {
        $dir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Glimpse'
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        $line = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff') + ' [ps1] ERROR ' + $message + "`r`n"
        [System.IO.File]::AppendAllText((Join-Path $dir 'Glimpse.log'), $line, [System.Text.Encoding]::UTF8)
    } catch { }
}

function Show-GlimpseError([string]$message) {
    Write-GlimpseLog $message
    if ($SelfTest -or $CheckLang -or $Render) { Write-Error $message; return }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show($message, 'Glimpse', 'OK', 'Warning')
    } catch { Write-Error $message }
}

$source = Join-Path $here 'Glimpse.cs'
if (-not (Test-Path -LiteralPath $source)) {
    Show-GlimpseError "Glimpse.cs was not found next to Glimpse.ps1 ($here)."
    exit 1
}

# AppLocker or WDAC script rules put PowerShell into Constrained Language Mode, which blocks Add-Type.
if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
    Show-GlimpseError ("PowerShell runs in " + $ExecutionContext.SessionState.LanguageMode + " mode here, which blocks Add-Type. " +
        "Use Glimpse.exe, or ask IT to allow the Glimpse folder.")
    exit 1
}

# At logon (-Background) with Glimpse already running in this session: nothing to do, skip compiling.
if ($Background -and -not $Settings -and -not $Exit) {
    $existing = $null
    if ([System.Threading.Mutex]::TryOpenExisting('Local\Glimpse.SingleInstance', [ref]$existing)) {
        $existing.Dispose()
        exit 0
    }
}

# In the background: hand over to a powershell.exe without a console window and exit. A process started
# with CreateNoWindow has no window in any terminal; -WindowStyle Hidden cannot hide a Windows Terminal tab.
if ($Background -and -not $Detached -and -not $Settings -and -not $Exit) {
    function Quote([string]$s) { return '"' + ($s -replace '(\\+)$', '$1$1') + '"' }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $psi.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ' + (Quote $scriptPath) + ' -Background -Detached'
    if ($Ini) { $psi.Arguments += ' -Ini ' + (Quote $Ini) }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $here
    try {
        [void][System.Diagnostics.Process]::Start($psi)
        exit 0
    } catch {
        Write-GlimpseLog ("could not start Glimpse without a console window, running here: " + $_.Exception.Message)
    }
}

# Compile Glimpse.cs with the C# compiler of the .NET Framework (C# 5), once per PowerShell process.
if (-not ('Glimpse.Program' -as [type])) {
    try {
        Add-Type -Path $source -ReferencedAssemblies 'System.Windows.Forms', 'System.Drawing', 'System.Core' -IgnoreWarnings
    } catch {
        Show-GlimpseError ("Glimpse.cs could not be compiled: " + $_.Exception.Message)
        exit 1
    }
}

$argv = New-Object 'System.Collections.Generic.List[string]'
if ($Ini) { $argv.Add('/ini:' + $Ini) }
if ($Settings) { $argv.Add('/settings') }
if ($Exit) { $argv.Add('/exit') }
if ($Background) { $argv.Add('/background') }
if ($SelfTest) { if ($Cases) { $argv.Add('/selftest:' + $Cases) } else { $argv.Add('/selftest') } }
if ($CheckLang) { $argv.Add('/checklang') }
if ($Render) { $argv.Add('/render:' + $Render) }
if ($RenderLang) { $argv.Add('/renderlang:' + $RenderLang) }

# Runs Glimpse on its own STA thread with per-monitor DPI awareness, and waits until it exits.
$code = [Glimpse.Program]::RunHosted($here, $scriptPath, $argv.ToArray())
exit $code
