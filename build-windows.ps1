# Ladybird Windows Build Script
# This script helps build Ladybird on Windows using CMake presets

param(
    [Parameter(Position=0)]
    [ValidateSet("Release", "Debug", "Distribution", "Experimental")]
    [string]$BuildType = "Release",
    
    [Parameter()]
    [switch]$Configure,
    
    [Parameter()]
    [switch]$Build,
    
    [Parameter()]
    [switch]$Install,
    
    [Parameter()]
    [switch]$Package,
    
    [Parameter()]
    [switch]$Clean,
    
    [Parameter()]
    [switch]$All,
    
    [Parameter()]
    [switch]$Help
)

$ErrorActionPreference = "Stop"

function Show-Help {
    Write-Host @"
Ladybird Windows Build Script
==============================

Usage: .\build-windows.ps1 [BuildType] [Options]

Build Types:
  Release        - Optimized release build (default)
  Debug          - Debug build with symbols
  Distribution   - Distribution build for installer creation
  Experimental   - Experimental Windows build

Options:
  -Configure     - Run CMake configuration only
  -Build         - Build the project only
  -Install       - Install the built project
  -Package       - Create installer package (requires Distribution build)
  -Clean         - Clean build directory before building
  -All           - Configure, build, and install (default if no options specified)
  -Help          - Show this help message

Examples:
  .\build-windows.ps1                          # Build release version
  .\build-windows.ps1 Debug -All               # Configure, build, and install debug version
  .\build-windows.ps1 Distribution -Package    # Create installer package
  .\build-windows.ps1 Release -Clean -Build    # Clean and build release version

Requirements:
  - CMake 3.25 or newer
  - Visual Studio 2022 with Clang/LLVM toolchain
  - NSIS (for creating installers)
  - vcpkg (will be bootstrapped automatically)

"@
}

if ($Help) {
    Show-Help
    exit 0
}

# If no action specified, do all
if (-not ($Configure -or $Build -or $Install -or $Package)) {
    $All = $true
}

# Determine preset name
$PresetName = switch ($BuildType) {
    "Release"      { "Windows_Release" }
    "Debug"        { "Windows_Experimental" }
    "Distribution" { "Windows_Distribution" }
    "Experimental" { "Windows_Experimental" }
}

$BuildDir = switch ($BuildType) {
    "Release"      { "Build/windows-release" }
    "Debug"        { "Build/debug" }
    "Distribution" { "Build/windows-distribution" }
    "Experimental" { "Build/debug" }
}

Write-Host "==================================" -ForegroundColor Cyan
Write-Host "Ladybird Windows Build" -ForegroundColor Cyan
Write-Host "==================================" -ForegroundColor Cyan
Write-Host "Build Type: $BuildType" -ForegroundColor Yellow
Write-Host "Preset: $PresetName" -ForegroundColor Yellow
Write-Host "Build Directory: $BuildDir" -ForegroundColor Yellow
Write-Host ""

# Check for required tools
Write-Host "Checking prerequisites..." -ForegroundColor Green

# Check CMake
try {
    $cmakeVersion = cmake --version | Select-Object -First 1
    Write-Host "✓ CMake found: $cmakeVersion" -ForegroundColor Green
} catch {
    Write-Host "✗ CMake not found. Please install CMake 3.25 or newer." -ForegroundColor Red
    exit 1
}

# Check for clang-cl
try {
    $clangVersion = clang-cl --version | Select-Object -First 1
    Write-Host "✓ Clang-CL found: $clangVersion" -ForegroundColor Green
} catch {
    Write-Host "✗ Clang-CL not found. Please install LLVM/Clang toolchain." -ForegroundColor Red
    Write-Host "  Download from: https://github.com/llvm/llvm-project/releases" -ForegroundColor Yellow
    exit 1
}

