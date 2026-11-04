/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Theme.h>

#include <QImage>
#include <QPixmap>

#import <AppKit/AppKit.h>

namespace Ladybird {

// Draws an SF Symbol in one color, centered on a square canvas of the given size.
static QPixmap system_symbol_pixmap(NSString* name, QColor const& color, int logical_size, qreal device_pixel_ratio)
{
    NSImage* symbol = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (!symbol)
        return {};

    auto* ns_color = [NSColor colorWithSRGBRed:color.redF() green:color.greenF() blue:color.blueF() alpha:color.alphaF()];
    auto* configuration = [[NSImageSymbolConfiguration configurationWithPointSize:logical_size * 0.8 weight:NSFontWeightRegular]
        configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:@[ ns_color ]]];
    NSImage* image = [symbol imageWithSymbolConfiguration:configuration];

    auto pixel_size = static_cast<NSInteger>(ceil(logical_size * device_pixel_ratio));
    auto* representation = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                   pixelsWide:pixel_size
                                                                   pixelsHigh:pixel_size
                                                                bitsPerSample:8
                                                              samplesPerPixel:4
                                                                     hasAlpha:YES
                                                                     isPlanar:NO
                                                               colorSpaceName:NSCalibratedRGBColorSpace
                                                                  bytesPerRow:pixel_size * 4
                                                                 bitsPerPixel:32];
    representation.size = NSMakeSize(logical_size, logical_size);

    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:representation];
    auto image_size = image.size;
    auto scale = MIN(1.0, MIN(logical_size / image_size.width, logical_size / image_size.height));
    auto width = image_size.width * scale;
    auto height = image_size.height * scale;
    [image drawInRect:NSMakeRect((logical_size - width) / 2, (logical_size - height) / 2, width, height)];
    [NSGraphicsContext restoreGraphicsState];

    QImage qimage { representation.bitmapData, static_cast<int>(pixel_size), static_cast<int>(pixel_size), static_cast<qsizetype>(representation.bytesPerRow), QImage::Format_RGBA8888_Premultiplied };
    auto pixmap = QPixmap::fromImage(qimage.copy());
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    return pixmap;
}

// NB: The system's translate symbol may only be used for Apple's translation features, which this button offers.
QIcon create_translate_icon(QPalette const& palette, bool page_is_translated)
{
    auto color = page_is_translated ? ChromeStyle::chrome_accent() : Theme::chrome(palette).muted;
    QIcon icon;
    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS) {
        if (auto pixmap = system_symbol_pixmap(@"translate", color, CHROME_ICON_CANVAS_SIZE, device_pixel_ratio); !pixmap.isNull())
            icon.addPixmap(pixmap);
    }
    if (icon.isNull())
        return create_chrome_icon(ChromeIcon::Globe, palette);
    return icon;
}

}
