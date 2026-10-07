[CmdletBinding()]
param(
    [switch]$BuildOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot "..")
)
$buildDir = Join-Path $projectRoot "build"
$binaryPath = Join-Path $buildDir "IoTSE.bin"
$elfPath = Join-Path $buildDir "IoTSE.elf"
$flasherArgsPath = Join-Path $buildDir "flasher_args.json"

$idfPath = "C:\esp\master\esp-idf"
$pythonEnv = "C:\Espressif\tools\python\master\venv"
$pythonExe = Join-Path $pythonEnv "Scripts\python.exe"
$cmakeDir = "C:\Espressif\tools\cmake\4.0.3\bin"
$cmakeExe = Join-Path $cmakeDir "cmake.exe"
$openOcdRoot = "C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260831\openocd-esp32"
$openOcdExe = Join-Path $openOcdRoot "bin\openocd.exe"
$openOcdScripts = Join-Path $openOcdRoot "share\openocd\scripts"
$romElfDir = "C:\Espressif\tools\esp-rom-elfs\20260528"

$requiredPaths = @(
    $idfPath,
    $pythonExe,
    $cmakeExe,
    $openOcdExe,
    $openOcdScripts,
    $romElfDir,
    $buildDir
)
foreach ($path in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required ESP-IDF path is missing: $path"
    }
}

$env:IDF_PATH = $idfPath
$env:IDF_PYTHON_ENV_PATH = $pythonEnv
$env:PYTHON = $pythonExe
$env:ESP_ROM_ELF_DIR = $romElfDir
$env:OPENOCD_SCRIPTS = $openOcdScripts
$toolPaths = @(
    "C:\Espressif\tools\xtensa-esp-elf\esp-16.1.0_20260609\xtensa-esp-elf\bin",
    "C:\Espressif\tools\esp-idf-configdep\0.2.3\esp-idf-configdep-0.2.3\bin",
    $cmakeDir,
    "C:\Espressif\tools\ninja\1.12.1"
)
$env:Path = ($toolPaths -join ";") + ";" + $env:Path

Write-Host "[1/3] Building current workspace..." -ForegroundColor Cyan
& $cmakeExe --build $buildDir
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE. Flash was not started."
}

foreach ($artifact in @($binaryPath, $elfPath, $flasherArgsPath)) {
    if (-not (Test-Path -LiteralPath $artifact)) {
        throw "Build artifact is missing: $artifact"
    }
}

$sourceFiles = @(
    Get-ChildItem -LiteralPath (Join-Path $projectRoot "src") -Recurse -File
    Get-ChildItem -LiteralPath (Join-Path $projectRoot "components") -Recurse -File
    Get-Item -LiteralPath (Join-Path $projectRoot "CMakeLists.txt")
    Get-Item -LiteralPath (Join-Path $projectRoot "sdkconfig")
    Get-Item -LiteralPath (Join-Path $projectRoot "partitions.csv")
)
$latestSource = $sourceFiles |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
$binary = Get-Item -LiteralPath $binaryPath
if ($binary.LastWriteTimeUtc -lt $latestSource.LastWriteTimeUtc) {
    throw "Refusing stale flash: IoTSE.bin is older than $($latestSource.FullName)"
}

$hash = Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256
Write-Host "[2/3] Fresh artifact verified" -ForegroundColor Green
Write-Host "      File: $($binary.FullName)"
Write-Host "      Size: $($binary.Length) bytes"
Write-Host "      Time: $($binary.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
Write-Host "      SHA256: $($hash.Hash)"

if ($BuildOnly) {
    Write-Host "Build-only check completed; flash was not started." -ForegroundColor Green
    return
}

$buildForOpenOcd = $buildDir.Replace("\", "/")
Write-Host "[3/3] JTAG flash + verify..." -ForegroundColor Cyan
& $openOcdExe `
    -s $openOcdScripts `
    -f "board/esp32s3-builtin.cfg" `
    -c "adapter speed 5000" `
    -c "program_esp_bins $buildForOpenOcd flasher_args.json verify reset exit"
if ($LASTEXITCODE -ne 0) {
    throw "JTAG flash failed with exit code $LASTEXITCODE"
}

Write-Host "Fresh build flashed and verified successfully." -ForegroundColor Green
