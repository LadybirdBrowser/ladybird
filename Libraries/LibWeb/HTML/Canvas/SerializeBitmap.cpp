/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/MemoryStream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>

namespace Web::HTML {

StringView serialized_bitmap_mime_type_to_byte_string(SerializedBitmapMimeType mime_type)
{
    switch (mime_type) {
    case SerializedBitmapMimeType::PNG:
        return "image/png"sv;
    case SerializedBitmapMimeType::JPEG:
        return "image/jpeg"sv;
    }
    VERIFY_NOT_REACHED();
}

Utf16View serialized_bitmap_mime_type_to_utf16_view(SerializedBitmapMimeType mime_type)
{
    switch (mime_type) {
    case SerializedBitmapMimeType::PNG:
        return u"image/png"sv;
    case SerializedBitmapMimeType::JPEG:
        return u"image/jpeg"sv;
    }
    VERIFY_NOT_REACHED();
}

static bool type_is_jpeg(Utf16View type)
{
    return type.equals_ignoring_ascii_case(u"image/jpeg"sv);
}

bool serialization_of_the_bitmap_supports_color_profiles(Utf16View type)
{
    // NB: The color space of a canvas is described by coding-independent code points, which PNG can carry and JPEG
    //     cannot. Every type that is not JPEG is serialized as PNG.
    return !type_is_jpeg(type);
}

// https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
ErrorOr<SerializeBitmapResult> serialize_bitmap(Gfx::DecodedImageFrame const& frame, Utf16View type, Optional<double> quality)
{
    auto const& bitmap = frame.bitmap();
    Optional<ReadonlyBytes> icc_data;
    Optional<Media::CodingIndependentCodePoints> cicp;
    frame.color_space().data().visit(
        [](Empty) { },
        [&](Gfx::ColorSpace::CICP const& color_space_cicp) {
            cicp = Media::CodingIndependentCodePoints { color_space_cicp.color_primaries, color_space_cicp.transfer_characteristics, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full };
        },
        [&](NonnullRefPtr<Gfx::ColorSpace::ICCProfile const> const& icc_profile) {
            icc_data = icc_profile->bytes();
        });

    // If type is an image format that supports variable quality (such as "image/jpeg"), quality is given, and type is not "image/png", then,
    // if quality is a Number in the range 0.0 to 1.0 inclusive, the user agent must treat quality as the desired quality level.
    // Otherwise, the user agent must use its default quality value, as if the quality argument had not been given.
    bool valid_quality = quality.has_value() and quality.value() >= 0.0 && quality.value() <= 1.0;

    // For image types that support color profiles, the serialized image must include a color profile indicating the
    // color space of the underlying bitmap. For image types that do not support color profiles, the serialized image
    // must be converted to the 'srgb' color space using 'relative-colorimetric' rendering intent.
    if (type_is_jpeg(type)) {
        VERIFY(!cicp.has_value());
        AllocatingMemoryStream file;
        Gfx::JPEGWriter::Options jpeg_options;
        jpeg_options.icc_data = icc_data;
        if (valid_quality)
            jpeg_options.quality = static_cast<int>(quality.value() * 100);
        TRY(Gfx::JPEGWriter::encode(file, bitmap, jpeg_options));
        return SerializeBitmapResult { TRY(file.read_until_eof()), SerializedBitmapMimeType::JPEG };
    }

    // User agents must support PNG ("image/png"). User agents may support other types.
    // If the user agent does not support the requested type, then it must create the file using the PNG format. [PNG]
    return SerializeBitmapResult { TRY(Gfx::PNGWriter::encode(bitmap, { .icc_data = icc_data, .cicp = cicp })), SerializedBitmapMimeType::PNG };
}

}
