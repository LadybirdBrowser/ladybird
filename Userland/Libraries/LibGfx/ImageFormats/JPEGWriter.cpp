/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CMYKBitmap.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <jpeglib.h>

namespace Gfx {

struct MemoryDestinationManager : public jpeg_destination_mgr {
    Vector<u8>& buffer;
    static constexpr size_t BUFFER_SIZE_INCREMENT = 65536;

    MemoryDestinationManager(Vector<u8>& buffer)
        : buffer(buffer)
    {
        init_destination = [](j_compress_ptr cinfo) {
            auto* dest = static_cast<MemoryDestinationManager*>(cinfo->dest);
            dest->buffer.resize(BUFFER_SIZE_INCREMENT);
            dest->next_output_byte = dest->buffer.data();
            dest->free_in_buffer = dest->buffer.capacity();
        };

        empty_output_buffer = [](j_compress_ptr cinfo) -> boolean {
            auto* dest = static_cast<MemoryDestinationManager*>(cinfo->dest);
            size_t old_size = dest->buffer.size();
            dest->buffer.resize(old_size + BUFFER_SIZE_INCREMENT);
            dest->next_output_byte = dest->buffer.data() + old_size;
            dest->free_in_buffer = BUFFER_SIZE_INCREMENT;
            return TRUE;
        };

        term_destination = [](j_compress_ptr cinfo) {
            auto* dest = static_cast<MemoryDestinationManager*>(cinfo->dest);
            dest->buffer.resize(dest->buffer.size() - dest->free_in_buffer);
        };

        next_output_byte = nullptr;
        free_in_buffer = 0;
    }
};

ErrorOr<void> JPEGWriter::encode_impl(Stream& stream, auto const& bitmap, Options const& options, ColorSpace color_space)
{
    struct jpeg_compress_struct cinfo { };
    struct jpeg_error_mgr jerr { };

    cinfo.err = jpeg_std_error(&jerr);

    jpeg_create_compress(&cinfo);

    Vector<u8> buffer;
    MemoryDestinationManager dest_manager(buffer);
    cinfo.dest = &dest_manager;

    cinfo.image_width = bitmap.size().width();
    cinfo.image_height = bitmap.size().height();
    cinfo.input_components = 4;

    switch (color_space) {
    case ColorSpace::RGB:
        cinfo.in_color_space = JCS_EXT_BGRX;
        break;
    case ColorSpace::CMYK:
        cinfo.in_color_space = JCS_CMYK;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, JCS_YCbCr);
    jpeg_set_quality(&cinfo, options.quality, TRUE);

    if (options.icc_data.has_value()) {
        jpeg_write_icc_profile(&cinfo, options.icc_data->data(), options.icc_data->size());
    }

    jpeg_start_compress(&cinfo, TRUE);

    Vector<JSAMPLE> row_buffer;
    row_buffer.resize(bitmap.size().width() * 4);

    while (cinfo.next_scanline < cinfo.image_height) {
        auto const* row_ptr = reinterpret_cast<u8 const*>(bitmap.scanline(cinfo.next_scanline));
        JSAMPROW row_pointer = const_cast<JSAMPROW>(row_ptr);
        jpeg_write_scanlines(&cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    TRY(stream.write_until_depleted(buffer));
    return {};
}

ErrorOr<void> JPEGWriter::encode(Stream& stream, Bitmap const& bitmap, Options const& options)
{
    return encode_impl(stream, bitmap, options, ColorSpace::RGB);
}

ErrorOr<void> JPEGWriter::encode(Stream& stream, CMYKBitmap const& bitmap, Options const& options)
{
    return encode_impl(stream, bitmap, options, ColorSpace::CMYK);
}

}
