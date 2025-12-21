# Build phoenix-discovery with MSYS2/UCRT64
# Run from MSYS2 UCRT64 terminal or PowerShell with MSYS2 in PATH

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build"

Write-Host "=== Building phoenix-discovery ===" -ForegroundColor Cyan

# Create build directory
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

Push-Location $BuildDir

try {
    # Check for MSYS2
    $msys2Path = "C:\msys64\ucrt64\bin"
    if (Test-Path $msys2Path) {
        $env:PATH = "$msys2Path;$env:PATH"
    }
    
    # Configure with CMake
    Write-Host "Configuring..." -ForegroundColor Yellow
    cmake -G "MinGW Makefiles" ..
    
    # Build
    Write-Host "Building..." -ForegroundColor Yellow
    mingw32-make -j4
    
    Write-Host ""
    Write-Host "=== Build complete ===" -ForegroundColor Green
    Write-Host "Test program: $BuildDir\test_discovery.exe"
    Write-Host ""
    Write-Host "To test:" -ForegroundColor Cyan
    Write-Host "  Terminal 1: .\test_discovery.exe server SDR1"
    Write-Host "  Terminal 2: .\test_discovery.exe client WF1"
}
finally {
    Pop-Location
}
