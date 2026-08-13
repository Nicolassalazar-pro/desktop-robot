# -----------------------------------------------------------------------------
# tools/host_preview/build.ps1 -- build & run the native LCD preview harness.
#
# Mirrors test/run_motor_host_tests.ps1 conventions: g++ with -std=c++17
# -Wall -Wextra, an isolated .host-build directory, PowerShell throwing on
# non-zero exit.  Does NOT use -Werror -- the vendored ricmoo/QRCode qrcode.c
# emits a benign -Wtype-limits warning that is not ours to fix under the "do
# not modify anything under .pio/libdeps" rule from the task brief.
# -----------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$root      = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$here      = $PSScriptRoot
$buildDir  = Join-Path $here '.host-build'
$outDir    = Join-Path $root 'build\preview'

New-Item -ItemType Directory -Force $buildDir | Out-Null
New-Item -ItemType Directory -Force $outDir   | Out-Null

Push-Location $root
try {
    # The Arduino_GFX 1.3.9 source path contains a space (" GFX Library for
    # Arduino "). PowerShell splat + -I with a quoted value handles it; do not
    # inline it as a bare token.
    $gfxSrc = '.pio\libdeps\esp32-s3-devkitc-1\GFX Library for Arduino\src'
    $qrSrc  = '.pio\libdeps\esp32-s3-devkitc-1\QRCode\src'

    # -D LCD_WIDTH / LCD_HEIGHT match the values platformio.ini gives the
    # firmware build. screens.h static_asserts against these -- if the
    # panel geometry ever changes, both the firmware and this harness must be
    # updated together, and the assert will refuse to compile the old value.
    $commonInc = @(
        '-Itest\stubs\arduino_host',
        "-I$gfxSrc",
        "-I$qrSrc",
        '-Isrc'
    )
    $commonDefs = @(
        '-DLCD_WIDTH=240',
        '-DLCD_HEIGHT=280'
    )
    $cxxflags = @('-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter')
    $cflags   = @('-std=c99',   '-Wall', '-Wextra',
                  '-Wno-unknown-pragmas',    # qrcode.c uses `#pragma mark`
                  '-Wno-type-limits')        # qrcode.c has one benign compare

    function Invoke-Cxx {
        param([string]$src, [string]$obj)
        & g++ @cxxflags @commonInc @commonDefs -c $src -o $obj
        if ($LASTEXITCODE -ne 0) {
            throw "g++ failed on $src (exit $LASTEXITCODE)"
        }
    }

    function Invoke-Cc {
        param([string]$src, [string]$obj)
        & gcc @cflags @commonInc @commonDefs -c $src -o $obj
        if ($LASTEXITCODE -ne 0) {
            throw "gcc failed on $src (exit $LASTEXITCODE)"
        }
    }

    Write-Host "== compiling Arduino_GFX (host build) =="
    $g1 = Join-Path $buildDir 'Arduino_G.o'
    $g2 = Join-Path $buildDir 'Arduino_GFX.o'
    $g3 = Join-Path $buildDir 'Arduino_Canvas.o'
    Invoke-Cxx "$gfxSrc\Arduino_G.cpp" $g1
    Invoke-Cxx "$gfxSrc\Arduino_GFX.cpp" $g2
    Invoke-Cxx "$gfxSrc\canvas\Arduino_Canvas.cpp" $g3

    Write-Host "== compiling QRCode =="
    $qr = Join-Path $buildDir 'qrcode.o'
    Invoke-Cc "$qrSrc\qrcode.c" $qr

    Write-Host "== compiling shared drawing (src\screens.cpp) =="
    $screens = Join-Path $buildDir 'screens.o'
    Invoke-Cxx 'src\screens.cpp' $screens

    Write-Host "== compiling sheet_transition (host-tested Arduino-free code) =="
    $sheet = Join-Path $buildDir 'sheet_transition.o'
    Invoke-Cxx 'src\sheet_transition.cpp' $sheet

    Write-Host "== compiling harness main =="
    $harness = Join-Path $buildDir 'main.o'
    Invoke-Cxx 'tools\host_preview\main.cpp' $harness

    $exe = Join-Path $buildDir 'lcd_preview.exe'
    Write-Host "== linking $exe =="
    & g++ @cxxflags $g1 $g2 $g3 $qr $screens $sheet $harness -o $exe
    if ($LASTEXITCODE -ne 0) { throw "g++ link failed (exit $LASTEXITCODE)" }

    Write-Host "== running $exe (writes to $outDir\) =="
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "lcd_preview.exe exit=$LASTEXITCODE" }

    Write-Host ""
    Write-Host "OK. Preview images in $outDir"
    Get-ChildItem $outDir -Filter *.ppm | Sort-Object Name |
        ForEach-Object { "  $($_.Name)  ($([Math]::Round($_.Length/1KB,1)) KB)" }
}
finally {
    Pop-Location
}
