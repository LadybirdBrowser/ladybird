/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/SVG/AttributeParser.h>

namespace Web::CSS {

struct Inset {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string(SerializationMode) const;

    bool operator==(Inset const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> top;
    ValueComparingNonnullRefPtr<StyleValue const> right;
    ValueComparingNonnullRefPtr<StyleValue const> bottom;
    ValueComparingNonnullRefPtr<StyleValue const> left;
};

struct Xywh {
    String to_string(SerializationMode) const;

    bool operator==(Xywh const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> x;
    ValueComparingNonnullRefPtr<StyleValue const> y;
    ValueComparingNonnullRefPtr<StyleValue const> width;
    ValueComparingNonnullRefPtr<StyleValue const> height;
};

struct Rect {
    String to_string(SerializationMode) const;

    bool operator==(Rect const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> top;
    ValueComparingNonnullRefPtr<StyleValue const> right;
    ValueComparingNonnullRefPtr<StyleValue const> bottom;
    ValueComparingNonnullRefPtr<StyleValue const> left;
};

struct Circle {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string(SerializationMode) const;

    bool operator==(Circle const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> radius;
    ValueComparingNonnullRefPtr<PositionStyleValue const> position;
};

struct Ellipse {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string(SerializationMode) const;

    bool operator==(Ellipse const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> radius_x;
    ValueComparingNonnullRefPtr<StyleValue const> radius_y;
    ValueComparingNonnullRefPtr<PositionStyleValue const> position;
};

struct Polygon {
    struct Point {
        bool operator==(Point const&) const = default;
        ValueComparingNonnullRefPtr<StyleValue const> x;
        ValueComparingNonnullRefPtr<StyleValue const> y;
    };

    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string(SerializationMode) const;

    bool operator==(Polygon const&) const = default;

    Gfx::WindingRule fill_rule;
    Vector<Point> points;
};

// https://drafts.csswg.org/css-shapes/#funcdef-basic-shape-path
struct Path {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string(SerializationMode) const;

    bool operator==(Path const&) const = default;

    Gfx::WindingRule fill_rule;
    SVG::Path path_instructions;
};

// https://www.w3.org/TR/css-shapes-1/#basic-shape-functions
using BasicShape = Variant<Inset, Xywh, Rect, Circle, Ellipse, Polygon, Path>;

class BasicShapeStyleValue : public StyleValueWithDefaultOperators<BasicShapeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BasicShapeStyleValue const> create(BasicShape basic_shape)
    {
        return adopt_ref(*new (nothrow) BasicShapeStyleValue(move(basic_shape)));
    }
    virtual ~BasicShapeStyleValue() override;

    BasicShape const& basic_shape() const { return m_basic_shape; }

    virtual String to_string(SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(BasicShapeStyleValue const& other) const { return m_basic_shape == other.m_basic_shape; }

    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;

private:
    BasicShapeStyleValue(BasicShape basic_shape)
        : StyleValueWithDefaultOperators(Type::BasicShape)
        , m_basic_shape(move(basic_shape))
    {
    }

    BasicShape m_basic_shape;
};

}
