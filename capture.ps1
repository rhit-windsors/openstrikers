# Capture gameplay frames from a skip-front-end run.
#
# OPENSTRIKERS_SKIP_FE drops straight into the match load, but the match still
# waits on the A button before kickoff, so a run left alone sits on the loading
# transition forever and every dumped frame comes out black. OPENSTRIKERS_AUTO_A
# pulses A on port 0 for exactly that. A run that actually reached the match
# logs a VIEWDBG line carrying view 0, the shadow-texture pass, which the
# loading screen never draws.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutDir,
    [string]$DumpSpec = 'every:300',
    [int]$Seconds = 45,
    [int]$Attempts = 3,
    [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'
# Point OPENSTRIKERS_ISO at your own disc image. No game data ships with this
# repository, so there is no default to fall back to.
$iso = $env:OPENSTRIKERS_ISO
if (-not $iso) { throw "Set OPENSTRIKERS_ISO to the path of your Super Mario Strikers disc image." }
if (-not (Test-Path $iso)) { throw "OPENSTRIKERS_ISO does not exist: $iso" }
$exe = Join-Path $PSScriptRoot 'build\openstrikers.exe'

foreach ($k in @('OPENSTRIKERS_SKIP_VIEW_MASK','OPENSTRIKERS_V11_RANGE','OPENSTRIKERS_V11_LOG',
                 'OPENSTRIKERS_POS_DEBUG','OPENSTRIKERS_SKIN_DEBUG','OPENSTRIKERS_MTX_DEBUG')) {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
$env:OPENSTRIKERS_SKIP_FE = '1'
$env:AURORA_DUMP_FRAME = $DumpSpec
$env:OPENSTRIKERS_AUTO_A = '1'
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }

for ($i = 1; $i -le $Attempts; $i++) {
    Remove-Item -Recurse -Force $OutDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $p = Start-Process -FilePath $exe -ArgumentList "`"$iso`"" -WorkingDirectory $OutDir `
         -RedirectStandardOutput "$OutDir\out.log" -RedirectStandardError "$OutDir\err.log" -PassThru

    $null = $p.WaitForExit($Seconds * 1000)
    if (-not $p.HasExited) { $p.Kill(); $null = $p.WaitForExit(5000) }

    if (Select-String -Path "$OutDir\err.log" -Pattern '^VIEWDBG frame=\d+ 0:' -Quiet) {
        "attempt ${i}: reached gameplay"
        return
    }
    "attempt ${i}: never left the loading transition"
}
"gave up after $Attempts attempts"
