/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibWeb/Export.h>

namespace Web::SVG {

struct Transform {
    struct Translate {
        float x;
        float y;
    };
    struct Scale {
        float x;
        float y;
    };
    struct Rotate {
        float a;
        float x;
        float y;
    };
    struct SkewX {
        float a;
    };
    struct SkewY {
        float a;
    };
    struct Matrix {
        float a;
        float b;
        float c;
        float d;
        float e;
        float f;
    };

    using Operation = Variant<Translate, Scale, Rotate, SkewX, SkewY, Matrix>;
    Operation operation;
};

struct PreserveAspectRatio {
    enum class Align {
        None,
        xMinYMin,
        xMidYMin,
        xMaxYMin,
        xMinYMid,
        xMidYMid,
        xMaxYMid,
        xMinYMax,
        xMidYMax,
        xMaxYMax
    };
    enum class MeetOrSlice {
        Meet,
        Slice
    };
    Align align { Align::xMidYMid };
    MeetOrSlice meet_or_slice { MeetOrSlice::Meet };
};

enum class SVGUnits {
    ObjectBoundingBox,
    UserSpaceOnUse
};

struct ViewBox {
    double min_x { 0 };
    double min_y { 0 };
    double width { 0 };
    double height { 0 };
};

using GradientUnits = SVGUnits;
using MaskUnits = SVGUnits;
using MaskContentUnits = SVGUnits;
using ClipPathUnits = SVGUnits;

enum class SpreadMethod {
    Pad,
    Repeat,
    Reflect
};

class WEB_API NumberPercentage {
public:
    NumberPercentage(float value, bool is_percentage)
        : m_value(is_percentage ? value / 100 : value)
        , m_is_percentage(is_percentage)
    {
    }

    static NumberPercentage create_percentage(float value)
    {
        return NumberPercentage(value, true);
    }

    static NumberPercentage create_number(float value)
    {
        return NumberPercentage(value, false);
    }

    float resolve_relative_to(float length) const;

    float value() const { return m_value; }
    bool is_percentage() const { return m_is_percentage; }

private:
    float m_value;
    bool m_is_percentage { false };
};

enum class TextAnchor {
    Start,
    Middle,
    End
};

WEB_API Optional<i32> parse_integer(Utf16View);
WEB_API Optional<NumberPercentage> parse_number_percentage(Utf16View);
WEB_API Vector<Gfx::FloatPoint> parse_points(Utf16View);
WEB_API Optional<Vector<Transform>> parse_transform(Utf16View);
WEB_API Optional<PreserveAspectRatio> parse_preserve_aspect_ratio(Utf16View);
WEB_API Optional<SVGUnits> parse_units(Utf16View);
WEB_API Vector<float> parse_table_values(Utf16View);
WEB_API Optional<SpreadMethod> parse_spread_method(Utf16View);
WEB_API Optional<ViewBox> parse_viewbox(Utf16View);

}
