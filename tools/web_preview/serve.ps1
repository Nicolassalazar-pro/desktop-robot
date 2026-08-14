# -----------------------------------------------------------------------------
# tools/web_preview/serve.ps1 -- serve the built WASM preview locally.
#
# WHY THIS EXISTS
#
# Browsers refuse to load .wasm from a file:// URL: the fetch API returns
# `TypeError: Failed to fetch` because file:// has no MIME type for
# application/wasm. Any HTTP server works, but standardising on the PlatformIO
# Python that's already installed for pio itself means no extra tooling and
# no PATH surprises.
#
# Port 8770 is arbitrary but stays out of the way of common dev servers
# (webpack 8080, PIO's own device_monitor rewriter, etc). Change with -Port
# if 8770 is taken.
#
# -Lan binds 0.0.0.0 instead of loopback so a phone on the same Wi-Fi can open
# the LCD preview too. It is OPT-IN rather than the default because binding all
# interfaces exposes the page to everything on the network and will usually
# raise a Windows Firewall prompt; loopback is the right default for a tool
# whose normal use is a browser on this machine.
# -----------------------------------------------------------------------------
param(
    [int]$Port = 8770,
    [switch]$Lan
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$serveDir = Join-Path $root 'build\web'

if (-not (Test-Path (Join-Path $serveDir 'preview.wasm'))) {
    throw @"
$serveDir does not contain preview.wasm.

Build it first:
  powershell -File tools\web_preview\build.ps1
"@
}

# PlatformIO ships its own Python interpreter under the user profile. Prefer
# it over a system Python so this script works even on a fresh checkout with
# nothing else installed.
$pioPy = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
if (-not (Test-Path $pioPy)) {
    # Fall back to whatever `python` is on PATH. Any Python 3.x has
    # http.server built in.
    $pioPy = 'python'
    Write-Host "note: PlatformIO python not found, falling back to system 'python'"
}

$bindAddr = if ($Lan) { '0.0.0.0' } else { '127.0.0.1' }

if ($Lan) {
    # Report the address a phone would actually use, not the bind wildcard.
    $lanIp = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
        Select-Object -First 1 -ExpandProperty IPAddress)
    Write-Host "Serving $serveDir on http://${lanIp}:$Port/  (all interfaces)"
    Write-Host "Open that URL from a phone on the same Wi-Fi."
} else {
    Write-Host "Serving $serveDir on http://localhost:$Port/"
    Write-Host "Open http://localhost:$Port/index.html in a browser."
    Write-Host "(use -Lan to also reach it from a phone)"
}
Write-Host "Press Ctrl+C to stop."

Push-Location $serveDir
try {
    # http.server is intentionally simple: single-threaded, no persistent
    # state, no auth. That is fine here -- everything served is a compiled
    # artifact under version-controlled sources. The bind address is the only
    # exposure control, which is why -Lan is opt-in.
    & $pioPy -m http.server $Port --bind $bindAddr
}
finally {
    Pop-Location
}
