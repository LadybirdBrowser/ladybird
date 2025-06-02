/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <LibGfx/ImageOrientation.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

inline Gfx::ScalingMode to_gfx_scaling_mode(CSS::ImageRendering css_value, Gfx::IntRect source, Gfx::IntRect target)
{
    switch (css_value) {
    case CSS::ImageRendering::Auto:
    case CSS::ImageRendering::HighQuality:
    case CSS::ImageRendering::Smooth:
        if (target.width() < source.width() || target.height() < source.height())
            return Gfx::ScalingMode::BoxSampling;
        return Gfx::ScalingMode::BilinearBlend;
    case CSS::ImageRendering::CrispEdges:
        return Gfx::ScalingMode::NearestNeighbor;
    case CSS::ImageRendering::Pixelated:
        return Gfx::ScalingMode::SmoothPixels;
    }
    VERIFY_NOT_REACHED();
}

[[nodiscard]]
inline Gfx::ImageOrientation to_gfx_image_orientation(CSS::ImageOrientation css_value)
{
    switch (css_value) {
    case CSS::ImageOrientation::None:
        return Gfx::ImageOrientation::FromDecoded;
    case CSS::ImageOrientation::FromImage:
        return Gfx::ImageOrientation::FromExif;
    default:
        VERIFY_NOT_REACHED();
    }
}

}
