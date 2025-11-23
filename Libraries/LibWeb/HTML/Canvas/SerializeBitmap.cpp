/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
ErrorOr<SerializeBitmapResult> serialize_bitmap(Gfx::Bitmap const& bitmap, StringView type, Optional<double> quality)
{
    // If type is an image format that supports variable quality (such as "image/jpeg"), quality is given, and type is not "image/png", then,
    // if quality is a Number in the range 0.0 to 1.0 inclusive, the user agent must treat quality as the desired quality level.
    // Otherwise, the user agent must use its default quality value, as if the quality argument had not been given.
    bool valid_quality = quality.has_value() and quality.value() >= 0.0 && quality.value() <= 1.0;

    if (type.equals_ignoring_ascii_case("image/jpeg"sv)) {
        AllocatingMemoryStream file;
        Gfx::JPEGWriter::Options jpeg_options;
        if (valid_quality)
            jpeg_options.quality = static_cast<int>(quality.value() * 100);
        TRY(Gfx::JPEGWriter::encode(file, bitmap, jpeg_options));
        return SerializeBitmapResult { TRY(file.read_until_eof()), "image/jpeg"sv };
    }

    // User agents must support PNG ("image/png"). User agents may support other types.
    // If the user agent does not support the requested type, then it must create the file using the PNG format. [PNG]
    return SerializeBitmapResult { TRY(Gfx::PNGWriter::encode(bitmap)), "image/png"sv };
}

}
