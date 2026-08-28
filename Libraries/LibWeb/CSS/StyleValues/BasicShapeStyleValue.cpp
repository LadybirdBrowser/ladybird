/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BasicShapeStyleValue.h"
#include <LibGfx/Path.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusRectStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/SVG/Path.h>

namespace Web::CSS {

// The fill-rule discriminant crosses the style value FFI as a raw code; the Rust serializer
// depends on it.
static_assert(to_underlying(Gfx::WindingRule::Nonzero) == 0);
static_assert(to_underlying(Gfx::WindingRule::EvenOdd) == 1);

StyleValueFFI::StyleValueData const* BasicShapeStyleValue::make_basic_shape_data(BasicShape const& basic_shape)
{
    auto retain = [](StyleValue const* value) {
        return value ? StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) : nullptr;
    };
    return basic_shape.visit(
        [&](Inset const& inset) {
            return StyleValueFFI::rust_style_value_create_basic_shape(0, retain(inset.top.ptr()), retain(inset.right.ptr()), retain(inset.bottom.ptr()), retain(inset.left.ptr()), retain(inset.border_radius.ptr()), 0, nullptr, 0, 0);
        },
        [&](Xywh const& xywh) {
            return StyleValueFFI::rust_style_value_create_basic_shape(1, retain(xywh.x.ptr()), retain(xywh.y.ptr()), retain(xywh.width.ptr()), retain(xywh.height.ptr()), retain(xywh.border_radius.ptr()), 0, nullptr, 0, 0);
        },
        [&](Rect const& rect) {
            return StyleValueFFI::rust_style_value_create_basic_shape(2, retain(rect.top.ptr()), retain(rect.right.ptr()), retain(rect.bottom.ptr()), retain(rect.left.ptr()), retain(rect.border_radius.ptr()), 0, nullptr, 0, 0);
        },
        [&](Circle const& circle) {
            return StyleValueFFI::rust_style_value_create_basic_shape(3, retain(circle.radius.ptr()), retain(circle.position.ptr()), nullptr, nullptr, nullptr, 0, nullptr, 0, 0);
        },
        [&](Ellipse const& ellipse) {
            return StyleValueFFI::rust_style_value_create_basic_shape(4, retain(ellipse.radius.ptr()), retain(ellipse.position.ptr()), nullptr, nullptr, nullptr, 0, nullptr, 0, 0);
        },
        [&](Polygon const& polygon) {
            Vector<StyleValueFFI::RetainedShapePoint> points;
            points.ensure_capacity(polygon.points.size());
            for (auto const& point : polygon.points)
                points.unchecked_append({ { retain(point.x.ptr()) }, { retain(point.y.ptr()) } });
            return StyleValueFFI::rust_style_value_create_basic_shape(5, nullptr, nullptr, nullptr, nullptr, nullptr, static_cast<u8>(to_underlying(polygon.fill_rule)), points.data(), points.size(), 0);
        },
        [](Path const& path) {
            return StyleValueFFI::rust_style_value_create_basic_shape(6, nullptr, nullptr, nullptr, nullptr, nullptr, static_cast<u8>(to_underlying(path.fill_rule)), nullptr, 0, Utf16String::from_utf8(path.path_instructions.serialize()).to_raw_leaked());
        });
}

