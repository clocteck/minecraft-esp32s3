param(
    [string]$HostRoot = "C:\Users\wzh\Documents\PlatformIO\Projects\260105-200034-arduino-wifiscan",
    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$SourceRoot = Join-Path $ProjectRoot "src"
$BuildRoot = Join-Path $ProjectRoot "build"
$PackageRoot = Join-Path $ProjectRoot "package"
$PackageModule = Join-Path $ProjectRoot "package\modules\classicube.so"
$DistArchive = Join-Path $ProjectRoot "dist\classicube-esp32s3-0.1.0.zip"
$IdfRoot = Join-Path $env:USERPROFILE ".platformio\packages\framework-espidf"
$PythonEnv = Join-Path $env:USERPROFILE ".espressif\python_env\idf5.5_py3.10_env"
$IdfPython = Join-Path $PythonEnv "Scripts\python.exe"
$Ninja = Join-Path $env:USERPROFILE ".platformio\packages\tool-ninja\ninja.exe"

if (-not (Test-Path $IdfPython)) { throw "ESP-IDF Python not found: $IdfPython" }
if (-not (Test-Path $Ninja)) { throw "Ninja not found: $Ninja" }
if (-not (Test-Path (Join-Path $HostRoot "src\dynmod\module_abi.h"))) {
    throw "Current host ABI not found under: $HostRoot"
}

$env:IDF_PATH = $IdfRoot
$env:IDF_TOOLS_PATH = Join-Path $env:USERPROFILE ".espressif"
$env:IDF_PYTHON_ENV_PATH = $PythonEnv
$ToolPaths = @(
    (Join-Path $env:USERPROFILE ".espressif\tools\cmake\3.30.2\bin"),
    (Join-Path $env:USERPROFILE ".espressif\tools\ninja\1.12.1"),
    (Join-Path $env:USERPROFILE ".espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64"),
    (Join-Path $env:USERPROFILE ".espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin")
)
$env:PATH = ($ToolPaths -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH

if ($Reconfigure -or -not (Test-Path -LiteralPath (Join-Path $BuildRoot "build.ninja"))) {
    & $IdfPython "$IdfRoot\tools\idf.py" -C $SourceRoot -B $BuildRoot -D IDF_TARGET=esp32s3 -D CUBIC_HOST_ROOT=$HostRoot reconfigure
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF configure failed" }
}

& $Ninja -C $BuildRoot so
if ($LASTEXITCODE -ne 0) { throw "Shared-object build failed" }

New-Item -ItemType Directory -Force (Split-Path $PackageModule) | Out-Null
Copy-Item -Force (Join-Path $BuildRoot "classicube.so") $PackageModule
$hash = (Get-FileHash -Algorithm SHA256 $PackageModule).Hash.ToLowerInvariant()
$size = (Get-Item $PackageModule).Length
Write-Host "Built $PackageModule"
Write-Host "size=$size sha256=$hash"
New-Item -ItemType Directory -Force (Split-Path $DistArchive) | Out-Null
Compress-Archive -Path (Join-Path $PackageRoot "*") -DestinationPath $DistArchive -Force
$archiveHash = (Get-FileHash -Algorithm SHA256 $DistArchive).Hash.ToLowerInvariant()
$archiveSize = (Get-Item $DistArchive).Length
Write-Host "Packaged $DistArchive"
Write-Host "size=$archiveSize sha256=$archiveHash"
