/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/CMYKBitmap.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <jpeglib.h>
#include <setjmp.h>

namespace Gfx {

struct JPEGLoadingContext {
    enum class State {
        NotDecoded,
        Error,
        Decoded,
    };

    State state { State::NotDecoded };

    RefPtr<Gfx::Bitmap> rgb_bitmap;
    RefPtr<Gfx::CMYKBitmap> cmyk_bitmap;

    ReadonlyBytes data;
    Vector<u8> icc_data;

    JPEGLoadingContext(ReadonlyBytes data)
        : data(data)
    {
    }

    ErrorOr<void> decode();
};

struct JPEGErrorManager : jpeg_error_mgr {
    jmp_buf setjmp_buffer {};
};

ErrorOr<void> JPEGLoadingContext::decode()
{
    struct jpeg_decompress_struct cinfo;
    ScopeGuard guard { [&]() { jpeg_destroy_decompress(&cinfo); } };

    struct JPEGErrorManager jerr;
    cinfo.err = jpeg_std_error(&jerr);

    jpeg_source_mgr source_manager {};

    if (setjmp(jerr.setjmp_buffer))
        return Error::from_string_literal("Failed to decode JPEG");

    jerr.error_exit = [](j_common_ptr cinfo) {
        char buffer[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message)(cinfo, buffer);
        dbgln("JPEG error: {}", buffer);
        longjmp(static_cast<JPEGErrorManager*>(cinfo->err)->setjmp_buffer, 1);
    };

    jpeg_create_decompress(&cinfo);

    source_manager.next_input_byte = data.data();
    source_manager.bytes_in_buffer = data.size();
    source_manager.init_source = [](j_decompress_ptr) { };
    source_manager.fill_input_buffer = [](j_decompress_ptr) -> boolean { return false; };
    source_manager.skip_input_data = [](j_decompress_ptr context, long num_bytes) {
        if (num_bytes > static_cast<long>(context->src->bytes_in_buffer)) {
            context->src->bytes_in_buffer = 0;
            return;
        }
        context->src->next_input_byte += num_bytes;
        context->src->bytes_in_buffer -= num_bytes;
    };
    source_manager.resync_to_restart = jpeg_resync_to_restart;
    source_manager.term_source = [](j_decompress_ptr) { };

    cinfo.src = &source_manager;

    jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
        return Error::from_string_literal("Failed to read JPEG header");

    if (cinfo.jpeg_color_space == JCS_CMYK) {
        cinfo.out_color_space = JCS_CMYK;
    } else if (cinfo.jpeg_color_space == JCS_YCCK) {
        cinfo.out_color_space = JCS_YCCK;
    } else {
        cinfo.out_color_space = JCS_EXT_BGRX;
    }

    jpeg_start_decompress(&cinfo);
    bool could_read_all_scanlines = true;

    if (cinfo.out_color_space == JCS_EXT_BGRX) {
        rgb_bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { static_cast<int>(cinfo.output_width), static_cast<int>(cinfo.output_height) }));
        while (cinfo.output_scanline < cinfo.output_height) {
            auto* row_ptr = (u8*)rgb_bitmap->scanline(cinfo.output_scanline);
            auto out_size = jpeg_read_scanlines(&cinfo, &row_ptr, 1);
            if (cinfo.output_scanline < cinfo.output_height && out_size == 0) {
                dbgln("JPEG Warning: Decoding produced no more scanlines in scanline {}/{}.", cinfo.output_scanline, cinfo.output_height);
                could_read_all_scanlines = false;
                break;
            }
        }
    } else {
        cmyk_bitmap = TRY(CMYKBitmap::create_with_size({ static_cast<int>(cinfo.output_width), static_cast<int>(cinfo.output_height) }));
        while (cinfo.output_scanline < cinfo.output_height) {
            auto* row_ptr = (u8*)cmyk_bitmap->scanline(cinfo.output_scanline);
            auto out_size = jpeg_read_scanlines(&cinfo, &row_ptr, 1);
            if (cinfo.output_scanline < cinfo.output_height && out_size == 0) {
                dbgln("JPEG Warning: Decoding produced no more scanlines in scanline {}/{}.", cinfo.output_scanline, cinfo.output_height);
                could_read_all_scanlines = false;
                break;
            }
        }

        // If image is in YCCK color space, we convert it to CMYK
        // and then CMYK code path will handle the rest
        if (cinfo.out_color_space == JCS_YCCK) {
            for (int i = 0; i < cmyk_bitmap->size().height(); ++i) {
                for (int j = 0; j < cmyk_bitmap->size().width(); ++j) {
                    auto const& cmyk = cmyk_bitmap->scanline(i)[j];

                    auto y = cmyk.c;
                    auto cb = cmyk.m;
                    auto cr = cmyk.y;
                    auto k = cmyk.k;

                    int r = y + 1.402f * (cr - 128);
                    int g = y - 0.3441f * (cb - 128) - 0.7141f * (cr - 128);
                    int b = y + 1.772f * (cb - 128);

                    y = clamp(r, 0, 255);
                    cb = clamp(g, 0, 255);
                    cr = clamp(b, 0, 255);
                    k = 255 - k;

                    cmyk_bitmap->scanline(i)[j] = {
                        y,
                        cb,
                        cr,
                        k,
                    };
                }
            }
        }
    }

    JOCTET* icc_data_ptr = nullptr;
    unsigned int icc_data_length = 0;
    if (jpeg_read_icc_profile(&cinfo, &icc_data_ptr, &icc_data_length)) {
        icc_data.resize(icc_data_length);
        memcpy(icc_data.data(), icc_data_ptr, icc_data_length);
        free(icc_data_ptr);
    }

    if (could_read_all_scanlines)
        jpeg_finish_decompress(&cinfo);
    else
        jpeg_abort_decompress(&cinfo);

    if (cmyk_bitmap && !rgb_bitmap)
        rgb_bitmap = TRY(cmyk_bitmap->to_low_quality_rgb());

    return {};
}

