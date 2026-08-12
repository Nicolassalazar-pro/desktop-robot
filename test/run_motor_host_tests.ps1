$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $PSScriptRoot '.host-build'
New-Item -ItemType Directory -Force $build | Out-Null

function Invoke-NativeTest {
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [string[]]$Sources,

        [string[]]$IncludeArgs = @()
    )

    $output = Join-Path $build "$Name.exe"
    & g++ -std=c++17 -Wall -Wextra -Werror -UNDEBUG @IncludeArgs @Sources -o $output
    if ($LASTEXITCODE -ne 0) {
        throw "$Name compilation failed with exit code $LASTEXITCODE"
    }
    & $output
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
    Write-Host "PASS: $Name"
}

try {
    Push-Location $root
    Invoke-NativeTest -Name 'drive-session' -Sources @(
        'test\drive_session_test.cpp',
        'src\drive_session.cpp'
    )
    Invoke-NativeTest -Name 'drive-calibration' -IncludeArgs @(
        '-Itest\stubs',
        '-Isrc'
    ) -Sources @(
        'test\fake_nvs.cpp',
        'test\drive_calibration_test.cpp',
        'src\drive_calibration.cpp'
    )
    Invoke-NativeTest -Name 'serial-line-buffer' -Sources @(
        'test\serial_line_buffer_test.cpp'
    )
    Invoke-NativeTest -Name 'display-transition' -Sources @(
        'test\display_transition_test.cpp'
    )
    Invoke-NativeTest -Name 'movement-expression' -IncludeArgs @(
        '-Isrc'
    ) -Sources @(
        'test\movement_expression_test.cpp',
        'src\movement_expression.cpp'
    )
    Invoke-NativeTest -Name 'gesture-menu' -Sources @(
        'test\gesture_menu_test.cpp'
    )
    Invoke-NativeTest -Name 'spin-calibration' -IncludeArgs @(
        '-Itest\stubs',
        '-Isrc'
    ) -Sources @(
        'test\fake_nvs.cpp',
        'test\spin_calibration_test.cpp',
        'src\spin_calibration.cpp',
        'src\movement_expression.cpp'
    )

    Invoke-NativeTest -Name 'bench-log' -IncludeArgs @(
        '-Itest\stubs',
        '-Isrc'
    ) -Sources @(
        'test\fake_nvs.cpp',
        'test\bench_log_test.cpp',
        'src\bench_log.cpp'
    )

    Invoke-NativeTest -Name 'rf-map' -IncludeArgs @(
        '-Itest\stubs',
        '-Isrc'
    ) -Sources @(
        'test\fake_nvs.cpp',
        'test\rf_map_test.cpp',
        'src\rf_map.cpp'
    )

    Invoke-NativeTest -Name 'motion-events' -IncludeArgs @(
        '-Itest\stubs',
        '-Isrc'
    ) -Sources @(
        'test\motion_events_test.cpp',
        'src\motion_events.cpp'
    )

    Invoke-NativeTest -Name 'melody' -IncludeArgs @(
        '-Itest\stubs',
        '-Isrc'
    ) -Sources @(
        'test\melody_test.cpp',
        'src\melody.cpp'
    )

    Invoke-NativeTest -Name 'melody-gen' -IncludeArgs @(
        '-Itest\stubs',
        '-Isrc'
    ) -Sources @(
        'test\melody_gen_test.cpp',
        'src\melody_gen.cpp',
        'src\melody.cpp'
    )

    Invoke-NativeTest -Name 'turn-measure' -IncludeArgs @(
        '-Isrc'
    ) -Sources @(
        'test\turn_measure_test.cpp',
        'src\turn_measure.cpp',
        'src\movement_expression.cpp'
    )

    Invoke-NativeTest -Name 'ws-owner-reaper' -Sources @(
        'test\ws_owner_reaper_test.cpp'
    )

    Invoke-NativeTest -Name 'ws-ingress-drain' -Sources @(
        'test\ws_ingress_drain_test.cpp'
    )

    Invoke-NativeTest -Name 'ws-nonowner-filter' -IncludeArgs @(
        '-Isrc'
    ) -Sources @(
        'test\ws_nonowner_filter_test.cpp',
        'src\drive_session.cpp'
    )

    Invoke-NativeTest -Name 'sheet-transition' -IncludeArgs @(
        '-Isrc'
    ) -Sources @(
        'test\sheet_transition_test.cpp',
        'src\sheet_transition.cpp'
    )

    # The gesture-menu test mirrors constants that live in main.cpp. Verify the
    # copies still match, so the geometry cannot silently drift out of sync.
    $mainSrc = Get-Content 'src\main.cpp' -Raw
    $expected = @{
        'kGestureBtnH'    = '50'
        'kGestureBtnTop'  = '40'
        'kGestureBtnGap'  = '6'
        'kGestureBtnX'    = '10'
        'kGestureFooterY' = '264'
    }
    foreach ($name in $expected.Keys) {
        $pattern = "constexpr int16_t $name = $($expected[$name]);"
        if ($mainSrc -notmatch [regex]::Escape($pattern)) {
            throw "gesture-menu constant drift: expected '$pattern' in src\main.cpp"
        }
    }
    Write-Host '  gesture-menu constants match src\main.cpp'

    # Python contract suite. Every one of these files ran offline for months
    # while test_telemetry_opcode_frame_and_big_endian_decode_match sat red,
    # because nothing invoked pytest/unittest. Gate the runner on unittest so
    # a Python failure fails the whole build.
    $python = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
    if (-not (Test-Path $python)) {
        throw "PlatformIO python venv not found at $python; install PlatformIO or update this path."
    }
    $pyTests = @(
        'test.test_motor_us_contract',
        'test.test_ws_protocol_drift',
        'test.test_apply_calibration',
        'test.test_audio_to_piezo_midi_timing',
        'test.test_audio_to_piezo_stream',
        'test.test_melody_generate',
        'test.test_melody_pitch',
        'test.test_melody_player',
        'test.test_melody_rng',
        'test.test_midi_bridge_core',
        'test.test_midi_serial_bridge',
        'test.test_power_animation_contract'
    )
    $pyLog = Join-Path $build 'python-tests.log'
    # Some Python tests print diagnostic ERR lines to stderr on purpose (they
    # exercise error paths). Under $ErrorActionPreference = 'Stop', any native
    # stderr write becomes a terminating error, so we can't rely on that
    # signal. Merge stderr and stdout via cmd.exe and gate strictly on the
    # native exit code, which is what actually reflects test pass/fail.
    $pyArgList = @('-m', 'unittest') + $pyTests
    $prevPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $joined = ($pyArgList | ForEach-Object { '"' + $_ + '"' }) -join ' '
        & cmd /c "`"$python`" $joined > `"$pyLog`" 2>&1"
        $pyExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevPref
    }
    if ($pyExit -ne 0) {
        Write-Host '--- python test output ---'
        Get-Content $pyLog | ForEach-Object { Write-Host $_ }
        throw "Python contract tests failed with exit code $pyExit"
    }
    # unittest writes its "OK" / "Ran N tests" summary at the tail. Surface
    # just that so the PASS line stays consistent with the C++ tests above.
    $tail = Get-Content $pyLog | Where-Object { $_ -match '^(OK|Ran \d+ tests?)' }
    if ($tail) {
        $tail | ForEach-Object { Write-Host "  $_" }
    }
    Write-Host 'PASS: python-contract'
} finally {
    Pop-Location
    Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
}
