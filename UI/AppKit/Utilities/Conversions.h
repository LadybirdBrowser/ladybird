/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

String ns_string_to_string(NSString*);
ByteString ns_string_to_byte_string(NSString*);
NSString* string_to_ns_string(StringView);

NSData* string_to_ns_data(StringView);

NSDictionary* deserialize_json_to_dictionary(StringView);

Gfx::IntRect ns_rect_to_gfx_rect(NSRect);
NSRect gfx_rect_to_ns_rect(Gfx::IntRect);

Gfx::IntSize ns_size_to_gfx_size(NSSize);
NSSize gfx_size_to_ns_size(Gfx::IntSize);

Gfx::IntPoint ns_point_to_gfx_point(NSPoint);
NSPoint gfx_point_to_ns_point(Gfx::IntPoint);

Gfx::Color ns_color_to_gfx_color(NSColor*);
NSColor* gfx_color_to_ns_color(Gfx::Color);

Gfx::IntPoint compute_origin_relative_to_window(NSWindow*, Gfx::IntPoint);

NSImage* gfx_bitmap_to_ns_image(Gfx::Bitmap const&);

}
