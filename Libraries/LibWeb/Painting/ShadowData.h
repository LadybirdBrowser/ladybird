/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
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

inline ShadowPlacement shadow_placement_from_css(CSS::ShadowPlacement placement)
{
    switch (placement) {
    case CSS::ShadowPlacement::Outer:
        return ShadowPlacement::Outer;
    case CSS::ShadowPlacement::Inner:
        return ShadowPlacement::Inner;
    }
    VERIFY_NOT_REACHED();
}

struct ShadowData {
    Gfx::Color color;
    CSSPixels offset_x;
    CSSPixels offset_y;
    CSSPixels blur_radius;
    CSSPixels spread_distance;
    ShadowPlacement placement;

    static ShadowData from_css(CSS::ShadowData const& shadow, Layout::Node const& layout_node)
    {
        return {
            shadow.color,
            shadow.offset_x.to_px(layout_node),
            shadow.offset_y.to_px(layout_node),
            shadow.blur_radius.to_px(layout_node),
            shadow.spread_distance.to_px(layout_node),
            shadow_placement_from_css(shadow.placement),
        };
    }
};

}
