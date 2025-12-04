# Quick Reference: Windows Installer Creation

This is a quick reference guide for creating Windows installers. For complete documentation, see [WindowsInstaller.md](../Documentation/WindowsInstaller.md).

## Prerequisites

- Visual Studio 2022 with Clang tools
- CMake 3.25+
- NSIS 3.0+ (for .exe installer)
- Git for Windows

## Quick Commands

### Create Installer (One Command)

```powershell
.\build-windows.ps1 Distribution -Package
```

### Step by Step

```powershell
# 1. Configure
cmake --preset Windows_Distribution

# 2. Build
cmake --build --preset Windows_Distribution --parallel

# 3. Package
cd Build/windows-distribution
cpack -C Release
```

## Output Files

After successful packaging, you'll find in `Build/windows-distribution/`:

- `Ladybird-X.X.X-win64.exe` - NSIS installer (GUI)
- `Ladybird-X.X.X-win64.zip` - Portable ZIP archive

## Installer Features

- Desktop shortcut
- Start Menu entry  
- Uninstaller
- PATH integration (optional)
- Upgrade support

## Testing

```powershell
# Silent install (for testing)
.\Ladybird-X.X.X-win64.exe /S

# Silent uninstall
"C:\Program Files\Ladybird\uninstall.exe" /S
```

## Customization

Edit `Meta/CMake/windows_installer.cmake` to customize:
- Install directory
- Shortcuts
- Registry entries
- Branding

Add custom images to `Meta/Installer/`:
- `banner.bmp` (493×58 px)
- `dialog.bmp` (493×312 px)

## Troubleshooting

| Issue | Solution |
|-------|----------|
| NSIS not found | Install NSIS and add to PATH |
| Missing DLLs | Use Windows_Distribution preset |
| Large installer | Check for debug symbols |
| Install fails | Test on clean Windows VM |

## Distribution Checklist

Before releasing:

- [ ] Version number updated
- [ ] Built with Windows_Distribution preset
- [ ] Tested on clean Windows install
- [ ] Tested upgrade from previous version
- [ ] Tested uninstallation
- [ ] Virus scan completed
- [ ] SHA256 hash documented

## Support

- [Full Documentation](../Documentation/WindowsInstaller.md)
- [Report Issues](https://github.com/LadybirdBrowser/ladybird/issues)
- [Discord](https://discord.gg/ladybird)
