/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Stream.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CMYKBitmap.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <jpeglib.h>
#include <setjmp.h>

namespace Gfx {

struct JPEGErrorManager : jpeg_error_mgr {
    jmp_buf setjmp_buffer {};
};

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
            dest->free_in_buffer = dest->buffer.size();
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
    // NB: Everything with a destructor is constructed before setjmp(), so that a longjmp() from libjpeg does not
    //     skip any destructors. The guard comes last, so it destroys cinfo while the objects it points to still live.
    //     setjmp() is called in a separate function, as objects changed between setjmp() and longjmp() in the frame
    //     that calls setjmp() have indeterminate values after the longjmp().
    Vector<u8> buffer;
    MemoryDestinationManager dest_manager(buffer);

    // NB: We store CMYK data inverted, as Photoshop does. This is the convention our decoder (and every browser)
    //     expects from CMYK JPEGs with an Adobe marker, which libjpeg writes for YCCK.
    Vector<JSAMPLE> inverted_row;
    if (color_space == ColorSpace::CMYK)
        TRY(inverted_row.try_resize(bitmap.size().width() * 4));

    struct JPEGErrorManager jerr;

    struct jpeg_compress_struct cinfo {};
    ScopeGuard guard { [&]() { jpeg_destroy_compress(&cinfo); } };

    cinfo.err = jpeg_std_error(&jerr);

    jerr.error_exit = [](j_common_ptr cinfo) {
        char buffer[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message)(cinfo, buffer);
        dbgln("JPEG error: {}", buffer);
        longjmp(static_cast<JPEGErrorManager*>(cinfo->err)->setjmp_buffer, 1);
    };

    auto compress = [&]() -> bool {
        if (setjmp(jerr.setjmp_buffer))
            return false;

        jpeg_create_compress(&cinfo);

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
        // NB: libjpeg can only convert CMYK input to YCCK, not to YCbCr.
        jpeg_set_colorspace(&cinfo, color_space == ColorSpace::CMYK ? JCS_YCCK : JCS_YCbCr);
        jpeg_set_quality(&cinfo, options.quality, TRUE);

        jpeg_start_compress(&cinfo, TRUE);

        // NB: libjpeg only accepts markers after jpeg_start_compress() and before the first scanline.
        if (options.icc_data.has_value())
            jpeg_write_icc_profile(&cinfo, options.icc_data->data(), options.icc_data->size());

        while (cinfo.next_scanline < cinfo.image_height) {
            auto const* row_ptr = reinterpret_cast<u8 const*>(bitmap.scanline(cinfo.next_scanline));
            JSAMPROW row_pointer = const_cast<JSAMPROW>(row_ptr);
            if (color_space == ColorSpace::CMYK) {
                for (size_t i = 0; i < inverted_row.size(); ++i)
                    inverted_row[i] = 255 - row_ptr[i];
                row_pointer = inverted_row.data();
            }
            jpeg_write_scanlines(&cinfo, &row_pointer, 1);
        }

        jpeg_finish_compress(&cinfo);
        return true;
    };

    if (!compress())
        return Error::from_string_literal("Failed to encode JPEG");

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
