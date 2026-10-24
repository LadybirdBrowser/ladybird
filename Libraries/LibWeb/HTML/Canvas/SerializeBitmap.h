/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
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

// Whether a serialization of the bitmap as a file of the type includes a color profile that indicates the color space
// of the bitmap. The bitmap of any other file is converted to sRGB.
bool serialization_of_the_bitmap_supports_color_profiles(Utf16View type);

// https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
// NB: The bitmap of the frame is in the color space of the frame, which has to be sRGB unless the type supports color
//     profiles.
ErrorOr<SerializeBitmapResult> serialize_bitmap(Gfx::DecodedImageFrame const&, Utf16View type, Optional<double> quality);

}
