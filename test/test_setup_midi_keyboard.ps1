[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Passed = 0
$script:Failed = 0
$script:ScenarioNumber = 0
$repoRoot = Split-Path -Parent $PSScriptRoot
$setupScript = Join-Path $repoRoot 'tools\setup_midi_keyboard.ps1'
$workspace = Join-Path $PSScriptRoot '.setup-midi-keyboard-tests'
$powershell = (Get-Command powershell.exe -CommandType Application -ErrorAction Stop).Source

function Assert-Equal {
    param(
        [Parameter(Mandatory)][object]$Expected,
        [AllowNull()][object]$Actual,
        [Parameter(Mandatory)][string]$Message
    )

    if ($Expected -ne $Actual) {
        throw "$Message Expected '$Expected', got '$Actual'."
    }
}

function Invoke-Test {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][scriptblock]$Body
    )

    try {
        & $Body
        $script:Passed++
        Write-Host "PASS: $Name" -ForegroundColor Green
    } catch {
        $script:Failed++
        Write-Host "FAIL: $Name" -ForegroundColor Red
        Write-Host "  $($_.Exception.Message)" -ForegroundColor Red
    }
}

function New-InstrumentedScript {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Overrides
    )

    $source = Get-Content -LiteralPath $setupScript -Raw
    $entryPoint = [regex]::Match(
        $source,
        '(?ms)\r?\ntry \{\r?\n    Invoke-MidiSetup\r?\n\} catch \{.*\z'
    )
    if (-not $entryPoint.Success) {
        throw "Could not locate the final Invoke-MidiSetup error wrapper in '$setupScript'."
    }

    $script:ScenarioNumber++
    $safeName = $Name -replace '[^A-Za-z0-9_.-]', '-'
    $scenarioPath = Join-Path $workspace ('{0:D2}-{1}.ps1' -f $script:ScenarioNumber, $safeName)
    $instrumented = $source.Insert(
        $entryPoint.Index,
        [Environment]::NewLine + $Overrides + [Environment]::NewLine
    )
    Set-Content -LiteralPath $scenarioPath -Value $instrumented -Encoding UTF8
    return $scenarioPath
}

function Invoke-Scenario {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Overrides,
        [string[]]$Arguments = @()
    )

    $scenarioPath = New-InstrumentedScript -Name $Name -Overrides $Overrides
    $processInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $processInfo.FileName = $powershell
    $processInfo.Arguments = (
        '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" {1}' -f
            $scenarioPath,
            ($Arguments -join ' ')
    ).Trim()
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $processInfo
    [void]$process.Start()
    $standardOutput = $process.StandardOutput.ReadToEnd()
    $standardError = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $process.Dispose()

    $combinedOutput = @($standardOutput.TrimEnd(), $standardError.TrimEnd()) |
        Where-Object { $_ }
    return [PSCustomObject]@{
        ExitCode = $exitCode
        Output   = ($combinedOutput -join [Environment]::NewLine)
    }
}