BasicShapeStyleValue::BasicShapeStyleValue(StyleValueFFI::StyleValueData const* data)
    : StyleValueWithDefaultOperators(Type::BasicShape, data)
    , m_shape([&]() -> BasicShape {
        auto adopt = [](auto const& retained) -> ValueComparingNonnullRefPtr<StyleValue const> {
            auto const* child_data = static_cast<StyleValueFFI::StyleValueData const*>(retained.pointer);
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data));
        };
        auto adopt_optional = [](auto const& retained) -> ValueComparingRefPtr<StyleValue const> {
            auto const* child_data = static_cast<StyleValueFFI::StyleValueData const*>(retained.pointer);
            if (!child_data)
                return nullptr;
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data));
        };
        auto const& shape = data->basic_shape;
        switch (shape.kind) {
        case 0:
            return Inset { adopt(shape.v0), adopt(shape.v1), adopt(shape.v2), adopt(shape.v3), adopt(shape.v4) };
        case 1:
            return Xywh { adopt(shape.v0), adopt(shape.v1), adopt(shape.v2), adopt(shape.v3), adopt(shape.v4) };
        case 2:
            return Rect { adopt(shape.v0), adopt(shape.v1), adopt(shape.v2), adopt(shape.v3), adopt(shape.v4) };
        case 3:
            return Circle { adopt(shape.v0), adopt_optional(shape.v1) };
        case 4:
            return Ellipse { adopt(shape.v0), adopt_optional(shape.v1) };
        case 5: {
            Vector<Polygon::Point> points;
            points.ensure_capacity(shape.points.length);
            for (size_t i = 0; i < shape.points.length; ++i)
                points.unchecked_append({ adopt(shape.points.pointer[i].x), adopt(shape.points.pointer[i].y) });
            return Polygon { static_cast<Gfx::WindingRule>(shape.fill_rule), move(points) };
        }
        case 6: {
            auto path_string = Utf16String::from_raw(shape.path_string.raw);
            return Path { static_cast<Gfx::WindingRule>(shape.fill_rule), SVG::parse_path_data(path_string) };
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }())
{
}

BasicShape const& BasicShapeStyleValue::basic_shape() const
{
    return m_shape;
}

Gfx::Path Path::to_path(CSSPixelRect) const
{
    auto result = path_instructions.to_gfx_path();
    result.set_fill_type(fill_rule);
    return result;
}

BasicShapeStyleValue::~BasicShapeStyleValue() = default;

