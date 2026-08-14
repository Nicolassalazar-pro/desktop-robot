# -----------------------------------------------------------------------------
# tools/web_preview/hotspot.ps1 -- turn this PC into the robot's pairing AP.
#
# WHY THIS EXISTS
#
# The firmware's pairing flow is: pull the LCD sheet down -> WiFi.softAP() comes
# up as "Hanger OVERRIDE" -> the LCD shows a Wi-Fi QR for those credentials and a
# URL QR for http://192.168.4.1/?k=<token> -> the phone joins and loads the web
# app. tools/web_preview/robot_sim.py already serves the real web app, but a
# phone can only reach it over whatever network both devices share. On a
# corporate Wi-Fi with client isolation that is nothing at all, and the Wi-Fi QR
# is meaningless because no "Hanger OVERRIDE" network exists.
#
# Windows Mobile Hotspot closes that gap: it advertises a real SSID with a real
# passphrase, so BOTH QR codes behave exactly as they do on the device.
#
# WHY WinRT AND NOT netsh
#
# `netsh wlan set hostednetwork` is the old Soft-AP path and is dead on modern
# drivers -- `netsh wlan show drivers` on this machine reports
# "Hosted network supported : No" (Intel AX200). Mobile Hotspot uses Wi-Fi
# Direct underneath and is exposed only through the WinRT
# NetworkOperatorTetheringManager API, so that is what this drives.
#
# WHY IT RE-LAUNCHES ITSELF
#
# PowerShell 7 removed the WinRT type projection, so
# [Windows.Networking.NetworkOperators...] cannot resolve there. Windows
# PowerShell 5.1 still has it and ships on every Windows box. Rather than make
# the caller remember which shell to use, this script detects the edition and
# re-executes itself under 5.1.
#
# SIDE EFFECTS -- READ BEFORE RUNNING
#
# Starting the hotspot shares this machine's current internet connection. The PC
# keeps its existing Wi-Fi link (the AX200 runs the AP on a virtual adapter), so
# the LAN address stays reachable, but a second network appears with the host at
# 192.168.137.1. Stop it again with -Action Stop.
# -----------------------------------------------------------------------------
[CmdletBinding()]
param(
    [ValidateSet('Status', 'Start', 'Stop')]
    [string]$Action = 'Status',

    # Defaults mirror src/main.cpp kOverrideSsid / kOverridePassword so the
    # Wi-Fi QR rendered by the LCD preview joins THIS hotspot unmodified.
    [string]$Ssid = 'Hanger OVERRIDE',
    [string]$Passphrase = 'TheGarageAtl'
)

$ErrorActionPreference = 'Stop'

# -- re-launch under Windows PowerShell 5.1 if we're on PowerShell 7+ ----------
if ($PSVersionTable.PSEdition -eq 'Core') {
    $ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path $ps51)) {
        throw 'Windows PowerShell 5.1 not found; WinRT tethering API is unavailable.'
    }
    & $ps51 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath `
        -Action $Action -Ssid $Ssid -Passphrase $Passphrase
    exit $LASTEXITCODE
}

Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

# WinRT async methods return IAsyncOperation/IAsyncAction, which PowerShell
# cannot await directly. WindowsRuntimeSystemExtensions.AsTask bridges them to
# .NET Tasks, but the overloads are generic and must be picked by reflection --
# there is no cleaner route from PowerShell.
$asTaskOp = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]

$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.FullName -eq 'Windows.Foundation.IAsyncAction'
})[0]

function Wait-WinRtOperation {
    param($Operation, [Type]$ResultType)
    $task = $asTaskOp.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    $task.Wait(-1) | Out-Null
    $task.Result
}

function Wait-WinRtAction {
    param($Action)
    $task = $asTaskAction.Invoke($null, @($Action))
    $task.Wait(-1) | Out-Null
}

function Get-TetheringManager {
    $profile = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking, ContentType = WindowsRuntime]::GetInternetConnectionProfile()
    if ($null -eq $profile) {
        throw 'No internet connection profile. Mobile Hotspot shares an existing connection, so this PC must be online first.'
    }
    [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking, ContentType = WindowsRuntime]::CreateFromConnectionProfile($profile)
}

$mgr = Get-TetheringManager
$cfg = $mgr.GetCurrentAccessPointConfiguration()

function Show-Status {
    param($Manager, $Config)
    Write-Host ''
    Write-Host 'Mobile Hotspot'
    Write-Host "  state      : $($Manager.TetheringOperationalState)"
    Write-Host "  SSID       : $($Config.Ssid)"
    Write-Host "  clients    : $($Manager.ClientCount) / $($Manager.MaxClientCount)"
    $ip = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -like '192.168.137.*' } |
        Select-Object -First 1 -ExpandProperty IPAddress
    if ($ip) { Write-Host "  host IP    : $ip" }
    Write-Host ''
}

switch ($Action) {
    'Status' {
        Show-Status -Manager $mgr -Config $cfg
    }

    'Start' {
        # WPA2 requires 8..63 characters. Checking here gives a clear message
        # instead of an opaque WinRT HRESULT.
        if ($Passphrase.Length -lt 8 -or $Passphrase.Length -gt 63) {
            throw "Passphrase must be 8-63 characters (got $($Passphrase.Length))."
        }

        if ($cfg.Ssid -ne $Ssid -or $cfg.Passphrase -ne $Passphrase) {
            Write-Host "Configuring hotspot as '$Ssid'..."
            $cfg.Ssid = $Ssid
            $cfg.Passphrase = $Passphrase
            Wait-WinRtAction -Action $mgr.ConfigureAccessPointAsync($cfg)
        }

        if ($mgr.TetheringOperationalState -eq 'On') {
            Write-Host 'Hotspot already running.'
        } else {
            Write-Host 'Starting hotspot...'
            $result = Wait-WinRtOperation -Operation $mgr.StartTetheringAsync() `
                -ResultType ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
            if ($result.Status -ne 'Success') {
                throw "StartTetheringAsync failed: $($result.Status) -- $($result.AdditionalErrorMessage)"
            }
        }

        # Re-read: the manager caches state from construction time.
        $mgr = Get-TetheringManager
        Show-Status -Manager $mgr -Config $mgr.GetCurrentAccessPointConfiguration()
        Write-Host 'Phone: join that SSID, then browse to http://192.168.137.1:8080/'
        Write-Host 'Restart the simulator with --advertise-host 192.168.137.1 so the'
        Write-Host 'LCD preview QR encodes an address the phone can actually reach.'
    }

    'Stop' {
        if ($mgr.TetheringOperationalState -eq 'Off') {
            Write-Host 'Hotspot already stopped.'
        } else {
            Write-Host 'Stopping hotspot...'
            $result = Wait-WinRtOperation -Operation $mgr.StopTetheringAsync() `
                -ResultType ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
            if ($result.Status -ne 'Success') {
                throw "StopTetheringAsync failed: $($result.Status) -- $($result.AdditionalErrorMessage)"
            }
        }
        $mgr = Get-TetheringManager
        Show-Status -Manager $mgr -Config $mgr.GetCurrentAccessPointConfiguration()
    }
}
