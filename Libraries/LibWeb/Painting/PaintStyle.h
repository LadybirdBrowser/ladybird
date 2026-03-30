/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Variant.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Rect.h>

namespace Web::Painting {

class DisplayList;

struct ColorStop {
    Color color;
    float position = AK::NaN<float>;
    Optional<float> transition_hint = {};
};

struct GradientPaintData {
    enum class SpreadMethod {
        Pad,
        Repeat,
        Reflect
    };

    Vector<ColorStop, 4> color_stops;
    Optional<float> repeat_length;
    Optional<Gfx::AffineTransform> gradient_transform;
    SpreadMethod spread_method { SpreadMethod::Pad };
    Gfx::InterpolationColorSpace color_space { Gfx::InterpolationColorSpace::SRGB };

    void add_color_stop(float position, Color color, Optional<float> transition_hint = {})
    {
        add_color_stop(ColorStop { color, position, transition_hint });
    }

    void add_color_stop(ColorStop stop, bool sort = true)
    {
        color_stops.append(stop);
        if (sort)
            quick_sort(color_stops, [](auto& a, auto& b) { return a.position < b.position; });
    }
};

struct SVGLinearGradientPaintStyle {
    GradientPaintData gradient;
    Gfx::FloatPoint start_point;
    Gfx::FloatPoint end_point;
};

struct SVGRadialGradientPaintStyle {
    GradientPaintData gradient;
    Gfx::FloatPoint start_center;
    float start_radius { 0.0f };
    Gfx::FloatPoint end_center;
    float end_radius { 0.0f };
};

// Out-of-line special members because DisplayList is incomplete here.
struct SVGPatternPaintStyle {
    NonnullRefPtr<DisplayList> tile_display_list;
    Gfx::FloatRect tile_rect;
    Optional<Gfx::AffineTransform> pattern_transform;

    SVGPatternPaintStyle(NonnullRefPtr<DisplayList>, Gfx::FloatRect, Optional<Gfx::AffineTransform>);
    ~SVGPatternPaintStyle();
    SVGPatternPaintStyle(SVGPatternPaintStyle const&);
    SVGPatternPaintStyle& operator=(SVGPatternPaintStyle const&);
    SVGPatternPaintStyle(SVGPatternPaintStyle&&);
    SVGPatternPaintStyle& operator=(SVGPatternPaintStyle&&);
};

using PaintStyle = Variant<SVGLinearGradientPaintStyle, SVGRadialGradientPaintStyle, SVGPatternPaintStyle>;
using PaintStyleOrColor = Variant<SVGLinearGradientPaintStyle, SVGRadialGradientPaintStyle, SVGPatternPaintStyle, Gfx::Color>;

}
