/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Optional.h>
#include <UI/Qt/PlatformColors.h>

#import <AppKit/AppKit.h>

namespace Ladybird {

static Optional<Gfx::Color> color_in_appearance(NSColor* color, Web::CSS::PreferredColorScheme color_scheme)
{
    __block Optional<Gfx::Color> result;
    auto* appearance = [NSAppearance appearanceNamed:color_scheme == Web::CSS::PreferredColorScheme::Dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    [appearance performAsCurrentDrawingAppearance:^{
        auto* srgb_color = [color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (!srgb_color)
            return;
        auto channel = [](CGFloat value) { return static_cast<u8>(clamp(AK::round(value * 255), 0, 255)); };
        result = Gfx::Color(channel(srgb_color.redComponent), channel(srgb_color.greenComponent), channel(srgb_color.blueComponent),
            channel(srgb_color.alphaComponent));
    }];
    return result;
}

// NB: The system's text selection color follows the user's highlight color. LibWeb makes it translucent.
Gfx::Color macos_web_selection_color(Web::CSS::PreferredColorScheme color_scheme)
{
    return color_in_appearance([NSColor selectedTextBackgroundColor], color_scheme).value_or(Gfx::Color(128, 188, 254, 153));
}

Gfx::Color macos_web_inactive_selection_color()
{
    return Gfx::Color(183, 183, 183, 153);
}

}
