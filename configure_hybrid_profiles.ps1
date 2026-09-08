param(
    [ValidateSet('ESP01','ESP02','ESP03','ESP04','ESP05','ALL')]
    [string]$Device = 'ALL'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$profiles = @{
    'ESP01' = @{ Target='esp32';   Config='sdkconfig.esp01'; Defaults='sdkconfig.defaults.esp01'; Build='build-esp01' }
    'ESP02' = @{ Target='esp32';   Config='sdkconfig.esp02'; Defaults='sdkconfig.defaults.esp02'; Build='build-esp02' }
    'ESP03' = @{ Target='esp32s3'; Config='sdkconfig.esp03'; Defaults='sdkconfig.defaults.esp03'; Build='build-esp03' }
    'ESP04' = @{ Target='esp32s3'; Config='sdkconfig.esp04'; Defaults='sdkconfig.defaults.esp04'; Build='build-esp04' }
    'ESP05' = @{ Target='esp32s3'; Config='sdkconfig.esp05'; Defaults='sdkconfig.defaults.esp05'; Build='build-esp05' }
}

$selected = if ($Device -eq 'ALL') { @('ESP01','ESP02','ESP03','ESP04','ESP05') } else { @($Device) }

foreach ($name in $selected) {
    $p = $profiles[$name]
    $configPath = Join-Path $Root $p.Config
    $defaults = "$(Join-Path $Root 'sdkconfig.defaults');$(Join-Path $Root $p.Defaults)"
    $buildPath = Join-Path $Root $p.Build

    Write-Host ""
    Write-Host "=== $name : target=$($p.Target) ===" -ForegroundColor Cyan

    # For S3 profiles, always generate a fresh target-correct sdkconfig.
    # For classic ESP01/ESP02, preserve the supplied known-good sdkconfig unless absent.
    if ($p.Target -eq 'esp32s3') {
        if (Test-Path $configPath) { Remove-Item -Force $configPath }
    }
    if (Test-Path $buildPath) { Remove-Item -Recurse -Force $buildPath }

    $cmakeArgs = @(
        '-B', $buildPath,
        '-D', "SDKCONFIG=$configPath",
        '-D', "SDKCONFIG_DEFAULTS=$defaults"
    )

    & idf.py @cmakeArgs set-target $p.Target
    if ($LASTEXITCODE -ne 0) { throw "set-target failed for $name" }

    & idf.py @cmakeArgs reconfigure
    if ($LASTEXITCODE -ne 0) { throw "reconfigure failed for $name" }

    Write-Host "Generated $($p.Config) for $($p.Target)." -ForegroundColor Green
}

Write-Host ""
Write-Host "Profiles configured. Build a device with .\build_device.ps1 -Device ESP03" -ForegroundColor Green
