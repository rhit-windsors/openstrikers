# Play whole matches unattended and collect every crash.
#
# Each run starts a skip-front-end match with OPENSTRIKERS_AUTOPLAY driving
# port 0 from a seed, OPENSTRIKERS_FIXED_STEP fixing the game's delta and
# OPENSTRIKERS_UNCAPPED letting it run as fast as the GPU presents (about 2.75x
# real time on a 165 Hz display). The AI is set to Legend so goals -- and with
# them replays, overlays and celebration cinematics -- come often. A run ends
# as one of:
#
#   crash     the in-process handler printed FATAL EXCEPTION; its stack is
#             symbolized and the run is grouped with others of the same stack
#   died      the process exited with no report (exit code recorded)
#   hang      the heartbeat stopped advancing for -StallSeconds
#   stuck     frames keep coming but nothing happens -- no goal and no FE scene
#             change for -StuckSeconds; a soft-lock (seen on the post-match
#             summary screen, with A being pressed throughout)
#   survived  still running at -RunSeconds, which is the pass
#
# Outside live play autoplay only taps A, which takes Rematch after every match,
# so one run plays match after match; the summary counts finished matches and
# goals, so a pass says how much ground it covered.
#
# Everything lands in build\logs\soak-<stamp>\: one directory per seed, plus
# summary.tsv. A crash prints the command that reruns its seed alone.
#
#   .\soak.ps1 -Runs 20 -Parallel 2
#   .\soak.ps1 -Seed 1234 -Runs 1            # rerun one seed
#   .\soak.ps1 -MatchSeconds 60              # short matches, to reach the recaps
[CmdletBinding()]
param(
    [string]$IsoPath = $env:OPENSTRIKERS_ISO,
    [string]$ExePath = '',
    [int]$Runs = 10,
    # First seed; run i uses Seed + i. Defaults to something time-derived.
    [long]$Seed = 0,
    # More than one at a time saturates the GPU and makes the desktop laggy;
    # only raise it on a machine nobody is using.
    [int]$Parallel = 1,
    # 0 keeps the game's own length (300 game-seconds).
    [int]$MatchSeconds = 0,
    # 0 Training .. 4 Legend.
    [int]$AiSkill = 4,
    [int]$RunSeconds = 900,
    [int]$StallSeconds = 120,
    [int]$StuckSeconds = 240,
    # Dump a frame this often (host frames) and keep the newest -KeepFrames, so
    # a failure leaves its last few screens behind. 0 turns it off.
    [int]$DumpEvery = 1800,
    [int]$KeepFrames = 5,
    # Extra environment for every run, e.g. @{ OPENSTRIKERS_TEST_AUTO_REPLAY = '1' }.
    [hashtable]$Env = @{}
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($IsoPath)) {
    throw "Pass -IsoPath, or set OPENSTRIKERS_ISO to your game disc image. No game data ships with this repository."
}
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $PSScriptRoot 'build\openstrikers.exe'
}
$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
if ($Seed -eq 0) {
    $Seed = [long]((Get-Date).ToFileTimeUtc() % 1000000)
}

