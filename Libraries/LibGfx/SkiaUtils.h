/*
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/BlendMode.h>
#include <LibGfx/Filter.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/WindingRule.h>
#include <core/SkBlendMode.h>
#include <core/SkColor.h>
#include <core/SkColorType.h>
#include <core/SkImageFilter.h>
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

}

constexpr SkBlendMode to_skia_blend_mode(Gfx::BlendMode blend_mode)
{
    switch (blend_mode) {
    case Gfx::BlendMode::Normal:
        return SkBlendMode::kSrc;
    case Gfx::BlendMode::Darken:
        return SkBlendMode::kDarken;
    case Gfx::BlendMode::Multiply:
        return SkBlendMode::kMultiply;
    case Gfx::BlendMode::ColorBurn:
        return SkBlendMode::kColorBurn;
    case Gfx::BlendMode::Lighten:
        return SkBlendMode::kLighten;
    case Gfx::BlendMode::Screen:
        return SkBlendMode::kScreen;
    case Gfx::BlendMode::ColorDodge:
        return SkBlendMode::kColorDodge;
    case Gfx::BlendMode::Overlay:
        return SkBlendMode::kOverlay;
    case Gfx::BlendMode::SoftLight:
        return SkBlendMode::kSoftLight;
    case Gfx::BlendMode::HardLight:
        return SkBlendMode::kHardLight;
    case Gfx::BlendMode::Difference:
        return SkBlendMode::kDifference;
    case Gfx::BlendMode::Exclusion:
        return SkBlendMode::kExclusion;
    case Gfx::BlendMode::Hue:
        return SkBlendMode::kHue;
    case Gfx::BlendMode::Saturation:
        return SkBlendMode::kSaturation;
    case Gfx::BlendMode::Color:
        return SkBlendMode::kColor;
    case Gfx::BlendMode::Luminosity:
        return SkBlendMode::kLuminosity;
    }
    VERIFY_NOT_REACHED();
}
