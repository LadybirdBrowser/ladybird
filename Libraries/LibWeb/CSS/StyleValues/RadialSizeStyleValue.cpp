/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RadialSizeStyleValue.h"
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> RadialSizeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    bool any_component_required_absolutization = false;
    Vector<Component> absolutized_components;

    for (auto const& component : m_components) {
        if (component.has<RadialExtent>()) {
            absolutized_components.append(component);
        } else {
            auto const& absolutized_length_percentage = component.get<NonnullRefPtr<StyleValue const>>()->absolutized(computation_context);

            if (!absolutized_length_percentage->equals(component.get<NonnullRefPtr<StyleValue const>>()))
                any_component_required_absolutization = true;

            absolutized_components.append(absolutized_length_percentage);
        }
    }

    if (!any_component_required_absolutization)
        return *this;

    return RadialSizeStyleValue::create(move(absolutized_components));
}

String RadialSizeStyleValue::to_string(SerializationMode serialization_mode) const
{
    StringBuilder builder;

    for (auto const& component : m_components) {
        if (!builder.is_empty())
            builder.append(' ');

        component.visit(
            [&](RadialExtent extent) {
                builder.append(CSS::to_string(extent));
            },
            [&](NonnullRefPtr<StyleValue const> const& length_percentage) {
                builder.append(length_percentage->to_string(serialization_mode));
            });
    }

    return builder.to_string_without_validation();
}

static CSSPixelSize side_shape(CSSPixelPoint const& center, CSSPixelRect const& reference_box, Function<CSSPixels(CSSPixels, CSSPixels)> distance_function)
{
    auto x_dist = distance_function(abs(reference_box.left() - center.x()), abs(reference_box.right() - center.x()));
    auto y_dist = distance_function(abs(reference_box.top() - center.y()), abs(reference_box.bottom() - center.y()));

    return CSSPixelSize { x_dist, y_dist };
}

static CSSPixelSize closest_side_shape(CSSPixelPoint const& center, CSSPixelRect const& size)
{
    return side_shape(center, size, AK::min<CSSPixels>);
}

static CSSPixelSize farthest_side_shape(CSSPixelPoint const& center, CSSPixelRect const& size)
{
    return side_shape(center, size, AK::max<CSSPixels>);
}

static CSSPixels corner_distance(CSSPixelPoint const& center, CSSPixelRect const& reference_box, Function<bool(CSSPixels, CSSPixels)> distance_compare_function, CSSPixelPoint& corner)
{
    auto top_left_distance_squared = square_distance_between(reference_box.top_left(), center);
    auto top_right_distance_squared = square_distance_between(reference_box.top_right(), center);
    auto bottom_right_distance_squared = square_distance_between(reference_box.bottom_right(), center);
    auto bottom_left_distance_squared = square_distance_between(reference_box.bottom_left(), center);
    auto distance_squared = top_left_distance_squared;
    corner = reference_box.top_left();
    if (distance_compare_function(top_right_distance_squared, distance_squared)) {
        corner = reference_box.top_right();
        distance_squared = top_right_distance_squared;
    }
    if (distance_compare_function(bottom_right_distance_squared, distance_squared)) {
        corner = reference_box.bottom_right();
        distance_squared = bottom_right_distance_squared;
    }
    if (distance_compare_function(bottom_left_distance_squared, distance_squared)) {
        corner = reference_box.bottom_left();
        distance_squared = bottom_left_distance_squared;
    }
    return sqrt(distance_squared);
}

static CSSPixels closest_corner_distance(CSSPixelPoint const& center, CSSPixelRect const& reference_box, CSSPixelPoint& corner)
{
    return corner_distance(center, reference_box, [](CSSPixels a, CSSPixels b) { return a < b; }, corner);
}

static CSSPixels farthest_corner_distance(CSSPixelPoint const& center, CSSPixelRect const& reference_box, CSSPixelPoint& corner)
{
    return corner_distance(center, reference_box, [](CSSPixels a, CSSPixels b) { return a > b; }, corner);
}

