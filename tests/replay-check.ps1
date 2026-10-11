# Determinism check for netplay: record an autoplayed session, replay it under
# different wall-clock timing, and require the per-frame state hash to match
# throughout. Lockstep netplay sends only controller input, so this is the
# property it stands on.
#
# Each round is two runs of the game, one after the other (never two at once):
# the recording runs uncapped as fast as the machine allows; the replay adds a
# random 0..JitterMs sleep to every frame. Matches are 60 seconds at Legend so
# goals, replays, celebrations and rematches all happen.
[CmdletBinding()]
param(
    [string]$IsoPath = $env:OPENSTRIKERS_ISO,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-audio'),
    [int[]]$Seeds = @(11),
    [int]$RecordSeconds = 150,
    [int]$JitterMs = 2,
    [int]$ReplayTimeoutSeconds = 900
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
$exe = Join-Path $BuildDir 'openstrikers.exe'
$work = Join-Path $BuildDir ('replay-check-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Start-Game([hashtable]$environment, [string]$logName) {
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.Arguments = '"' + $IsoPath + '" --windowed --window-size 640x480 --no-vsync'
    $info.WorkingDirectory = $work
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $common = @{
        OPENSTRIKERS_CACHE_PATH = (Join-Path $work 'cache')
        OPENSTRIKERS_SKIP_FE = '1'
        OPENSTRIKERS_AI_SKILL = '4'
        OPENSTRIKERS_MATCH_SECONDS = '60'
        OPENSTRIKERS_UNCAPPED = '1'
        OPENSTRIKERS_AUDIO_MUTE = '1'
    }
    foreach ($k in $common.Keys) { $info.EnvironmentVariables[$k] = $common[$k] }
    foreach ($k in $environment.Keys) { $info.EnvironmentVariables[$k] = $environment[$k] }
    $p = [Diagnostics.Process]::Start($info)
    [pscustomobject]@{ Process = $p; Out = $p.StandardOutput.ReadToEndAsync(); Err = $p.StandardError.ReadToEndAsync(); Log = (Join-Path $work $logName) }
}

function Stop-Game($game, [int]$afterSeconds) {
    if (-not $game.Process.WaitForExit($afterSeconds * 1000)) {
        $game.Process.Kill()
        $game.Process.WaitForExit()
    }
    $text = $game.Err.Result + $game.Out.Result
    Set-Content -LiteralPath $game.Log -Value $text
    $text
}

$failed = 0
$totalGoals = 0
$totalMatches = 0
foreach ($seed in $Seeds) {
    $recording = Join-Path $work "seed$seed.rec"
    $record = Start-Game @{ OPENSTRIKERS_AUTOPLAY = "$seed"; OPENSTRIKERS_NET_RECORD = $recording } "seed$seed-record.log"
    $recText = Stop-Game $record $RecordSeconds
    $goals = ([regex]::Matches($recText, '(?m)^openstrikers: score \d+-\d+')).Count
    $matchCount = ([regex]::Matches($recText, 'FESCENE push art/fe/post_game_menu')).Count

    $replay = Start-Game @{ OPENSTRIKERS_NET_REPLAY = $recording; OPENSTRIKERS_NET_JITTER_MS = "$JitterMs" } "seed$seed-replay.log"
    $repText = Stop-Game $replay $ReplayTimeoutSeconds
    $verdict = [regex]::Match($repText, '(?m)^openstrikers: replay: \d+ frames, .*$').Value
    $ok = $replay.Process.ExitCode -eq 0 -and $verdict -match 'state identical throughout'
    if (-not $ok) { ++$failed }
    $totalGoals += $goals
    $totalMatches += $matchCount
    Write-Host ("seed {0,-6} matches {1,-3} goals {2,-3} {3} {4}" -f $seed, $matchCount, $goals, $(if ($ok) { 'PASS' } else { 'FAIL' }), $verdict)
}
Write-Host ("{0} round(s), {1} match(es), {2} goal(s), {3} failed; logs in {4}" -f $Seeds.Count, $totalMatches, $totalGoals, $failed, $work)
if ($failed -ne 0) { exit 1 }
