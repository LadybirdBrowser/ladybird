/*
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <Compositor/Forward.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/StrokeStyle.h>
#include <LibGfx/WindingRule.h>
#include <core/SkBlender.h>
#include <core/SkColor.h>
#include <core/SkColorSpace.h>
#include <core/SkColorType.h>
#include <core/SkImage.h>
#include <core/SkImageFilter.h>
#include <core/SkMatrix.h>
#include <core/SkPaint.h>
#include <core/SkPath.h>
#include <core/SkSamplingOptions.h>

namespace Gfx {

constexpr SkColorType to_skia_color_type(Gfx::BitmapFormat format)
{
    switch (format) {
    case Gfx::BitmapFormat::Invalid:
        return kUnknown_SkColorType;
    case Gfx::BitmapFormat::BGRA8888:
        return kBGRA_8888_SkColorType;
    case Gfx::BitmapFormat::BGRx8888:
        // FIXME: This is not fully correct, since our bitmap's alpha component might contain garbage data.
        // If the alpha component does not contain 0xFF, Skia might wrongly use that value as alpha.
        return kBGRA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBA8888:
        return kRGBA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBx8888:
        return kRGB_888x_SkColorType;
    }
    VERIFY_NOT_REACHED();
}

constexpr SkAlphaType to_skia_alpha_type(Gfx::BitmapFormat format, Gfx::AlphaType alpha_type)
{
    if (format == BitmapFormat::BGRx8888 || format == BitmapFormat::RGBx8888)
        return kOpaque_SkAlphaType;

    switch (alpha_type) {
    case AlphaType::Premultiplied:
        return kPremul_SkAlphaType;
    case AlphaType::Unpremultiplied:
        return kUnpremul_SkAlphaType;
    }
    VERIFY_NOT_REACHED();
}

constexpr SkRect to_skia_rect(auto const& rect)
{
    return SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
}

constexpr SkColor to_skia_color(Gfx::Color const& color)
{
    return SkColorSetARGB(color.alpha(), color.red(), color.green(), color.blue());
}

constexpr SkPaint::Join to_skia_join(Gfx::JoinStyle const& join_style)
{
    switch (join_style) {
    case Gfx::JoinStyle::Round:
        return SkPaint::kRound_Join;
    case Gfx::JoinStyle::Bevel:
        return SkPaint::kBevel_Join;
    case Gfx::JoinStyle::Miter:
        return SkPaint::kMiter_Join;
    }
    VERIFY_NOT_REACHED();
}

constexpr SkPaint::Cap to_skia_cap(Gfx::CapStyle const& cap_style)
{
    switch (cap_style) {
    case Gfx::CapStyle::Butt:
        return SkPaint::kButt_Cap;
    case Gfx::CapStyle::Round:
        return SkPaint::kRound_Cap;
    case Gfx::CapStyle::Square:
        return SkPaint::kSquare_Cap;
    }
    VERIFY_NOT_REACHED();
}

constexpr SkColor4f to_skia_color4f(Color const& color)
{
    return {
        .fR = color.red() / 255.0f,
        .fG = color.green() / 255.0f,
        .fB = color.blue() / 255.0f,
        .fA = color.alpha() / 255.0f,
    };
}

constexpr SkPoint to_skia_point(auto const& point)
{
    return SkPoint::Make(point.x(), point.y());
}

constexpr SkPathFillType to_skia_path_fill_type(Gfx::WindingRule winding_rule)
{
    switch (winding_rule) {
    case Gfx::WindingRule::Nonzero:
        return SkPathFillType::kWinding;
    case Gfx::WindingRule::EvenOdd:
        return SkPathFillType::kEvenOdd;
    }
    VERIFY_NOT_REACHED();
}

constexpr SkSamplingOptions to_skia_sampling_options(ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case ScalingMode::None:
        return SkSamplingOptions();
    case ScalingMode::Bilinear:
        return SkSamplingOptions(SkFilterMode::kLinear);
    case ScalingMode::BilinearMipmap:
        return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    case ScalingMode::NearestNeighbor:
        return SkSamplingOptions(SkFilterMode::kNearest);
    }
    VERIFY_NOT_REACHED();
}

SkMatrix to_skia_matrix(AffineTransform const&);
SkPath to_skia_path(PathData const&);
SkPath skia_path_from_serialized_bytes(ReadonlyBytes);
// Builds the SkImageFilter tree a serialized filter graph describes; `image_frame` supplies the
// frames the graph names by id.
// With `blur_edge`, every blur() filter function reads its input mirrored at that rect, not what lies outside it.
sk_sp<SkImageFilter> to_skia_image_filter(ReadonlyBytes serialized_filter, Function<DecodedImageFrame const&(u64)> const& image_frame, Optional<FloatRect> blur_edge = {});
sk_sp<SkBlender> to_skia_blender(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator);

// The returned SkImage references the source bitmap's pixels without copying; the caller
// must keep `bitmap` alive for as long as the SkImage (or anything derived from it) is in use.
// Returns null for sRGB, which Skia assumes for an image without a color space.
sk_sp<SkColorSpace> to_skia_color_space(ColorSpace const&);

sk_sp<SkImage> sk_image_from_bitmap(Bitmap const& bitmap, ColorSpace const& color_space);
sk_sp<SkImage> sk_image_from_decoded_image_frame(DecodedImageFrame const&);

// Samples the IOSurface of the frame where it is, so it is null for a frame without one, or when the GPU of the context
// can't sample it.
sk_sp<SkImage> sk_texture_image_sharing_pixels_of_decoded_image_frame(DecodedImageFrame const&, SkiaBackendContext&);

// Hands the source bitmap's pixel storage to the returned SkImage without copying: the image
// holds a reference to the bitmap and drops it on destruction.
sk_sp<SkImage> sk_image_adopting_bitmap(NonnullRefPtr<Bitmap> bitmap, ColorSpace const& color_space);

}