JPEGImageDecoderPlugin::JPEGImageDecoderPlugin(NonnullOwnPtr<JPEGLoadingContext> context)
    : m_context(move(context))
{
}

JPEGImageDecoderPlugin::~JPEGImageDecoderPlugin() = default;

IntSize JPEGImageDecoderPlugin::size()
{
    if (m_context->state == JPEGLoadingContext::State::NotDecoded)
        (void)frame(0);

    if (m_context->state == JPEGLoadingContext::State::Error)
        return {};
    if (m_context->rgb_bitmap)
        return m_context->rgb_bitmap->size();
    if (m_context->cmyk_bitmap)
        return m_context->cmyk_bitmap->size();
    return {};
}

bool JPEGImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return data.size() > 3
        && data.data()[0] == 0xFF
        && data.data()[1] == 0xD8
        && data.data()[2] == 0xFF;
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JPEGImageDecoderPlugin::create(ReadonlyBytes data)
{
    return adopt_own(*new JPEGImageDecoderPlugin(make<JPEGLoadingContext>(data)));
}

ErrorOr<ImageFrameDescriptor> JPEGImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("JPEGImageDecoderPlugin: Invalid frame index");

    if (m_context->state == JPEGLoadingContext::State::Error)
        return Error::from_string_literal("JPEGImageDecoderPlugin: Decoding failed");

    if (m_context->state < JPEGLoadingContext::State::Decoded) {
        if (auto result = m_context->decode(); result.is_error()) {
            m_context->state = JPEGLoadingContext::State::Error;
            return result.release_error();
        }

        m_context->state = JPEGLoadingContext::State::Decoded;
    }

    return ImageFrameDescriptor { m_context->rgb_bitmap, 0 };
}

Optional<Metadata const&> JPEGImageDecoderPlugin::metadata()
{
    return OptionalNone {};
}

ErrorOr<Optional<ReadonlyBytes>> JPEGImageDecoderPlugin::icc_data()
{
    if (m_context->state == JPEGLoadingContext::State::NotDecoded)
        (void)frame(0);

    if (!m_context->icc_data.is_empty())
        return m_context->icc_data;
    return OptionalNone {};
}

NaturalFrameFormat JPEGImageDecoderPlugin::natural_frame_format() const
{
    if (m_context->state == JPEGLoadingContext::State::NotDecoded)
        (void)const_cast<JPEGImageDecoderPlugin&>(*this).frame(0);

    if (m_context->cmyk_bitmap)
        return NaturalFrameFormat::CMYK;
    return NaturalFrameFormat::RGB;
}

ErrorOr<NonnullRefPtr<CMYKBitmap>> JPEGImageDecoderPlugin::cmyk_frame()
{
    if (m_context->state == JPEGLoadingContext::State::NotDecoded)
        (void)frame(0);

    if (m_context->state == JPEGLoadingContext::State::Error)
        return Error::from_string_literal("JPEGImageDecoderPlugin: Decoding failed");
    if (!m_context->cmyk_bitmap)
        return Error::from_string_literal("JPEGImageDecoderPlugin: No CMYK data available");
    return *m_context->cmyk_bitmap;
}

}
