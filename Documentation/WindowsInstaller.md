# Ladybird Windows Installer

This directory contains configuration and resources for creating Windows installers for Ladybird.

## Quick Start

To create a Windows installer:

```powershell
# Using the build script (recommended)
.\build-windows.ps1 Distribution -Package

# Or manually
cmake --preset Windows_Distribution
cmake --build --preset Windows_Distribution --parallel
cd Build/windows-distribution
cpack -C Release
```

## Installer Types

The build system can create two types of installers:

### 1. NSIS Installer (.exe)
- **File**: `Ladybird-X.X.X-win64.exe`
- **Type**: Executable installer with GUI
- **Features**:
  - Graphical installation wizard
  - Desktop shortcut creation
  - Start Menu integration
  - Uninstaller included
  - PATH environment variable modification (optional)
  - Upgrade detection and handling

### 2. ZIP Archive (.zip)
- **File**: `Ladybird-X.X.X-win64.zip`
- **Type**: Portable archive
- **Features**:
  - No installation required
  - Extract and run
  - Useful for portable/USB installations

## Prerequisites

### For Building Installers

1. **NSIS** (Nullsoft Scriptable Install System)
   - Download: https://nsis.sourceforge.io/Download
   - Version 3.0 or newer
   - Add to system PATH

2. **WiX Toolset** (Optional, alternative to NSIS)
   - Download: https://wixtoolset.org/
   - Version 3.11 or newer
   - Creates MSI installers

3. **Complete Ladybird Build**
   - Must use `Windows_Distribution` preset
   - All dependencies statically linked
   - Optimized for distribution

## Installer Configuration

The installer is configured in `Meta/CMake/windows_installer.cmake` using CPack.

### Key Settings

- **Package Name**: Ladybird
- **Vendor**: Ladybird Browser Initiative
- **Install Directory**: `C:\Program Files\Ladybird`
- **Components**: Runtime binaries and libraries
- **Shortcuts**: Desktop and Start Menu

### Customization

To customize the installer, edit `Meta/CMake/windows_installer.cmake`:

```cmake
# Change install directory
set(CPACK_PACKAGE_INSTALL_DIRECTORY "MyCustomDir")

# Add custom icons
set(CPACK_NSIS_MUI_ICON "${LADYBIRD_SOURCE_DIR}/path/to/icon.ico")

# Modify shortcuts
set(CPACK_NSIS_CREATE_ICONS_EXTRA "...")
```

## Installer Resources

### Required Resources

The following resources are used by the installer:

1. **License File**: `LICENSE` (from repository root)
2. **README**: `README.md` (from repository root)
3. **Application Icon**: `Base/res/icons/ladybird.ico`
4. **Banner Image** (optional): `Meta/Installer/banner.bmp` (493×58 pixels)
5. **Dialog Image** (optional): `Meta/Installer/dialog.bmp` (493×312 pixels)

### Creating Custom Images

For WiX installers, you can create custom branding images:

**Banner Image** (banner.bmp):
- Size: 493×58 pixels
- Format: BMP, 24-bit color
- Usage: Top banner in installer dialogs

**Dialog Image** (dialog.bmp):
- Size: 493×312 pixels
- Format: BMP, 24-bit color
- Usage: Left side of installer dialogs

Place these files in `Meta/Installer/` directory.

## Build Process

### Step-by-Step

1. **Configure** with distribution preset:
   ```powershell
   cmake --preset Windows_Distribution
   ```

2. **Build** all targets:
   ```powershell
   cmake --build --preset Windows_Distribution --parallel
   ```

3. **Install** to staging directory:
   ```powershell
   cmake --install Build/windows-distribution --prefix Build/windows-distribution/install
   ```

4. **Package** with CPack:
   ```powershell
   cd Build/windows-distribution
   cpack -C Release
   ```

### Output Location

Installers are created in `Build/windows-distribution/`:
- `Ladybird-X.X.X-win64.exe` (NSIS)
- `Ladybird-X.X.X-win64.zip` (ZIP)
- `Ladybird-X.X.X-win64.msi` (WiX, if configured)

