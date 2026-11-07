/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/PNGWriter.h>

#import <UI/Qt/Conversions.h>

namespace Ladybird {

Gfx::Color ns_color_to_gfx_color(NSColor* color)
{
    auto rgb_color = [color colorUsingColorSpace:NSColorSpace.genericRGBColorSpace];
    if (rgb_color != nil)
        return {
            static_cast<u8>([rgb_color redComponent] * 255),
            static_cast<u8>([rgb_color greenComponent] * 255),
            static_cast<u8>([rgb_color blueComponent] * 255),
            static_cast<u8>([rgb_color alphaComponent] * 255)
        };
    return {};
}

NSURL* url_to_ns_url(URL::URL const& url)
{
    auto serialized_url = url.serialize();
    return [NSURL URLWithDataRepresentation:string_to_ns_data(serialized_url.bytes_as_string_view()) relativeToURL:nil];
}

}
