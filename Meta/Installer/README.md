# Installer Resources

This directory contains resources used for creating Windows installers.

## Files

### Required Files (Auto-detected from repository)

- **LICENSE** - From repository root
- **README.md** - From repository root
- **Application Icon** - `Base/res/icons/ladybird.ico`

### Optional Branding Files

You can add custom branding images for WiX installers:

#### banner.bmp
- **Size**: 493×58 pixels
- **Format**: BMP, 24-bit color
- **Usage**: Top banner in installer dialogs
- **Location**: Place in this directory as `banner.bmp`

#### dialog.bmp
- **Size**: 493×312 pixels
- **Format**: BMP, 24-bit color
- **Usage**: Left side of installer dialogs
- **Location**: Place in this directory as `dialog.bmp`

## Creating Custom Images

### Using GIMP or Photoshop

1. Create a new image with the specified dimensions
2. Design your branding (logo, colors, etc.)
3. Export as BMP, 24-bit color
4. Save to this directory

### Using ImageMagick

```bash
# Convert existing image to banner
magick convert your-image.png -resize 493x58 -depth 24 banner.bmp

# Convert existing image to dialog
magick convert your-image.png -resize 493x312 -depth 24 dialog.bmp
```

## Default Behavior

If custom branding files are not provided:
- NSIS will use default Windows installer appearance
- WiX will use default MSI installer appearance
- The installer will still function correctly

## Testing

After adding custom images:

1. Rebuild the installer:
   ```powershell
   .\build-windows.ps1 Distribution -Package
   ```

2. Run the installer to verify the images appear correctly

3. Check that images are not distorted or pixelated

## Notes

- Keep file sizes reasonable (< 1 MB each)
- Use professional-looking designs
- Ensure images are readable at the specified sizes
- Test on different Windows themes (light/dark)
