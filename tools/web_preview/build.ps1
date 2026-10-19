# -----------------------------------------------------------------------------
# tools/web_preview/build.ps1 -- compile the LCD preview to WebAssembly.
#
# WHY THIS EXISTS
#
# Mirrors tools/host_preview/build.ps1 exactly, but swaps g++ for Emscripten's
# emcc so the output is a browser-runnable preview.js + preview.wasm instead of
# a native .exe. The developer's inner loop for UI work is now
#   edit src/screens.cpp -> tools/web_preview/build.ps1 -> refresh browser
# with no ESP32 attached, and no risk of the drift that killed the earlier
# tools/lcd_preview.py: this compiles THE SAME six translation units the
# firmware compiles (see the list below), against a real Arduino_Canvas.
#
# EMSDK LOCATION
#
# The user installs emsdk to $env:USERPROFILE\Desktop\Scripts\emsdk. We do NOT
# rely on `emcc` being on PATH -- dot-sourcing emsdk_env.ps1 only mutates the
# CURRENT shell, and every powershell tool invocation runs in a fresh process.
# Instead we call emcc.exe by full path from wherever emsdk is on this machine.
#
# THE ONE COMPILATION RULE
#
# Do NOT define ESP32 or __AVR__. Arduino_GFX 1.3.9 has architecture branches
# gated on those macros. Leaving them UNDEFINED steers the library into its
# generic non-AVR fallback: plain malloc(), the empty PROGMEM path, and the
# software 5x7 GFX font. That is what lets the vendored library compile
# unmodified against the tiny test/stubs/arduino_host/{Arduino,Print}.h shim.
# See docs/host-render-research.md and the same rule at the top of
# tools/host_preview/build.ps1 for the full reasoning.
#
# INCREMENTAL BUILDS
#
# emcc is a Python-driven Clang wrapper -- a cold "compile then link" of the
# seven TUs takes ~15 s on this machine, dominated by per-file startup and
# the final link. That's a bad edit-preview loop, so we cache each .o file
# under .wasm-build/ and skip recompiling any TU whose fingerprint (source
# hash + flags + relevant headers) hasn't changed. Then we also skip the
# link if NOTHING was rebuilt this run. Force a full rebuild with -Clean.
# -----------------------------------------------------------------------------
[CmdletBinding()]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$here     = $PSScriptRoot
$buildDir = Join-Path $here '.wasm-build'
$outDir   = Join-Path $root 'build\web'

New-Item -ItemType Directory -Force $buildDir | Out-Null
New-Item -ItemType Directory -Force $outDir   | Out-Null

if ($Clean) {
    Write-Host "== -Clean: wiping $buildDir + $outDir =="
    Get-ChildItem -Path $buildDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
    Get-ChildItem -Path $outDir   -File -ErrorAction SilentlyContinue | Remove-Item -Force
}

# Locate emcc.exe and em++.exe. Users install emsdk in different places; the
# task brief names one specific location, but we ALSO look at $env:EMSDK
# (which emsdk sets when it is activated) so a system-wide install still
# works. em++ compiles the six C++ TUs and does the final link (Arduino_GFX
# pulls in libc++: vtables for the Arduino_G/Arduino_GFX class hierarchy,
# __cxa_pure_virtual, operator delete). emcc handles the one C TU
# (qrcode.c). Never .bat -- this install does not ship emcc.bat/em++.bat.
$emsdkRoots = @()
if ($env:EMSDK) { $emsdkRoots += $env:EMSDK }
$emsdkRoots += (Join-Path $env:USERPROFILE 'Desktop\Scripts\emsdk')

$emcc = $null
$empp = $null
foreach ($rt in $emsdkRoots) {
    $cand1 = Join-Path $rt 'upstream\emscripten\emcc.exe'
    $cand2 = Join-Path $rt 'upstream\emscripten\em++.exe'
    if ((Test-Path $cand1) -and (Test-Path $cand2)) {
        $emcc = $cand1; $empp = $cand2; break
    }
}
if (($null -eq $emcc) -or ($null -eq $empp)) {
    throw @"
Cannot find emcc.exe / em++.exe. Install emsdk and activate 'latest', or set
`$env:EMSDK to point at your emsdk root. Roots tried:
$($emsdkRoots -join "`n")
"@
}
Write-Host "using emcc at $emcc"
Write-Host "using em++ at $empp"

