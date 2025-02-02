/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConicGradientStyleValue.h"
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::CSS {

String ConicGradientStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.append("conic-gradient("sv);
    bool has_from_angle = m_properties.from_angle.to_degrees() != 0;
    bool has_at_position = !m_properties.position->is_center();
    if (has_from_angle)
        builder.appendff("from {}", m_properties.from_angle.to_string());
    if (has_at_position) {
        if (has_from_angle)
            builder.append(' ');
        builder.appendff("at {}"sv, m_properties.position->to_string(mode));
    }
    if (has_from_angle || has_at_position)
        builder.append(", "sv);
    serialize_color_stop_list(builder, m_properties.color_stop_list, mode);
    builder.append(')');
    return MUST(builder.to_string());
}

void ConicGradientStyleValue::resolve_for_size(Layout::NodeWithStyleAndBoxModelMetrics const& node, CSSPixelSize size) const
{
    if (!m_resolved.has_value())
        m_resolved = ResolvedData { Painting::resolve_conic_gradient_data(node, *this), {} };
    m_resolved->position = m_properties.position->resolved(node, CSSPixelRect { { 0, 0 }, size });
}

void ConicGradientStyleValue::paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering) const
{
    VERIFY(m_resolved.has_value());
    auto destination_rect = dest_rect.to_type<int>();
    auto position = context.rounded_device_point(m_resolved->position).to_type<int>();
    context.display_list_recorder().fill_rect_with_conic_gradient(destination_rect, m_resolved->data, position);
}

bool ConicGradientStyleValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto& other_gradient = other.as_conic_gradient();
    return m_properties == other_gradient.m_properties;
}

float ConicGradientStyleValue::angle_degrees() const
{
    return m_properties.from_angle.to_degrees();
}

}
