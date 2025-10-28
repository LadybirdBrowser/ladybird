/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BasicShapeStyleValue.h"
#include <LibGfx/Path.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/SVG/Path.h>

namespace Web::CSS {

static Gfx::Path path_from_resolved_rect(float top, float right, float bottom, float left)
{
    Gfx::Path path;
    path.move_to(Gfx::FloatPoint { left, top });
    path.line_to(Gfx::FloatPoint { right, top });
    path.line_to(Gfx::FloatPoint { right, bottom });
    path.line_to(Gfx::FloatPoint { left, bottom });
    path.close();
    return path;
}

Gfx::Path Inset::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    auto resolved_top = LengthPercentageOrAuto::from_style_value(top).to_px_or_zero(node, reference_box.height()).to_float();
    auto resolved_right = LengthPercentageOrAuto::from_style_value(right).to_px_or_zero(node, reference_box.width()).to_float();
    auto resolved_bottom = LengthPercentageOrAuto::from_style_value(bottom).to_px_or_zero(node, reference_box.height()).to_float();
    auto resolved_left = LengthPercentageOrAuto::from_style_value(left).to_px_or_zero(node, reference_box.width()).to_float();

    // A pair of insets in either dimension that add up to more than the used dimension
    // (such as left and right insets of 75% apiece) use the CSS Backgrounds 3 § 4.5 Overlapping Curves rules
    // to proportionally reduce the inset effect to 100%.
    if (resolved_top + resolved_bottom > reference_box.height().to_float() || resolved_left + resolved_right > reference_box.width().to_float()) {
        // https://drafts.csswg.org/css-backgrounds-3/#corner-overlap
        // Let f = min(Li/Si), where i ∈ {top, right, bottom, left}, Si is the sum of the two corresponding radii of the
        // corners on side i, and Ltop = Lbottom = the width of the box, and Lleft = Lright = the height of the box. If
        // f < 1, then all corner radii are reduced by multiplying them by f.

        // NB: We only care about vertical and horizontal here as top = bottom and left = right
        auto s_vertical = resolved_top + resolved_bottom;
        auto s_horizontal = resolved_left + resolved_right;

        auto f = min(reference_box.height() / s_vertical, reference_box.width() / s_horizontal);

        resolved_top *= f;
        resolved_right *= f;
        resolved_bottom *= f;
        resolved_left *= f;
    }

    return path_from_resolved_rect(resolved_top, reference_box.width().to_float() - resolved_right, reference_box.height().to_float() - resolved_bottom, resolved_left);
}

String Inset::to_string(SerializationMode mode) const
{
    return MUST(String::formatted("inset({} {} {} {})", top->to_string(mode), right->to_string(mode), bottom->to_string(mode), left->to_string(mode)));
}

String Xywh::to_string(SerializationMode mode) const
{
    return MUST(String::formatted("xywh({} {} {} {})", x->to_string(mode), y->to_string(mode), width->to_string(mode), height->to_string(mode)));
}

String Rect::to_string(SerializationMode mode) const
{
    return MUST(String::formatted("rect({} {} {} {})", top->to_string(mode), right->to_string(mode), bottom->to_string(mode), left->to_string(mode)));
}

Gfx::Path Circle::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    // Translating the reference box because PositionStyleValues are resolved to an absolute position.
    auto center = position->resolved(node, reference_box.translated(-reference_box.x(), -reference_box.y()));

    auto radius_px = [&]() {
        if (radius->is_keyword()) {
            switch (*keyword_to_fit_side(radius->to_keyword())) {
            case FitSide::ClosestSide:
                float closest;
                closest = min(abs(center.x()), abs(center.y())).to_float();
                closest = min(closest, abs(reference_box.width() - center.x()).to_float());
                closest = min(closest, abs(reference_box.height() - center.y()).to_float());
                return closest;
            case FitSide::FarthestSide:
                float farthest;
                farthest = max(abs(center.x()), abs(center.y())).to_float();
                farthest = max(farthest, abs(reference_box.width() - center.x()).to_float());
                farthest = max(farthest, abs(reference_box.height() - center.y()).to_float());
                return farthest;
            }
            VERIFY_NOT_REACHED();
        }

        auto radius_ref = sqrt(pow(reference_box.width().to_float(), 2) + pow(reference_box.height().to_float(), 2)) / AK::Sqrt2<float>;
        return max(0.0f, LengthPercentage::from_style_value(radius).to_px(node, CSSPixels(radius_ref)).to_float());
    }();

    Gfx::Path path;
    path.move_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_px });
    path.arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() - radius_px }, radius_px, true, true);
    path.arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_px }, radius_px, true, true);
    return path;
}

