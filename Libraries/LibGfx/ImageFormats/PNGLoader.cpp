/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibGfx/ImageFormats/ExifOrientedBitmap.h>
#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/PNGLoader.h>

#include "ImageDecoderStream.h"

#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Painter.h>
#include <png.h>

namespace Gfx {

struct PNGLoadingContext {
    PNGLoadingContext(NonnullRefPtr<ImageDecoderStream> stream)
        : stream(move(stream))
    {
    }

    ~PNGLoadingContext()
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    }

    struct CurrentFrameInfo {
        u32 width { 0 };
        u32 height { 0 };
        u32 x { 0 };
        u32 y { 0 };
        u16 delay_num { 0 };
        u16 delay_den { 0 };
        u8 dispose_op { PNG_DISPOSE_OP_NONE };
        u8 blend_op { PNG_BLEND_OP_SOURCE };
        RefPtr<Bitmap> bitmap;
    };

    png_structp png_ptr { nullptr };
    png_infop info_ptr { nullptr };

    NonnullRefPtr<ImageDecoderStream> stream;
    IntSize size;
    u32 loop_count { 0 };
    Vector<ImageFrameDescriptor> frame_descriptors;
    Optional<Media::CodingIndependentCodePoints> cicp;
    Optional<ByteBuffer> icc_profile;
    OwnPtr<ExifMetadata> exif_metadata;
    CurrentFrameInfo current_frame_info {};
    RefPtr<Bitmap> animation_output_buffer;
    OwnPtr<Painter> animation_painter;
    bool read_info { false };
    bool reached_end { false };

    ErrorOr<void> read_frames();
    ErrorOr<void> apply_exif_orientation();

    ErrorOr<void> read_all_frames()
    {
        // NOTE: We need to setjmp() here because libpng uses longjmp() for error handling.
        if (auto error_value = setjmp(png_jmpbuf(png_ptr)); error_value) {
            return Error::from_errno(error_value);
        }

        TRY(read_frames());

        if (exif_metadata)
            TRY(apply_exif_orientation());
        return {};
    }
};

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> PNGImageDecoderPlugin::create(NonnullRefPtr<ImageDecoderStream> stream)
{
    auto decoder = adopt_own(*new PNGImageDecoderPlugin(move(stream)));
    TRY(decoder->initialize());

    auto result = decoder->m_context->read_all_frames();
    if (result.is_error()) {
        if (!decoder->m_context->read_info)
            return result.release_error();

        // NOTE: We can create a single-frame bitmap with the decoded size and return it.
        //       This is weird, but kinda matches the behavior of other browsers.
        auto bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Premultiplied, decoder->m_context->size));
        decoder->m_context->frame_descriptors.append({ move(bitmap), 0 });
        return decoder;
    }

    return decoder;
}

PNGImageDecoderPlugin::PNGImageDecoderPlugin(NonnullRefPtr<ImageDecoderStream> stream)
    : m_context(adopt_own(*new PNGLoadingContext(move(stream))))
{
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
    return m_context->frame_descriptors.size() > 1;
}

size_t PNGImageDecoderPlugin::loop_count()
{
    return m_context->loop_count;
}

size_t PNGImageDecoderPlugin::frame_count()
{
    return m_context->frame_descriptors.size();
}

ErrorOr<ImageFrameDescriptor> PNGImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index >= m_context->frame_descriptors.size())
        return Error::from_errno(EINVAL);

    return m_context->frame_descriptors[index];
}

ErrorOr<Optional<Media::CodingIndependentCodePoints>> PNGImageDecoderPlugin::cicp()
{
    return m_context->cicp;
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

static void png_frame_info_callback(png_structp png_ptr, png_uint_32)
{
    auto* context = static_cast<PNGLoadingContext*>(png_get_progressive_ptr(png_ptr));

    context->current_frame_info = {};

    auto& current_frame_info = context->current_frame_info;
    png_get_next_frame_fcTL(png_ptr,
        context->info_ptr,
        &current_frame_info.width,
        &current_frame_info.height,
        &current_frame_info.x,
        &current_frame_info.y,
        &current_frame_info.delay_num,
        &current_frame_info.delay_den,
        &current_frame_info.dispose_op,
        &current_frame_info.blend_op);

    auto maybe_error = Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, { current_frame_info.width, current_frame_info.height });
    if (maybe_error.is_error())
        png_error(png_ptr, "Failed to allocate bitmap for animation frame");

    auto bitmap = maybe_error.release_value();
    current_frame_info.bitmap = bitmap;
}

