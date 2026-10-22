/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibURL/URL.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

String ns_string_to_string(NSString*);
NSString* string_to_ns_string(StringView);

NSData* string_to_ns_data(StringView);

Gfx::Color ns_color_to_gfx_color(NSColor*);

NSURL* url_to_ns_url(URL::URL const&);

}
