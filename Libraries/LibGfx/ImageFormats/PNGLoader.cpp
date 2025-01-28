/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibGfx/ImageFormats/ExifOrientedBitmap.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Painter.h>
#include <png.h>

namespace Gfx {

struct PNGLoadingContext {
    ~PNGLoadingContext()
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    }

    png_structp png_ptr { nullptr };
    png_infop info_ptr { nullptr };

    ReadonlyBytes data;
    IntSize size;
    u32 frame_count { 0 };
    u32 loop_count { 0 };
    Vector<ImageFrameDescriptor> frame_descriptors;
    Optional<ByteBuffer> icc_profile;
    OwnPtr<ExifMetadata> exif_metadata;

    ErrorOr<size_t> read_frames(png_structp, png_infop);
    ErrorOr<void> apply_exif_orientation();

    ErrorOr<void> read_all_frames()
    {
        // NOTE: We need to setjmp() here because libpng uses longjmp() for error handling.
        if (auto error_value = setjmp(png_jmpbuf(png_ptr)); error_value) {
            return Error::from_errno(error_value);
        }

        png_read_update_info(png_ptr, info_ptr);

        frame_count = TRY(read_frames(png_ptr, info_ptr));

        if (exif_metadata)
            TRY(apply_exif_orientation());
        return {};
    }
};

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> PNGImageDecoderPlugin::create(ReadonlyBytes bytes)
{
    auto decoder = adopt_own(*new PNGImageDecoderPlugin(bytes));
    TRY(decoder->initialize());

    auto result = decoder->m_context->read_all_frames();
    if (result.is_error()) {
        // NOTE: If we didn't fail in initialize(), that means we have size information.
        //       We can create a single-frame bitmap with that size and return it.
        //       This is weird, but kinda matches the behavior of other browsers.
        auto bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Premultiplied, decoder->m_context->size));
        decoder->m_context->frame_descriptors.append({ move(bitmap), 0 });
        decoder->m_context->frame_count = 1;
        return decoder;
    }

    return decoder;
}

PNGImageDecoderPlugin::PNGImageDecoderPlugin(ReadonlyBytes data)
    : m_context(adopt_own(*new PNGLoadingContext))
{
    m_context->data = data;
}

size_t PNGImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

IntSize PNGImageDecoderPlugin::size()
{
    return m_context->size;
}

bool PNGImageDecoderPlugin::is_animated()
{
    return m_context->frame_count > 1;
}

size_t PNGImageDecoderPlugin::loop_count()
{
    return m_context->loop_count;
}

size_t PNGImageDecoderPlugin::frame_count()
{
    return m_context->frame_count;
}

ErrorOr<ImageFrameDescriptor> PNGImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index >= m_context->frame_descriptors.size())
        return Error::from_errno(EINVAL);

    return m_context->frame_descriptors[index];
}

ErrorOr<Optional<ReadonlyBytes>> PNGImageDecoderPlugin::icc_data()
{
    if (m_context->icc_profile.has_value())
        return Optional<ReadonlyBytes>(*m_context->icc_profile);
    return OptionalNone {};
}

static void log_png_error(png_structp png_ptr, char const* error_message)
{
    dbgln("libpng error: {}", error_message);
    png_longjmp(png_ptr, 1);
}

static void log_png_warning(png_structp, char const* warning_message)
{
    dbgln("libpng warning: {}", warning_message);
}

