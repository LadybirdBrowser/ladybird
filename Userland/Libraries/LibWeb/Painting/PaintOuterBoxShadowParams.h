/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/BordersData.h>
#include <LibWeb/Painting/ShadowData.h>

namespace Web::Painting {

struct PaintOuterBoxShadowParams {
    Gfx::Color color;
    ShadowPlacement placement;
    CornerRadii corner_radii;
    DevicePixels offset_x;
    DevicePixels offset_y;
    DevicePixels blur_radius;
    DevicePixels spread_distance;
    DevicePixelRect device_content_rect;
};

}