function Assert-ScenarioExit {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Overrides,
        [Parameter(Mandatory)][int]$ExpectedExitCode
    )

    $result = Invoke-Scenario -Name $Name -Overrides $Overrides
    Assert-Equal -Expected $ExpectedExitCode -Actual $result.ExitCode `
        -Message "Scenario output:`n$($result.Output)`n"
}

if (-not (Test-Path -LiteralPath $setupScript -PathType Leaf)) {
    throw "Final MIDI setup script was not found at '$setupScript'."
}

if (Test-Path -LiteralPath $workspace) {
    Remove-Item -LiteralPath $workspace -Recurse -Force
}
New-Item -ItemType Directory -Path $workspace -Force | Out-Null
Set-Content -LiteralPath (Join-Path $workspace 'requirements-midi-bridge.txt') -Value '' -Encoding ASCII

try {
    Invoke-Test 'unknown executable-only installation is rejected' {
        Assert-ScenarioExit -Name 'unknown-executable' -ExpectedExitCode 1 -Overrides @'
function Get-CommandPath { param([string]$Name) return 'mock-winget.exe' }
function Get-PackageDetection {
    return [PSCustomObject]@{
        Installed = $true
        WinGetState = [PSCustomObject]@{
            Installed = $false
            QuerySucceeded = $true
            Status = 'NotInstalled'
            ExitCode = 0
            Detail = 'No installed package found.'
        }
        RegistryEntries = @()
        RegistryEntry = $null
        RegistryVersionVerified = $false
        ExecutablePath = 'C:\unknown\app.exe'
        ExecutableOnly = $true
    }
}
function Invoke-MidiSetup {
    Install-WinGetPackage -SectionName 'Unknown app' -PackageId 'Vendor.App' `
        -RegistryDisplayName 'Known app' -FindExecutable { 'C:\unknown\app.exe' } `
        -PackageVersion '1.2.3'
}
'@
    }

    Invoke-Test 'mismatched exact registry version fails' {
        Assert-ScenarioExit -Name 'version-mismatch' -ExpectedExitCode 1 -Overrides @'
function Invoke-MidiSetup {
    Assert-PinnedRegistryVersion `
        -RegistryEntries @([PSCustomObject]@{ DisplayVersion = '1.2.2' }) `
        -RegistryDisplayName 'Known app' -PackageVersion '1.2.3'
}
'@
    }

    Invoke-Test 'matching exact registry version passes' {
        Assert-ScenarioExit -Name 'version-match' -ExpectedExitCode 0 -Overrides @'
function Invoke-MidiSetup {
    Assert-PinnedRegistryVersion `
        -RegistryEntries @([PSCustomObject]@{ DisplayVersion = '1.2.3' }) `
        -RegistryDisplayName 'Known app' -PackageVersion '1.2.3'
}
'@
    }

    Invoke-Test 'burn bundle registering twice at one version passes' {
        # loopMIDI 1.0.16.27 installs as a WiX burn bundle: the bundle and the
        # MSI it carries both register under DisplayName "loopMIDI" with the
        # same DisplayVersion. Agreeing duplicates are not ambiguous.
        Assert-ScenarioExit -Name 'burn-bundle-duplicate' -ExpectedExitCode 0 -Overrides @'
function Invoke-MidiSetup {
    Assert-PinnedRegistryVersion `
        -RegistryEntries @(
            [PSCustomObject]@{ DisplayVersion = '1.0.16.27' },
            [PSCustomObject]@{ DisplayVersion = '1.0.16.27' }
        ) `
        -RegistryDisplayName 'loopMIDI' -PackageVersion '1.0.16.27'
}
'@
    }

    Invoke-Test 'duplicate entries at conflicting versions still fail' {
        Assert-ScenarioExit -Name 'conflicting-duplicate' -ExpectedExitCode 1 -Overrides @'
function Invoke-MidiSetup {
    Assert-PinnedRegistryVersion `
        -RegistryEntries @(
            [PSCustomObject]@{ DisplayVersion = '1.0.16.27' },
            [PSCustomObject]@{ DisplayVersion = '9.9.9.9' }
        ) `
        -RegistryDisplayName 'loopMIDI' -PackageVersion '1.0.16.27'
}
'@
    }

    Invoke-Test 'agreeing duplicates at the wrong version still fail' {
        Assert-ScenarioExit -Name 'duplicate-wrong-version' -ExpectedExitCode 1 -Overrides @'
function Invoke-MidiSetup {
    Assert-PinnedRegistryVersion `
        -RegistryEntries @(
            [PSCustomObject]@{ DisplayVersion = '1.0.15.0' },
            [PSCustomObject]@{ DisplayVersion = '1.0.15.0' }
        ) `
        -RegistryDisplayName 'loopMIDI' -PackageVersion '1.0.16.27'
}
'@
    }

    Invoke-Test 'WinGet 1.29 list omits unsupported version filter and parses installed version' {
        Assert-ScenarioExit -Name 'winget-129-list' -ExpectedExitCode 0 -Overrides @'
$script:WinGetListCalls = 0
function Invoke-WinGet {
    param([string]$WinGetPath, [string[]]$Arguments)
    $script:WinGetListCalls++
    $expectedArguments = @(
        'list',
        '--id', 'VMPK.VMPK',
        '-e',
        '--source', 'winget',
        '--accept-source-agreements',
        '--disable-interactivity'
    )
    if (($Arguments -join [char]0) -ne ($expectedArguments -join [char]0)) {
        throw "Unexpected WinGet arguments: $($Arguments -join ' ')"
    }
    return [PSCustomObject]@{
        ExitCode = 0
        Output = @(
            'Name                            Id        Version Source',
            '---------------------------------------------------------',
            'Virtual MIDI Piano Keyboard     VMPK.VMPK 0.9.1   winget'
        )
        Text = "Name                            Id        Version Source`r`n" +
            "---------------------------------------------------------`r`n" +
            'Virtual MIDI Piano Keyboard     VMPK.VMPK 0.9.1   winget'
    }
}
function Invoke-MidiSetup {
    $state = Get-WinGetPackageState -WinGetPath 'mock-winget.exe' `
        -PackageId 'VMPK.VMPK' -PackageVersion '0.9.1'
    if (-not $state.Installed -or
        -not $state.QuerySucceeded -or
        $state.Status -ne 'ExactMatch') {
        throw "Expected exact installed state, got $($state.Status)."
    }
    if ($script:WinGetListCalls -ne 1) {
        throw "Expected one WinGet list query, got $script:WinGetListCalls."
    }
}
'@
    }

    Invoke-Test 'WinGet real no-match exit is treated as not installed' {
        Assert-ScenarioExit -Name 'winget-no-match' -ExpectedExitCode 0 -Overrides @'
function Invoke-WinGet {
    param([string]$WinGetPath, [string[]]$Arguments)
    return [PSCustomObject]@{
        ExitCode = -1978335212
        Output = @('No installed package found matching input criteria.')
        Text = 'No installed package found matching input criteria.'
    }
}
function Invoke-MidiSetup {
    $state = Get-WinGetPackageState -WinGetPath 'mock-winget.exe' `
        -PackageId 'Vendor.NotInstalled' -PackageVersion '1.2.3'
    if ($state.Installed -or
        -not $state.QuerySucceeded -or
        $state.Status -ne 'NotInstalled' -or
        $state.ExitCode -ne -1978335212) {
        throw "Expected real WinGet no-match state, got $($state.Status) / $($state.ExitCode)."
    }
}
'@
    }

    Invoke-Test 'WinGet unsupported arguments remain a native query failure' {
        Assert-ScenarioExit -Name 'winget-unsupported-argument' `
            -ExpectedExitCode -1978335230 -Overrides @'
function Invoke-WinGet {
    param([string]$WinGetPath, [string[]]$Arguments)
    return [PSCustomObject]@{
        ExitCode = -1978335230
        Output = @("Argument name was not recognized for the current command: '--version'")
        Text = "Argument name was not recognized for the current command: '--version'"
    }
}
function Invoke-MidiSetup {
    $state = Get-WinGetPackageState -WinGetPath 'mock-winget.exe' `
        -PackageId 'Vendor.App' -PackageVersion '1.2.3'
    Assert-WinGetQuerySucceeded -State $state -PackageId 'Vendor.App' `
        -PackageVersion '1.2.3'
}
'@
    }

    Invoke-Test 'WinGet exit code propagates exactly' {
        Assert-ScenarioExit -Name 'winget-exit' -ExpectedExitCode 23 -Overrides @'
function Get-CommandPath { param([string]$Name) return 'mock-winget.exe' }
function Get-PackageDetection {
    return [PSCustomObject]@{
        Installed = $false
        WinGetState = [PSCustomObject]@{
            Installed = $false
            QuerySucceeded = $true
            Status = 'NotInstalled'
            ExitCode = 0
            Detail = ''
        }
        RegistryEntries = @()
        RegistryEntry = $null
        RegistryVersionVerified = $false
        ExecutablePath = $null
        ExecutableOnly = $false
    }
}
function Invoke-WinGet {
    return [PSCustomObject]@{ ExitCode = 23; Output = @('mock failure'); Text = 'mock failure' }
}
function Invoke-MidiSetup {
    Install-WinGetPackage -SectionName 'Mock package' -PackageId 'Vendor.App' `
        -RegistryDisplayName 'Mock package' -FindExecutable { $null } `
        -PackageVersion '1.2.3'
}
'@
    }

    Invoke-Test 'pip exit code propagates exactly' {
        Assert-ScenarioExit -Name 'pip-exit' -ExpectedExitCode 31 -Overrides @'
function Get-CommandPath { param([string]$Name) return 'mock-py.exe' }
function Invoke-NativeCommand {
    return [PSCustomObject]@{ ExitCode = 31; Output = @('mock failure'); Text = 'mock failure' }
}
function Invoke-MidiSetup { Install-BridgeRequirements }
'@
    }

    Invoke-Test 'bridge exit code propagates exactly' {
        Assert-ScenarioExit -Name 'bridge-exit' -ExpectedExitCode 43 -Overrides @'
function Get-CommandPath { param([string]$Name) return 'mock.exe' }
function Find-LoopMidiExecutable { return 'C:\mock\loopMIDI.exe' }
function Find-VmpkExecutable { return 'C:\mock\vmpk.exe' }
function Get-PackageDetection {
    return [PSCustomObject]@{
        Installed = $true
        WinGetState = [PSCustomObject]@{
            Installed = $true
            QuerySucceeded = $true
            Status = 'ExactMatch'
            ExitCode = 0
            Detail = ''
        }
        RegistryEntries = @()
        RegistryEntry = $null
        RegistryVersionVerified = $false
        ExecutablePath = 'C:\mock\app.exe'
        ExecutableOnly = $false
    }
}
function Find-MidiBridge { return 'C:\mock\midi_serial_bridge.py' }
function Get-BridgePortListOption { param([string]$BridgePath) return '--list-ports' }
function Invoke-NativeCommand {
    param([string]$Path, [string[]]$Arguments)
    if ($Arguments -contains 'check') {
        return [PSCustomObject]@{ ExitCode = 0; Output = @('No broken requirements found.'); Text = 'No broken requirements found.' }
    }
    return [PSCustomObject]@{ ExitCode = 43; Output = @('mock failure'); Text = 'mock failure' }
}
function Invoke-MidiSetup { Test-Installation }
'@
    }

    Invoke-Test 'immediately exiting GUI exit code propagates exactly' {
        Assert-ScenarioExit -Name 'gui-exit' -ExpectedExitCode 47 -Overrides @'
function Get-Process { return $null }
function Start-Sleep {}
function Start-Process {
    $process = [PSCustomObject]@{ HasExited = $true; ExitCode = 47 }
    $process | Add-Member -MemberType ScriptMethod -Name Refresh -Value {} -Force
    return $process
}
function Invoke-MidiSetup {
    Start-ProgramOnce -DisplayName 'Mock GUI' -ProcessName 'mock-gui' `
        -ExecutablePath 'C:\mock\mock-gui.exe'
}
'@
    }

    $safetyOverrides = @'
$script:TracePath = Join-Path $PSScriptRoot 'launch-trace.txt'
function Add-LaunchTrace {
    param([string]$Kind, [string]$Detail)
    [IO.File]::AppendAllText(
        $script:TracePath,
        "$Kind|$Detail$([Environment]::NewLine)"
    )
}
function Test-IsAdministrator { return $false }
function Get-CommandPath { param([string]$Name) return "mock-$Name" }
function Get-Python312RegistryPath { return $null }
function Invoke-WinGet {
    param([string]$WinGetPath, [string[]]$Arguments)
    if ($Arguments -contains '--version') {
        return [PSCustomObject]@{ ExitCode = 0; Output = @('v1.0'); Text = 'v1.0' }
    }
    if ($Arguments -contains 'install') {
        Add-LaunchTrace -Kind 'WinGetInstall' -Detail ($Arguments -join ' ')
    }
    return [PSCustomObject]@{
        ExitCode = -1978335212
        Output = @('No installed package found.')
        Text = 'No installed package found.'
    }
}
function Invoke-NativeCommand {
    param([string]$Path, [string[]]$Arguments)
    $argumentList = @($Arguments)
    $isPreflightVersionProbe = $argumentList.Count -eq 2 -and
        $argumentList[0] -eq '-3.12' -and $argumentList[1] -eq '--version'
    if (-not $isPreflightVersionProbe) {
        Add-LaunchTrace -Kind 'NativeInstallOrBridge' -Detail "$Path $($Arguments -join ' ')"
    }
    return [PSCustomObject]@{ ExitCode = 0; Output = @('Python 3.12.0'); Text = 'Python 3.12.0' }
}
function Get-PackageDetection {
    return [PSCustomObject]@{
        Installed = $false
        WinGetState = [PSCustomObject]@{
            Installed = $false
            QuerySucceeded = $true
            Status = 'NotInstalled'
            ExitCode = -1978335212
            Detail = 'No installed package found.'
        }
        RegistryEntries = @()
        RegistryEntry = $null
        RegistryVersionVerified = $false
        ExecutablePath = $null
        ExecutableOnly = $false
    }
}
function Find-LoopMidiExecutable { return $null }
function Find-VmpkExecutable { return $null }
function Find-MidiBridge { return $null }
function Get-Process { return $null }
function Start-Process {
    param([string]$FilePath)
    Add-LaunchTrace -Kind 'GUI' -Detail $FilePath
    throw 'A GUI process launch was attempted.'
}
'@

    foreach ($simulation in @(
        [PSCustomObject]@{ Name = 'DryRun'; Argument = '-DryRun' },
        [PSCustomObject]@{ Name = 'WhatIf'; Argument = '-WhatIf' }
    )) {
        Invoke-Test "$($simulation.Name) launches no UAC, install, bridge, or GUI processes" {
            $tracePath = Join-Path $workspace 'launch-trace.txt'
            if (Test-Path -LiteralPath $tracePath) {
                Remove-Item -LiteralPath $tracePath -Force
            }

            $result = Invoke-Scenario -Name $simulation.Name -Overrides $safetyOverrides `
                -Arguments @($simulation.Argument)
            Assert-Equal -Expected 0 -Actual $result.ExitCode `
                -Message "Simulation failed. Output:`n$($result.Output)`n"

            $launches = @()
            if (Test-Path -LiteralPath $tracePath) {
                $launches = @(Get-Content -LiteralPath $tracePath)
            }
            Assert-Equal -Expected 0 -Actual $launches.Count `
                -Message "Simulation launched forbidden external processes: $($launches -join '; ')."
        }
    }
} finally {
    if (Test-Path -LiteralPath $workspace) {
        Remove-Item -LiteralPath $workspace -Recurse -Force
    }
}

Write-Host "`nTests: $($script:Passed + $script:Failed), Passed: $script:Passed, Failed: $script:Failed"
if ($script:Failed -ne 0) {
    exit 1
}
