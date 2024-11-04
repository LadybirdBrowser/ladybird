/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibGfx/DeprecatedPainter.h>
#include <LibGfx/ImageFormats/ExifOrientedBitmap.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
#include <png.h>

namespace Gfx {

struct AnimationFrame {
    RefPtr<Bitmap> bitmap;
    int x_offset { 0 };
    int y_offset { 0 };
    int width { 0 };
    int height { 0 };
    int delay_den { 0 };
    int delay_num { 0 };
    u8 blend_op { 0 };
    u8 dispose_op { 0 };

    AnimationFrame(RefPtr<Bitmap> bitmap, int x_offset, int y_offset, int width, int height, int delay_den, int delay_num, u8 blend_op, u8 dispose_op)
        : bitmap(move(bitmap))
        , x_offset(x_offset)
        , y_offset(y_offset)
        , width(width)
        , height(height)
        , delay_den(delay_den)
        , delay_num(delay_num)
        , blend_op(blend_op)
        , dispose_op(dispose_op)
    {
    }

    [[nodiscard]] int duration_ms() const
    {
        if (delay_num == 0)
            return 1;
        u32 const denominator = delay_den != 0 ? static_cast<u32>(delay_den) : 100u;
        auto unsigned_duration_ms = (delay_num * 1000) / denominator;
        if (unsigned_duration_ms > INT_MAX)
            return INT_MAX;
        return static_cast<int>(unsigned_duration_ms);
    }

    [[nodiscard]] IntRect rect() const { return { x_offset, y_offset, width, height }; }
};

struct PNGLoadingContext {
    ReadonlyBytes data;
    IntSize size;
    u32 frame_count { 0 };
    u32 loop_count { 0 };
    Vector<ImageFrameDescriptor> frame_descriptors;
    Optional<ByteBuffer> icc_profile;
    OwnPtr<ExifMetadata> exif_metadata;

    Vector<AnimationFrame> animation_frames;
    Vector<u8*> row_pointers;
    Vector<u8> image_data;
    RefPtr<Gfx::Bitmap> decoded_frame_bitmap;

    ErrorOr<size_t> read_frames(png_structp, png_infop);
    ErrorOr<void> apply_exif_orientation();
};

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> PNGImageDecoderPlugin::create(ReadonlyBytes bytes)
{
    auto decoder = adopt_own(*new PNGImageDecoderPlugin(bytes));
    TRY(decoder->initialize());
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

ErrorOr<void> PNGImageDecoderPlugin::initialize()
{
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
        return Error::from_string_view("Failed to allocate read struct"sv);

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        return Error::from_string_view("Failed to allocate info struct"sv);
    }

    if (auto error_value = setjmp(png_jmpbuf(png_ptr)); error_value) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        return Error::from_errno(error_value);
    }

    png_set_read_fn(png_ptr, &m_context->data, [](png_structp png_ptr, png_bytep data, png_size_t length) {
        auto* read_data = reinterpret_cast<ReadonlyBytes*>(png_get_io_ptr(png_ptr));
        if (read_data->size() < length) {
            png_error(png_ptr, "Read error");
            return;
        }
        memcpy(data, read_data->data(), length);
        *read_data = read_data->slice(length);
    });

    png_read_info(png_ptr, info_ptr);

    u32 width = 0;
    u32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);
    m_context->size = { static_cast<int>(width), static_cast<int>(height) };

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

    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    png_set_bgr(png_ptr);

    char* profile_name = nullptr;
    int compression_type = 0;
    u8* profile_data = nullptr;
    u32 profile_len = 0;
    if (png_get_iCCP(png_ptr, info_ptr, &profile_name, &compression_type, &profile_data, &profile_len))
        m_context->icc_profile = TRY(ByteBuffer::copy(profile_data, profile_len));

    png_read_update_info(png_ptr, info_ptr);
    m_context->frame_count = TRY(m_context->read_frames(png_ptr, info_ptr));

    u8* exif_data = nullptr;
    u32 exif_length = 0;
    int const num_exif_chunks = png_get_eXIf_1(png_ptr, info_ptr, &exif_length, &exif_data);
    if (num_exif_chunks > 0)
        m_context->exif_metadata = TRY(TIFFImageDecoderPlugin::read_exif_metadata({ exif_data, exif_length }));

    if (m_context->exif_metadata)
        TRY(m_context->apply_exif_orientation());

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
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

static ErrorOr<NonnullRefPtr<Bitmap>> render_animation_frame(AnimationFrame const& prev_animation_frame, AnimationFrame const& animation_frame, Bitmap const& decoded_frame_bitmap)
{
    auto rendered_bitmap = TRY(prev_animation_frame.bitmap->clone());
    DeprecatedPainter painter(rendered_bitmap);

    auto frame_rect = animation_frame.rect();
    switch (prev_animation_frame.dispose_op) {
    case PNG_DISPOSE_OP_BACKGROUND:
        painter.clear_rect(rendered_bitmap->rect(), Color::NamedColor::Transparent);
        break;
    case PNG_DISPOSE_OP_PREVIOUS:
        painter.blit(frame_rect.location(), decoded_frame_bitmap, frame_rect, 1.0f, false);
        break;
    default:
        break;
    }
    switch (animation_frame.blend_op) {
    case PNG_BLEND_OP_SOURCE:
        painter.blit(frame_rect.location(), decoded_frame_bitmap, decoded_frame_bitmap.rect(), 1.0f, false);
        break;
    case PNG_BLEND_OP_OVER:
        painter.blit(frame_rect.location(), decoded_frame_bitmap, decoded_frame_bitmap.rect(), 1.0f, true);
        break;
    default:
        break;
    }
    return rendered_bitmap;
}

ErrorOr<size_t> PNGLoadingContext::read_frames(png_structp png_ptr, png_infop info_ptr)
{
    if (png_get_acTL(png_ptr, info_ptr, &frame_count, &loop_count)) {
        // acTL chunk present: This is an APNG.

        png_set_acTL(png_ptr, info_ptr, frame_count, loop_count);

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

            if (png_get_valid(png_ptr, info_ptr, PNG_INFO_fcTL)) {
                png_get_next_frame_fcTL(png_ptr, info_ptr, &width, &height, &x, &y, &delay_num, &delay_den, &dispose_op, &blend_op);
            } else {
                width = png_get_image_width(png_ptr, info_ptr);
                height = png_get_image_height(png_ptr, info_ptr);
            }

            decoded_frame_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, IntSize { static_cast<int>(width), static_cast<int>(height) }));

            row_pointers.resize(height);
            for (u32 i = 0; i < height; ++i) {
                row_pointers[i] = decoded_frame_bitmap->scanline_u8(i);
            }

            png_read_image(png_ptr, row_pointers.data());

            auto animation_frame = AnimationFrame(nullptr, x, y, width, height, delay_den, delay_num, blend_op, dispose_op);

            if (frame_index == 0) {
                animation_frame.bitmap = decoded_frame_bitmap;
                frame_descriptors.append({ decoded_frame_bitmap, animation_frame.duration_ms() });
            } else {
                animation_frame.bitmap = TRY(render_animation_frame(animation_frames.last(), animation_frame, *decoded_frame_bitmap));
                frame_descriptors.append({ animation_frame.bitmap, animation_frame.duration_ms() });
            }
            animation_frames.append(move(animation_frame));
        }
    } else {
        // This is a single-frame PNG.

        frame_count = 1;
        loop_count = 0;

        decoded_frame_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, size));
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
