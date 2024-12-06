/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>

namespace Web::CSS {

struct Inset {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string() const;

    bool operator==(Inset const&) const = default;

    LengthBox inset_box;
};

struct Xywh {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string() const;

    bool operator==(Xywh const&) const = default;

    LengthPercentage x;
    LengthPercentage y;
    LengthPercentage width;
    LengthPercentage height;
};

struct Rect {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string() const;

    bool operator==(Rect const&) const = default;

    LengthBox box;
};

enum class FitSide {
    ClosestSide,
    FarthestSide,
};

using ShapeRadius = Variant<LengthPercentage, FitSide>;

struct Circle {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string() const;

    bool operator==(Circle const&) const = default;

    ShapeRadius radius;
    ValueComparingNonnullRefPtr<PositionStyleValue> position;
};

struct Ellipse {
    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string() const;

    bool operator==(Ellipse const&) const = default;

    ShapeRadius radius_x;
    ShapeRadius radius_y;
    ValueComparingNonnullRefPtr<PositionStyleValue> position;
};

struct Polygon {
    struct Point {
        bool operator==(Point const&) const = default;
        LengthPercentage x;
        LengthPercentage y;
    };

    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string() const;

    bool operator==(Polygon const&) const = default;

    Gfx::WindingRule fill_rule;
    Vector<Point> points;
};

// FIXME: Implement path(). See: https://www.w3.org/TR/css-shapes-1/#basic-shape-functions
using BasicShape = Variant<Inset, Xywh, Rect, Circle, Ellipse, Polygon>;

class BasicShapeStyleValue : public StyleValueWithDefaultOperators<BasicShapeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BasicShapeStyleValue> create(BasicShape basic_shape)
    {
        return adopt_ref(*new (nothrow) BasicShapeStyleValue(move(basic_shape)));
    }
    virtual ~BasicShapeStyleValue() override;

    BasicShape const& basic_shape() const { return m_basic_shape; }

    virtual String to_string(SerializationMode) const override;

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
