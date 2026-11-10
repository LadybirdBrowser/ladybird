/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <Compositor/BitmapExport.h>
#include <Compositor/SkiaUtils.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>

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
    case ExportFormat::RGBA4444:
        return SkColorType::kARGB_4444_SkColorType;
    case ExportFormat::RGBA5551:
    case ExportFormat::RGB888:
        // These need to be converted manually because Skia has no color type for them.
        VERIFY_NOT_REACHED();
    case ExportFormat::RGBA8888:
        return SkColorType::kRGBA_8888_SkColorType;
    default:
        VERIFY_NOT_REACHED();
    }
}

static ErrorOr<void> draw_bitmap_into_pixels(Bitmap const& bitmap, ColorSpace const& color_space, SkColorType color_type, int flags, int width, int height, void* pixels, size_t pitch)
{
    auto image = sk_image_from_bitmap(bitmap, color_space);
    if (!image)
        return Error::from_string_literal("Failed to create a Skia image for this Bitmap");

    auto image_info = SkImageInfo::Make(
        width,
        height,
        color_type,
        flags & ExportFlags::PremultiplyAlpha ? SkAlphaType::kPremul_SkAlphaType : SkAlphaType::kUnpremul_SkAlphaType,
        SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::WrapPixels(image_info, pixels, pitch);
    if (!surface)
        return Error::from_string_literal("Failed to create a Skia surface for the exported pixels");
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
    return {};
}

ErrorOr<BitmapExportResult> export_bitmap_to_byte_buffer(
    Bitmap const& bitmap,
    ColorSpace const& color_space,
    ExportFormat format,
    int flags,
    Optional<int> target_width,
    Optional<int> target_height)
{
    int width = target_width.value_or(bitmap.width());
    int height = target_height.value_or(bitmap.height());

    if (format == ExportFormat::RGB888 && (width != bitmap.width() || height != bitmap.height())) {
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
        if (format == ExportFormat::RGB888) {
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
        } else if (format == ExportFormat::RGBA5551) {
            // 16 bit RGBA with 1 bit of alpha is not supported by Skia, so we draw into 32 bit RGBA and pack that.
            Checked<size_t> rgba_pitch = width;
            rgba_pitch *= 4;
            if (rgba_pitch.has_overflow() || Checked<size_t>::multiplication_would_overflow(rgba_pitch.value(), height))
                return Error::from_string_literal("Gfx::export_bitmap_to_byte_buffer size overflow");
            auto rgba_buffer = TRY(ByteBuffer::create_zeroed(rgba_pitch.value() * height));
            TRY(draw_bitmap_into_pixels(bitmap, color_space, SkColorType::kRGBA_8888_SkColorType, flags, width, height, rgba_buffer.data(), rgba_pitch.value()));

            for (auto y = 0; y < height; y++) {
                auto const* source_row = rgba_buffer.data() + (y * rgba_pitch.value());
                auto* destination_row = buffer.data() + (y * buffer_pitch.value());
                for (auto x = 0; x < width; x++) {
                    auto const* source = source_row + (x * 4ull);
                    u16 pixel = ((source[0] >> 3) << 11) | ((source[1] >> 3) << 6) | ((source[2] >> 3) << 1) | (source[3] >> 7);
                    ByteReader::store(destination_row + (x * 2ull), pixel);
                }
            }
        } else {
            TRY(draw_bitmap_into_pixels(bitmap, color_space, export_format_to_skia_color_type(format), flags, width, height, buffer.data(), buffer_pitch.value()));
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
