# Read back actual GPU pixels: framebuffer-size logs alone cannot catch a black frame.
[CmdletBinding()]
param(
    [string]$IsoPath = $env:OPENSTRIKERS_ISO,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [ValidateSet('Original', 'Widescreen', 'Stretch')][string]$PictureMode = 'Widescreen',
    [int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
$testHome = Join-Path $BuildDir ('render-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $testHome 'cache') -Force | Out-Null
$aspectArgument = switch ($PictureMode) {
    'Original' { '--original-aspect' }
    'Widescreen' { '--widescreen' }
    'Stretch' { '--stretch' }
}
$expectedWidth = if ($PictureMode -eq 'Original') { 2880 } else { 3840 }
$info = [Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDir 'openstrikers.exe'))
$info.Arguments = '"' + $IsoPath + '" --windowed --resolution 3840x2160 --vsync ' + $aspectArgument
$info.WorkingDirectory = $testHome
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.EnvironmentVariables['OPENSTRIKERS_CACHE_PATH'] = Join-Path $testHome 'cache'
$info.EnvironmentVariables['OPENSTRIKERS_SKIP_FE'] = '1'
$info.EnvironmentVariables['OPENSTRIKERS_AUTO_A'] = '1'
$info.EnvironmentVariables['OPENSTRIKERS_AUDIO_MUTE'] = '1'
$info.EnvironmentVariables['AURORA_DUMP_FRAME'] = 'every:300'
$game = [Diagnostics.Process]::Start($info)
$stdout = $game.StandardOutput.ReadToEndAsync()
$stderr = $game.StandardError.ReadToEndAsync()
$seen = @{}
try {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 200
        if ($game.HasExited) { throw "Game exited before rendering a match (code $($game.ExitCode))." }
        foreach ($frame in Get-ChildItem -LiteralPath $testHome -Filter 'frame_*.ppm') {
            if ($frame.BaseName -notmatch '^frame_(\d+)$' -or [int]$Matches[1] -lt 900 -or $seen[$frame.Name]) { continue }
            # The game may still hold the file open while writing it; retry next pass.
            try { $bytes = [IO.File]::ReadAllBytes($frame.FullName) } catch [IO.IOException] { continue }
            $header = [Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min(64, $bytes.Length))
            if ($header -notmatch '^P6\n(\d+) (\d+)\n255\n') { throw 'Unexpected frame format.' }
            $offset = $Matches[0].Length
            $width = [int]$Matches[1]
            $height = [int]$Matches[2]
            # The file may still be in the middle of its GPU readback/write.
            if ($bytes.Length -ne $offset + $width * $height * 3) { continue }
            $seen[$frame.Name] = $true
            if ($width -ne $expectedWidth -or $height -ne 2160) { throw "Unexpected render size ${width}x${height}." }
            $lit = 0
            $samples = 0
            for ($pixel = $offset; $pixel -lt $bytes.Length - 2; $pixel += 3003) {
                if ([int]$bytes[$pixel] + [int]$bytes[$pixel + 1] + [int]$bytes[$pixel + 2] -gt 60) { ++$lit }
                ++$samples
            }
            if ($lit -gt $samples / 4) {
                Write-Host "PASS: $PictureMode gameplay rendered at ${width}x${height}; GPU readback contains a visible scene."
                Write-Host "Test artifacts: $testHome"
                return
            }
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Timed out waiting for visible 4K gameplay.'
}
finally {
    if (-not $game.HasExited) { $game.Kill(); [void]$game.WaitForExit(10000) }
    [IO.File]::WriteAllText((Join-Path $testHome 'stdout.log'), $stdout.Result)
    [IO.File]::WriteAllText((Join-Path $testHome 'stderr.log'), $stderr.Result)
}
