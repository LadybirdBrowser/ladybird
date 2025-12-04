# Building Ladybird on Windows (Native)

This guide covers building Ladybird natively on Windows without WSL.

## Table of Contents
- [Prerequisites](#prerequisites)
- [Quick Start](#quick-start)
- [Build Options](#build-options)
- [Creating an Installer](#creating-an-installer)
- [Manual Build Steps](#manual-build-steps)
- [Troubleshooting](#troubleshooting)

## Prerequisites

### Required Software

1. **Visual Studio 2022** (Community Edition or higher)
   - Install the "Desktop development with C++" workload
   - Ensure the "C++ Clang tools for Windows" component is selected
   - Download: https://visualstudio.microsoft.com/downloads/

2. **CMake 3.25 or newer**
   - Download: https://cmake.org/download/
   - Make sure to add CMake to your system PATH during installation

3. **Git for Windows**
   - Download: https://git-scm.com/download/win
   - Required for cloning the repository and vcpkg

4. **LLVM/Clang** (if not using Visual Studio's bundled version)
   - Download: https://github.com/llvm/llvm-project/releases
   - Version 18 or newer recommended
   - Add LLVM bin directory to PATH

5. **NSIS** (Optional, for creating installers)
   - Download: https://nsis.sourceforge.io/Download
   - Version 3.0 or newer
   - Add NSIS to your system PATH

### Recommended Software

- **Ninja Build System** - Faster builds than MSBuild
  - Download: https://github.com/ninja-build/ninja/releases
  - Or install via: `choco install ninja` (if using Chocolatey)

- **pkg-config** - Required for vcpkg
  - Install via Chocolatey: `choco install pkgconfiglite -y`
  - Or download from: http://ftp.gnome.org/pub/gnome/binaries/win32/dependencies/

## Quick Start

### Using the PowerShell Build Script (Recommended)

The easiest way to build Ladybird on Windows is using the provided PowerShell script:

```powershell
# Clone the repository
git clone https://github.com/LadybirdBrowser/ladybird.git
cd ladybird

# Build release version (configure, build, and install)
.\build-windows.ps1

# Or build debug version
.\build-windows.ps1 Debug

# Create installer package
.\build-windows.ps1 Distribution -Package
```

### Build Script Options

```powershell
# Show help
.\build-windows.ps1 -Help

# Configure only
.\build-windows.ps1 Release -Configure

# Build only (after configuration)
.\build-windows.ps1 Release -Build

# Clean build
.\build-windows.ps1 Release -Clean -All

# Create installer
.\build-windows.ps1 Distribution -Package
```

## Build Options

### Available Build Types

1. **Release** - Optimized release build
   - Preset: `Windows_Release`
   - Build directory: `Build/windows-release`
   - Best for daily use and testing

2. **Debug** - Debug build with symbols
   - Preset: `Windows_Experimental`
   - Build directory: `Build/debug`
   - Best for development and debugging

3. **Distribution** - Distribution build for installer creation
   - Preset: `Windows_Distribution`
   - Build directory: `Build/windows-distribution`
   - Static linking, optimized for distribution
   - Required for creating installers

4. **Experimental** - Experimental Windows build
   - Preset: `Windows_Experimental`
   - Build directory: `Build/debug`
   - May not support all features

### CMake Presets

The project uses CMake presets for easy configuration:

```powershell
# List available presets
cmake --list-presets

# Configure with a specific preset
cmake --preset Windows_Release

# Build with a preset
cmake --build --preset Windows_Release
```

## Creating an Installer

### Prerequisites for Installer Creation

1. Install NSIS (Nullsoft Scriptable Install System)
2. Build using the Distribution preset
3. Ensure all dependencies are properly linked

### Steps to Create Installer

#### Using the Build Script (Recommended)

```powershell
# One-step installer creation
.\build-windows.ps1 Distribution -Package
```

This will:
1. Configure the project with distribution settings
2. Build all targets
3. Install to staging directory
4. Create NSIS installer (.exe)
5. Create ZIP archive

#### Manual Steps

```powershell
# 1. Configure for distribution
cmake --preset Windows_Distribution

# 2. Build the project
cmake --build --preset Windows_Distribution --parallel

# 3. Create the installer
cd Build/windows-distribution
cpack -C Release
```

### Installer Output

The installer will be created in `Build/windows-distribution/`:
- `Ladybird-X.X.X-win64.exe` - NSIS installer
- `Ladybird-X.X.X-win64.zip` - Portable ZIP archive

### Installer Features

- **Desktop Shortcut** - Creates a Ladybird shortcut on the desktop
- **Start Menu Entry** - Adds Ladybird to the Start Menu
- **Uninstaller** - Includes a complete uninstaller
- **PATH Integration** - Optionally adds Ladybird to system PATH
- **Upgrade Support** - Properly handles upgrades from previous versions

## Manual Build Steps

If you prefer to build manually without the script:

### 1. Bootstrap vcpkg

```powershell
# Clone vcpkg (if not already done)
git clone https://github.com/microsoft/vcpkg.git Build/vcpkg

# Bootstrap vcpkg
.\Build\vcpkg\bootstrap-vcpkg.bat
```

### 2. Configure the Project

```powershell
# For Release build
cmake --preset Windows_Release

# For Distribution build (installer)
cmake --preset Windows_Distribution
```

### 3. Build the Project

```powershell
# Build with all available cores
cmake --build --preset Windows_Release --parallel

# Or specify number of parallel jobs
cmake --build --preset Windows_Release --parallel 8
```

### 4. Install (Optional)

```powershell
cmake --install Build/windows-release --prefix Build/windows-release/install
```

### 5. Run Ladybird

```powershell
.\Build\windows-release\bin\Ladybird.exe
```

## Advanced Configuration

### Custom CMake Options

You can override CMake options when configuring:

```powershell
cmake --preset Windows_Release `
  -DENABLE_QT=ON `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

### Using Different Compilers

The Windows presets are configured to use `clang-cl` by default. To use a different compiler:

```powershell
cmake --preset Windows_Release `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl
```

### Vcpkg Configuration

vcpkg is configured automatically via CMake presets. The configuration includes:
- Binary caching for faster rebuilds
- Custom triplets for different build types
- Overlay ports for Ladybird-specific dependencies

## Troubleshooting

### Common Issues

#### 1. "CMake was unable to find a build program corresponding to Ninja"

This usually indicates a vcpkg dependency build failure. Check:
- `Build/windows-release/vcpkg-manifest-install.log` for detailed errors
- Ensure you have all prerequisites installed
- Try cleaning and rebuilding: `.\build-windows.ps1 Release -Clean -All`

#### 2. "clang-cl: command not found"

Solutions:
- Install LLVM/Clang from the official releases
- Or ensure Visual Studio's Clang tools are installed
- Add Clang to your system PATH

#### 3. "LINK : fatal error LNK1181: cannot open input file"

This usually means a dependency failed to build. Try:
- Check vcpkg logs in `Build/caches/`
- Ensure you have enough disk space (vcpkg requires significant space)
- Try building with fewer parallel jobs: `cmake --build --preset Windows_Release --parallel 4`

#### 4. Out of Memory During Build

If you encounter out-of-memory errors:
- Reduce parallel jobs: `cmake --build --preset Windows_Release --parallel 2`
- Close other applications
- Consider using the Debug preset which has faster compilation

#### 5. NSIS Installer Creation Fails

Ensure:
- NSIS is installed and in your PATH
- You built with the Distribution preset
- All files were properly installed to the staging directory

### Getting Help

If you encounter issues not covered here:
1. Check the main [build documentation](Documentation/BuildInstructionsLadybird.md)
2. Search existing [GitHub issues](https://github.com/LadybirdBrowser/ladybird/issues)
3. Ask on the [Ladybird Discord](https://discord.gg/ladybird)
4. Create a new issue with:
   - Your Windows version
   - Visual Studio version
   - CMake version
   - Complete error messages
   - Build logs

## Performance Tips

### Faster Builds

1. **Use Ninja** instead of MSBuild:
   - Already configured in CMake presets
   - Significantly faster for incremental builds

2. **Enable ccache** (if available on Windows):
   ```powershell
   cmake --preset Windows_Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
   ```

3. **Use vcpkg binary caching**:
   - Already configured in CMake presets
   - Caches built dependencies in `Build/caches/vcpkg-binary-cache`

4. **Adjust parallel jobs** based on your system:
   ```powershell
   # Use 75% of available cores
   cmake --build --preset Windows_Release --parallel 6
   ```

### Disk Space Management

vcpkg and builds can consume significant disk space:
- vcpkg packages: ~5-10 GB
- Build artifacts: ~2-5 GB per configuration
- Binary cache: ~3-8 GB

To clean up:
```powershell
# Clean build directory
Remove-Item -Recurse -Force Build/windows-release

# Clean vcpkg binary cache
Remove-Item -Recurse -Force Build/caches/vcpkg-binary-cache

# Clean vcpkg packages (will require rebuilding)
Remove-Item -Recurse -Force Build/vcpkg/packages
```

## Development Workflow

### Recommended Setup for Development

1. **Initial Setup**:
   ```powershell
   .\build-windows.ps1 Debug -All
   ```

2. **Incremental Builds**:
   ```powershell
   .\build-windows.ps1 Debug -Build
   ```

3. **Testing Changes**:
   ```powershell
   .\Build\debug\bin\Ladybird.exe
   ```

4. **Creating Release Build**:
   ```powershell
   .\build-windows.ps1 Release -All
   ```

5. **Creating Installer for Distribution**:
   ```powershell
   .\build-windows.ps1 Distribution -Package
   ```

## Next Steps

- Read the [Contributing Guide](CONTRIBUTING.md)
- Check out [Coding Style](Documentation/CodingStyle.md)
- Explore [Browser Architecture](Documentation/BrowserArchitecture.md)
- Join the [Ladybird Community](https://ladybird.org/community)
