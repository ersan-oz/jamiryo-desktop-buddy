param([string]$Device, [string]$HostAddress, [switch]$SkipBuild, [string]$Firmware)
$ErrorActionPreference = 'Stop'
$jamiryoRoot = Split-Path $PSScriptRoot -Parent
$jamiryoPython = (Get-Command python -ErrorAction Stop).Source
if (-not $SkipBuild) {
    & $jamiryoPython (Join-Path $PSScriptRoot 'build.py')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if (-not $Firmware) {
    $jamiryoVersion = [regex]::Match((Get-Content -LiteralPath (Join-Path $jamiryoRoot 'CMakeLists.txt') -Raw), 'set\(PROJECT_VER "([^"]+)"\)').Groups[1].Value
    $Firmware = Join-Path $jamiryoRoot ('.local/artifacts/Jamiryo-' + $jamiryoVersion + '.bin')
}
[string[]]$jamiryoArguments = @((Join-Path $PSScriptRoot 'deploy_wifi.py'), 'upload', '--firmware', $Firmware)
if ($Device) { $jamiryoArguments += @('--device', $Device) }
if ($HostAddress) { $jamiryoArguments += @('--host', $HostAddress) }
& $jamiryoPython @jamiryoArguments
exit $LASTEXITCODE
