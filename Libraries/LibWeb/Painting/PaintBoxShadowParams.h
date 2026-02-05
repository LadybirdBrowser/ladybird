/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/BordersData.h>
#include <LibWeb/Painting/ShadowData.h>

namespace Web::Painting {

struct PaintBoxShadowParams {
    Gfx::Color color;
    ShadowPlacement placement;
    CornerRadii corner_radii;
    int offset_x;
    int offset_y;
    int blur_radius;
    int spread_distance;
    Gfx::IntRect device_content_rect;
};

}
