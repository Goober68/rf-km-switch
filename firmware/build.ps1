# Build the RF KM Switch firmware targets via the NCS toolchain.
#
# Usage:
#   .\firmware\build.ps1                          # build all targets
#   .\firmware\build.ps1 -Target console_ptx      # build just one
#   .\firmware\build.ps1 -Pristine                # force pristine rebuild
#
# Requires:
#   - nrfutil on PATH (winget install NordicSemiconductor.nrfutil)
#   - NCS v3.3.0 installed: nrfutil toolchain-manager install --ncs-version v3.3.0
#   - NCS workspace at C:\ncs\v3.3.0 (west init -m ... ; west update)

[CmdletBinding()]
param(
    [ValidateSet('console_ptx','receiver_prx','all')]
    [string]$Target = 'all',
    [switch]$Pristine
)

# Don't set $ErrorActionPreference='Stop' here: in Windows PowerShell 5.1, native
# stderr lines (which Zephyr/west use for progress) get wrapped as NativeCommandError
# and would abort the script even when the build succeeded. We check $LASTEXITCODE
# manually after each call instead.

$ncsVersion = 'v3.3.0'
$ncsRoot    = 'C:\ncs\v3.3.0'

$nrfutilCmd = Get-Command nrfutil -ErrorAction SilentlyContinue
if ($nrfutilCmd) {
    $nrfutil = $nrfutilCmd.Source
} else {
    $shim = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\nrfutil.exe'
    if (Test-Path $shim) {
        $nrfutil = $shim
    } else {
        throw "nrfutil not found. Install with: winget install NordicSemiconductor.nrfutil"
    }
}

$env:WEST_TOPDIR = $ncsRoot
$env:ZEPHYR_BASE = Join-Path $ncsRoot 'zephyr'

$repo = Split-Path $PSScriptRoot -Parent

$apps = @(
    @{ Name = 'console_ptx';  Board = 'xiao_ble' },
    @{ Name = 'receiver_prx'; Board = 'nrf52840dongle/nrf52840' }
)

foreach ($app in $apps) {
    if ($Target -ne 'all' -and $Target -ne $app.Name) { continue }

    Write-Host "==> Building $($app.Name) for $($app.Board)" -ForegroundColor Cyan

    $westArgs = @(
        'toolchain-manager','launch',
        '--ncs-version', $ncsVersion,
        '--chdir', $repo,
        '--',
        'west','build',
        '-b', $app.Board,
        '-d', "build/$($app.Name)"
    )
    if ($Pristine) { $westArgs += @('-p','always') }
    $westArgs += "firmware/$($app.Name)"

    & $nrfutil @westArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Build $($app.Name) failed (nrfutil exit $LASTEXITCODE)"
    }
}

Write-Host "==> Done." -ForegroundColor Green