ErrorOr<void> PNGImageDecoderPlugin::initialize()
{
    m_context->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!m_context->png_ptr)
        return Error::from_string_view("Failed to allocate read struct"sv);

    m_context->info_ptr = png_create_info_struct(m_context->png_ptr);
    if (!m_context->info_ptr) {
        return Error::from_string_view("Failed to allocate info struct"sv);
    }

    if (auto error_value = setjmp(png_jmpbuf(m_context->png_ptr)); error_value) {
        return Error::from_errno(error_value);
    }

    png_set_read_fn(m_context->png_ptr, &m_context->data, [](png_structp png_ptr, png_bytep data, png_size_t length) {
        auto* read_data = reinterpret_cast<ReadonlyBytes*>(png_get_io_ptr(png_ptr));
        if (read_data->size() < length) {
            png_error(png_ptr, "Read error");
            return;
        }
        memcpy(data, read_data->data(), length);
        *read_data = read_data->slice(length);
    });

    png_set_error_fn(m_context->png_ptr, nullptr, log_png_error, log_png_warning);

    png_read_info(m_context->png_ptr, m_context->info_ptr);

    u32 width = 0;
    u32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace_type = 0;
    png_get_IHDR(m_context->png_ptr, m_context->info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, nullptr, nullptr);
    m_context->size = { static_cast<int>(width), static_cast<int>(height) };

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(m_context->png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(m_context->png_ptr);

    if (png_get_valid(m_context->png_ptr, m_context->info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(m_context->png_ptr);

    if (bit_depth == 16)
        png_set_strip_16(m_context->png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(m_context->png_ptr);

    if (interlace_type != PNG_INTERLACE_NONE)
        png_set_interlace_handling(m_context->png_ptr);

    png_set_filler(m_context->png_ptr, 0xFF, PNG_FILLER_AFTER);
    png_set_bgr(m_context->png_ptr);

    char* profile_name = nullptr;
    int compression_type = 0;
    u8* profile_data = nullptr;
    u32 profile_len = 0;
    if (png_get_iCCP(m_context->png_ptr, m_context->info_ptr, &profile_name, &compression_type, &profile_data, &profile_len))
        m_context->icc_profile = TRY(ByteBuffer::copy(profile_data, profile_len));

    u8* exif_data = nullptr;
    u32 exif_length = 0;
    int const num_exif_chunks = png_get_eXIf_1(m_context->png_ptr, m_context->info_ptr, &exif_length, &exif_data);
    if (num_exif_chunks > 0)
        m_context->exif_metadata = TRY(TIFFImageDecoderPlugin::read_exif_metadata({ exif_data, exif_length }));

    return {};
}

ErrorOr<void> PNGLoadingContext::apply_exif_orientation()
{
    auto orientation = exif_metadata->orientation().value_or(TIFF::Orientation::Default);
    if (orientation == TIFF::Orientation::Default)
        return {};

    for (auto& img_frame_descriptor : frame_descriptors) {
        auto& img = img_frame_descriptor.image;
        auto oriented_bmp = TRY(ExifOrientedBitmap::create(orientation, img->size(), img->format()));

        for (int y = 0; y < img->size().height(); ++y) {
            for (int x = 0; x < img->size().width(); ++x) {
                auto pixel = img->get_pixel(x, y);
                oriented_bmp.set_pixel(x, y, pixel.value());
            }
        }

        img_frame_descriptor.image = oriented_bmp.bitmap();
    }

    size = ExifOrientedBitmap::oriented_size(size, orientation);

    return {};
}

ErrorOr<size_t> PNGLoadingContext::read_frames(png_structp png_ptr, png_infop info_ptr)
{
    if (png_get_acTL(png_ptr, info_ptr, &frame_count, &loop_count)) {
        // acTL chunk present: This is an APNG.

        png_set_acTL(png_ptr, info_ptr, frame_count, loop_count);

        // Conceptually, at the beginning of each play the output buffer must be completely initialized to a fully transparent black rectangle, with width and height dimensions from the `IHDR` chunk.
        auto output_buffer = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, size));
        auto painter = Painter::create(output_buffer);
        Vector<u8*> row_pointers;

        for (size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
            png_read_frame_head(png_ptr, info_ptr);
            u32 width = 0;
            u32 height = 0;
            u32 x = 0;
            u32 y = 0;
            u16 delay_num = 0;
            u16 delay_den = 0;
            u8 dispose_op = PNG_DISPOSE_OP_NONE;
            u8 blend_op = PNG_BLEND_OP_SOURCE;

            auto duration_ms = [&]() -> int {
                if (delay_num == 0)
                    return 1;
                u32 const denominator = delay_den != 0 ? static_cast<u32>(delay_den) : 100u;
                auto unsigned_duration_ms = (delay_num * 1000) / denominator;
                if (unsigned_duration_ms > INT_MAX)
                    return INT_MAX;
                return static_cast<int>(unsigned_duration_ms);
            };

            if (png_get_valid(png_ptr, info_ptr, PNG_INFO_fcTL)) {
                png_get_next_frame_fcTL(png_ptr, info_ptr, &width, &height, &x, &y, &delay_num, &delay_den, &dispose_op, &blend_op);
            } else {
                width = png_get_image_width(png_ptr, info_ptr);
                height = png_get_image_height(png_ptr, info_ptr);
            }
            auto frame_rect = FloatRect { x, y, width, height };

            auto decoded_frame_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, IntSize { static_cast<int>(width), static_cast<int>(height) }));
            row_pointers.resize(height);
            for (u32 i = 0; i < height; ++i) {
                row_pointers[i] = decoded_frame_bitmap->scanline_u8(i);
            }
            png_read_image(png_ptr, row_pointers.data());

            RefPtr<Bitmap> prev_output_buffer;
            if (dispose_op == PNG_DISPOSE_OP_PREVIOUS) // Only actually clone if it's necessary
                prev_output_buffer = TRY(output_buffer->clone());

            switch (blend_op) {
            case PNG_BLEND_OP_SOURCE:
                // All color components of the frame, including alpha, overwrite the current contents of the frame's output buffer region.
                painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*decoded_frame_bitmap), decoded_frame_bitmap->rect(), Gfx::ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::Copy);
                break;
            case PNG_BLEND_OP_OVER:
                // The frame should be composited onto the output buffer based on its alpha, using a simple OVER operation as described in the "Alpha Channel Processing" section of the PNG specification.
                painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*decoded_frame_bitmap), decoded_frame_bitmap->rect(), ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::SourceOver);
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            frame_descriptors.append({ TRY(output_buffer->clone()), duration_ms() });

            switch (dispose_op) {
            case PNG_DISPOSE_OP_NONE:
                // No disposal is done on this frame before rendering the next; the contents of the output buffer are left as is.
                break;
            case PNG_DISPOSE_OP_BACKGROUND:
                // The frame's region of the output buffer is to be cleared to fully transparent black before rendering the next frame.
                painter->clear_rect(frame_rect, Gfx::Color::Transparent);
                break;
            case PNG_DISPOSE_OP_PREVIOUS:
                // The frame's region of the output buffer is to be reverted to the previous contents before rendering the next frame.
                painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*prev_output_buffer), IntRect { x, y, width, height }, Gfx::ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::Copy);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }
    } else {
        // This is a single-frame PNG.

        frame_count = 1;
        loop_count = 0;

        auto decoded_frame_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, size));
        Vector<u8*> row_pointers;
        row_pointers.resize(size.height());
        for (int i = 0; i < size.height(); ++i)
            row_pointers[i] = decoded_frame_bitmap->scanline_u8(i);

        png_read_image(png_ptr, row_pointers.data());
        frame_descriptors.append({ move(decoded_frame_bitmap), 0 });
    }
    return this->frame_count;
}

PNGImageDecoderPlugin::~PNGImageDecoderPlugin() = default;

bool PNGImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    auto constexpr png_signature_size_in_bytes = 8;
    if (data.size() < png_signature_size_in_bytes)
        return false;
    return png_sig_cmp(data.data(), 0, png_signature_size_in_bytes) == 0;
}

Optional<Metadata const&> PNGImageDecoderPlugin::metadata()
{
    if (m_context->exif_metadata)
        return *m_context->exif_metadata;
    return OptionalNone {};
}

}
