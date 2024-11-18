/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RadialGradientStyleValue.h"
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

String RadialGradientStyleValue::to_string() const
{
    StringBuilder builder;
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.appendff("radial-gradient({} "sv,
        m_properties.ending_shape == EndingShape::Circle ? "circle"sv : "ellipse"sv);

    m_properties.size.visit(
        [&](Extent extent) {
            builder.append([&] {
                switch (extent) {
                case Extent::ClosestCorner:
                    return "closest-corner"sv;
                case Extent::ClosestSide:
                    return "closest-side"sv;
                case Extent::FarthestCorner:
                    return "farthest-corner"sv;
                case Extent::FarthestSide:
                    return "farthest-side"sv;
                default:
                    VERIFY_NOT_REACHED();
                }
            }());
        },
        [&](CircleSize const& circle_size) {
            builder.append(circle_size.radius.to_string());
        },
        [&](EllipseSize const& ellipse_size) {
            builder.appendff("{} {}", ellipse_size.radius_a.to_string(), ellipse_size.radius_b.to_string());
        });

    if (!m_properties.position->is_center())
        builder.appendff(" at {}"sv, m_properties.position->to_string());

    builder.append(", "sv);
    serialize_color_stop_list(builder, m_properties.color_stop_list);
    builder.append(')');
    return MUST(builder.to_string());
}

