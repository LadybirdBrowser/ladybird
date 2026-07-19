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
    Gfx::Path to_path(CSSPixelRect reference_box) const;
    void serialize(StringBuilder&, SerializationMode) const;

    bool operator==(Inset const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> top;
    ValueComparingNonnullRefPtr<StyleValue const> right;
    ValueComparingNonnullRefPtr<StyleValue const> bottom;
    ValueComparingNonnullRefPtr<StyleValue const> left;

    ValueComparingNonnullRefPtr<StyleValue const> border_radius;
};

struct Xywh {
    void serialize(StringBuilder&, SerializationMode) const;

    bool operator==(Xywh const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> x;
    ValueComparingNonnullRefPtr<StyleValue const> y;
    ValueComparingNonnullRefPtr<StyleValue const> width;
    ValueComparingNonnullRefPtr<StyleValue const> height;

    ValueComparingNonnullRefPtr<StyleValue const> border_radius;
};

struct Rect {
    void serialize(StringBuilder&, SerializationMode) const;

    bool operator==(Rect const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> top;
    ValueComparingNonnullRefPtr<StyleValue const> right;
    ValueComparingNonnullRefPtr<StyleValue const> bottom;
    ValueComparingNonnullRefPtr<StyleValue const> left;

    ValueComparingNonnullRefPtr<StyleValue const> border_radius;
};

struct Circle {
    Gfx::Path to_path(CSSPixelRect reference_box) const;
    void serialize(StringBuilder&, SerializationMode) const;

    bool operator==(Circle const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> radius;
    ValueComparingRefPtr<StyleValue const> position;
};

struct Ellipse {
    Gfx::Path to_path(CSSPixelRect reference_box) const;
    void serialize(StringBuilder&, SerializationMode) const;

    bool operator==(Ellipse const&) const = default;

    ValueComparingNonnullRefPtr<StyleValue const> radius;
    ValueComparingRefPtr<StyleValue const> position;
};

struct Polygon {
    struct Point {
        bool operator==(Point const&) const = default;
        ValueComparingNonnullRefPtr<StyleValue const> x;
        ValueComparingNonnullRefPtr<StyleValue const> y;
    };

    Gfx::Path to_path(CSSPixelRect reference_box) const;
    void serialize(StringBuilder&, SerializationMode) const;

    bool operator==(Polygon const&) const = default;

    Gfx::WindingRule fill_rule;
    Vector<Point> points;
};

// https://drafts.csswg.org/css-shapes/#funcdef-basic-shape-path
struct Path {
    Gfx::Path to_path(CSSPixelRect reference_box) const;
    void serialize(StringBuilder&, SerializationMode) const;

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

    BasicShape const& basic_shape() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(BasicShapeStyleValue const& other) const { return basic_shape() == other.basic_shape(); }

    Gfx::Path to_path(CSSPixelRect reference_box) const;

private:
    BasicShapeStyleValue(BasicShape basic_shape)
        : StyleValueWithDefaultOperators(Type::BasicShape, make_basic_shape_data(basic_shape))
        , m_shape(move(basic_shape))
    {
    }

    static StyleValueFFI::StyleValueData* make_basic_shape_data(BasicShape const&);

    // NB: Eagerly materialized copy of the Rust-owned data (rebuilding a path shape would
    //     re-parse its serialized path data); the Rust allocation stays authoritative, and the
    //     copy is immutable after construction, so sharing across style workers is safe.
    BasicShape m_shape;
};

}
