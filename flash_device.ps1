param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('ESP01','ESP02','ESP03','ESP04','ESP05')]
    [string]$Device,
    [Parameter(Mandatory=$true)]
    [string]$Port
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root
$build = "build-" + $Device.ToLower()
if (!(Test-Path $build)) { throw "$build does not exist. Build $Device first." }
& idf.py -B $build -p $Port flash monitor
