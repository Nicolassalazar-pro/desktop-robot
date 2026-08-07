[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$LoopMidiPackageId = 'TobiasErichsen.loopMIDI'
$LoopMidiVersion = '1.0.16.27'
$VmpkPackageId = 'VMPK.VMPK'
$VmpkVersion = '0.9.1'
$WinGetSource = 'winget'
$RequirementsPath = Join-Path $PSScriptRoot 'requirements-midi-bridge.txt'
$IsSimulation = $DryRun -or [bool]$WhatIfPreference
$NativeExitCodeDataKey = 'DesktopRobot.NativeExitCode'

function Write-Section {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host "`n== $Message ==" -ForegroundColor Cyan
}

function Write-Detail {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host "  $Message" -ForegroundColor DarkGray
}

function Write-Plan {
    param([Parameter(Mandatory)][string]$Message)
    $prefix = if ($DryRun) { 'DRY RUN' } else { 'WHATIF' }
    Write-Host "[$prefix] $Message" -ForegroundColor Yellow
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } finally {
        $identity.Dispose()
    }
}

function Get-CommandPath {
    param([Parameter(Mandatory)][string]$Name)
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        return $null
    }
    return $command.Source
}

function Get-PropertyValue {
    param(
        [Parameter(Mandatory)][object]$InputObject,
        [Parameter(Mandatory)][string]$Name
    )
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Get-InstalledProgram {
    param([Parameter(Mandatory)][string]$DisplayName)

    $registryLocations = @(
        [PSCustomObject]@{
            Hive = [Microsoft.Win32.RegistryHive]::LocalMachine
            View = [Microsoft.Win32.RegistryView]::Registry64
        },
        [PSCustomObject]@{
            Hive = [Microsoft.Win32.RegistryHive]::LocalMachine
            View = [Microsoft.Win32.RegistryView]::Registry32
        },
        [PSCustomObject]@{
            Hive = [Microsoft.Win32.RegistryHive]::CurrentUser
            View = [Microsoft.Win32.RegistryView]::Registry64
        },
        [PSCustomObject]@{
            Hive = [Microsoft.Win32.RegistryHive]::CurrentUser
            View = [Microsoft.Win32.RegistryView]::Registry32
        }
    )

    foreach ($location in $registryLocations) {
        $baseKey = $null
        $uninstallKey = $null
        try {
            $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey($location.Hive, $location.View)
            $uninstallKey = $baseKey.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
            if ($null -eq $uninstallKey) {
                continue
            }

            foreach ($subKeyName in $uninstallKey.GetSubKeyNames()) {
                $programKey = $uninstallKey.OpenSubKey($subKeyName)
                if ($null -eq $programKey) {
                    continue
                }
                try {
                    $installedDisplayName = [string]$programKey.GetValue('DisplayName')
                    if ($installedDisplayName -and [string]::Equals(
                            $installedDisplayName,
                            $DisplayName,
                            [StringComparison]::Ordinal
                        )) {
                        [PSCustomObject]@{
                            DisplayName     = $installedDisplayName
                            DisplayVersion  = [string]$programKey.GetValue('DisplayVersion')
                            DisplayIcon     = [string]$programKey.GetValue('DisplayIcon')
                            InstallLocation = [string]$programKey.GetValue('InstallLocation')
                            RegistryHive    = [string]$location.Hive
                            RegistryView    = [string]$location.View
                        }
                    }
                } finally {
                    $programKey.Dispose()
                }
            }
        } finally {
            if ($uninstallKey) {
                $uninstallKey.Dispose()
            }
            if ($baseKey) {
                $baseKey.Dispose()
            }
        }
    }
}

function Find-ProgramExecutable {
    param(
        [Parameter(Mandatory)][string]$DisplayName,
        [Parameter(Mandatory)][string]$ExecutableName,
        [Parameter(Mandatory)][string[]]$KnownPaths
    )

    foreach ($program in @(Get-InstalledProgram -DisplayName $DisplayName)) {
        if ($program.DisplayIcon) {
            $displayIcon = ($program.DisplayIcon -replace ',\s*-?\d+$', '').Trim().Trim('"')
            if ((Split-Path -Leaf $displayIcon) -ieq $ExecutableName -and
                (Test-Path -LiteralPath $displayIcon -PathType Leaf)) {
                return $displayIcon
            }
        }
        if ($program.InstallLocation) {
            $candidate = Join-Path $program.InstallLocation $ExecutableName
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    foreach ($candidate in $KnownPaths) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    return $null
}

function Find-LoopMidiExecutable {
    # The installer places loopMIDI under a "Tobias Erichsen" vendor folder and
    # leaves InstallLocation empty in the registry, so the vendor-qualified
    # paths must be probed explicitly.
    $relativePaths = @(
        'Tobias Erichsen\loopMIDI\loopMIDI.exe',
        'loopMIDI\loopMIDI.exe'
    )
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) |
        Where-Object { $_ }
    $knownPaths = foreach ($root in $roots) {
        foreach ($relative in $relativePaths) {
            Join-Path $root $relative
        }
    }
    return Find-ProgramExecutable -DisplayName 'loopMIDI' -ExecutableName 'loopMIDI.exe' -KnownPaths $knownPaths
}

function Find-VmpkExecutable {
    $relativePaths = @(
        'Virtual MIDI Piano Keyboard\vmpk.exe',
        'VMPK\vmpk.exe'
    )
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) |
        Where-Object { $_ }
    $knownPaths = foreach ($root in $roots) {
        foreach ($relative in $relativePaths) {
            Join-Path $root $relative
        }
    }
    return Find-ProgramExecutable -DisplayName 'Virtual MIDI Piano Keyboard' -ExecutableName 'vmpk.exe' -KnownPaths $knownPaths
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    $previousErrorActionPreference = $ErrorActionPreference
    $nativePreference = Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue
    if ($nativePreference) {
        $previousNativePreference = $nativePreference.Value
        $PSNativeCommandUseErrorActionPreference = $false
    }
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $Path @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
        if ($nativePreference) {
            $PSNativeCommandUseErrorActionPreference = $previousNativePreference
        }
    }

    return [PSCustomObject]@{
        ExitCode = $exitCode
        Output   = $output
        Text     = ($output -join [Environment]::NewLine)
    }
}