// https://www.w3.org/TR/css-shapes-1/#basic-shape-computed-values
ValueComparingNonnullRefPtr<StyleValue const> BasicShapeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    // The values in a <basic-shape> function are computed as specified, with these exceptions:
    // - Omitted values are included and compute to their defaults.
    // FIXME: - A <position> value in circle() or ellipse() is computed as a pair of offsets (horizontal then vertical) from the top left origin, each given as a <length-percentage>.
    // FIXME: - A <'border-radius'> value in a <basic-shape-rect> function is computed as an expanded list of all eight <length-percentage> values.
    // - All <basic-shape-rect> functions compute to the equivalent inset() function.

    CalculationContext calculation_context { .percentages_resolve_as = ValueType::Length };

    auto const one_hundred_percent_minus = [&](Vector<NonnullRefPtr<StyleValue const>> const& values, CalculationContext const& calculation_context) {
        Vector<CalcNodeRef> sum_components;
        sum_components.append(CalcNodeRef::numeric(Percentage { 100 }));

        for (auto const& value : values)
            sum_components.append(CalcNodeRef::negate(CalcNodeRef::from_style_value(value)));

        return CalculatedStyleValue::create(CalcNodeRef::sum(move(sum_components)), NumericType { NumericType::BaseType::Length, 1 }, calculation_context);
    };

    auto const absolutize_if_nonnull = [&](RefPtr<StyleValue const> const& value) -> ValueComparingRefPtr<StyleValue const> {
        if (!value)
            return nullptr;
        return value->absolutized(computation_context);
    };

    auto absolutized_shape = basic_shape().visit(
        [&](Inset const& shape) -> BasicShape {
            auto absolutized_top = shape.top->absolutized(computation_context);
            auto absolutized_right = shape.right->absolutized(computation_context);
            auto absolutized_bottom = shape.bottom->absolutized(computation_context);
            auto absolutized_left = shape.left->absolutized(computation_context);

            auto absolutized_border_radius = shape.border_radius->absolutized(computation_context);

            if (absolutized_top == shape.top && absolutized_right == shape.right && absolutized_bottom == shape.bottom && absolutized_left == shape.left && absolutized_border_radius == shape.border_radius)
                return shape;

            return Inset { absolutized_top, absolutized_right, absolutized_bottom, absolutized_left, absolutized_border_radius };
        },
        [&](Xywh const& shape) -> BasicShape {
            // Note: Given xywh(x y w h), the equivalent function is inset(y calc(100% - x - w) calc(100% - y - h) x).
            auto absolutized_top = shape.y->absolutized(computation_context);
            auto absolutized_right = one_hundred_percent_minus({ shape.x, shape.width }, calculation_context)->absolutized(computation_context);
            auto absolutized_bottom = one_hundred_percent_minus({ shape.y, shape.height }, calculation_context)->absolutized(computation_context);
            auto absolutized_left = shape.x->absolutized(computation_context);
            auto absolutized_border_radius = shape.border_radius->absolutized(computation_context);

            return Inset { *absolutized_top, *absolutized_right, *absolutized_bottom, *absolutized_left, absolutized_border_radius };
        },
        [&](Rect const& shape) -> BasicShape {
            // Note: Given rect(t r b l), the equivalent function is inset(t calc(100% - r) calc(100% - b) l).

            auto resolve_auto = [](ValueComparingNonnullRefPtr<StyleValue const> const& style_value, Percentage value_of_auto) -> ValueComparingNonnullRefPtr<StyleValue const> {
                // An auto value makes the edge of the box coincide with the corresponding edge of the reference box:
                // it’s equivalent to 0% as the first (top) or fourth (left) value, and equivalent to 100% as the second
                // (right) or third (bottom) value.
                if (style_value->is_keyword()) {
                    VERIFY(style_value->to_keyword() == Keyword::Auto);
                    return PercentageStyleValue::create(value_of_auto);
                }

                return style_value;
            };

            auto absolutized_top = resolve_auto(shape.top, Percentage { 0 })->absolutized(computation_context);
            auto absolutized_right = one_hundred_percent_minus({ resolve_auto(shape.right, Percentage { 100 }) }, calculation_context)->absolutized(computation_context);
            auto absolutized_bottom = one_hundred_percent_minus({ resolve_auto(shape.bottom, Percentage { 100 }) }, calculation_context)->absolutized(computation_context);
            auto absolutized_left = resolve_auto(shape.left, Percentage { 0 })->absolutized(computation_context);
            auto absolutized_border_radius = shape.border_radius->absolutized(computation_context);

            return Inset { *absolutized_top, *absolutized_right, *absolutized_bottom, *absolutized_left, absolutized_border_radius };
        },
        [&](Circle const& shape) -> BasicShape {
            auto absolutized_radius = shape.radius->absolutized(computation_context);
            auto absolutized_position = absolutize_if_nonnull(shape.position);

            if (absolutized_radius == shape.radius && absolutized_position == shape.position)
                return shape;

            return Circle { absolutized_radius, absolutized_position };
        },
        [&](Ellipse const& shape) -> BasicShape {
            auto absolutized_radius = shape.radius->absolutized(computation_context);
            auto absolutized_position = absolutize_if_nonnull(shape.position);

            if (absolutized_radius == shape.radius && absolutized_position == shape.position)
                return shape;

            return Ellipse { absolutized_radius, absolutized_position };
        },
        [&](Polygon const& shape) -> BasicShape {
            Vector<Polygon::Point> absolutized_points;
            absolutized_points.ensure_capacity(shape.points.size());

            bool any_point_required_absolutization = false;

            for (auto const& point : shape.points) {
                auto absolutized_x = point.x->absolutized(computation_context);
                auto absolutized_y = point.y->absolutized(computation_context);

                if (absolutized_x == point.x && absolutized_y == point.y) {
                    absolutized_points.append(point);
                    continue;
                }

                any_point_required_absolutization = true;
                absolutized_points.append({ absolutized_x, absolutized_y });
            }

            if (!any_point_required_absolutization)
                return shape;

            return Polygon { shape.fill_rule, absolutized_points };
        },
        [&](Path const& shape) -> BasicShape {
            return shape;
        });

    if (absolutized_shape == basic_shape())
        return *this;

    return BasicShapeStyleValue::create(absolutized_shape);
}

}
