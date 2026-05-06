/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/BitmapExport.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkSurface.h>

namespace Gfx {

static int bytes_per_pixel_for_export_format(BitmapFormat format)
{
    switch (format) {
    case BitmapFormat::Gray8:
    case BitmapFormat::Alpha8:
        return 1;
    case BitmapFormat::RGB565:
    case BitmapFormat::RGBA5551:
    case BitmapFormat::RGBA4444:
        return 2;
    case BitmapFormat::RGB888:
        return 3;
    case BitmapFormat::BGRA8888:
    case BitmapFormat::BGRx8888:
    case BitmapFormat::RGBA8888:
    case BitmapFormat::RGBx8888:
        return 4;
    case BitmapFormat::RGBAF16:
        return 8;
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkColorType export_format_to_skia_color_type(BitmapFormat format)
{
    switch (format) {
    case BitmapFormat::RGBA5551:
        dbgln("FIXME: Support conversion to RGBA5551.");
        return SkColorType::kUnknown_SkColorType;
    case BitmapFormat::RGB888:
        // This one needs to be converted manually because Skia has no valid 24-bit color type.
        VERIFY_NOT_REACHED();
    default:
        return to_skia_color_type(format);
    }
}

ErrorOr<BitmapExportResult> export_bitmap_to_byte_buffer(
    Bitmap const& bitmap,
    ColorSpace const& color_space,
    BitmapFormat format,
    int flags,
    Optional<int> target_width,
    Optional<int> target_height)
{
    int width = target_width.value_or(bitmap.width());
    int height = target_height.value_or(bitmap.height());

    if (format == BitmapFormat::RGB888 && (width != bitmap.width() || height != bitmap.height())) {
        dbgln("FIXME: Ignoring target width and height because scaling is not implemented for this export format.");
        width = bitmap.width();
        height = bitmap.height();
    }

    Checked<size_t> buffer_pitch = width;
    int number_of_bytes = bytes_per_pixel_for_export_format(format);
    buffer_pitch *= number_of_bytes;
    if (buffer_pitch.has_overflow())
        return Error::from_string_literal("Gfx::export_bitmap_to_byte_buffer size overflow");

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return Error::from_string_literal("Gfx::export_bitmap_to_byte_buffer size overflow");

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    if (width > 0 && height > 0) {
        if (format == BitmapFormat::RGB888) {
            // 24 bit RGB is not supported by Skia, so we need to handle this format ourselves.
            auto* raw_buffer = buffer.data();
            for (auto y = 0; y < height; y++) {
                auto target_y = flags & ExportFlags::FlipY ? height - y - 1 : y;
                for (auto x = 0; x < width; x++) {
                    auto pixel = bitmap.get_pixel(x, y);
                    auto buffer_offset = (target_y * buffer_pitch.value()) + (x * 3ull);
                    raw_buffer[buffer_offset + 0] = pixel.red();
                    raw_buffer[buffer_offset + 1] = pixel.green();
                    raw_buffer[buffer_offset + 2] = pixel.blue();
                }
            }
        } else {
            auto image = sk_image_from_bitmap(bitmap, color_space);
            if (!image)
                return Error::from_string_literal("Failed to create a Skia image for this Bitmap");

            auto skia_format = export_format_to_skia_color_type(format);
            auto skia_color_space = SkColorSpace::MakeSRGB();

            auto image_info = SkImageInfo::Make(
                width,
                height,
                skia_format,
                flags & ExportFlags::PremultiplyAlpha ? SkAlphaType::kPremul_SkAlphaType : SkAlphaType::kUnpremul_SkAlphaType,
                skia_color_space);
            auto surface = SkSurfaces::WrapPixels(image_info, buffer.data(), buffer_pitch.value());
            VERIFY(surface);
            auto* surface_canvas = surface->getCanvas();
            auto dst_rect = Gfx::to_skia_rect(Gfx::Rect { 0, 0, width, height });

            if (flags & ExportFlags::FlipY) {
                surface_canvas->translate(0, dst_rect.height());
                surface_canvas->scale(1, -1);
            }

            surface_canvas->drawImageRect(
                image.get(),
                dst_rect,
                Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor));
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

}