## Testing the Installer

### Before Distribution

1. **Test Installation**:
   - Run the installer on a clean Windows VM
   - Verify all files are installed correctly
   - Check desktop and Start Menu shortcuts
   - Test the application launches

2. **Test Uninstallation**:
   - Run the uninstaller
   - Verify all files are removed
   - Check registry entries are cleaned up

3. **Test Upgrade**:
   - Install an older version
   - Install the new version over it
   - Verify upgrade process works correctly

### Automated Testing

You can script installer testing:

```powershell
# Silent install
.\Ladybird-X.X.X-win64.exe /S

# Silent uninstall
"C:\Program Files\Ladybird\uninstall.exe" /S
```

## Troubleshooting

### Common Issues

#### NSIS Not Found
```
CPack Error: Cannot find NSIS compiler makensis
```
**Solution**: Install NSIS and add to PATH, or specify manually:
```powershell
cmake --preset Windows_Distribution -DCPACK_NSIS_EXECUTABLE="C:\Program Files (x86)\NSIS\makensis.exe"
```

#### Missing Dependencies
```
CPack Error: Cannot find file: xyz.dll
```
**Solution**: Ensure you built with `Windows_Distribution` preset which statically links dependencies.

#### Installer Too Large
If the installer is unexpectedly large:
- Check for debug symbols (should not be in distribution build)
- Verify static linking is working
- Check for duplicate libraries

#### Installation Fails
If users report installation failures:
- Check Windows version compatibility
- Verify administrator privileges
- Check antivirus/Windows Defender logs
- Test on clean Windows installation

## Distribution Checklist

Before distributing the installer:

- [ ] Built with `Windows_Distribution` preset
- [ ] Version number is correct in CMakeLists.txt
- [ ] All dependencies are statically linked
- [ ] Tested on clean Windows installation
- [ ] Tested upgrade from previous version
- [ ] Tested uninstallation
- [ ] License file is included
- [ ] README is included
- [ ] Digital signature applied (if applicable)
- [ ] Virus scan completed
- [ ] File hash (SHA256) documented

## Code Signing (Optional)

For production releases, consider code signing:

### Using signtool (Windows SDK)

```powershell
# Sign the installer
signtool sign /f "certificate.pfx" /p "password" /t http://timestamp.digicert.com Ladybird-X.X.X-win64.exe

# Verify signature
signtool verify /pa Ladybird-X.X.X-win64.exe
```

### Benefits of Code Signing

- Removes "Unknown Publisher" warnings
- Builds trust with users
- Required for some enterprise deployments
- Prevents tampering detection

## Advanced Configuration

### Multi-Language Support

To add multiple languages to the installer:

```cmake
# In windows_installer.cmake
set(CPACK_NSIS_INSTALLER_MUI_LANGUAGES "English;French;German;Spanish")
```

### Custom Install Components

To create optional components:

```cmake
# Define components
set(CPACK_COMPONENTS_ALL Runtime Development Debug)

# Make some optional
set(CPACK_COMPONENT_DEVELOPMENT_DISABLED ON)
set(CPACK_COMPONENT_DEBUG_DISABLED ON)
```

### Registry Integration

To add registry entries:

```cmake
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
  WriteRegStr HKLM 'Software\\\\Ladybird' 'InstallPath' '$INSTDIR'
  WriteRegStr HKLM 'Software\\\\Ladybird' 'Version' '${CPACK_PACKAGE_VERSION}'
")

set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
  DeleteRegKey HKLM 'Software\\\\Ladybird'
")
```

## Support

For issues with the installer:
- Check [Build Instructions](BuildInstructionsWindows.md)
- Review [GitHub Issues](https://github.com/LadybirdBrowser/ladybird/issues)
- Ask on [Discord](https://discord.gg/ladybird)

## License

The installer configuration is part of the Ladybird project and is licensed under the same terms. See LICENSE file for details.
