param(
    [int]$IntroSeconds = 60,       # how long to keep pressing Cross to skip intros
    [int]$PostStartSeconds = 100,  # how long to watch (and keep the emulator alive) after Start
    [string]$LogTag = (Get-Date -Format 'MMdd_HHmmss'),  # unique per run so logs never clobber
    [string]$Printf = "Silent",    # printf-direction: Silent or Console
    [string]$Build = "windows-prod" # build dir under _Build; use windows-nolauncher for logging
)

$ErrorActionPreference = "Stop"
# Resolve the project root from this script's location so the driver is portable.
$root   = $PSScriptRoot
$exe    = Join-Path $root "_Build\$Build\install\kyty_emulator.exe"
$game   = "Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin"
$logDir = Join-Path $root "_gta3_logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }
$outLog = Join-Path $logDir "gta3_$LogTag.out.log"
$errLog = Join-Path $logDir "gta3_$LogTag.err.log"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
}
"@

$WM_KEYDOWN = 0x0100
$WM_KEYUP   = 0x0101

# vk / scancode pairs
$KEYS = @{
    'Cross'   = @(0x4A, 0x24) # J
    'Options' = @(0x0D, 0x1C) # Enter/Return
    'Up'      = @(0x57, 0x11) # W
    'Down'    = @(0x53, 0x1F) # S
    'Left'    = @(0x41, 0x1E) # A
    'Right'   = @(0x44, 0x20) # D
    'Circle'  = @(0x4C, 0x26) # L
}

function Find-GameWindow([int]$targetPid) {
    $script:found = [IntPtr]::Zero
    $cb = [Win+EnumWindowsProc]{
        param($h, $l)
        $p = 0
        [void][Win]::GetWindowThreadProcessId($h, [ref]$p)
        if ($p -eq $targetPid -and [Win]::IsWindowVisible($h)) {
            $sb = New-Object System.Text.StringBuilder 512
            [void][Win]::GetWindowText($h, $sb, 512)
            if ($sb.Length -gt 0) { $script:found = $h; return $false }
        }
        return $true
    }
    [void][Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Get-WindowTitle([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][Win]::GetWindowText($h, $sb, 512)
    return $sb.ToString()
}

function Send-Key([IntPtr]$h, [string]$name, [int]$holdMs = 450) {
    $vk = $KEYS[$name][0]
    $sc = $KEYS[$name][1]
    $lpDown = [IntPtr](($sc -shl 16) -bor 1)
    $lpUp   = [IntPtr](($sc -shl 16) -bor 0xC0000001)
    [void][Win]::SetForegroundWindow($h)
    [void][Win]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$vk, $lpDown)
    Start-Sleep -Milliseconds $holdMs
    [void][Win]::PostMessage($h, $WM_KEYUP, [IntPtr]$vk, $lpUp)
}

Write-Host "Launching emulator..."
$p = Start-Process -FilePath $exe -ArgumentList @(
    "--printf-direction",$Printf,
    "--game",$game,
    "--avplayer-skip","true"
) -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru -WorkingDirectory $root

Write-Host "PID=$($p.Id). Waiting for window..."
$hwnd = [IntPtr]::Zero
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 60) {
    if ($p.HasExited) { Write-Host "Process exited early with code $($p.ExitCode) before window appeared."; exit $p.ExitCode }
    $hwnd = Find-GameWindow $p.Id
    if ($hwnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "No window found."; if(-not $p.HasExited){$p.Kill()}; exit 1 }
Write-Host "Window found: '$(Get-WindowTitle $hwnd)'"

# Phase 1: press Cross every 5s to skip intros
Write-Host "Phase 1: skipping intros with Cross (J) for $IntroSeconds s..."
$sw.Restart()
$next = 0
while ($sw.Elapsed.TotalSeconds -lt $IntroSeconds) {
    if ($p.HasExited) { Write-Host "CRASH/EXIT during intro phase, code $($p.ExitCode)"; exit 100 }
    if ($sw.Elapsed.TotalSeconds -ge $next) {
        Send-Key $hwnd 'Cross'
        Write-Host ("  t={0:N0}s Cross | title='{1}'" -f $sw.Elapsed.TotalSeconds, (Get-WindowTitle $hwnd))
        $next += 5
    }
    Start-Sleep -Milliseconds 500
}

# Phase 2: on the title/menu, press Start (Options/Enter). Also try Cross as confirm.
Write-Host "Phase 2: pressing Start (Options/Enter) to begin game..."
for ($i=0; $i -lt 3; $i++) {
    if ($p.HasExited) { Write-Host "CRASH/EXIT before start press, code $($p.ExitCode)"; exit 101 }
    Send-Key $hwnd 'Options'
    Start-Sleep -Milliseconds 700
    Send-Key $hwnd 'Cross'
    Write-Host ("  start attempt $i | title='{0}'" -f (Get-WindowTitle $hwnd))
    Start-Sleep -Milliseconds 1500
}

# Phase 3: watch for crash
Write-Host "Phase 3: watching $PostStartSeconds s for crash..."
$sw.Restart()
while ($sw.Elapsed.TotalSeconds -lt $PostStartSeconds) {
    if ($p.HasExited) { Write-Host "CRASH/EXIT after start, code $($p.ExitCode) at t=$([math]::Round($sw.Elapsed.TotalSeconds,1))s"; exit 102 }
    # keep tapping cross in case of further prompts
    Send-Key $hwnd 'Cross'
    Write-Host ("  t={0:N0}s watching | title='{1}'" -f $sw.Elapsed.TotalSeconds, (Get-WindowTitle $hwnd))
    Start-Sleep -Seconds 3
}

Write-Host "Survived watch window. Title now: '$(Get-WindowTitle $hwnd)'"
if (-not $p.HasExited) { Write-Host "Still running; killing."; $p.Kill() }
exit 0
