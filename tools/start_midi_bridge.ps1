[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $BridgeArguments
)

$ErrorActionPreference = 'Stop'
$ScriptPath = Join-Path $PSScriptRoot 'midi_serial_bridge.py'

if (-not (Get-Command py -CommandType Application -ErrorAction SilentlyContinue)) {
    throw 'The Python launcher (py) was not found. Install it with Python 3.12, then install tools\requirements-midi-bridge.txt.'
}

& py -3.12 -c 'import sys; sys.exit(0 if sys.version_info[:2] == (3, 12) else 1)'
if ($LASTEXITCODE -ne 0) {
    throw 'Python 3.12 is not available through the Python launcher. Install Python 3.12, then install tools\requirements-midi-bridge.txt.'
}

& py -3.12 $ScriptPath @BridgeArguments
exit $LASTEXITCODE
