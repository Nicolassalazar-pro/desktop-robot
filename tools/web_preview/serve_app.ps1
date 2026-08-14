# -----------------------------------------------------------------------------
# tools/web_preview/serve_app.ps1 -- serve the override phone app locally.
#
# WHY THIS EXISTS
#
# The ESP32-S3 normally starts a soft-AP and serves src\override_html.cpp at
# http://192.168.4.1/?k=<token>, with a binary WebSocket behind /ws. This wrapper
# starts the host-side simulator so the phone app can be developed without the
# robot attached, while still loading THE SAME firmware HTML at runtime.
#
# Port 8080 mirrors common web-app dev defaults. Change with -Port if another
# tool already owns it. The simulator binds to 0.0.0.0 by default so a phone on
# the same LAN can open the printed URL.
# -----------------------------------------------------------------------------
param(
    [int]$Port = 8080,
    [string]$HostName = '0.0.0.0',
    [string]$Token = 'deadbeefcafe1234',
    [string]$RequireToken = '',
    [string]$AdvertiseHost = ''
)

$ErrorActionPreference = 'Stop'

$sim = Join-Path $PSScriptRoot 'robot_sim.py'
if (-not (Test-Path $sim)) {
    throw "Cannot find $sim"
}

# PlatformIO ships its own Python interpreter under the user profile. Prefer
# it over a system Python so this script works even on a fresh checkout with
# nothing else installed.
$pioPy = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
if (-not (Test-Path $pioPy)) {
    # Fall back to whatever `python` is on PATH. Python 3.8+ is enough for the
    # simulator, and this repo's target workstation has Python 3.14 installed.
    $pioPy = 'python'
    Write-Host "note: PlatformIO python not found, falling back to system 'python'"
}

$simArgs = @($sim, '--host', $HostName, '--port', $Port, '--token', $Token)
if ($RequireToken) {
    $simArgs += @('--require-token', $RequireToken)
}
if ($AdvertiseHost) {
    $simArgs += @('--advertise-host', $AdvertiseHost)
}

& $pioPy @simArgs
