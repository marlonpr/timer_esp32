param(
    [ValidateRange(1,15)][int]$Start = 1,
    [ValidateRange(1,15)][int]$End = 15,
    [switch]$Fullclean
)

$ErrorActionPreference = 'Stop'
if ($End -lt $Start) { throw "End must be >= Start" }

$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    python tools\generate_fleet_sdkconfigs.py --start $Start --end $End --check-only
    if ($LASTEXITCODE -ne 0) { throw "Fleet sdkconfig verification failed" }

    for ($n = $Start; $n -le $End; $n++) {
        $id = "{0:D2}" -f $n
        $cfg = "sdkconfig.esp$id"
        $build = "build-esp$id-v62315"
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
