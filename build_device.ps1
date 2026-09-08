param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('ESP01','ESP02','ESP03','ESP04','ESP05')]
    [string]$Device
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
$p=$profiles[$Device]
$configPath=Join-Path $Root $p.Config
$defaults="$(Join-Path $Root 'sdkconfig.defaults');$(Join-Path $Root $p.Defaults)"
$buildPath=Join-Path $Root $p.Build

if (!(Test-Path $configPath)) {
    Write-Host "$($p.Config) does not exist yet; generating it for $($p.Target)..." -ForegroundColor Yellow
    & idf.py -B $buildPath -D "SDKCONFIG=$configPath" -D "SDKCONFIG_DEFAULTS=$defaults" set-target $p.Target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& idf.py -B $buildPath -D "SDKCONFIG=$configPath" -D "SDKCONFIG_DEFAULTS=$defaults" build
exit $LASTEXITCODE
