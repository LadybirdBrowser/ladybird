/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGfx/Path.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

struct Polygon {
    struct Point {
        bool operator==(Point const&) const = default;
        LengthPercentage x;
        LengthPercentage y;
    };

    Gfx::Path to_path(CSSPixelRect reference_box, Layout::Node const&) const;
    String to_string() const;

    bool operator==(Polygon const&) const = default;

    FillRule fill_rule;
    Vector<Point> points;
};

// FIXME: Implement other basic shapes. See: https://www.w3.org/TR/css-shapes-1/#basic-shape-functions
using BasicShape = Variant<Polygon>;

class BasicShapeStyleValue : public StyleValueWithDefaultOperators<BasicShapeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BasicShapeStyleValue> create(BasicShape basic_shape)
    {
        return adopt_ref(*new (nothrow) BasicShapeStyleValue(move(basic_shape)));
    }
    virtual ~BasicShapeStyleValue() override;

    BasicShape const& basic_shape() const { return m_basic_shape; }

    virtual String to_string() const override;

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