CSSPixels RadialSizeStyleValue::resolve_circle_size(CSSPixelPoint const& center, CSSPixelRect const& reference_box, Layout::Node const& node) const
{
    VERIFY(m_components.size() == 1);

    auto resolved_size = m_components[0].visit(
        [&](RadialExtent const& radial_extent) {
            switch (radial_extent) {
            case RadialExtent::ClosestSide: {
                auto side_distances = closest_side_shape(center, reference_box);
                return AK::min(side_distances.width(), side_distances.height());
            }
            case RadialExtent::FarthestSide: {
                auto side_distances = farthest_side_shape(center, reference_box);
                return AK::max(side_distances.width(), side_distances.height());
            }
            case RadialExtent::ClosestCorner: {
                CSSPixelPoint corner {};
                return closest_corner_distance(center, reference_box, corner);
            }
            case RadialExtent::FarthestCorner: {
                CSSPixelPoint corner {};
                return farthest_corner_distance(center, reference_box, corner);
            }
            }

            VERIFY_NOT_REACHED();
        },
        [&](NonnullRefPtr<StyleValue const> const& length_percentage) {
            auto radius_ref = sqrt(pow(reference_box.width().to_float(), 2) + pow(reference_box.height().to_float(), 2)) / AK::Sqrt2<float>;
            // FIXME: We don't need to pass `node` here since we know that all relative lengths have already been absolutized
            return CSSPixels::nearest_value_for(max(0.0f, LengthPercentage::from_style_value(length_percentage).to_px(node, CSSPixels::nearest_value_for(radius_ref)).to_float()));
        });

    // https://w3c.github.io/csswg-drafts/css-images/#degenerate-radials
    // If the ending shape is a circle with zero radius:
    if (resolved_size == 0) {
        // Render as if the ending shape was a circle whose radius was an arbitrary very small number greater than zero.
        // This will make the gradient continue to look like a circle.
        return CSSPixels::smallest_positive_value();
    }

    return resolved_size;
}

static CSSPixelSize ellipse_corner_shape(CSSPixelPoint const& center, CSSPixelRect const& reference_box, Function<CSSPixels(CSSPixelPoint const&, CSSPixelRect const&, CSSPixelPoint&)> get_corner, Function<CSSPixelSize(CSSPixelPoint const&, CSSPixelRect const&)> get_shape)
{
    CSSPixelPoint corner {};
    get_corner(center, reference_box, corner);

    auto shape = get_shape(center, reference_box);
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

CSSPixelSize RadialSizeStyleValue::resolve_ellipse_size(CSSPixelPoint const& center, CSSPixelRect const& reference_box, Layout::Node const& node) const
{
    VERIFY(m_components.size() == 1 || m_components.size() == 2);

    auto const resolve_component = [&](Component const& component, CSSPixels const& reference_size) -> CSSPixelSize {
        return component.visit(
            [&](RadialExtent const& radial_extent) {
                switch (radial_extent) {
                case RadialExtent::ClosestSide:
                    return closest_side_shape(center, reference_box);
                case RadialExtent::FarthestSide:
                    return farthest_side_shape(center, reference_box);
                case RadialExtent::ClosestCorner:
                    return ellipse_corner_shape(center, reference_box, closest_corner_distance, closest_side_shape);
                case RadialExtent::FarthestCorner:
                    return ellipse_corner_shape(center, reference_box, farthest_corner_distance, farthest_side_shape);
                }

                VERIFY_NOT_REACHED();
            },
            [&](NonnullRefPtr<StyleValue const> const& length_percentage) {
                // FIXME: We don't need to pass `node` here since we know that all relative lengths have already been absolutized
                auto value = LengthPercentage::from_style_value(length_percentage).to_px(node, reference_size);

                return CSSPixelSize { value, value };
            });
    };

    CSSPixelSize resolved_size = CSSPixelSize {
        resolve_component(m_components[0], reference_box.width()).width(),
        resolve_component(m_components.size() == 1 ? m_components[0] : m_components[1], reference_box.height()).height()
    };

    // Handle degenerate cases
    // https://w3c.github.io/csswg-drafts/css-images/#degenerate-radials
    constexpr auto arbitrary_small_number = CSSPixels::smallest_positive_value();
    constexpr auto arbitrary_large_number = CSSPixels::max();

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

}
