/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <png.h>

namespace Gfx {

struct WriterContext {
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

    if (bitmap.format() == BitmapFormat::BGRA8888 || bitmap.format() == BitmapFormat::BGRx8888) {
        png_set_bgr(png_ptr);
    }

    png_set_write_fn(png_ptr, &context->png_data, [](png_structp png_ptr, u8* data, size_t length) {
        auto* buffer = reinterpret_cast<ByteBuffer*>(png_get_io_ptr(png_ptr));
        buffer->append(data, length); }, nullptr);

    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

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
