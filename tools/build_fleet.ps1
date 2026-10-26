param(
    [ValidateRange(1,15)][int]$Start = 1,
    [ValidateRange(1,15)][int]$End = 15,
    [switch]$Fullclean,
    [ValidateSet('Zones','SharedSsid')][string]$NetworkMode = 'Zones',
    [string]$ProfileDirectory = ''
)

$ErrorActionPreference = 'Stop'
if ($End -lt $Start) { throw "End must be >= Start" }
if ([string]::IsNullOrWhiteSpace($ProfileDirectory)) {
    $ProfileDirectory = if ($NetworkMode -eq 'Zones') { '.' } else { 'shared_profiles' }
}

$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    if ($NetworkMode -eq 'SharedSsid') {
        python tools\generate_fleet_sdkconfigs.py --mode shared --output-dir $ProfileDirectory --start $Start --end $End --check-only
    } elseif ($ProfileDirectory -eq '.') {
        python tools\generate_fleet_sdkconfigs.py --start $Start --end $End --check-only
    } else {
        python tools\generate_zone_sdkconfigs.py --output-dir $ProfileDirectory --check-only
    }
    if ($LASTEXITCODE -ne 0) { throw "Fleet sdkconfig verification failed" }

    for ($n = $Start; $n -le $End; $n++) {
        $id = "{0:D2}" -f $n
        $cfg = Join-Path $ProfileDirectory "sdkconfig.esp$id"
        $build = if ($NetworkMode -eq 'Zones') { "build-esp$id-zone-v6243" } else { "build-esp$id-shared-v6243" }
        Write-Host "=== ESP$id : $cfg -> $build ==="
        if ($Fullclean -and (Test-Path $build)) {
            Remove-Item -Recurse -Force $build
        }
        & idf.py -B $build -D "SDKCONFIG=$cfg" build
        if ($LASTEXITCODE -ne 0) { throw "ESP$id build failed" }
    }
    Write-Host "FLEET_BUILD=PASS range=ESP$('{0:D2}' -f $Start)-ESP$('{0:D2}' -f $End)"
}
finally {
    Pop-Location
}
