/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <png.h>
#include <zlib.h>

namespace Gfx {

struct WriterContext {
    AK_ALLOC_WITH_KMALLOC;

    Vector<u8*> row_pointers;
    ByteBuffer png_data;
};

ErrorOr<ByteBuffer> PNGWriter::encode(Gfx::Bitmap const& bitmap, Options options)
{
    auto context = make<WriterContext>();
    int width = bitmap.width();
    int height = bitmap.height();

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        return Error::from_string_literal("Failed to create PNG write struct");
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        return Error::from_string_literal("Failed to create PNG info struct");
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return Error::from_string_literal("Error during PNG encoding");
    }

    if (options.icc_data.has_value()) {
        png_set_iCCP(png_ptr, info_ptr, "embedded profile", 0, options.icc_data->data(), options.icc_data->size());
    }

    if (options.cicp.has_value()) {
        auto const& cicp = *options.cicp;
        png_set_cICP(png_ptr, info_ptr, to_underlying(cicp.color_primaries()), to_underlying(cicp.transfer_characteristics()), to_underlying(cicp.matrix_coefficients()), to_underlying(cicp.video_full_range_flag()));
    }

    if (bitmap.format() == BitmapFormat::BGRA8888 || bitmap.format() == BitmapFormat::BGRx8888) {
        png_set_bgr(png_ptr);
    }

    png_set_write_fn(png_ptr, &context->png_data, [](png_structp png_ptr, u8* data, size_t length) {
        auto* buffer = reinterpret_cast<ByteBuffer*>(png_get_io_ptr(png_ptr));
        buffer->append(data, length); }, nullptr);

    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    if (options.compression == PNGCompression::Fast) {
        // NB: Trying every filter on each row and compressing with zlib's default level dominate the encoding time.
        png_set_filter(png_ptr, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
        png_set_compression_level(png_ptr, Z_BEST_SPEED);
        png_set_compression_strategy(png_ptr, Z_RLE);
    }

    context->row_pointers.resize(height);
    for (int y = 0; y < height; ++y) {
        context->row_pointers[y] = const_cast<u8*>(bitmap.scanline_u8(y));
    }

    png_set_rows(png_ptr, info_ptr, context->row_pointers.data());
    png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, nullptr);

    png_destroy_write_struct(&png_ptr, &info_ptr);

    return context->png_data;
}

}
