/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/PNGWriter.h>

#import <UI/Qt/Conversions.h>

namespace Ladybird {

String ns_string_to_string(NSString* string)
{
    auto const* utf8 = [string UTF8String];
    return MUST(String::from_utf8({ utf8, strlen(utf8) }));
}

NSString* string_to_ns_string(StringView string)
{
    return [[NSString alloc] initWithData:string_to_ns_data(string) encoding:NSUTF8StringEncoding];
}

NSData* string_to_ns_data(StringView string)
{
    return [NSData dataWithBytes:string.characters_without_null_termination() length:string.length()];
}

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
