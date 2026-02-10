/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibWeb/Export.h>
#include <LibWeb/SVG/Path.h>

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

enum class FillRule {
    Nonzero,
    Evenodd
};

using ClipRule = FillRule;

enum class TextAnchor {
    Start,
    Middle,
    End
};

class WEB_API AttributeParser final {
public:
    ~AttributeParser() = default;

    static Optional<float> parse_coordinate(StringView input);
    static Optional<float> parse_length(StringView input);
    static Optional<i32> parse_integer(StringView input);
    static Optional<NumberPercentage> parse_number_percentage(StringView input);
    static Optional<float> parse_positive_length(StringView input);
    static Vector<Gfx::FloatPoint> parse_points(StringView input);
    static Path parse_path_data(StringView input);
    static Optional<Vector<Transform>> parse_transform(StringView input);
    static Optional<PreserveAspectRatio> parse_preserve_aspect_ratio(StringView input);
    static Optional<SVGUnits> parse_units(StringView input);
    static Optional<SpreadMethod> parse_spread_method(StringView input);
    static Vector<float> parse_table_values(StringView);
    static Optional<ViewBox> parse_viewbox(StringView input);

private:
    AttributeParser(StringView source);

    ErrorOr<void> parse_drawto();
    ErrorOr<void> parse_moveto();
    void parse_closepath();
    ErrorOr<void> parse_lineto();
    ErrorOr<void> parse_horizontal_lineto();
    ErrorOr<void> parse_vertical_lineto();
    ErrorOr<void> parse_curveto();
    ErrorOr<void> parse_smooth_curveto();
    ErrorOr<void> parse_quadratic_bezier_curveto();
    ErrorOr<void> parse_smooth_quadratic_bezier_curveto();
    ErrorOr<void> parse_elliptical_arc();

    Optional<Vector<Transform>> parse_transform();

    ErrorOr<float> parse_length();
    ErrorOr<float> parse_coordinate();
    ErrorOr<i32> parse_integer();
    ErrorOr<Vector<float>> parse_coordinate_pair();
    ErrorOr<Vector<float>> parse_coordinate_sequence();
    ErrorOr<Vector<Vector<float>>> parse_coordinate_pair_sequence();
    ErrorOr<Vector<float>> parse_coordinate_pair_double();
    ErrorOr<Vector<float>> parse_coordinate_pair_triplet();
    ErrorOr<Vector<float>> parse_elliptical_arc_argument();
    void parse_whitespace(bool must_match_once = false);
    void parse_comma_whitespace();
    ErrorOr<float> parse_number();
    ErrorOr<float> parse_nonnegative_number();
    ErrorOr<float> parse_flag();
    // -1 if negative, +1 otherwise
    int parse_sign();

    enum class AllowDot {
        No,
        Yes,
    };

    bool match_whitespace() const;
    bool match_comma_whitespace() const;
    bool match_coordinate() const;
    bool match_length(AllowDot allow_dot) const;
    bool match_number() const;
    bool match_integer() const;
    bool match(char c) const { return !done() && ch() == c; }

    bool done() const { return m_lexer.is_eof(); }
    char ch(size_t offset = 0) const { return m_lexer.peek(offset); }
    char consume() { return m_lexer.consume(); }

    GenericLexer m_lexer;
    Vector<PathInstruction> m_instructions;
};

}