Push-Location $root
try {
    # The Arduino_GFX 1.3.9 source path contains a space (" GFX Library for
    # Arduino "). @-splatting a -I"$var" preserves quoting; do not inline it
    # as a bare token.
    $gfxSrc = '.pio\libdeps\esp32-s3-devkitc-1\GFX Library for Arduino\src'
    $qrSrc  = '.pio\libdeps\esp32-s3-devkitc-1\QRCode\src'

    if (-not (Test-Path $gfxSrc)) {
        throw @"
Cannot find Arduino_GFX source at
  $gfxSrc
Run `pio run` once first so PlatformIO fetches the library, or point this
script at a different libdeps location if you moved it.
"@
    }

    # -D LCD_WIDTH / LCD_HEIGHT match the values platformio.ini gives the
    # firmware build (see the [env:esp32-s3-devkitc-1] block, line 66-67).
    # screens.h static_asserts against these -- if the panel geometry ever
    # changes, both the firmware and this harness must be updated together
    # and the assert refuses to compile the old value.
    $commonInc = @(
        '-Itest\stubs\arduino_host',
        "-I$gfxSrc",
        "-I$qrSrc",
        '-Ilib\EyeEngine',
        '-Isrc'
    )
    $commonDefs = @(
        '-DLCD_WIDTH=240',
        '-DLCD_HEIGHT=280'
    )
    # -O1 is a deliberate choice, not a sloppy default. Fast rebuilds matter
    # more than runtime speed here: the whole point of this harness is to
    # tighten the edit-preview loop. -O0 balloons the .wasm past 3 MB; -O3
    # roughly doubles the LTO link time. -O1 lands both around a couple of
    # seconds and produces a ~500 KB .wasm that runs comfortably fast at
    # 60 fps for a 240x280 buffer.
    $cxxflags = @('-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter', '-O1')
    $cflags   = @('-std=c99',   '-Wall', '-Wextra',
                  '-Wno-unknown-pragmas',    # qrcode.c uses `#pragma mark`
                  '-Wno-type-limits',        # qrcode.c has one benign compare
                  '-O1')

    function Invoke-Cxx {
        param([string]$src, [string]$obj)
        & $empp @cxxflags @commonInc @commonDefs -c $src -o $obj
        if ($LASTEXITCODE -ne 0) {
            throw "em++ (C++) failed on $src (exit $LASTEXITCODE)"
        }
    }

    function Invoke-Cc {
        param([string]$src, [string]$obj)
        # qrcode.c is C, so drive it with emcc.exe (not em++.exe): emcc picks
        # the C front end by extension. Distinct name kept so the two roles are
        # visible in the log output.
        & $emcc @cflags @commonInc @commonDefs -c $src -o $obj
        if ($LASTEXITCODE -ne 0) {
            throw "emcc (C) failed on $src (exit $LASTEXITCODE)"
        }
    }

    # ---------------------------------------------------------------------
    # Fingerprint-based incremental cache.
    #
    # Each .o gets a sidecar .stamp file containing a hash of:
    #   1. The source file's size and mtime (avoids re-hashing multi-MB
    #      library sources every build -- mtime+size is what make(1) uses
    #      and it's plenty for this workflow).
    #   2. The compiler flag strings that will be passed for this file.
    #   3. For TUs under src/ (screens, sheet_transition, wasm_main), the
    #      size+mtime of every src\*.h header -- because those are the
    #      headers the developer will actually edit, and skipping the
    #      recompile after a header edit would silently show stale code.
    #
    # Vendored library TUs (Arduino_GFX, qrcode.c) don't get src\ header
    # tracking because their headers only change when PlatformIO reinstalls the
    # lib, which will also touch the source-file mtimes.
    #
    # BUT every TU -- vendored ones included -- pulls in the Arduino shim
    # (test\stubs\arduino_host\*.h) and may pull in lib\EyeEngine\*.h. Those are
    # OURS and get edited by hand, so they are tracked GLOBALLY and folded into
    # every fingerprint. Leaving them out meant editing the shim produced a
    # fully-cached "successful" build that still ran the old code -- precisely
    # the silent staleness this cache is supposed to prevent.
    # ---------------------------------------------------------------------
    $srcHeaderKey = (Get-ChildItem -Path 'src' -Filter '*.h' -File -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join '/'

    $globalHeaderKey = (@(
        Get-ChildItem -Path 'test\stubs\arduino_host' -Filter '*.h' -File -ErrorAction SilentlyContinue
        Get-ChildItem -Path 'lib\EyeEngine'           -Filter '*.h' -File -ErrorAction SilentlyContinue
    ) | Sort-Object FullName |
        ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join '/'

    function Get-Fingerprint {
        param([string]$src, [string]$flagsKey, [bool]$includeSrcHeaders)
        $f = Get-Item $src
        $parts = @(
            "$($f.Name)|$($f.Length)|$($f.LastWriteTimeUtc.Ticks)"
            $flagsKey
            "glob:$globalHeaderKey"
        )
        if ($includeSrcHeaders) { $parts += "hdr:$srcHeaderKey" }
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($parts -join '||'))
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try   { ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '' }
        finally { $sha.Dispose() }
    }

    $script:anyRebuilt = $false
    $cxxFlagsKey = "cxx|$($cxxflags -join ' ')|$($commonInc -join ' ')|$($commonDefs -join ' ')"
    $cFlagsKey   = "c|$($cflags   -join ' ')|$($commonInc -join ' ')|$($commonDefs -join ' ')"

    function Invoke-CxxCached {
        param([string]$src, [string]$obj, [switch]$DependsOnSrcHeaders)
        $stamp = "$obj.stamp"
        $fp = Get-Fingerprint -src $src -flagsKey $cxxFlagsKey `
                              -includeSrcHeaders ([bool]$DependsOnSrcHeaders)
        if ((Test-Path $obj) -and (Test-Path $stamp) -and
            ((Get-Content $stamp -Raw) -eq $fp)) {
            Write-Host "  [cached] $src"
            return
        }
        Invoke-Cxx $src $obj
        Set-Content -Path $stamp -Value $fp -NoNewline
        $script:anyRebuilt = $true
    }

    function Invoke-CcCached {
        param([string]$src, [string]$obj)
        $stamp = "$obj.stamp"
        $fp = Get-Fingerprint -src $src -flagsKey $cFlagsKey -includeSrcHeaders $false
        if ((Test-Path $obj) -and (Test-Path $stamp) -and
            ((Get-Content $stamp -Raw) -eq $fp)) {
            Write-Host "  [cached] $src"
            return
        }
        Invoke-Cc $src $obj
        Set-Content -Path $stamp -Value $fp -NoNewline
        $script:anyRebuilt = $true
    }

    Write-Host "== Arduino_GFX (wasm) =="
    $g1 = Join-Path $buildDir 'Arduino_G.o'
    $g2 = Join-Path $buildDir 'Arduino_GFX.o'
    $g3 = Join-Path $buildDir 'Arduino_Canvas.o'
    Invoke-CxxCached "$gfxSrc\Arduino_G.cpp"              $g1
    Invoke-CxxCached "$gfxSrc\Arduino_GFX.cpp"            $g2
    Invoke-CxxCached "$gfxSrc\canvas\Arduino_Canvas.cpp"  $g3

    Write-Host "== QRCode =="
    $qr = Join-Path $buildDir 'qrcode.o'
    Invoke-CcCached "$qrSrc\qrcode.c" $qr

    Write-Host "== EyeEngine =="
    # The mascot's eyes are a separate in-tree library (lib/EyeEngine). It
    # depends only on Arduino_GFX + <Arduino.h> + <math.h>, so it compiles for
    # wasm unmodified -- the same "no drift" property screens.cpp has. Without
    # it the preview shows an eyeless mascot, which is exactly the recipe that
    # caused a real shipped bug once already.
    $eyes = Join-Path $buildDir 'EyeEngine.o'
    Invoke-CxxCached 'lib\EyeEngine\EyeEngine.cpp' $eyes

    Write-Host "== src\ TUs (recompile on any src\*.h edit) =="
    $screens = Join-Path $buildDir 'screens.o'
    Invoke-CxxCached 'src\screens.cpp'          $screens  -DependsOnSrcHeaders

    $sheet = Join-Path $buildDir 'sheet_transition.o'
    Invoke-CxxCached 'src\sheet_transition.cpp' $sheet    -DependsOnSrcHeaders

    $wasmMain = Join-Path $buildDir 'wasm_main.o'
    Invoke-CxxCached 'tools\web_preview\wasm_main.cpp' $wasmMain -DependsOnSrcHeaders

    # -- link ------------------------------------------------------------------
    #
    # The exported symbol list mirrors extern "C" in wasm_main.cpp. Each entry
    # keeps a leading underscore, which is Emscripten's convention for exported
    # C symbols (matches Clang name mangling on wasm targets).
    #
    # ccall/cwrap in EXPORTED_RUNTIME_METHODS let the tiny JS glue in
    # index.html invoke exported functions by name without importing the module
    # object directly; ALLOW_MEMORY_GROWTH removes the fixed heap ceiling so
    # the RGBA framebuffer + emmalloc'd canvases fit even if we double the
    # panel size in the future.
    $exports = @(
        '_wp_width',
        '_wp_height',
        '_wp_framebuffer_rgba',
        '_wp_render_screen',
        '_wp_touch',
        '_wp_tick',
        '_wp_sheet_y',
        '_wp_sheet_state',
        '_wp_dismiss_active',
        '_wp_stage_count',
        '_wp_last_event',
        '_wp_set_pairing'
    ) -join ','

    $outJs   = Join-Path $outDir 'preview.js'
    $outWasm = Join-Path $outDir 'preview.wasm'

    if ((-not $script:anyRebuilt) -and (Test-Path $outJs) -and (Test-Path $outWasm)) {
        Write-Host "== [cached] link (no TU changed) =="
    } else {
        Write-Host "== linking $outJs + preview.wasm =="
        # ENVIRONMENT=web,worker keeps the JS glue tight (no Node/shell shims)
        # and matches how index.html loads it. If you need to sanity-test the
        # module from Node in the future, add ",node" here and re-run build.
        #
        # HEAPU8 is exported explicitly because Emscripten >= 3.1.44 removed
        # the implicit heap-view attachment. Without it, Module.HEAPU8 is
        # undefined and the JS side cannot read the RGBA framebuffer.
        & $empp @cxxflags `
            $g1 $g2 $g3 $qr $eyes $screens $sheet $wasmMain `
            -sEXPORTED_FUNCTIONS="[$exports]" `
            "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8']" `
            -sALLOW_MEMORY_GROWTH=1 `
            "-sENVIRONMENT=web,worker" `
            -sMODULARIZE=1 `
            -sEXPORT_ES6=0 `
            -sEXPORT_NAME=createPreviewModule `
            -o $outJs
        if ($LASTEXITCODE -ne 0) { throw "em++ link failed (exit $LASTEXITCODE)" }
    }

    Write-Host "== copying index.html to $outDir =="
    Copy-Item -Force (Join-Path $here 'index.html') $outDir

    Write-Host ""
    Write-Host "OK. Output in $outDir"
    Get-ChildItem $outDir | Sort-Object Name | ForEach-Object {
        "  $($_.Name)  ($([Math]::Round($_.Length/1KB,1)) KB)"
    }
    Write-Host ""
    if ($script:anyRebuilt) {
        Write-Host "(rebuilt one or more TUs; force a full rebuild with -Clean)"
    } else {
        Write-Host "(all TUs cached; force a full rebuild with -Clean)"
    }
    Write-Host ""
    Write-Host "To view in the browser, run:"
    Write-Host "  powershell -File tools\web_preview\serve.ps1"
    Write-Host "then open http://localhost:8770/  (WASM cannot load from file://)"
}
finally {
    Pop-Location
}
