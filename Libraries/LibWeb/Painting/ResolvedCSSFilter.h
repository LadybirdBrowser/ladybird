/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Filter.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

struct ResolvedCSSFilter {
    struct Blur {
        CSSPixels radius;
    };

    struct DropShadow {
        CSSPixels offset_x;
        CSSPixels offset_y;
        CSSPixels radius;
        Gfx::Color color;
    };

    struct Color {
        Gfx::ColorFilterType operation;
        float amount;
    };

    struct HueRotate {
        float angle_degrees;
    };

    using ResolvedFilterValue = Variant<Blur, DropShadow, Color, HueRotate>;
    Vector<ResolvedFilterValue> operations;

    // For SVG url() filters - store the resolved Gfx::Filter directly
    // since SVG filters have their own coordinate system
    Optional<Gfx::Filter> svg_filter;
    Optional<CSSPixelRect> svg_filter_bounds;

    bool has_filters() const { return !operations.is_empty() || svg_filter.has_value(); }
    bool has_svg_filters() const { return svg_filter.has_value(); }

    void dump(StringBuilder&) const;
};

Optional<Gfx::Filter> to_gfx_filter(ResolvedCSSFilter const&, double device_pixels_per_css_pixel);

}