static void png_frame_end_callback(png_structp png_ptr, png_uint_32 frame_number)
{
    auto* context = static_cast<PNGLoadingContext*>(png_get_progressive_ptr(png_ptr));
    if (frame_number == 0 && png_get_first_frame_is_hidden(png_ptr, context->info_ptr))
        return;

    auto& current_frame_info = context->current_frame_info;

    auto duration_ms = [&]() -> int {
        if (current_frame_info.delay_num == 0)
            return 1;
        u32 const denominator = current_frame_info.delay_den != 0 ? static_cast<u32>(current_frame_info.delay_den) : 100u;
        auto unsigned_duration_ms = (current_frame_info.delay_num * 1000) / denominator;
        if (unsigned_duration_ms > INT_MAX)
            return INT_MAX;
        return static_cast<int>(unsigned_duration_ms);
    };

    auto frame_rect = FloatRect { current_frame_info.x, current_frame_info.y, current_frame_info.width, current_frame_info.height };

    RefPtr<Bitmap> prev_output_buffer;
    // Only actually clone if it's necessary
    if (current_frame_info.dispose_op == PNG_DISPOSE_OP_PREVIOUS) {
        auto maybe_error = context->animation_output_buffer->clone();
        if (maybe_error.is_error())
            png_error(png_ptr, "Failed to clone output buffer");

        prev_output_buffer = maybe_error.release_value();
    }

    switch (current_frame_info.blend_op) {
    case PNG_BLEND_OP_SOURCE:
        // All color components of the frame, including alpha, overwrite the current contents of the frame's output buffer region.
        context->animation_painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*current_frame_info.bitmap), current_frame_info.bitmap->rect(), Gfx::ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::Copy);
        break;
    case PNG_BLEND_OP_OVER:
        // The frame should be composited onto the output buffer based on its alpha, using a simple OVER operation as described in the "Alpha Channel Processing" section of the PNG specification.
        context->animation_painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*current_frame_info.bitmap), current_frame_info.bitmap->rect(), ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::SourceOver);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto maybe_error = context->animation_output_buffer->clone();
    if (maybe_error.is_error())
        png_error(png_ptr, "Failed to clone output buffer");

    context->frame_descriptors.append({ maybe_error.release_value(), duration_ms() });

    switch (current_frame_info.dispose_op) {
    case PNG_DISPOSE_OP_NONE:
        // No disposal is done on this frame before rendering the next; the contents of the output buffer are left as is.
        break;
    case PNG_DISPOSE_OP_BACKGROUND:
        // The frame's region of the output buffer is to be cleared to fully transparent black before rendering the next frame.
        context->animation_painter->clear_rect(frame_rect, Gfx::Color::Transparent);
        break;
    case PNG_DISPOSE_OP_PREVIOUS:
        // The frame's region of the output buffer is to be reverted to the previous contents before rendering the next frame.
        context->animation_painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*prev_output_buffer), IntRect { current_frame_info.x, current_frame_info.y, current_frame_info.width, current_frame_info.height }, Gfx::ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::Copy);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

