/*
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/WindingRule.h>
#include <core/SkBlender.h>
#include <core/SkColor.h>
#include <core/SkColorType.h>
#include <core/SkImageFilter.h>
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
    case Gfx::BitmapFormat::BGRx8888:
        return kBGRA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBA8888:
        return kRGBA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBx8888:
        return kRGB_888x_SkColorType;
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

constexpr SkPaint::Join to_skia_join(Gfx::Path::JoinStyle const& join_style)
{
    switch (join_style) {
    case Gfx::Path::JoinStyle::Round:
        return SkPaint::kRound_Join;
    case Gfx::Path::JoinStyle::Bevel:
        return SkPaint::kBevel_Join;
    case Gfx::Path::JoinStyle::Miter:
        return SkPaint::kMiter_Join;
    }
    VERIFY_NOT_REACHED();
}

constexpr SkPaint::Cap to_skia_cap(Gfx::Path::CapStyle const& cap_style)
{
    switch (cap_style) {
    case Gfx::Path::CapStyle::Butt:
        return SkPaint::kButt_Cap;
    case Gfx::Path::CapStyle::Round:
        return SkPaint::kRound_Cap;
    case Gfx::Path::CapStyle::Square:
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

constexpr SkSamplingOptions to_skia_sampling_options(Gfx::ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case Gfx::ScalingMode::NearestNeighbor:
        return SkSamplingOptions(SkFilterMode::kNearest);
    case Gfx::ScalingMode::BilinearBlend:
    case Gfx::ScalingMode::SmoothPixels:
        return SkSamplingOptions(SkFilterMode::kLinear);
    case Gfx::ScalingMode::BoxSampling:
        return SkSamplingOptions(SkCubicResampler::Mitchell());
    default:
        VERIFY_NOT_REACHED();
    }
}

SkPath to_skia_path(Path const& path);
sk_sp<SkImageFilter> to_skia_image_filter(Gfx::Filter const& filter);
sk_sp<SkBlender> to_skia_blender(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator);
}
