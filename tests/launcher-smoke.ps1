# Tests saved preferences and the actual launcher -> game process handoff.
[CmdletBinding()]
param(
    [string]$IsoPath = $env:OPENSTRIKERS_ISO,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [int]$TimeoutSeconds = 90,
    [ValidateSet('Original', 'Widescreen', 'Stretch')][string]$PictureMode = 'Original'
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
$launcherExe = Join-Path $BuildDir 'openstrikers-launcher.exe'
$gameExe = Join-Path $BuildDir 'openstrikers.exe'
$testHome = Join-Path $BuildDir ('launcher-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testHome | Out-Null
New-Item -ItemType Directory -Path (Join-Path $testHome 'cache') | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LauncherNative {
    [DllImport("user32.dll")]
    public static extern int GetWindowLong(IntPtr hwnd, int index);
    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, string lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")]
    public static extern IntPtr SendValue(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
}
'@

function Wait-Until([scriptblock]$Condition, [string]$Message) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $Message
}
function Start-Launcher {
    $info = [Diagnostics.ProcessStartInfo]::new($launcherExe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.EnvironmentVariables['OPENSTRIKERS_LAUNCHER_HOME'] = $testHome
    $info.EnvironmentVariables['OPENSTRIKERS_CACHE_PATH'] = $testHome + '\cache'
    $info.EnvironmentVariables['OPENSTRIKERS_SKIP_FE'] = '1'
    $info.EnvironmentVariables['OPENSTRIKERS_AUTO_A'] = '1'
    $script:launcher = [Diagnostics.Process]::Start($info)
    Wait-Until {
        $script:hwnd = [LauncherNative]::FindWindow('OpenStrikersLauncher', 'OpenStrikers Launcher')
        [uint32]$ownerPid = 0
        [void][LauncherNative]::GetWindowThreadProcessId($script:hwnd, [ref]$ownerPid)
        $ownerPid -eq $script:launcher.Id
    } 'Launcher window did not appear. Close other launchers before running this test.'
}
function Set-Text([int]$Id, [string]$Value) {
    [void][LauncherNative]::SendMessage([LauncherNative]::GetDlgItem($hwnd, $Id), 0x000C, [IntPtr]::Zero, $Value)
}
function Get-Text([int]$Id) {
    $buffer = [Text.StringBuilder]::new(32768)
    [void][LauncherNative]::GetWindowText([LauncherNative]::GetDlgItem($hwnd, $Id), $buffer, $buffer.Capacity)
    return $buffer.ToString()
}
function Get-Selection([int]$Id) {
    return [LauncherNative]::SendValue([LauncherNative]::GetDlgItem($hwnd, $Id), 0x0147,
                                      [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
}
function Set-Selection([int]$Id, [int]$Value) {
    [void][LauncherNative]::SendValue([LauncherNative]::GetDlgItem($hwnd, $Id), 0x014E,
                                     [IntPtr]$Value, [IntPtr]::Zero)
}
$launcher = $null
$game = $null
try {
    Start-Launcher
    Set-Text 101 $IsoPath
    Set-Text 103 $gameExe
    if ((Get-Selection 105) -ne 4 -or (Get-Selection 106) -ne 0) { throw 'Default 4K fullscreen settings missing.' }
    Set-Selection 105 3
    Set-Selection 106 1
    Set-Selection 108 2
    [void][LauncherNative]::SendValue([LauncherNative]::GetDlgItem($hwnd, 109), 0x00F1, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][LauncherNative]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $launcher.WaitForExit(10000)) { throw 'Launcher did not close.' }
    Start-Launcher
    if ((Get-Text 101) -ne $IsoPath -or (Get-Text 103) -ne $gameExe) { throw 'Paths did not survive launcher restart.' }
    if ((Get-Selection 105) -ne 3 -or (Get-Selection 106) -ne 1 -or (Get-Selection 108) -ne 2 -or
        [LauncherNative]::SendValue([LauncherNative]::GetDlgItem($hwnd, 109), 0x00F0,
                                   [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -ne 0) {
        throw 'Display settings did not survive launcher restart.'
    }
    Set-Selection 105 4
    Set-Selection 106 0
    Set-Selection 108 $(if ($PictureMode -eq 'Widescreen') { 2 } elseif ($PictureMode -eq 'Stretch') { 1 } else { 0 })
    [void][LauncherNative]::SendValue([LauncherNative]::GetDlgItem($hwnd, 109), 0x00F1, [IntPtr]1, [IntPtr]::Zero)
    $before = @(Get-Process openstrikers -ErrorAction SilentlyContinue | ForEach-Object Id)
    [void][LauncherNative]::PostMessage($hwnd, 0x0111, [IntPtr]110, [IntPtr]::Zero)
    Wait-Until {
        $script:game = Get-Process openstrikers -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -notin $before } | Select-Object -First 1
        $null -ne $script:game
    } 'Play did not start the game.'
    Wait-Until {
        $script:log = Get-ChildItem (Join-Path $testHome 'logs') -Filter '*.log' | Select-Object -First 1
        $script:log -and (Get-Content $script:log.FullName -Raw) -match '-- Memory upon Exiting InitializeGameState'
    } 'Launched game did not reach a match.'
    Wait-Until {
        (Get-Content $log.FullName -Raw) -match 'Render framebuffer'
    } 'Launched game did not create its rendering framebuffer.'
    $content = Get-Content $log.FullName -Raw
    $expectedSize = if ($PictureMode -eq 'Original') { '2880x2160' } else { '3840x2160' }
    if ($content -notmatch "requested render $expectedSize, fullscreen, vsync on" -or
        $content -notmatch "Render framebuffer $expectedSize") { throw 'Requested 4K picture mode was not configured.' }
    if ($PictureMode -eq 'Widescreen' -and $content -notmatch 'game aspect 16:9') {
        throw 'The launcher did not enable true widescreen.'
    }
    Start-Sleep -Seconds 5
    $game.Refresh()
    if ($game.HasExited) { throw 'Game exited during launcher smoke test.' }
    $gameWindow = $game.MainWindowHandle
    if ($gameWindow -eq [IntPtr]::Zero -or
        ([LauncherNative]::GetWindowLong($gameWindow, -16) -band 0x00C00000) -ne 0) {
        throw 'Game did not enter borderless fullscreen.'
    }
    [void][LauncherNative]::PostMessage($gameWindow, 0x0100, [IntPtr]0x7A, [IntPtr]::Zero)
    [void][LauncherNative]::PostMessage($gameWindow, 0x0101, [IntPtr]0x7A, [IntPtr]::Zero)
    Wait-Until {
        ([LauncherNative]::GetWindowLong($gameWindow, -16) -band 0x00C00000) -ne 0
    } 'F11 did not return the game to a window.'
    [void][LauncherNative]::PostMessage($gameWindow, 0x0100, [IntPtr]0x7A, [IntPtr]::Zero)
    [void][LauncherNative]::PostMessage($gameWindow, 0x0101, [IntPtr]0x7A, [IntPtr]::Zero)
    Wait-Until {
        ([LauncherNative]::GetWindowLong($gameWindow, -16) -band 0x00C00000) -eq 0
    } 'F11 did not restore fullscreen.'
    [void][LauncherNative]::PostMessage($gameWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Wait-Until { $game.Refresh(); $game.HasExited } 'Game did not close normally.'
    Wait-Until {
        (Get-Text 112) -match 'Match finished' -and
        [LauncherNative]::IsWindowEnabled([LauncherNative]::GetDlgItem($hwnd, 110))
    } 'Launcher did not return to its ready state after the game exited.'
    $before = @(Get-Process openstrikers -ErrorAction SilentlyContinue | ForEach-Object Id)
    [void][LauncherNative]::PostMessage($hwnd, 0x0111, [IntPtr]110, [IntPtr]::Zero)
    Wait-Until {
        $script:game = Get-Process openstrikers -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -notin $before } | Select-Object -First 1
        $null -ne $script:game
    } 'Launcher did not start a second game.'
    # Closing the launcher must not kill the game.
    [void][LauncherNative]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $launcher.WaitForExit(10000)) { throw 'Launcher did not close while playing.' }
    $game.Refresh()
    if ($game.HasExited) { throw 'Closing the launcher killed the game.' }
    Write-Host 'PASS: saved ISO/executable, 4K fullscreen with VSync, match startup, F11 switching, return after game exit, relaunch, launcher close keeps game running.'
    Write-Host "Test artifacts: $testHome"
}
finally {
    if ($game -and -not $game.HasExited) { Stop-Process -Id $game.Id -Force; [void]$game.WaitForExit(10000) }
    if ($launcher -and -not $launcher.HasExited) { Stop-Process -Id $launcher.Id -Force }
}