String Circle::to_string(SerializationMode mode) const
{
    return MUST(String::formatted("circle({} at {})", radius->to_string(mode), position->to_string(mode)));
}

Gfx::Path Ellipse::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    // Translating the reference box because PositionStyleValues are resolved to an absolute position.
    auto center = position->resolved(node, reference_box.translated(-reference_box.x(), -reference_box.y()));

    auto radius_x_px = [&]() {
        if (radius_x->is_keyword()) {
            switch (*keyword_to_fit_side(radius_x->to_keyword())) {
            case FitSide::ClosestSide:
                return min(abs(center.x()), abs(reference_box.width() - center.x())).to_float();
            case FitSide::FarthestSide:
                return max(abs(center.x()), abs(reference_box.width() - center.x())).to_float();
            }
            VERIFY_NOT_REACHED();
        }
        return max(0.0f, LengthPercentage::from_style_value(radius_x).to_px(node, reference_box.width()).to_float());
    }();

    auto radius_y_px = [&]() {
        if (radius_y->is_keyword()) {
            switch (*keyword_to_fit_side(radius_y->to_keyword())) {
            case FitSide::ClosestSide:
                return min(abs(center.y()), abs(reference_box.height() - center.y())).to_float();
            case FitSide::FarthestSide:
                return max(abs(center.y()), abs(reference_box.height() - center.y())).to_float();
            }
            VERIFY_NOT_REACHED();
        }
        return max(0.0f, LengthPercentage::from_style_value(radius_y).to_px(node, reference_box.height()).to_float());
    }();

    Gfx::Path path;
    path.move_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_y_px });
    path.elliptical_arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() - radius_y_px }, Gfx::FloatSize { radius_x_px, radius_y_px }, 0, true, true);
    path.elliptical_arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_y_px }, Gfx::FloatSize { radius_x_px, radius_y_px }, 0, true, true);
    return path;
}

String Ellipse::to_string(SerializationMode mode) const
{
    return MUST(String::formatted("ellipse({} {} at {})", radius_x->to_string(mode), radius_y->to_string(mode), position->to_string(mode)));
}

Gfx::Path Polygon::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    Gfx::Path path;
    path.set_fill_type(fill_rule);
    bool first = true;
    for (auto const& point : points) {
        Gfx::FloatPoint resolved_point {
            LengthPercentage::from_style_value(point.x).to_px(node, reference_box.width()).to_float(),
            LengthPercentage::from_style_value(point.y).to_px(node, reference_box.height()).to_float()
        };
        if (first)
            path.move_to(resolved_point);
        else
            path.line_to(resolved_point);
        first = false;
    }
    path.close();
    return path;
}

String Polygon::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("polygon("sv);
    switch (fill_rule) {
    case Gfx::WindingRule::Nonzero:
        builder.append("nonzero"sv);
        break;
    case Gfx::WindingRule::EvenOdd:
        builder.append("evenodd"sv);
    }
    for (auto const& point : points) {
        builder.appendff(", {} {}", point.x->to_string(mode), point.y->to_string(mode));
    }
    builder.append(')');
    return MUST(builder.to_string());
}

Gfx::Path Path::to_path(CSSPixelRect, Layout::Node const&) const
{
    auto result = path_instructions.to_gfx_path();
    result.set_fill_type(fill_rule);
    return result;
}

