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

// https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
ErrorOr<SerializeBitmapResult> serialize_bitmap(Gfx::Bitmap const& bitmap, Utf16View type, Optional<double> quality);

}
