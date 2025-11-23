/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkSurface.h>

namespace Gfx {

StringView export_format_name(ExportFormat format)
{
    switch (format) {
#define ENUMERATE_EXPORT_FORMAT(format) \
    case Gfx::ExportFormat::format:     \
        return #format##sv;
        ENUMERATE_EXPORT_FORMATS(ENUMERATE_EXPORT_FORMAT)
#undef ENUMERATE_EXPORT_FORMAT
    }
    VERIFY_NOT_REACHED();
}

struct ImmutableBitmapImpl {
    sk_sp<SkImage> sk_image;
    SkBitmap sk_bitmap;
    Variant<NonnullRefPtr<Gfx::Bitmap>, NonnullRefPtr<Gfx::PaintingSurface>, Empty> source;
    ColorSpace color_space;
};

int ImmutableBitmap::width() const
{
    return m_impl->sk_image->width();
}

int ImmutableBitmap::height() const
{
    return m_impl->sk_image->height();
}

IntRect ImmutableBitmap::rect() const
{
    return { {}, size() };
}

IntSize ImmutableBitmap::size() const
{
    return { width(), height() };
}

AlphaType ImmutableBitmap::alpha_type() const
{
    // We assume premultiplied alpha type for opaque surfaces since that is Skia's preferred alpha type and the
    // effective pixel data is identical between premultiplied and unpremultiplied in that case.
    return m_impl->sk_image->alphaType() == kUnpremul_SkAlphaType ? AlphaType::Unpremultiplied : AlphaType::Premultiplied;
}

SkImage const* ImmutableBitmap::sk_image() const
{
    return m_impl->sk_image.get();
}

static int bytes_per_pixel_for_export_format(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
    case ExportFormat::Alpha8:
        return 1;
    case ExportFormat::RGB565:
    case ExportFormat::RGBA5551:
    case ExportFormat::RGBA4444:
        return 2;
    case ExportFormat::RGB888:
        return 3;
    case ExportFormat::RGBA8888:
        return 4;
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkColorType export_format_to_skia_color_type(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
        return SkColorType::kGray_8_SkColorType;
    case ExportFormat::Alpha8:
        return SkColorType::kAlpha_8_SkColorType;
    case ExportFormat::RGB565:
        return SkColorType::kRGB_565_SkColorType;
    case ExportFormat::RGBA5551:
        dbgln("FIXME: Support conversion to RGBA5551.");
        return SkColorType::kUnknown_SkColorType;
    case ExportFormat::RGBA4444:
        return SkColorType::kARGB_4444_SkColorType;
    case ExportFormat::RGB888:
        // This one needs to be converted manually because Skia has no valid 24-bit color type.
        VERIFY_NOT_REACHED();
    case ExportFormat::RGBA8888:
        return SkColorType::kRGBA_8888_SkColorType;
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<BitmapExportResult> ImmutableBitmap::export_to_byte_buffer(ExportFormat format, int flags, Optional<int> target_width, Optional<int> target_height) const
{
    int width = target_width.value_or(this->width());
    int height = target_height.value_or(this->height());

    if (format == ExportFormat::RGB888 && (width != this->width() || height != this->height())) {
        dbgln("FIXME: Ignoring target width and height because scaling is not implemented for this export format.");
        width = this->width();
        height = this->height();
    }

    Checked<size_t> buffer_pitch = width;
    int number_of_bytes = bytes_per_pixel_for_export_format(format);
    buffer_pitch *= number_of_bytes;
    if (buffer_pitch.has_overflow())
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    if (width > 0 && height > 0) {
        if (format == ExportFormat::RGB888) {
            // 24 bit RGB is not supported by Skia, so we need to handle this format ourselves.
            auto raw_buffer = buffer.data();
            for (auto y = 0; y < height; y++) {
                auto target_y = flags & ExportFlags::FlipY ? height - y - 1 : y;
                for (auto x = 0; x < width; x++) {
                    auto pixel = get_pixel(x, y);
                    auto buffer_offset = (target_y * buffer_pitch.value()) + (x * 3ull);
                    raw_buffer[buffer_offset + 0] = pixel.red();
                    raw_buffer[buffer_offset + 1] = pixel.green();
                    raw_buffer[buffer_offset + 2] = pixel.blue();
                }
            }
        } else {
            auto skia_format = export_format_to_skia_color_type(format);
            auto color_space = SkColorSpace::MakeSRGB();

            auto image_info = SkImageInfo::Make(width, height, skia_format, flags & ExportFlags::PremultiplyAlpha ? SkAlphaType::kPremul_SkAlphaType : SkAlphaType::kUnpremul_SkAlphaType, color_space);
            auto surface = SkSurfaces::WrapPixels(image_info, buffer.data(), buffer_pitch.value());
            VERIFY(surface);
            auto* surface_canvas = surface->getCanvas();
            auto dst_rect = Gfx::to_skia_rect(Gfx::Rect { 0, 0, width, height });

            if (flags & ExportFlags::FlipY) {
                surface_canvas->translate(0, dst_rect.height());
                surface_canvas->scale(1, -1);
            }

            surface_canvas->drawImageRect(sk_image(), dst_rect, Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor));
        }
    } else {
        VERIFY(buffer.is_empty());
    }

    return BitmapExportResult {
        .buffer = move(buffer),
        .width = width,
        .height = height,
    };
}

RefPtr<Gfx::Bitmap const> ImmutableBitmap::bitmap() const
{
    // FIXME: Implement for PaintingSurface
    return m_impl->source.get<NonnullRefPtr<Gfx::Bitmap>>();
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    // FIXME: Implement for PaintingSurface
    return m_impl->source.get<NonnullRefPtr<Gfx::Bitmap>>()->get_pixel(x, y);
}

static SkAlphaType to_skia_alpha_type(Gfx::AlphaType alpha_type)
{
    switch (alpha_type) {
    case AlphaType::Premultiplied:
        return kPremul_SkAlphaType;
    case AlphaType::Unpremultiplied:
        return kUnpremul_SkAlphaType;
    default:
        VERIFY_NOT_REACHED();
    }
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space)
{
    ImmutableBitmapImpl impl;
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->alpha_type()), color_space.color_space<sk_sp<SkColorSpace>>());
    impl.sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(bitmap->scanline(0))), bitmap->pitch());
    impl.sk_bitmap.setImmutable();
    impl.sk_image = impl.sk_bitmap.asImage();
    impl.source = bitmap;
    impl.color_space = move(color_space);
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, AlphaType alpha_type, ColorSpace color_space)
{
    // Convert the source bitmap to the right alpha type on a mismatch. We want to do this when converting from a
    // Bitmap to an ImmutableBitmap, since at that point we usually know the right alpha type to use in context.
    auto source_bitmap = bitmap;
    if (source_bitmap->alpha_type() != alpha_type) {
        source_bitmap = MUST(bitmap->clone());
        source_bitmap->set_alpha_type_destructive(alpha_type);
    }

    return create(source_bitmap, move(color_space));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface> painting_surface)
{
    ImmutableBitmapImpl impl;
    impl.sk_image = painting_surface->sk_image_snapshot<sk_sp<SkImage>>();
    impl.source = painting_surface;
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

ImmutableBitmap::ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> impl)
    : m_impl(move(impl))
{
}

ImmutableBitmap::~ImmutableBitmap() = default;

}