$addr2line = 'C:\msys64\ucrt64\bin\addr2line.exe'
if (-not (Test-Path -LiteralPath $addr2line)) {
    $cmd = Get-Command 'addr2line.exe' -ErrorAction SilentlyContinue
    $addr2line = if ($null -ne $cmd) { $cmd.Source } else { $null }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$soakDir = Join-Path (Split-Path -Parent $ExePath) "logs\soak-$stamp"
New-Item -ItemType Directory -Force -Path $soakDir | Out-Null

$runEnv = [ordered]@{
    OPENSTRIKERS_SKIP_FE    = '1'
    OPENSTRIKERS_FIXED_STEP = '16.6667'
    OPENSTRIKERS_UNCAPPED   = '1'
    OPENSTRIKERS_AI_SKILL   = "$AiSkill"
    # Fast-forwarded audio is noise; the audio build still mixes, just silently.
    OPENSTRIKERS_AUDIO_MUTE = '1'
}
if ($MatchSeconds -gt 0) {
    $runEnv['OPENSTRIKERS_MATCH_SECONDS'] = "$MatchSeconds"
}
foreach ($k in $Env.Keys) {
    $runEnv[$k] = [string]$Env[$k]
}
# Switches a previous session may have left in this shell would change the run
# without saying so.
$clear = @('OPENSTRIKERS_AUTO_A', 'OPENSTRIKERS_INPUT', 'OPENSTRIKERS_SKIP_VIEW_MASK',
           'AURORA_DUMP_FRAME', 'OPENSTRIKERS_LOG_FPS')
if ($DumpEvery -gt 0) {
    $runEnv['AURORA_DUMP_FRAME'] = "every:$DumpEvery"
}

function Read-Text([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    # The game still has the file open for writing; share it.
    try {
        $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        try { return (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    } catch { return '' }
}

# Symbolize the first stack in a FATAL report. Returns the frames and a short
# signature made of the innermost game functions, which is what groups runs.
function Get-CrashInfo([string]$errText) {
    $at = $errText.IndexOf('openstrikers: FATAL EXCEPTION')
    $report = $errText.Substring($at)
    $kind = ''
    if ($report -match '(?m)^\s+kind\s+(.+)$') { $kind = $Matches[1].Trim() }
    elseif ($report -match '(?m)^\s+code\s+(\S+)') { $kind = $Matches[1] }
    if ($report -match '(?m)^\s+(reading|writing|executing) address (\S+)') {
        $kind += " $($Matches[1]) $($Matches[2])"
    }
    $frame = ''
    if ($report -match '(?m)^\s+frame\s+(\d+)') { $frame = $Matches[1] }

    $addrs = @([regex]::Matches($report, '(?m)^\s+(0x14[0-9a-fA-F]+)\s*$') |
        Select-Object -First 40 | ForEach-Object { $_.Groups[1].Value })
    $lines = @()
    if ($addrs.Count -gt 0 -and $null -ne $addr2line) {
        $lines = @(& $addr2line -e $ExePath -f -C -i -p @addrs)
    }
    $skip = '^(osCrashHandler|osReportFatal|osAbortSignal|osTerminate|osInvalidParameter|osPureCall|osDumpBacktrace)\b|^\?\?|^ \(inlined by\)'
    $funcs = @($lines | Where-Object { $_ -notmatch $skip } |
        ForEach-Object { ($_ -replace ' at .*$', '') -replace '\(.*$', '' } |
        Select-Object -First 3)
    [pscustomobject]@{
        Kind      = $kind
        Frame     = $frame
        Stack     = $lines
        Signature = if ($funcs.Count -gt 0) { $funcs -join ' < ' } else { "unsymbolized $kind" }
    }
}

function Start-Run([long]$runSeed) {
    $dir = Join-Path $soakDir "seed-$runSeed"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($k in $clear) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $runEnv.Keys) { Set-Item "Env:$k" $runEnv[$k] }
    $env:OPENSTRIKERS_AUTOPLAY = "$runSeed"
    $p = Start-Process -FilePath $ExePath -ArgumentList ('"' + $IsoPath + '"') `
        -WorkingDirectory $dir -RedirectStandardOutput "$dir\out.log" `
        -RedirectStandardError "$dir\err.log" -PassThru
    # Uncapped, the game takes every frame the GPU will give it. Below-normal
    # priority keeps the rest of the desktop responsive while a soak runs.
    try { $p.PriorityClass = 'BelowNormal' } catch {}
    [pscustomobject]@{
        Seed = $runSeed; Dir = $dir; Process = $p; Started = Get-Date
        LastBeat = -1; LastBeatAt = Get-Date; Events = -1; LastEventAt = Get-Date
    }
}

# Milestones worth knowing about for any run, whatever ended it.
function Get-Milestones([string]$outText, [string]$errText) {
    # The HUD logs one line per scoreline change.
    $goals = ([regex]::Matches($errText, '(?m)^openstrikers: score \d+-\d+')).Count
    # FE scene pushes are logged to stderr; one post-game menu per finished match.
    $matches = ([regex]::Matches($errText + $outText, 'FESCENE push art/fe/post_game_menu')).Count
    $beat = 0
    $m = [regex]::Matches($errText, 'heartbeat frame (\d+)')
    if ($m.Count -gt 0) { $beat = [int]$m[$m.Count - 1].Groups[1].Value }
    [pscustomobject]@{ Goals = $goals; Matches = $matches; Frame = $beat }
}

function Complete-Run($run, [string]$outcome) {
    $p = $run.Process
    if (-not $p.HasExited) {
        try { $p.Kill() } catch {}
        $null = $p.WaitForExit(5000)
    }
    $errText = Read-Text "$($run.Dir)\err.log"
    $outText = Read-Text "$($run.Dir)\out.log"
    $ms = Get-Milestones $outText $errText
    $info = $null
    if ($errText.Contains('openstrikers: FATAL EXCEPTION')) {
        $outcome = 'crash'
        $info = Get-CrashInfo $errText
        ($info.Stack -join "`n") | Set-Content -LiteralPath "$($run.Dir)\stack.txt"
    }
    $exit = if ($p.HasExited) { $p.ExitCode } else { '' }
    $frame = if ($null -ne $info -and $info.Frame) { $info.Frame } else { $ms.Frame }
    $result = [pscustomobject]@{
        Seed      = $run.Seed
        Outcome   = $outcome
        Frame     = $frame
        Goals     = $ms.Goals
        Matches   = $ms.Matches
        Exit      = $exit
        Kind      = if ($null -ne $info) { $info.Kind } else { '' }
        Signature = if ($null -ne $info) { $info.Signature } else { '' }
        Dir       = $run.Dir
    }
    $color = switch ($outcome) { 'crash' { 'Red' } 'died' { 'Red' } 'hang' { 'Yellow' } 'stuck' { 'Yellow' } default { 'Green' } }
    $line = "seed {0,-8} {1,-8} frame {2,-6} matches {3,-2} goals {4,-3} {5}" -f $result.Seed, $outcome, $frame, $ms.Matches, $ms.Goals, $result.Signature
    Write-Host $line -ForegroundColor $color
    if ($outcome -in @('crash', 'died', 'hang', 'stuck')) {
        if ($null -ne $info) { $info.Stack | Select-Object -First 12 | ForEach-Object { Write-Host "    $_" } }
        Write-Host "    rerun: .\soak.ps1 -Seed $($run.Seed) -Runs 1$(if ($MatchSeconds -gt 0) { " -MatchSeconds $MatchSeconds" })"
    }
    $result
}

Write-Host "Soak: $Runs run(s) from seed $Seed, $Parallel at a time, AI skill $AiSkill, match $(if ($MatchSeconds -gt 0) { "$MatchSeconds s" } else { 'default length' })."
Write-Host "Logs: $soakDir"

$results = New-Object System.Collections.Generic.List[object]
$active = New-Object System.Collections.Generic.List[object]
$next = 0
try {
    while ($next -lt $Runs -or $active.Count -gt 0) {
        while ($next -lt $Runs -and $active.Count -lt $Parallel) {
            $active.Add((Start-Run ($Seed + $next)))
            $next++
        }
        Start-Sleep -Seconds 2
        foreach ($run in $active.ToArray()) {
            $p = $run.Process
            $now = Get-Date
            $outcome = $null
            if ($p.HasExited) {
                $outcome = 'died'
            } else {
                $errText = Read-Text "$($run.Dir)\err.log"
                $m = [regex]::Matches($errText, 'heartbeat frame (\d+)')
                if ($m.Count -gt 0) {
                    $beat = [int]$m[$m.Count - 1].Groups[1].Value
                    if ($beat -ne $run.LastBeat) { $run.LastBeat = $beat; $run.LastBeatAt = $now }
                }
                # Goals and scene pushes are the "something happened" signal.
                $events = ([regex]::Matches($errText, '(?m)^(FESCENE push|openstrikers: score )')).Count
                if ($events -ne $run.Events) { $run.Events = $events; $run.LastEventAt = $now }
                if (($now - $run.LastBeatAt).TotalSeconds -gt $StallSeconds) { $outcome = 'hang' }
                elseif (($now - $run.LastEventAt).TotalSeconds -gt $StuckSeconds) { $outcome = 'stuck' }
                elseif (($now - $run.Started).TotalSeconds -gt $RunSeconds) { $outcome = 'survived' }
            }
            if ($KeepFrames -gt 0) {
                Get-ChildItem -LiteralPath $run.Dir -Filter 'frame_*.ppm' -ErrorAction SilentlyContinue |
                    Sort-Object LastWriteTime -Descending | Select-Object -Skip $KeepFrames |
                    Remove-Item -ErrorAction SilentlyContinue
            }
            if ($null -ne $outcome) {
                $results.Add((Complete-Run $run $outcome))
                $null = $active.Remove($run)
            }
        }
    }
}
finally {
    foreach ($run in $active) {
        if (-not $run.Process.HasExited) { try { $run.Process.Kill() } catch {} }
    }
    foreach ($k in @($runEnv.Keys) + 'OPENSTRIKERS_AUTOPLAY') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
}

$results | Export-Csv -LiteralPath (Join-Path $soakDir 'summary.tsv') -Delimiter "`t" -NoTypeInformation

Write-Host ''
Write-Host 'Outcomes:'
$results | Group-Object Outcome | ForEach-Object { Write-Host ("  {0,-9} {1}" -f $_.Name, $_.Count) }
$m = ($results | Measure-Object Matches -Sum).Sum
$g = ($results | Measure-Object Goals -Sum).Sum
Write-Host ("  {0} finished match(es), {1} goal(s) across all runs" -f $m, $g)
$failures = @($results | Where-Object { $_.Outcome -in @('crash', 'died', 'hang', 'stuck') })
if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host 'Distinct failures:'
    $failures | Group-Object { if ($_.Signature) { $_.Signature } else { "$($_.Outcome) exit $($_.Exit)" } } |
        Sort-Object Count -Descending | ForEach-Object {
            Write-Host ("  {0,3}x  {1}" -f $_.Count, $_.Name) -ForegroundColor Red
            Write-Host ("        seeds: {0}" -f (($_.Group | ForEach-Object { $_.Seed }) -join ', '))
        }
    exit 1
}
exit 0
