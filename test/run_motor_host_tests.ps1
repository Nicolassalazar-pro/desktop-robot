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
} finally {
    Pop-Location
    Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
}
