/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/RustFFI.h>

namespace Gfx {

ErrorOr<ByteBuffer> PNGWriter::encode(Gfx::Bitmap const& bitmap, Options options)
{
    FFI::PNGEncodeOptions ffi_options {};
    if (options.icc_data.has_value()) {
        ffi_options.icc_profile = options.icc_data->data();
        ffi_options.icc_profile_len = options.icc_data->size();
    }
    if (options.cicp.has_value()) {
        auto const& cicp = *options.cicp;
        ffi_options.has_cicp = true;
        ffi_options.cicp_color_primaries = to_underlying(cicp.color_primaries());
        ffi_options.cicp_transfer_function = to_underlying(cicp.transfer_characteristics());
        ffi_options.cicp_matrix_coefficients = to_underlying(cicp.matrix_coefficients());
        ffi_options.cicp_video_full_range_flag = to_underlying(cicp.video_full_range_flag());
    }
    ffi_options.fast = options.compression == PNGCompression::Fast;
    ffi_options.premultiplied = bitmap.alpha_type() == AlphaType::Premultiplied && (bitmap.format() == BitmapFormat::BGRA8888 || bitmap.format() == BitmapFormat::RGBA8888);

    auto is_bgra = bitmap.format() == BitmapFormat::BGRA8888 || bitmap.format() == BitmapFormat::BGRx8888;

    ByteBuffer png_data;
    auto write = [](void* context, u8 const* data, size_t length) {
        return !static_cast<ByteBuffer*>(context)->try_append(data, length).is_error();
    };
    if (!FFI::png_rust_encode(bitmap.scanline_u8(0), bitmap.width(), bitmap.height(), bitmap.pitch(), is_bgra, &ffi_options, &png_data, write))
        return Error::from_string_literal("Error during PNG encoding");
    return png_data;
}

}
