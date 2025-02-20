/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/PNGWriter.h>

#import <Utilities/Conversions.h>

namespace Ladybird {

String ns_string_to_string(NSString* string)
{
    auto const* utf8 = [string UTF8String];
    return MUST(String::from_utf8({ utf8, strlen(utf8) }));
}

ByteString ns_string_to_byte_string(NSString* string)
{
    auto const* utf8 = [string UTF8String];
    return ByteString(utf8, strlen(utf8));
}

NSString* string_to_ns_string(StringView string)
{
    return [[NSString alloc] initWithData:string_to_ns_data(string) encoding:NSUTF8StringEncoding];
}

NSData* string_to_ns_data(StringView string)
{
    return [NSData dataWithBytes:string.characters_without_null_termination() length:string.length()];
}

NSDictionary* deserialize_json_to_dictionary(StringView json)
{
    auto* ns_json = string_to_ns_string(json);
    auto* json_data = [ns_json dataUsingEncoding:NSUTF8StringEncoding];

    NSError* error = nil;
    NSDictionary* dictionary = [NSJSONSerialization JSONObjectWithData:json_data
                                                               options:0
                                                                 error:&error];

    if (!dictionary) {
        NSLog(@"Error deserializing DOM tree: %@", error);
    }

    return dictionary;
}

Gfx::IntRect ns_rect_to_gfx_rect(NSRect rect)
{
    return {
        static_cast<int>(rect.origin.x),
        static_cast<int>(rect.origin.y),
        static_cast<int>(rect.size.width),
        static_cast<int>(rect.size.height),
    };
}

NSRect gfx_rect_to_ns_rect(Gfx::IntRect rect)
{
    return NSMakeRect(
        static_cast<CGFloat>(rect.x()),
        static_cast<CGFloat>(rect.y()),
        static_cast<CGFloat>(rect.width()),
        static_cast<CGFloat>(rect.height()));
}

Gfx::IntSize ns_size_to_gfx_size(NSSize size)
{
    return {
        static_cast<int>(size.width),
        static_cast<int>(size.height),
    };
}

NSSize gfx_size_to_ns_size(Gfx::IntSize size)
{
    return NSMakeSize(
        static_cast<CGFloat>(size.width()),
        static_cast<CGFloat>(size.height()));
}

Gfx::IntPoint ns_point_to_gfx_point(NSPoint point)
{
    return {
        static_cast<int>(point.x),
        static_cast<int>(point.y),
    };
}

NSPoint gfx_point_to_ns_point(Gfx::IntPoint point)
{
    return NSMakePoint(
        static_cast<CGFloat>(point.x()),
        static_cast<CGFloat>(point.y()));
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

NSColor* gfx_color_to_ns_color(Gfx::Color color)
{
    return [NSColor colorWithRed:(color.red() / 255.f)
                           green:(color.green() / 255.f)
                            blue:(color.blue() / 255.f)
                           alpha:(color.alpha() / 255.f)];
}

Gfx::IntPoint compute_origin_relative_to_window(NSWindow* window, Gfx::IntPoint position)
{
    // The origin of the NSWindow is its bottom-left corner, relative to the bottom-left of the screen. We must convert
    // window positions sent to/from WebContent between this origin and the window's and screen's top-left corners.
    auto screen_frame = Ladybird::ns_rect_to_gfx_rect([[window screen] frame]);
    auto window_frame = Ladybird::ns_rect_to_gfx_rect([window frame]);

    position.set_y(screen_frame.height() - window_frame.height() - position.y());
    return position;
}

NSImage* gfx_bitmap_to_ns_image(Gfx::Bitmap const& bitmap)
{
    auto png = Gfx::PNGWriter::encode(bitmap);
    if (png.is_error())
        return nullptr;

    auto* data = [NSData dataWithBytes:png.value().data()
                                length:png.value().size()];

    return [[NSImage alloc] initWithData:data];
}

}
