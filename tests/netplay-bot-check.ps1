# Network play with one game: the game hosts, tests/net_bot.cpp joins as the
# second player and plays random input, and the game must keep playing --
# matches, goals, rematches -- through the bot's simulated latency and loss.
# The bot runs no game, so this exercises the network path (handshake, input
# delay, stalls, leaving), not cross-machine determinism; replay-check.ps1
# covers that.
[CmdletBinding()]
param(
    [string]$IsoPath = $env:OPENSTRIKERS_ISO,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [int]$Seconds = 120,
    [int]$LatencyMs = 60,
    [int]$LossPercent = 5,
    [int]$Port = 52377
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
$work = Join-Path $BuildDir ('netplay-check-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

$info = [Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDir 'openstrikers.exe'))
$info.Arguments = '"' + $IsoPath + '" --windowed --window-size 640x480 --net-host ' + $Port + ' --net-players 2 --net-name host'
$info.WorkingDirectory = $work
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$envs = @{
    OPENSTRIKERS_CACHE_PATH = (Join-Path $work 'cache')
    OPENSTRIKERS_SKIP_FE = '1'
    OPENSTRIKERS_AI_SKILL = '4'
    OPENSTRIKERS_MATCH_SECONDS = '60'
    OPENSTRIKERS_AUDIO_MUTE = '1'
    OPENSTRIKERS_AUTOPLAY = '21'
    OPENSTRIKERS_NET_ACCEPT_ANY_BUILD = '1'
}
foreach ($k in $envs.Keys) { $info.EnvironmentVariables[$k] = $envs[$k] }
$game = [Diagnostics.Process]::Start($info)
$gameOut = $game.StandardOutput.ReadToEndAsync()
$gameErr = $game.StandardError.ReadToEndAsync()

$botInfo = [Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDir 'openstrikers-net-bot.exe'))
$botInfo.Arguments = "127.0.0.1:$Port --latency $LatencyMs --loss $LossPercent --seed 5"
$botInfo.UseShellExecute = $false
$botInfo.CreateNoWindow = $true
$botInfo.RedirectStandardOutput = $true
$botInfo.RedirectStandardError = $true
$bot = [Diagnostics.Process]::Start($botInfo)
$botOut = $bot.StandardOutput.ReadToEndAsync()

Start-Sleep -Seconds $Seconds
$gameAlive = -not $game.HasExited
$botAlive = -not $bot.HasExited
foreach ($p in @($bot, $game)) { if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() } }
$err = $gameErr.Result + $gameOut.Result
Set-Content -LiteralPath (Join-Path $work 'game.log') -Value $err
Set-Content -LiteralPath (Join-Path $work 'bot.log') -Value $botOut.Result

$started = $err -match 'netplay: started as player 1 of 2'
$goals = ([regex]::Matches($err, '(?m)^openstrikers: score \d+-\d+')).Count
$matchCount = ([regex]::Matches($err, 'FESCENE push art/fe/post_game_menu')).Count
$frames = 0
$m = [regex]::Matches($err, 'net frame (\d+)')
if ($m.Count -gt 0) { $frames = [int]$m[$m.Count - 1].Groups[1].Value }
$stalls = ([regex]::Matches($err, 'waiting for other players at frame')).Count
$problems = [regex]::Matches($err, '(?m)^openstrikers: netplay: (Netplay desync|Netplay ended|Could not join).*$') | ForEach-Object { $_.Value }
Write-Host ("started {0}, game alive {1}, bot alive {2}, frames {3}, matches {4}, goals {5}, stalls over 1 s {6}" -f $started, $gameAlive, $botAlive, $frames, $matchCount, $goals, $stalls)
Write-Host ("bot: " + (($botOut.Result -split "`n") | Select-Object -Last 2) -join ' | ')
if ($problems) { Write-Host ("problems: " + ($problems -join '; ')) }
Write-Host "logs in $work"
# Two random-input humans may well not score in a couple of minutes, so the
# bar is steady progress: at least half of real time's frames.
$enough = $frames -ge ($Seconds * 30)
if (-not ($started -and $gameAlive -and $botAlive -and $enough -and -not $problems)) { exit 1 }
