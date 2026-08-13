# Flash the robot without fighting the background watcher for COM4.
#
# tools/bot_watch.py holds the serial port, which makes `pio run -t upload`
# fail with "Access to the path 'COM4' is denied". Rather than remember to stop
# and restart the watcher by hand (and lose coverage in between), this drops a
# PAUSE sentinel the watcher polls for, flashes, then removes it so the watcher
# reconnects on its own.
#
#   powershell -File tools\flash.ps1
#
[CmdletBinding()]
param(
    [string]$Port = 'COM4'
)

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$pause = Join-Path $repo 'build\watch\PAUSE'
$pio   = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'

New-Item -ItemType Directory -Force -Path (Split-Path $pause) | Out-Null
Set-Content -Path $pause -Value 'flashing'
Write-Host 'flash: watcher paused'

try {
    # Give the watcher a moment to notice and close the handle. It polls the
    # sentinel once a second, so 2 s is comfortably enough.
    Start-Sleep -Seconds 2
    Push-Location $repo
    & $pio run -t upload --upload-port $Port
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw "upload failed with exit code $code" }
    Write-Host 'flash: upload OK'
}
finally {
    Remove-Item $pause -ErrorAction SilentlyContinue
    Write-Host 'flash: watcher resumed'
}
