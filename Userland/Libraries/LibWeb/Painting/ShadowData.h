/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

enum class ShadowPlacement {
    Outer,
    Inner,
};

struct ShadowData {
    Gfx::Color color;
    CSSPixels offset_x;
    CSSPixels offset_y;
    CSSPixels blur_radius;
    CSSPixels spread_distance;
    ShadowPlacement placement;
};

}