# Check for NSIS if packaging
if ($Package -or ($BuildType -eq "Distribution" -and $All)) {
    try {
        $nsisVersion = makensis /VERSION 2>&1
        Write-Host "✓ NSIS found: v$nsisVersion" -ForegroundColor Green
    } catch {
        Write-Host "⚠ NSIS not found. Installer creation will not be available." -ForegroundColor Yellow
        Write-Host "  Download from: https://nsis.sourceforge.io/Download" -ForegroundColor Yellow
    }
}

Write-Host ""

# Bootstrap vcpkg if needed
$vcpkgDir = "Build/vcpkg"
if (-not (Test-Path "$vcpkgDir/vcpkg.exe")) {
    Write-Host "Bootstrapping vcpkg..." -ForegroundColor Green
    if (-not (Test-Path $vcpkgDir)) {
        git clone https://github.com/microsoft/vcpkg.git $vcpkgDir
    }
    & "$vcpkgDir/bootstrap-vcpkg.bat"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ Failed to bootstrap vcpkg" -ForegroundColor Red
        exit 1
    }
}

# Clean if requested
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
    Write-Host "✓ Build directory cleaned" -ForegroundColor Green
    Write-Host ""
}

# Configure
if ($Configure -or $All) {
    Write-Host "==================================" -ForegroundColor Cyan
    Write-Host "Configuring..." -ForegroundColor Cyan
    Write-Host "==================================" -ForegroundColor Cyan
    
    cmake --preset $PresetName
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ Configuration failed" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "✓ Configuration completed" -ForegroundColor Green
    Write-Host ""
}

# Build
if ($Build -or $All) {
    Write-Host "==================================" -ForegroundColor Cyan
    Write-Host "Building..." -ForegroundColor Cyan
    Write-Host "==================================" -ForegroundColor Cyan
    
    cmake --build --preset $PresetName --parallel
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ Build failed" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "✓ Build completed" -ForegroundColor Green
    Write-Host ""
}

# Install
if ($Install -or $All) {
    Write-Host "==================================" -ForegroundColor Cyan
    Write-Host "Installing..." -ForegroundColor Cyan
    Write-Host "==================================" -ForegroundColor Cyan
    
    cmake --install $BuildDir --prefix "$BuildDir/install"
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ Installation failed" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "✓ Installation completed" -ForegroundColor Green
    Write-Host ""
}

# Package
if ($Package) {
    if ($BuildType -ne "Distribution") {
        Write-Host "⚠ Warning: Packaging is recommended with Distribution build type" -ForegroundColor Yellow
        Write-Host "  Current build type: $BuildType" -ForegroundColor Yellow
        Write-Host ""
    }
    
    Write-Host "==================================" -ForegroundColor Cyan
    Write-Host "Creating Installer Package..." -ForegroundColor Cyan
    Write-Host "==================================" -ForegroundColor Cyan
    
    Push-Location $BuildDir
    cpack -C Release
    Pop-Location
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ Packaging failed" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "✓ Packaging completed" -ForegroundColor Green
    Write-Host ""
    Write-Host "Installer created in: $BuildDir" -ForegroundColor Green
    Get-ChildItem "$BuildDir/*.exe", "$BuildDir/*.zip" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  - $($_.Name)" -ForegroundColor Cyan
    }
}

Write-Host ""
Write-Host "==================================" -ForegroundColor Green
Write-Host "Build Process Completed!" -ForegroundColor Green
Write-Host "==================================" -ForegroundColor Green

if ($All -and $BuildType -ne "Distribution") {
    Write-Host ""
    Write-Host "To run Ladybird:" -ForegroundColor Yellow
    Write-Host "  $BuildDir\bin\Ladybird.exe" -ForegroundColor Cyan
}

if ($BuildType -eq "Distribution" -and -not $Package) {
    Write-Host ""
    Write-Host "To create an installer package, run:" -ForegroundColor Yellow
    Write-Host "  .\build-windows.ps1 Distribution -Package" -ForegroundColor Cyan
}
