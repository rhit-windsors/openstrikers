[CmdletBinding()]
param(
    [string]$IsoPath = $env:OPENSTRIKERS_ISO,
    [string]$ExePath = '',
    [int]$StartupTimeoutSeconds = 90,
    [int]$SurvivalSeconds = 20,
    [switch]$KeepOpen,
    # Drive the front end by hand instead of jumping straight into a match.
    # Launches with OPENSTRIKERS_SKIP_FE unset and waits for the process to end
    # on its own, so a crash reached by clicking through the menus still gets a
    # symbolized stack. No ready marker is required and nothing is killed.
    [switch]$Manual
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($IsoPath)) {
    throw "Pass -IsoPath, or set OPENSTRIKERS_ISO to your Super Mario Strikers disc image. No game data ships with this repository."
}

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $PSScriptRoot 'build\openstrikers.exe'
}
$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$buildDir = Split-Path -Parent $ExePath
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
# Run logs get their own directory. Dropped straight into the build directory
# they bury the actual build output within a few dozen runs.
$logDir = Join-Path $buildDir 'logs'
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}
$prefix = if ($Manual) { 'manual' } else { 'smoke' }
$stdoutPath = Join-Path $logDir "$prefix-$stamp.stdout.log"
$stderrPath = Join-Path $logDir "$prefix-$stamp.stderr.log"
$gameReadyMarker = '-- Memory upon Exiting InitializeGameState'
$oldSkipFE = [Environment]::GetEnvironmentVariable('OPENSTRIKERS_SKIP_FE', 'Process')
$process = $null
$passed = $false

function Show-CrashStack {
    param([string]$LogText)

    $addresses = @([regex]::Matches($LogText, '(?m)^\s+(0x14[0-9a-fA-F]+)\s*$') |
        ForEach-Object { $_.Groups[1].Value })
    if ($addresses.Count -eq 0) {
        return
    }

    $addr2linePath = $null
    $msysAddr2line = 'C:\msys64\ucrt64\bin\addr2line.exe'
    if (Test-Path -LiteralPath $msysAddr2line) {
        $addr2linePath = $msysAddr2line
    }
    else {
        $addr2lineCommand = Get-Command 'addr2line.exe' -ErrorAction SilentlyContinue
        if ($null -ne $addr2lineCommand) {
            $addr2linePath = $addr2lineCommand.Source
        }
    }
    if ([string]::IsNullOrWhiteSpace($addr2linePath)) {
        Write-Warning 'addr2line.exe was not found; the raw stack remains in the stderr log.'
        return
    }

    Write-Host 'Symbolized crash stack:'
    & $addr2linePath -e $ExePath -f -C -i -p @addresses
}

function Get-LogText {
    if (-not (Test-Path -LiteralPath $stderrPath)) {
        return ''
    }
    return [string](Get-Content -LiteralPath $stderrPath -Raw)
}

try {
    if (-not $Manual) {
        [Environment]::SetEnvironmentVariable('OPENSTRIKERS_SKIP_FE', '1', 'Process')
    }
    $quotedIso = '"' + $IsoPath + '"'
    $process = Start-Process -FilePath $ExePath -ArgumentList $quotedIso `
        -WorkingDirectory $PSScriptRoot -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath -PassThru
    [Environment]::SetEnvironmentVariable('OPENSTRIKERS_SKIP_FE', $oldSkipFE, 'Process')

    if ($Manual) {
        Write-Host "OpenStrikers manual run PID $($process.Id)"
        Write-Host "stdout: $stdoutPath"
        Write-Host "stderr: $stderrPath"
        Write-Host 'Front end is live -- drive it. Waiting for the process to exit.'
        $process.WaitForExit()
        $logText = Get-LogText
        Write-Host "Exited with code $($process.ExitCode)."
        if ($null -ne $logText -and $logText.Contains('openstrikers: FATAL EXCEPTION')) {
            Show-CrashStack $logText
        }
        if ($process.ExitCode -eq 0) {
            exit 0
        }
        exit 1
    }

    Write-Host "OpenStrikers smoke test PID $($process.Id)"
    Write-Host "stderr: $stderrPath"

    $startupDeadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    while (-not $process.HasExited -and (Get-Date) -lt $startupDeadline) {
        if (Test-Path -LiteralPath $stderrPath) {
            $logText = Get-LogText
            if ($null -ne $logText -and $logText.Contains($gameReadyMarker)) {
                break
            }
        }
        Start-Sleep -Milliseconds 250
        $process.Refresh()
    }

    if ($process.HasExited) {
        $process.WaitForExit()
        $logText = Get-LogText
        Write-Host "FAIL: game exited before gameplay initialization (exit $($process.ExitCode))." -ForegroundColor Red
        if ($null -ne $logText -and $logText.Contains('openstrikers: FATAL EXCEPTION')) {
            Show-CrashStack $logText
        }
        exit 1
    }

    if ((Get-Date) -ge $startupDeadline) {
        Write-Host "FAIL: gameplay initialization did not finish within $StartupTimeoutSeconds seconds." -ForegroundColor Red
        exit 2
    }

    Write-Host "Gameplay initialized; checking $SurvivalSeconds seconds of gameplay."
    $survivalDeadline = (Get-Date).AddSeconds($SurvivalSeconds)
    while (-not $process.HasExited -and (Get-Date) -lt $survivalDeadline) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
    }

    if ($process.HasExited) {
        $process.WaitForExit()
        $logText = Get-LogText
        Write-Host "FAIL: game crashed during the survival window (exit $($process.ExitCode))." -ForegroundColor Red
        if ($null -ne $logText -and $logText.Contains('openstrikers: FATAL EXCEPTION')) {
            Show-CrashStack $logText
        }
        exit 1
    }

    $passed = $true
    Write-Host "PASS: gameplay initialized and remained alive for $SurvivalSeconds seconds."
    if ($KeepOpen) {
        Write-Host "Leaving PID $($process.Id) open."
    }
}
finally {
    [Environment]::SetEnvironmentVariable('OPENSTRIKERS_SKIP_FE', $oldSkipFE, 'Process')
    if ($null -ne $process -and -not $process.HasExited -and (-not $KeepOpen -or -not $passed)) {
        Stop-Process -Id $process.Id
    }
}

exit 0
