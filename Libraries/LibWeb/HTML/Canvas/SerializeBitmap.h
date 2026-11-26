/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <LibGC/Forward.h>
#include <LibGfx/Forward.h>

namespace Web::HTML {

enum class SerializedBitmapMimeType {
    PNG,
    JPEG,
};

struct SerializeBitmapResult {
    ByteBuffer buffer;
    SerializedBitmapMimeType mime_type;
};

StringView serialized_bitmap_mime_type_to_byte_string(SerializedBitmapMimeType);
Utf16View serialized_bitmap_mime_type_to_utf16_view(SerializedBitmapMimeType);

bool serialization_of_the_bitmap_supports_coding_independent_code_points(Utf16View type);

// Whether a serialization of the bitmap as a file of the type can include a color profile that indicates the color
// space. A bitmap in any other color space is converted to sRGB.
bool serialization_of_the_bitmap_supports_color_space(Utf16View type, Gfx::ColorSpace const&);

// https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
// NB: The bitmap of the frame is in the color space of the frame, which has to be sRGB unless the type supports that
//     color space.
ErrorOr<SerializeBitmapResult> serialize_bitmap(Gfx::DecodedImageFrame const&, Utf16View type, Optional<double> quality);

}

GC_UNTRACED_CAPTURE(Web::HTML::SerializeBitmapResult);