function Invoke-WinGet {
    param(
        [Parameter(Mandatory)][string]$WinGetPath,
        [Parameter(Mandatory)][string[]]$Arguments
    )
    return Invoke-NativeCommand -Path $WinGetPath -Arguments $Arguments
}

function Throw-NativeCommandFailure {
    param(
        [Parameter(Mandatory)][int]$ExitCode,
        [Parameter(Mandatory)][string]$Message
    )

    $exception = [System.Exception]::new($Message)
    $exception.Data[$NativeExitCodeDataKey] = $ExitCode
    throw $exception
}

function Get-NativeCommandFailureExitCode {
    param([Parameter(Mandatory)][System.Exception]$Exception)

    $currentException = $Exception
    while ($null -ne $currentException) {
        if ($currentException.Data.Contains($NativeExitCodeDataKey)) {
            return [int]$currentException.Data[$NativeExitCodeDataKey]
        }
        $currentException = $currentException.InnerException
    }
    return $null
}

function Get-WinGetPackageState {
    param(
        [Parameter(Mandatory)][string]$WinGetPath,
        [Parameter(Mandatory)][string]$PackageId,
        [Parameter(Mandatory)][string]$PackageVersion
    )

    $installedResult = Invoke-WinGet -WinGetPath $WinGetPath -Arguments @(
        'list',
        '--id', $PackageId,
        '-e',
        '--source', $WinGetSource,
        '--accept-source-agreements',
        '--disable-interactivity'
    )
    if ($installedResult.ExitCode -eq -1978335212) {
        return [PSCustomObject]@{
            Installed      = $false
            QuerySucceeded = $true
            Status         = 'NotInstalled'
            ExitCode       = $installedResult.ExitCode
            Detail         = $installedResult.Text
        }
    }
    if ($installedResult.ExitCode -ne 0) {
        return [PSCustomObject]@{
            Installed      = $false
            QuerySucceeded = $false
            Status         = 'QueryFailed'
            ExitCode       = $installedResult.ExitCode
            Detail         = "installed-package query exit $($installedResult.ExitCode): $($installedResult.Text)"
        }
    }

    $installedRows = @(Get-WinGetPackageRows -Text $installedResult.Text -PackageId $PackageId)
    if ($installedRows.Count -eq 1 -and [string]::Equals(
            $installedRows[0].Version,
            $PackageVersion,
            [StringComparison]::Ordinal
        )) {
        return [PSCustomObject]@{
            Installed      = $true
            QuerySucceeded = $true
            Status         = 'ExactMatch'
            ExitCode       = 0
            Detail         = $installedResult.Text
        }
    }

    $status = if ($installedRows.Count -gt 1) {
        'Ambiguous'
    } elseif ($installedRows.Count -eq 1) {
        'VersionMismatch'
    } else {
        'Unknown'
    }
    return [PSCustomObject]@{
        Installed      = $false
        QuerySucceeded = $true
        Status         = $status
        ExitCode       = $installedResult.ExitCode
        Detail         = $installedResult.Text
    }
}