static void png_info_callback(png_structp png_ptr, png_infop info_ptr)
{
    auto* context = static_cast<PNGLoadingContext*>(png_get_progressive_ptr(png_ptr));

    u32 width = 0;
    u32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, nullptr, nullptr);
    context->size = { static_cast<int>(width), static_cast<int>(height) };

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);

    if (bit_depth == 16)
        png_set_strip_16(png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    if (interlace_type != PNG_INTERLACE_NONE)
        png_set_interlace_handling(png_ptr);

    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    png_set_bgr(png_ptr);

    png_byte color_primaries { 0 };
    png_byte transfer_function { 0 };
    png_byte matrix_coefficients { 0 };
    png_byte video_full_range_flag { 0 };
    if (png_get_cICP(png_ptr, info_ptr, &color_primaries, &transfer_function, &matrix_coefficients, &video_full_range_flag)) {
        Media::ColorPrimaries cp { color_primaries };
        Media::TransferCharacteristics tc { transfer_function };
        Media::MatrixCoefficients mc { matrix_coefficients };
        Media::VideoFullRangeFlag rf { video_full_range_flag };
        context->cicp = Media::CodingIndependentCodePoints { cp, tc, mc, rf };
    } else {
        char* profile_name = nullptr;
        int compression_type = 0;
        u8* profile_data = nullptr;
        u32 profile_len = 0;
        if (png_get_iCCP(png_ptr, info_ptr, &profile_name, &compression_type, &profile_data, &profile_len)) {
            auto maybe_error = ByteBuffer::copy(profile_data, profile_len);
            if (maybe_error.is_error())
                png_error(png_ptr, "Failed to allocate memory for ICC profile");

            context->icc_profile = maybe_error.release_value();
        }
    }

    u8* exif_data = nullptr;
    u32 exif_length = 0;
    int const num_exif_chunks = png_get_eXIf_1(png_ptr, info_ptr, &exif_length, &exif_data);
    if (num_exif_chunks > 0) {
        auto maybe_error = ByteBuffer::copy(exif_data, exif_length);
        if (maybe_error.is_error())
            png_error(png_ptr, "Failed to allocate memory for Exif metadata");

        auto stream = adopt_ref(*new ImageDecoderStream());
        stream->append_chunk(maybe_error.release_value());
        stream->close();

        auto maybe_read_error = TIFFImageDecoderPlugin::read_exif_metadata(move(stream));
        if (maybe_read_error.is_error())
            png_error(png_ptr, "Failed to read Exif metadata");

        context->exif_metadata = maybe_read_error.release_value();
    }

    bool has_acTL = png_get_valid(png_ptr, info_ptr, PNG_INFO_acTL);
    if (has_acTL) {
        // acTL chunk present: This is an APNG.
        context->loop_count = png_get_num_plays(png_ptr, info_ptr);

        auto maybe_error = Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, context->size);
        if (maybe_error.is_error())
            png_error(png_ptr, "Failed to allocate bitmap for animation painter");

        context->animation_output_buffer = maybe_error.release_value();
        context->animation_painter = Painter::create(*context->animation_output_buffer);

        png_set_progressive_frame_fn(png_ptr, png_frame_info_callback, png_frame_end_callback);

        auto& current_frame_info = context->current_frame_info;
        context->current_frame_info = {};

        if (!png_get_first_frame_is_hidden(png_ptr, info_ptr)) {
            png_get_next_frame_fcTL(png_ptr,
                context->info_ptr,
                &current_frame_info.width,
                &current_frame_info.height,
                &current_frame_info.x,
                &current_frame_info.y,
                &current_frame_info.delay_num,
                &current_frame_info.delay_den,
                &current_frame_info.dispose_op,
                &current_frame_info.blend_op);
        } else {
            current_frame_info.width = width;
            current_frame_info.height = height;
        }

        auto maybe_bitmap_error = Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, { current_frame_info.width, current_frame_info.height });
        if (maybe_error.is_error())
            png_error(png_ptr, "Failed to allocate bitmap for first animation frame");

        current_frame_info.bitmap = maybe_bitmap_error.release_value();
    } else {
        // This is a single-frame PNG.
        context->loop_count = 0;

        auto maybe_error = Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, context->size);
        if (maybe_error.is_error())
            png_error(png_ptr, "Failed to allocate bitmap for single frame");

        auto bitmap = maybe_error.release_value();
        context->current_frame_info = PNGLoadingContext::CurrentFrameInfo {
            .width = width,
            .height = height,
            .bitmap = bitmap,
        };
        context->frame_descriptors.append({ move(bitmap), 0 });
    }

    png_read_update_info(png_ptr, info_ptr);
    context->read_info = true;
}

static void png_row_callback(png_structp png_ptr, png_bytep new_row, png_uint_32 row_number, int /* pass */)
{
    // This is nullptr if the row hasn't changed from a previous pass. In that case, we don't have to do anything.
    if (!new_row)
        return;

    auto* context = static_cast<PNGLoadingContext*>(png_get_progressive_ptr(png_ptr));
    png_progressive_combine_row(png_ptr, context->current_frame_info.bitmap->scanline_u8(row_number), new_row);
}

static void png_end_callback(png_structp png_ptr, png_infop)
{
    auto* context = static_cast<PNGLoadingContext*>(png_get_progressive_ptr(png_ptr));
    context->reached_end = true;
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

    png_set_error_fn(m_context->png_ptr, nullptr, log_png_error, log_png_warning);
    png_set_progressive_read_fn(m_context->png_ptr, m_context.ptr(), png_info_callback, png_row_callback, png_end_callback);
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

ErrorOr<void> PNGLoadingContext::read_frames()
{
    constexpr size_t READ_BUFFER_SIZE = 4 * KiB;
    Array<u8, READ_BUFFER_SIZE> read_buffer;

    while (!reached_end) {
        auto bytes = TRY(stream->read_some(read_buffer));
        if (bytes.is_empty())
            break;

        png_process_data(png_ptr, info_ptr, bytes.data(), bytes.size());
    }

    // If we didn't find any valid animation frames with fcTL chunks, fall back to using the base IDAT data as a single frame.
    if (frame_descriptors.is_empty() && current_frame_info.bitmap)
        frame_descriptors.append({ *current_frame_info.bitmap, 0 });

    return {};
}

PNGImageDecoderPlugin::~PNGImageDecoderPlugin() = default;

bool PNGImageDecoderPlugin::sniff(NonnullRefPtr<ImageDecoderStream> stream)
{
    auto constexpr png_signature_size_in_bytes = 8;
    Array<u8, png_signature_size_in_bytes> png_signature;
    auto maybe_error = stream->read_until_filled(png_signature);
    if (maybe_error.is_error())
        return false;

    return png_sig_cmp(png_signature.data(), 0, png_signature_size_in_bytes) == 0;
}

Optional<Metadata const&> PNGImageDecoderPlugin::metadata()
{
    if (m_context->exif_metadata)
        return *m_context->exif_metadata;
    return OptionalNone {};
}

}
