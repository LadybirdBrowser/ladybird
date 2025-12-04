# Windows Support for Ladybird Browser

Welcome to native Windows support for Ladybird! This document provides an overview of Windows-specific features and how to get started.

## Status

**Native Windows builds are now supported!**

Ladybird can be built and run natively on Windows without WSL. The Windows port includes:

- Native Windows compilation using Clang-CL
- CMake preset configurations for Windows
- PowerShell build automation script
- Windows installer creation (.exe)
- Static linking for distribution builds
- Qt-based UI on Windows
- Some features may still be experimental

## Quick Start

### Prerequisites

- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ and Clang tools
- CMake 3.25 or newer
- Git for Windows

### Build in 3 Steps

```powershell
# 1. Clone the repository
git clone https://github.com/LadybirdBrowser/ladybird.git
cd ladybird

# 2. Run the build script
.\build-windows.ps1

# 3. Run Ladybird
.\Build\windows-release\bin\Ladybird.exe
```

That's it!

## Documentation

- **[Complete Build Instructions](Documentation/BuildInstructionsWindows.md)** - Detailed guide for building on Windows
- **[Installer Creation Guide](Documentation/WindowsInstaller.md)** - How to create Windows installers
- **[Main Build Instructions](Documentation/BuildInstructionsLadybird.md)** - Cross-platform build guide

## Build Options

### Using the PowerShell Script

```powershell
# Release build (optimized)
.\build-windows.ps1 Release

# Debug build (for development)
.\build-windows.ps1 Debug

# Distribution build (for installer)
.\build-windows.ps1 Distribution

# Create installer package
.\build-windows.ps1 Distribution -Package

# Clean build
.\build-windows.ps1 Release -Clean -All

# Show help
.\build-windows.ps1 -Help
```

### Using CMake Directly

```powershell
# Configure
cmake --preset Windows_Release

# Build
cmake --build --preset Windows_Release --parallel

# Create installer
cd Build/windows-distribution
cpack -C Release
```

## Available CMake Presets

| Preset | Description | Use Case |
|--------|-------------|----------|
| `Windows_Release` | Optimized release build | Daily use, testing |
| `Windows_Debug` | Debug build with symbols | Development, debugging |
| `Windows_Distribution` | Static build for distribution | Creating installers |
| `Windows_Experimental` | Experimental features | Testing new features |
| `Windows_CI` | CI/CD build | Automated testing |

## Creating an Installer

To create a Windows installer (.exe):

```powershell
.\build-windows.ps1 Distribution -Package
```

This creates:
- `Ladybird-X.X.X-win64.exe` - NSIS installer with GUI
- `Ladybird-X.X.X-win64.zip` - Portable ZIP archive

The installer includes:
- Desktop shortcut
- Start Menu entry
- Uninstaller
- Optional PATH integration

See [Windows Installer Documentation](Documentation/WindowsInstaller.md) for details.

## System Requirements

### Minimum Requirements

- **OS**: Windows 10 (64-bit) or newer
- **RAM**: 8 GB (16 GB recommended for building)
- **Disk Space**: 20 GB free space
- **CPU**: x64 processor with SSE4.2 support

### Build Requirements

- **Visual Studio 2022** (Community or higher)
  - Desktop development with C++ workload
  - C++ Clang tools for Windows component
- **CMake**: 3.25 or newer
- **Git**: For cloning repository and vcpkg
- **NSIS**: 3.0+ (optional, for creating installers)

## Features

### What Works

- Core browser functionality
- Qt-based user interface
- JavaScript engine (LibJS)
- HTML/CSS rendering
- Network stack
- Resource loading
- Developer tools
- Multiple tabs
- Bookmarks and history

### Known Limitations

- Some Unix-specific features may not be available
- Audio support is experimental
- Some advanced features are still in development

See [GitHub Issues](https://github.com/LadybirdBrowser/ladybird/issues?q=is%3Aissue+is%3Aopen+label%3Awindows) for current status.

## Development

### Setting Up Development Environment

1. **Install Visual Studio 2022**
   - Include "Desktop development with C++"
   - Include "C++ Clang tools for Windows"

2. **Install CMake**
   - Download from https://cmake.org/download/
   - Add to PATH during installation

3. **Clone and Build**
   ```powershell
   git clone https://github.com/LadybirdBrowser/ladybird.git
   cd ladybird
   .\build-windows.ps1 Debug
   ```

### Development Workflow

```powershell
# Initial build
.\build-windows.ps1 Debug -All

# Make code changes...

# Incremental build (faster)
.\build-windows.ps1 Debug -Build

# Test your changes
.\Build\debug\bin\Ladybird.exe

# Create release build for testing
.\build-windows.ps1 Release -All
```

### IDE Support

Ladybird can be opened in:
- **Visual Studio 2022** - Open the CMakeLists.txt as a CMake project
- **Visual Studio Code** - Use CMake Tools extension
- **CLion** - Native CMake support

## Troubleshooting

### Common Issues

**Build fails with "clang-cl not found"**
- Install LLVM/Clang or ensure Visual Studio's Clang tools are installed
- Add Clang to your system PATH

**Out of memory during build**
- Reduce parallel jobs: `.\build-windows.ps1 Release -Build` (then manually build with fewer jobs)
- Close other applications
- Consider using Debug build which compiles faster

**vcpkg dependencies fail to build**
- Check `Build/windows-release/vcpkg-manifest-install.log`
- Ensure you have enough disk space (20+ GB)
- Try cleaning and rebuilding

**Installer creation fails**
- Install NSIS from https://nsis.sourceforge.io/
- Ensure NSIS is in your PATH
- Use the Distribution preset

For more troubleshooting, see [Build Instructions](Documentation/BuildInstructionsWindows.md#troubleshooting).

## Performance Tips

- Use **Ninja** build system (already configured in presets)
- Enable **vcpkg binary caching** (already configured)
- Adjust **parallel jobs** based on your system resources
- Use **Release** build for better runtime performance
- Use **Debug** build for faster compilation during development

## Contributing

We welcome contributions to improve Windows support!

### Areas That Need Help

- Testing on different Windows versions
- Performance optimizations
- Windows-specific bug fixes
- Documentation improvements
- Installer enhancements

### How to Contribute

1. Read the [Contributing Guide](CONTRIBUTING.md)
2. Check [Windows-related issues](https://github.com/LadybirdBrowser/ladybird/labels/windows)
3. Test on your Windows system and report bugs
4. Submit pull requests with improvements

## Getting Help

- **Documentation**: [BuildInstructionsWindows.md](Documentation/BuildInstructionsWindows.md)
- **Issues**: [GitHub Issues](https://github.com/LadybirdBrowser/ladybird/issues)
- **Discord**: [Ladybird Discord Server](https://discord.gg/ladybird)
- **Website**: [ladybird.org](https://ladybird.org)

## License

Ladybird is licensed under a BSD-style license. See [LICENSE](LICENSE) for details.

## Acknowledgments

Thanks to all contributors who made Windows support possible! Special thanks to the Ladybird team for creating this amazing browser.

---

**Ready to build?** Start with the [Windows Build Instructions](Documentation/BuildInstructionsWindows.md)!
