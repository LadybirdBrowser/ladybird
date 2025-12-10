/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RadialGradientStyleValue.h"
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::CSS {

String RadialGradientStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.append("radial-gradient("sv);

    auto const& serialized_size = m_properties.size->to_string(mode);

    bool has_size = serialized_size != "farthest-corner"sv;
    bool has_position = !m_properties.position->is_center();
    bool has_color_space = m_properties.interpolation_method.has_value() && m_properties.interpolation_method.value().color_space != InterpolationMethod::default_color_space(m_properties.color_syntax);

    if (has_size)
        builder.append(serialized_size);

    if (has_position) {
        if (has_size)
            builder.append(' ');

        builder.appendff("at {}", m_properties.position->to_string(mode));
    }

    if (has_color_space) {
        if (has_size || has_position)
            builder.append(' ');

        builder.append(m_properties.interpolation_method.value().to_string());
    }

    if (has_size || has_position || has_color_space)
        builder.append(", "sv);

    serialize_color_stop_list(builder, m_properties.color_stop_list, mode);
    builder.append(')');
    return MUST(builder.to_string());
}

CSSPixelSize RadialGradientStyleValue::resolve_size(CSSPixelPoint center, CSSPixelRect const& reference_box, Layout::NodeWithStyle const& node) const
{
    if (m_properties.ending_shape == EndingShape::Circle) {
        auto radius = m_properties.size->as_radial_size().resolve_circle_size(center, reference_box, node);
        return CSSPixelSize { radius, radius };
    }

    return m_properties.size->as_radial_size().resolve_ellipse_size(center, reference_box, node);
}

void RadialGradientStyleValue::resolve_for_size(Layout::NodeWithStyle const& node, CSSPixelSize paint_size) const
{
    CSSPixelRect gradient_box { { 0, 0 }, paint_size };
    auto center = m_properties.position->resolved(node, gradient_box);
    auto gradient_size = resolve_size(center, gradient_box, node);

    ResolvedDataCacheKey cache_key {
        .length_resolution_context = Length::ResolutionContext::for_layout_node(node),
        .size = paint_size,
    };
    if (m_resolved_data_cache_key != cache_key) {
        m_resolved_data_cache_key = move(cache_key);
        m_resolved = ResolvedData {
            Painting::resolve_radial_gradient_data(node, gradient_size, *this),
            gradient_size,
            center,
        };
    }
}

ValueComparingNonnullRefPtr<StyleValue const> RadialGradientStyleValue::absolutized(ComputationContext const& context) const
{
    Vector<ColorStopListElement> absolutized_color_stops;
    absolutized_color_stops.ensure_capacity(m_properties.color_stop_list.size());
    for (auto const& color_stop : m_properties.color_stop_list) {
        absolutized_color_stops.unchecked_append(color_stop.absolutized(context));
    }

    auto absolutized_size = m_properties.size->absolutized(context);
    NonnullRefPtr absolutized_position = m_properties.position->absolutized(context)->as_position();

    return create(m_properties.ending_shape, move(absolutized_size), move(absolutized_position), move(absolutized_color_stops), m_properties.repeating, m_properties.interpolation_method);
}

bool RadialGradientStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto& other_gradient = other.as_radial_gradient();
    return m_properties == other_gradient.m_properties;
}

void RadialGradientStyleValue::paint(DisplayListRecordingContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering) const
{
    VERIFY(m_resolved.has_value());
    auto center = context.rounded_device_point(m_resolved->center).to_type<int>();
    auto size = context.rounded_device_size(m_resolved->gradient_size).to_type<int>();
    context.display_list_recorder().fill_rect_with_radial_gradient(dest_rect.to_type<int>(), m_resolved->data, center, size);
}

}