CSSPixelSize RadialGradientStyleValue::resolve_size(Layout::Node const& node, CSSPixelPoint center, CSSPixelRect const& size) const
{
    auto const side_shape = [&](auto distance_function) {
        auto const distance_from = [&](CSSPixels v, CSSPixels a, CSSPixels b, auto distance_function) {
            return distance_function(abs(a - v), abs(b - v));
        };
        auto x_dist = distance_from(center.x(), size.left(), size.right(), distance_function);
        auto y_dist = distance_from(center.y(), size.top(), size.bottom(), distance_function);
        if (m_properties.ending_shape == EndingShape::Circle) {
            auto dist = distance_function(x_dist, y_dist);
            return CSSPixelSize { dist, dist };
        } else {
            return CSSPixelSize { x_dist, y_dist };
        }
    };

    auto const closest_side_shape = [&] {
        return side_shape(AK::min<CSSPixels>);
    };

    auto const farthest_side_shape = [&] {
        return side_shape(AK::max<CSSPixels>);
    };

    auto const corner_distance = [&](auto distance_compare, CSSPixelPoint& corner) {
        auto top_left_distance_squared = square_distance_between(size.top_left(), center);
        auto top_right_distance_squared = square_distance_between(size.top_right(), center);
        auto bottom_right_distance_squared = square_distance_between(size.bottom_right(), center);
        auto bottom_left_distance_squared = square_distance_between(size.bottom_left(), center);
        auto distance_squared = top_left_distance_squared;
        corner = size.top_left();
        if (distance_compare(top_right_distance_squared, distance_squared)) {
            corner = size.top_right();
            distance_squared = top_right_distance_squared;
        }
        if (distance_compare(bottom_right_distance_squared, distance_squared)) {
            corner = size.bottom_right();
            distance_squared = bottom_right_distance_squared;
        }
        if (distance_compare(bottom_left_distance_squared, distance_squared)) {
            corner = size.bottom_left();
            distance_squared = bottom_left_distance_squared;
        }
        return sqrt(distance_squared);
    };

    auto const closest_corner_distance = [&](CSSPixelPoint& corner) {
        return corner_distance([](CSSPixels a, CSSPixels b) { return a < b; }, corner);
    };

    auto const farthest_corner_distance = [&](CSSPixelPoint& corner) {
        return corner_distance([](CSSPixels a, CSSPixels b) { return a > b; }, corner);
    };

    auto const corner_shape = [&](auto corner_distance, auto get_shape) {
        CSSPixelPoint corner {};
        auto distance = corner_distance(corner);
        if (m_properties.ending_shape == EndingShape::Ellipse) {
            auto shape = get_shape();
            CSSPixels height = shape.height();
            CSSPixels width = shape.width();

            // Prevent division by zero
            // https://w3c.github.io/csswg-drafts/css-images/#degenerate-radials
            if (height == 0) {
                // Render as if the ending shape was an ellipse whose width was an arbitrary very large number and whose height
                // was an arbitrary very small number greater than zero. This will make the gradient look like a solid-color image equal
                // to the color of the last color-stop, or equal to the average color of the gradient if it’s repeating.
                constexpr auto arbitrary_small_number = CSSPixels::smallest_positive_value();
                constexpr auto arbitrary_large_number = CSSPixels::max();
                return CSSPixelSize { arbitrary_large_number, arbitrary_small_number };
            }

            auto aspect_ratio = width / height;

            auto p = corner - center;
            auto radius_a = sqrt(p.y() * p.y() * aspect_ratio * aspect_ratio + p.x() * p.x());
            auto radius_b = radius_a / aspect_ratio;
            return CSSPixelSize { radius_a, radius_b };
        }
        return CSSPixelSize { distance, distance };
    };

    // https://w3c.github.io/csswg-drafts/css-images/#radial-gradient-syntax
    auto resolved_size = m_properties.size.visit(
        [&](Extent extent) {
            switch (extent) {
            case Extent::ClosestSide:
                // The ending shape is sized so that it exactly meets the side of the gradient box closest to the gradient’s center.
                // If the shape is an ellipse, it exactly meets the closest side in each dimension.
                return closest_side_shape();
            case Extent::ClosestCorner:
                // The ending shape is sized so that it passes through the corner of the gradient box closest to the gradient’s center.
                // If the shape is an ellipse, the ending shape is given the same aspect-ratio it would have if closest-side were specified
                return corner_shape(closest_corner_distance, closest_side_shape);
            case Extent::FarthestCorner:
                // Same as closest-corner, except the ending shape is sized based on the farthest corner.
                // If the shape is an ellipse, the ending shape is given the same aspect ratio it would have if farthest-side were specified.
                return corner_shape(farthest_corner_distance, farthest_side_shape);
            case Extent::FarthestSide:
                // Same as closest-side, except the ending shape is sized based on the farthest side(s).
                return farthest_side_shape();
            default:
                VERIFY_NOT_REACHED();
            }
        },
        [&](CircleSize const& circle_size) {
            auto radius = circle_size.radius.to_px(node);
            return CSSPixelSize { radius, radius };
        },
        [&](EllipseSize const& ellipse_size) {
            auto radius_a = ellipse_size.radius_a.resolved(node, size.width()).to_px(node);
            auto radius_b = ellipse_size.radius_b.resolved(node, size.height()).to_px(node);
            return CSSPixelSize { radius_a, radius_b };
        });

    // Handle degenerate cases
    // https://w3c.github.io/csswg-drafts/css-images/#degenerate-radials

    constexpr auto arbitrary_small_number = CSSPixels::smallest_positive_value();
    constexpr auto arbitrary_large_number = CSSPixels::max();

    // If the ending shape is a circle with zero radius:
    if (m_properties.ending_shape == EndingShape::Circle && resolved_size.is_empty()) {
        // Render as if the ending shape was a circle whose radius was an arbitrary very small number greater than zero.
        // This will make the gradient continue to look like a circle.
        return CSSPixelSize { arbitrary_small_number, arbitrary_small_number };
    }
    // If the ending shape has zero width (regardless of the height):
    if (resolved_size.width() <= 0) {
        // Render as if the ending shape was an ellipse whose height was an arbitrary very large number
        // and whose width was an arbitrary very small number greater than zero.
        // This will make the gradient look similar to a horizontal linear gradient that is mirrored across the center of the ellipse.
        // It also means that all color-stop positions specified with a percentage resolve to 0px.
        return CSSPixelSize { arbitrary_small_number, arbitrary_large_number };
    }
    // Otherwise, if the ending shape has zero height:
    if (resolved_size.height() <= 0) {
        // Render as if the ending shape was an ellipse whose width was an arbitrary very large number and whose height
        // was an arbitrary very small number greater than zero. This will make the gradient look like a solid-color image equal
        // to the color of the last color-stop, or equal to the average color of the gradient if it’s repeating.
        return CSSPixelSize { arbitrary_large_number, arbitrary_small_number };
    }
    return resolved_size;
}

void RadialGradientStyleValue::resolve_for_size(Layout::NodeWithStyleAndBoxModelMetrics const& node, CSSPixelSize paint_size) const
{
    CSSPixelRect gradient_box { { 0, 0 }, paint_size };
    auto center = m_properties.position->resolved(node, gradient_box);
    auto gradient_size = resolve_size(node, center, gradient_box);
    if (m_resolved.has_value() && m_resolved->gradient_size == gradient_size)
        return;
    m_resolved = ResolvedData {
        Painting::resolve_radial_gradient_data(node, gradient_size, *this),
        gradient_size,
        center,
    };
}

bool RadialGradientStyleValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto& other_gradient = other.as_radial_gradient();
    return m_properties == other_gradient.m_properties;
}

void RadialGradientStyleValue::paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering) const
{
    VERIFY(m_resolved.has_value());
    auto center = context.rounded_device_point(m_resolved->center).to_type<int>();
    auto size = context.rounded_device_size(m_resolved->gradient_size).to_type<int>();
    context.display_list_recorder().fill_rect_with_radial_gradient(dest_rect.to_type<int>(), m_resolved->data, center, size);
}

}