function Get-WinGetPackageRows {
    param(
        [AllowNull()][string]$Text,
        [Parameter(Mandatory)][string]$PackageId
    )

    foreach ($line in @($Text -split '\r?\n')) {
        $columns = @($line.Trim() -split '\s+' | Where-Object { $_ })
        for ($index = $columns.Count - 2; $index -ge 0; $index--) {
            if ([string]::Equals($columns[$index], $PackageId, [StringComparison]::OrdinalIgnoreCase)) {
                [PSCustomObject]@{
                    Version = [string]$columns[$index + 1]
                    Line    = $line
                }
                break
            }
        }
    }
}

function Assert-WinGetQuerySucceeded {
    param(
        [Parameter(Mandatory)][object]$State,
        [Parameter(Mandatory)][string]$PackageId,
        [Parameter(Mandatory)][string]$PackageVersion
    )

    if (-not $State.QuerySucceeded) {
        Throw-NativeCommandFailure -ExitCode $State.ExitCode `
            -Message "WinGet could not verify $PackageId version $PackageVersion ($($State.Detail)). Refusing to continue until the installed-package query succeeds."
    }
    switch ($State.Status) {
        'ExactMatch' { return }
        'NotInstalled' { return }
        'VersionMismatch' {
            throw "WinGet detected $PackageId at a version other than the required exact pin '$PackageVersion'. Refusing to continue. Details: $($State.Detail)"
        }
        'Ambiguous' {
            throw "WinGet returned ambiguous installed-package results for $PackageId version $PackageVersion. Refusing to continue. Details: $($State.Detail)"
        }
        default {
            throw "WinGet could not unambiguously verify whether $PackageId version $PackageVersion is installed. Refusing to continue. Details: $($State.Detail)"
        }
    }
}

function Assert-PinnedRegistryVersion {
    param(
        [AllowNull()][object[]]$RegistryEntries,
        [Parameter(Mandatory)][string]$RegistryDisplayName,
        [Parameter(Mandatory)][string]$PackageVersion
    )

    if ($null -eq $RegistryEntries) {
        return
    }

    $entries = @($RegistryEntries | Where-Object { $null -ne $_ })
    if ($entries.Count -eq 0) {
        return
    }

    # A WiX burn bundle registers twice under one DisplayName: the bundle and
    # the MSI it carries. That is not ambiguity as long as both report the same
    # DisplayVersion, so compare distinct versions rather than entry count.
    $versions = @(
        $entries |
            ForEach-Object { [string]$_.DisplayVersion } |
            Sort-Object -Unique
    )
    if ($versions.Count -gt 1) {
        $rendered = ($versions | ForEach-Object {
            if ($_) { "'$_'" } else { 'with no DisplayVersion' }
        }) -join ', '
        throw "Detected conflicting registry versions for ${RegistryDisplayName}: $rendered. The installed version is ambiguous, so setup refuses to continue."
    }

    $installedVersion = $versions[0]
    if (-not [string]::Equals($installedVersion, $PackageVersion, [StringComparison]::Ordinal)) {
        $displayVersion = if ($installedVersion) { "'$installedVersion'" } else { 'with no DisplayVersion' }
        throw "Detected installed $RegistryDisplayName registry version $displayVersion, but this setup requires '$PackageVersion'. Uninstall the existing $RegistryDisplayName installation, then rerun this script so WinGet can install the pinned version. Refusing to continue."
    }
}

function Get-PackageDetection {
    param(
        [Parameter(Mandatory)][string]$WinGetPath,
        [Parameter(Mandatory)][string]$PackageId,
        [Parameter(Mandatory)][string]$RegistryDisplayName,
        [Parameter(Mandatory)][string]$PackageVersion,
        [AllowNull()][string]$ExecutablePath
    )

    $wingetState = Get-WinGetPackageState -WinGetPath $WinGetPath -PackageId $PackageId `
        -PackageVersion $PackageVersion
    $registryEntries = @(Get-InstalledProgram -DisplayName $RegistryDisplayName)
    $registryEntry = $registryEntries | Select-Object -First 1
    # Match Assert-PinnedRegistryVersion: judge by the DISTINCT version set,
    # not the entry count. A WiX burn bundle legitimately registers twice
    # (bundle + carried MSI) under one DisplayName at the same version, which
    # is agreement rather than ambiguity. Counting entries here would leave a
    # correctly installed loopMIDI classified as executable-only and abort
    # setup whenever WinGet does not also track it.
    $registryVersions = @(
        $registryEntries |
            ForEach-Object { [string]$_.DisplayVersion } |
            Sort-Object -Unique
    )
    $registryVersionVerified = $registryVersions.Count -eq 1 -and [string]::Equals(
        $registryVersions[0],
        $PackageVersion,
        [StringComparison]::Ordinal
    )
    $installed = $wingetState.Installed -or $registryVersionVerified

    return [PSCustomObject]@{
        Installed               = $installed
        WinGetState             = $wingetState
        RegistryEntries         = $registryEntries
        RegistryEntry           = $registryEntry
        RegistryVersionVerified = $registryVersionVerified
        ExecutablePath          = $ExecutablePath
        ExecutableOnly          = [bool]$ExecutablePath -and -not $installed
    }
}

function Get-Python312RegistryPath {
    $registryPaths = @(
        'HKCU:\SOFTWARE\Python\PythonCore\3.12\InstallPath',
        'HKLM:\SOFTWARE\Python\PythonCore\3.12\InstallPath',
        'HKLM:\SOFTWARE\WOW6432Node\Python\PythonCore\3.12\InstallPath'
    )
    foreach ($registryPath in $registryPaths) {
        if (-not (Test-Path -LiteralPath $registryPath)) {
            continue
        }
        $properties = Get-ItemProperty -LiteralPath $registryPath
        $executablePath = Get-PropertyValue -InputObject $properties -Name 'ExecutablePath'
        if ($executablePath -and (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
            return [string]$executablePath
        }
        $installDirectory = (Get-Item -LiteralPath $registryPath).GetValue('')
        if ($installDirectory) {
            $candidate = Join-Path ([string]$installDirectory) 'python.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }
    return $null
}

function Show-Preflight {
    Write-Section 'Preflight'

    if ($env:OS -ne 'Windows_NT') {
        throw 'This setup lane is Windows-only. Run it from Windows PowerShell or PowerShell on Windows.'
    }
    if (Test-IsAdministrator) {
        throw 'Run this setup script from an unelevated PowerShell session. WinGet will broker UAC for package installation when required.'
    }

    $windows = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $build = Get-PropertyValue -InputObject $windows -Name 'CurrentBuildNumber'
    $revision = Get-PropertyValue -InputObject $windows -Name 'UBR'
    $displayVersion = Get-PropertyValue -InputObject $windows -Name 'DisplayVersion'
    $productName = Get-PropertyValue -InputObject $windows -Name 'ProductName'
    $architecture = if ($env:PROCESSOR_ARCHITEW6432) {
        $env:PROCESSOR_ARCHITEW6432
    } else {
        $env:PROCESSOR_ARCHITECTURE
    }
    Write-Host "Windows: $productName $displayVersion, build $build.$revision, architecture $architecture"

    $wingetPath = Get-CommandPath -Name 'winget.exe'
    if (-not $wingetPath) {
        throw 'WinGet was not found. Install or update "App Installer" from Microsoft Store before running setup.'
    }
    $wingetVersion = Invoke-WinGet -WinGetPath $wingetPath -Arguments @('--version')
    if ($wingetVersion.ExitCode -ne 0) {
        Throw-NativeCommandFailure -ExitCode $wingetVersion.ExitCode `
            -Message "WinGet exists at '$wingetPath' but could not run (exit $($wingetVersion.ExitCode)): $($wingetVersion.Text)"
    }
    Write-Host "WinGet: $($wingetVersion.Text) ($wingetPath)"

    $pyPath = Get-CommandPath -Name 'py.exe'
    if (-not $pyPath) {
        throw 'The Python launcher (py.exe) was not found. Install Python 3.12 with the launcher enabled.'
    }
    $pythonVersion = Invoke-NativeCommand -Path $pyPath -Arguments @('-3.12', '--version')
    if ($pythonVersion.ExitCode -ne 0) {
        Throw-NativeCommandFailure -ExitCode $pythonVersion.ExitCode `
            -Message "Python 3.12 is unavailable through '$pyPath' (exit $($pythonVersion.ExitCode)). Install Python 3.12, then verify with: py -3.12 --version"
    }
    $pythonRegistryPath = Get-Python312RegistryPath
    $pythonLocation = if ($pythonRegistryPath) { "; interpreter $pythonRegistryPath" } else { '' }
    Write-Host "py -3.12: $($pythonVersion.Text) ($pyPath$pythonLocation)"

    if (-not (Test-Path -LiteralPath $RequirementsPath -PathType Leaf)) {
        throw "Required dependency file is missing: '$RequirementsPath'. Restore tools\requirements-midi-bridge.txt before running setup."
    }
    Write-Host "Requirements: $RequirementsPath"

    if ($IsSimulation) {
        Write-Plan 'Preflight probes ran; installs, pip changes, and GUI launches are disabled.'
    }
}

function Install-WinGetPackage {
    param(
        [Parameter(Mandatory)][string]$SectionName,
        [Parameter(Mandatory)][string]$PackageId,
        [Parameter(Mandatory)][string]$RegistryDisplayName,
        [Parameter(Mandatory)][scriptblock]$FindExecutable,
        [Parameter(Mandatory)][string]$PackageVersion
    )

    Write-Section $SectionName
    $wingetPath = Get-CommandPath -Name 'winget.exe'
    if (-not $wingetPath) {
        throw "Cannot inspect or install $PackageId because WinGet is unavailable."
    }

    $executable = & $FindExecutable
    $detection = Get-PackageDetection -WinGetPath $wingetPath -PackageId $PackageId `
        -RegistryDisplayName $RegistryDisplayName -PackageVersion $PackageVersion `
        -ExecutablePath $executable
    Assert-PinnedRegistryVersion -RegistryEntries $detection.RegistryEntries `
        -RegistryDisplayName $RegistryDisplayName -PackageVersion $PackageVersion
    Assert-WinGetQuerySucceeded -State $detection.WinGetState -PackageId $PackageId `
        -PackageVersion $PackageVersion
    if ($detection.ExecutableOnly) {
        throw "Detected an executable for $SectionName at '$executable', but neither WinGet nor an exact registry DisplayVersion verifies the required version '$PackageVersion'. Refusing to treat executable presence as a compliant installation."
    }
    if ($detection.Installed) {
        $description = if ($detection.RegistryEntry) {
            "$($detection.RegistryEntry.DisplayName) $($detection.RegistryEntry.DisplayVersion)".Trim()
        } else {
            "WinGet package $PackageId $PackageVersion"
        }
        Write-Host "Already installed: $description" -ForegroundColor Green
        return
    }

    if ($DryRun) {
        Write-Plan "Would run: winget install --id $PackageId -e --source $WinGetSource --version $PackageVersion --silent --accept-source-agreements --accept-package-agreements --disable-interactivity"
        return
    }

    if ($PSCmdlet.ShouldProcess($PackageId, "Install $SectionName with WinGet")) {
        $result = Invoke-WinGet -WinGetPath $wingetPath -Arguments @(
            'install',
            '--id', $PackageId,
            '-e',
            '--source', $WinGetSource,
            '--version', $PackageVersion,
            '--silent',
            '--accept-source-agreements',
            '--accept-package-agreements',
            '--disable-interactivity'
        )
        if ($result.ExitCode -ne 0) {
            Throw-NativeCommandFailure -ExitCode $result.ExitCode `
                -Message "WinGet failed to install $PackageId (exit $($result.ExitCode)): $($result.Text)"
        }

        $installedExecutable = & $FindExecutable
        $afterInstall = Get-PackageDetection -WinGetPath $wingetPath -PackageId $PackageId `
            -RegistryDisplayName $RegistryDisplayName -PackageVersion $PackageVersion `
            -ExecutablePath $installedExecutable
        Assert-PinnedRegistryVersion -RegistryEntries $afterInstall.RegistryEntries `
            -RegistryDisplayName $RegistryDisplayName -PackageVersion $PackageVersion
        Assert-WinGetQuerySucceeded -State $afterInstall.WinGetState -PackageId $PackageId `
            -PackageVersion $PackageVersion
        if (-not $afterInstall.Installed) {
            throw "WinGet reported success for $PackageId, but neither an exact-version WinGet result nor an exact registry DisplayVersion verified version '$PackageVersion'."
        }
        if (-not $installedExecutable) {
            throw "WinGet verified $PackageId version '$PackageVersion', but its executable is not detectable."
        }
        Write-Host "$SectionName installed." -ForegroundColor Green
    }
}

function Install-BridgeRequirements {
    Write-Section 'Python MIDI bridge dependencies'
    if ($DryRun) {
        Write-Plan "Would run: py -3.12 -m pip install --disable-pip-version-check -r `"$RequirementsPath`""
        return
    }

    if ($PSCmdlet.ShouldProcess($RequirementsPath, 'Install Python 3.12 MIDI bridge requirements with pip')) {
        if (-not (Test-Path -LiteralPath $RequirementsPath -PathType Leaf)) {
            throw "Cannot install bridge dependencies because '$RequirementsPath' does not exist."
        }
        $pyPath = Get-CommandPath -Name 'py.exe'
        if (-not $pyPath) {
            throw 'Cannot install bridge dependencies because py.exe is unavailable. Install Python 3.12 with the Python launcher.'
        }
        $result = Invoke-NativeCommand -Path $pyPath -Arguments @(
            '-3.12', '-m', 'pip', 'install', '--disable-pip-version-check', '-r', $RequirementsPath
        )
        if ($result.ExitCode -ne 0) {
            Throw-NativeCommandFailure -ExitCode $result.ExitCode `
                -Message "pip failed to install '$RequirementsPath' (exit $($result.ExitCode)): $($result.Text)"
        }
        Write-Host 'Python MIDI bridge dependencies installed.' -ForegroundColor Green
    }
}

function Start-ProgramOnce {
    param(
        [Parameter(Mandatory)][string]$DisplayName,
        [Parameter(Mandatory)][string]$ProcessName,
        [AllowNull()][string]$ExecutablePath
    )

    if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
        Write-Host "$DisplayName is already running; no duplicate was launched." -ForegroundColor Green
        return
    }

    if ($DryRun) {
        $target = if ($ExecutablePath) { $ExecutablePath } else { 'the executable located after installation' }
        Write-Plan "Would launch $DisplayName from $target."
        return
    }

    $targetDescription = if ($ExecutablePath) { $ExecutablePath } else { $DisplayName }
    if ($PSCmdlet.ShouldProcess($targetDescription, "Launch $DisplayName")) {
        if (-not $ExecutablePath) {
            throw "Cannot launch $DisplayName because its executable was not found. Confirm installation, then rerun this script."
        }
        try {
            $process = Start-Process -FilePath $ExecutablePath -PassThru -ErrorAction Stop
            Start-Sleep -Milliseconds 250
            $process.Refresh()
            if ($process.HasExited -and $process.ExitCode -ne 0) {
                Throw-NativeCommandFailure -ExitCode $process.ExitCode `
                    -Message "$DisplayName exited immediately with code $($process.ExitCode)."
            }
        } catch {
            if ($null -ne (Get-NativeCommandFailureExitCode -Exception $_.Exception)) {
                throw
            }
            throw "Failed to launch $DisplayName unelevated from '$ExecutablePath': $($_.Exception.Message)"
        }
        Write-Host "$DisplayName launched." -ForegroundColor Green
    }
}

function Find-MidiBridge {
    $preferredNames = @('midi_bridge.py', 'midi_keyboard_bridge.py')
    foreach ($name in $preferredNames) {
        $candidate = Join-Path $PSScriptRoot $name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    foreach ($candidate in @(Get-ChildItem -LiteralPath $PSScriptRoot -File -Filter '*.py' |
        Where-Object {
            $_.BaseName -match '(?i)(midi.*bridge|bridge.*midi)' -and
            $_.BaseName -notmatch '(?i)(?:_core|_test|test_)$'
        } |
        Sort-Object Name)) {
        $source = Get-Content -LiteralPath $candidate.FullName -Raw
        if ($source -match '(?m)__name__\s*==\s*[''"]__main__[''"]' -or $source -match '(?i)argparse') {
            return $candidate.FullName
        }
    }
    return $null
}

function Get-BridgePortListOption {
    param([Parameter(Mandatory)][string]$BridgePath)
    $source = Get-Content -LiteralPath $BridgePath -Raw
    foreach ($option in @('--list-devices', '--list-ports', '--list-midi-ports', '--ports')) {
        if ($source -match [regex]::Escape($option)) {
            return $option
        }
    }
    return $null
}

function Test-Installation {
    Write-Section 'Validation'
    $wingetPath = Get-CommandPath -Name 'winget.exe'
    if (-not $wingetPath) {
        throw 'Validation failed because WinGet is unavailable.'
    }

    $loopMidiExecutable = Find-LoopMidiExecutable
    $loopMidiDetection = Get-PackageDetection -WinGetPath $wingetPath -PackageId $LoopMidiPackageId `
        -RegistryDisplayName 'loopMIDI' -PackageVersion $LoopMidiVersion `
        -ExecutablePath $loopMidiExecutable
    Assert-PinnedRegistryVersion -RegistryEntries $loopMidiDetection.RegistryEntries `
        -RegistryDisplayName 'loopMIDI' -PackageVersion $LoopMidiVersion
    Assert-WinGetQuerySucceeded -State $loopMidiDetection.WinGetState -PackageId $LoopMidiPackageId `
        -PackageVersion $LoopMidiVersion
    if ($loopMidiDetection.ExecutableOnly) {
        throw "Validation failed: the loopMIDI executable exists, but version '$LoopMidiVersion' is not verified by an exact-version WinGet result or exact registry DisplayVersion."
    }
    if ($loopMidiDetection.Installed -and $loopMidiExecutable) {
        Write-Host "loopMIDI executable: $loopMidiExecutable" -ForegroundColor Green
    } elseif ($IsSimulation) {
        Write-Plan 'Would validate the installed loopMIDI executable.'
    } elseif (-not $loopMidiDetection.Installed) {
        throw "Validation failed: $LoopMidiPackageId version '$LoopMidiVersion' is not exactly verified as installed."
    } else {
        throw "Validation failed: $LoopMidiPackageId version '$LoopMidiVersion' is verified, but its executable is not detectable."
    }

    $vmpkExecutable = Find-VmpkExecutable
    $vmpkDetection = Get-PackageDetection -WinGetPath $wingetPath -PackageId $VmpkPackageId `
        -RegistryDisplayName 'Virtual MIDI Piano Keyboard' -PackageVersion $VmpkVersion `
        -ExecutablePath $vmpkExecutable
    Assert-PinnedRegistryVersion -RegistryEntries $vmpkDetection.RegistryEntries `
        -RegistryDisplayName 'Virtual MIDI Piano Keyboard' -PackageVersion $VmpkVersion
    Assert-WinGetQuerySucceeded -State $vmpkDetection.WinGetState -PackageId $VmpkPackageId `
        -PackageVersion $VmpkVersion
    if ($vmpkDetection.ExecutableOnly) {
        throw "Validation failed: the VMPK executable exists, but version '$VmpkVersion' is not verified by an exact-version WinGet result or exact registry DisplayVersion."
    }
    if ($vmpkDetection.Installed -and $vmpkExecutable) {
        Write-Host "VMPK executable: $vmpkExecutable" -ForegroundColor Green
    } elseif ($IsSimulation) {
        Write-Plan 'Would validate the installed VMPK executable.'
    } elseif (-not $vmpkDetection.Installed) {
        throw "Validation failed: $VmpkPackageId version '$VmpkVersion' is not exactly verified as installed."
    } else {
        throw "Validation failed: $VmpkPackageId version '$VmpkVersion' is verified, but the Virtual MIDI Piano Keyboard executable is not detectable."
    }

    if ($IsSimulation) {
        Write-Plan 'Would run py -3.12 -m pip check.'
    } else {
        $pyPath = Get-CommandPath -Name 'py.exe'
        if (-not $pyPath) {
            throw 'Dependency validation failed because py.exe is unavailable.'
        }
        $pipCheck = Invoke-NativeCommand -Path $pyPath -Arguments @('-3.12', '-m', 'pip', 'check')
        if ($pipCheck.ExitCode -ne 0) {
            Throw-NativeCommandFailure -ExitCode $pipCheck.ExitCode `
                -Message "Python dependency validation failed (exit $($pipCheck.ExitCode)): $($pipCheck.Text)"
        }
        Write-Host "pip check: $($pipCheck.Text)" -ForegroundColor Green
    }

    $bridgePath = Find-MidiBridge
    if (-not $bridgePath) {
        Write-Detail 'No MIDI bridge entry point was found under tools; MIDI-port listing was skipped.'
        return
    }

    $portListOption = Get-BridgePortListOption -BridgePath $bridgePath
    if (-not $portListOption) {
        Write-Warning "Bridge found at '$bridgePath', but no recognized read-only port-list option was found. Inspect it with: py -3.12 `"$bridgePath`" --help"
        return
    }

    if ($IsSimulation) {
        Write-Plan "Would safely list MIDI ports with: py -3.12 `"$bridgePath`" $portListOption"
        return
    }

    $pyPath = Get-CommandPath -Name 'py.exe'
    $portOutput = Invoke-NativeCommand -Path $pyPath -Arguments @('-3.12', $bridgePath, $portListOption)
    if ($portOutput.ExitCode -ne 0) {
        Throw-NativeCommandFailure -ExitCode $portOutput.ExitCode `
            -Message "The MIDI bridge could not list ports (exit $($portOutput.ExitCode)): $($portOutput.Text)"
    }
    Write-Host 'MIDI ports reported by the bridge:' -ForegroundColor Green
    $portOutput.Output | ForEach-Object { Write-Host "  $_" }
}

function Show-ManualSteps {
    Write-Section 'Required manual GUI configuration'
    Write-Host '1. In loopMIDI, enter Robot MIDI in the "New port-name" box and click +.'
    Write-Host '2. Confirm a port named "Robot MIDI" appears in loopMIDI.'
    Write-Host '3. In VMPK, open Edit > MIDI Connections.'
    Write-Host '4. Select "Robot MIDI" as the MIDI OUT connection, then apply/close.'
    Write-Host 'These GUI choices are intentionally not automated. No scheduled task or autostart entry is created.'
}

function Invoke-MidiSetup {
    Show-Preflight
    Install-WinGetPackage -SectionName 'loopMIDI' -PackageId $LoopMidiPackageId `
        -RegistryDisplayName 'loopMIDI' -FindExecutable { Find-LoopMidiExecutable } `
        -PackageVersion $LoopMidiVersion
    Install-WinGetPackage -SectionName 'VMPK' -PackageId $VmpkPackageId `
        -RegistryDisplayName 'Virtual MIDI Piano Keyboard' -FindExecutable { Find-VmpkExecutable } `
        -PackageVersion $VmpkVersion
    Install-BridgeRequirements

    Write-Section 'Applications'
    Start-ProgramOnce -DisplayName 'loopMIDI' -ProcessName 'loopMIDI' -ExecutablePath (Find-LoopMidiExecutable)
    Start-ProgramOnce -DisplayName 'VMPK' -ProcessName 'vmpk' -ExecutablePath (Find-VmpkExecutable)

    Show-ManualSteps
    Test-Installation

    Write-Host "`nMIDI keyboard setup lane completed." -ForegroundColor Green
}

try {
    Invoke-MidiSetup
} catch {
    [Console]::Error.WriteLine("ERROR: $($_.Exception.Message)")
    $nativeExitCode = Get-NativeCommandFailureExitCode -Exception $_.Exception
    if ($null -ne $nativeExitCode) {
        exit [int]$nativeExitCode
    }
    exit 1
}