// https://drafts.csswg.org/css-shapes/#basic-shape-serialization
String Path::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("path("sv);

    // For serializing computed values, component values are computed, and omitted when possible without changing the meaning.
    // NB: So, we don't include `nonzero` in that case.
    if (!(mode == SerializationMode::ResolvedValue && fill_rule == Gfx::WindingRule::Nonzero)) {
        switch (fill_rule) {
        case Gfx::WindingRule::Nonzero:
            builder.append("nonzero, "sv);
            break;
        case Gfx::WindingRule::EvenOdd:
            builder.append("evenodd, "sv);
        }
    }

    serialize_a_string(builder, path_instructions.serialize());

    builder.append(')');

    return builder.to_string_without_validation();
}

BasicShapeStyleValue::~BasicShapeStyleValue() = default;

Gfx::Path BasicShapeStyleValue::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    return m_basic_shape.visit([&](auto const& shape) -> Gfx::Path {
        // NB: Xywh and Rect don't require to_path functions as we should have already converted them to their
        //     respective Inset equivalents during absolutization
        if constexpr (requires { shape.to_path(reference_box, node); }) {
            return shape.to_path(reference_box, node);
        }

        VERIFY_NOT_REACHED();
    });
}

String BasicShapeStyleValue::to_string(SerializationMode mode) const
{
    return m_basic_shape.visit([mode](auto const& shape) {
        return shape.to_string(mode);
    });
}

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
        Vector<NonnullRefPtr<CalculationNode const>> sum_components = { NumericCalculationNode::create(Percentage { 100 }, calculation_context) };

        for (auto const& value : values)
            sum_components.append(NegateCalculationNode::create(CalculationNode::from_style_value(value, calculation_context)));

        return CalculatedStyleValue::create(SumCalculationNode::create(sum_components), NumericType { NumericType::BaseType::Length, 1 }, calculation_context);
    };

    auto absolutized_shape = m_basic_shape.visit(
        [&](Inset const& shape) -> BasicShape {
            auto absolutized_top = shape.top->absolutized(computation_context);
            auto absolutized_right = shape.right->absolutized(computation_context);
            auto absolutized_bottom = shape.bottom->absolutized(computation_context);
            auto absolutized_left = shape.left->absolutized(computation_context);

            if (absolutized_top == shape.top && absolutized_right == shape.right && absolutized_bottom == shape.bottom && absolutized_left == shape.left)
                return shape;

            return Inset { absolutized_top, absolutized_right, absolutized_bottom, absolutized_left };
        },
        [&](Xywh const& shape) -> BasicShape {
            // Note: Given xywh(x y w h), the equivalent function is inset(y calc(100% - x - w) calc(100% - y - h) x).
            auto absolutized_top = shape.y->absolutized(computation_context);
            auto absolutized_right = one_hundred_percent_minus({ shape.x, shape.width }, calculation_context)->absolutized(computation_context);
            auto absolutized_bottom = one_hundred_percent_minus({ shape.y, shape.height }, calculation_context)->absolutized(computation_context);
            auto absolutized_left = shape.x->absolutized(computation_context);

            return Inset { *absolutized_top, *absolutized_right, *absolutized_bottom, *absolutized_left };
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

            return Inset { *absolutized_top, *absolutized_right, *absolutized_bottom, *absolutized_left };
        },
        [&](Circle const& shape) -> BasicShape {
            auto absolutized_radius = shape.radius->absolutized(computation_context);
            auto absolutized_position = shape.position->absolutized(computation_context);

            if (absolutized_radius == shape.radius && absolutized_position->as_position() == *shape.position)
                return shape;

            return Circle { absolutized_radius, absolutized_position->as_position() };
        },
        [&](Ellipse const& shape) -> BasicShape {
            auto absolutized_radius_x = shape.radius_x->absolutized(computation_context);
            auto absolutized_radius_y = shape.radius_y->absolutized(computation_context);
            auto absolutized_position = shape.position->absolutized(computation_context);

            if (absolutized_radius_x == shape.radius_x && absolutized_radius_y == shape.radius_y && absolutized_position->as_position() == *shape.position)
                return shape;

            return Ellipse { absolutized_radius_x, absolutized_radius_y, absolutized_position->as_position() };
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

    if (absolutized_shape == m_basic_shape)
        return *this;

    return BasicShapeStyleValue::create(absolutized_shape);
}

}
