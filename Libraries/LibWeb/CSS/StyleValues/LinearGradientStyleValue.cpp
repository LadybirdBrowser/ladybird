/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LinearGradientStyleValue.h"
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::CSS {

String LinearGradientStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    auto side_or_corner_to_string = [](SideOrCorner value) {
        switch (value) {
        case SideOrCorner::Top:
            return "top"sv;
        case SideOrCorner::Bottom:
            return "bottom"sv;
        case SideOrCorner::Left:
            return "left"sv;
        case SideOrCorner::Right:
            return "right"sv;
        case SideOrCorner::TopLeft:
            return "left top"sv;
        case SideOrCorner::TopRight:
            return "right top"sv;
        case SideOrCorner::BottomLeft:
            return "left bottom"sv;
        case SideOrCorner::BottomRight:
            return "right bottom"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    auto default_direction = m_properties.gradient_type == GradientType::WebKit ? SideOrCorner::Top : SideOrCorner::Bottom;
    bool has_direction = m_properties.direction != default_direction;
    bool has_color_space = m_properties.interpolation_method.has_value() && m_properties.interpolation_method.value().color_space != InterpolationMethod::default_color_space(m_properties.color_syntax);

    if (m_properties.gradient_type == GradientType::WebKit)
        builder.append("-webkit-"sv);
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.append("linear-gradient("sv);
    if (has_direction) {
        m_properties.direction.visit(
            [&](SideOrCorner side_or_corner) {
                builder.appendff("{}{}"sv, m_properties.gradient_type == GradientType::Standard ? "to "sv : ""sv, side_or_corner_to_string(side_or_corner));
            },
            [&](Angle const& angle) {
                builder.append(angle.to_string());
            });

        if (has_color_space)
            builder.append(' ');
    }

    if (has_color_space)
        builder.append(m_properties.interpolation_method.value().to_string());

    if (has_direction || has_color_space)
        builder.append(", "sv);

    serialize_color_stop_list(builder, m_properties.color_stop_list, mode);
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool LinearGradientStyleValue::equals(CSSStyleValue const& other_) const
{
    if (type() != other_.type())
        return false;
    auto& other = other_.as_linear_gradient();
    return m_properties == other.m_properties;
}

float LinearGradientStyleValue::angle_degrees(CSSPixelSize gradient_size) const
{
    auto corner_angle_degrees = [&] {
        return AK::to_degrees(atan2(gradient_size.height().to_double(), gradient_size.width().to_double()));
    };
    return m_properties.direction.visit(
        [&](SideOrCorner side_or_corner) {
            auto angle = [&] {
                switch (side_or_corner) {
                case SideOrCorner::Top:
                    return 0.0;
                case SideOrCorner::Bottom:
                    return 180.0;
                case SideOrCorner::Left:
                    return 270.0;
                case SideOrCorner::Right:
                    return 90.0;
                case SideOrCorner::TopRight:
                    return corner_angle_degrees();
                case SideOrCorner::BottomLeft:
                    return corner_angle_degrees() + 180.0;
                case SideOrCorner::TopLeft:
                    return -corner_angle_degrees();
                case SideOrCorner::BottomRight:
                    return -(corner_angle_degrees() + 180.0);
                default:
                    VERIFY_NOT_REACHED();
                }
            }();
            // Note: For unknowable reasons the angles are opposite on the -webkit- version
            if (m_properties.gradient_type == GradientType::WebKit)
                return angle + 180.0;
            return angle;
        },
        [&](Angle const& angle) {
            return angle.to_degrees();
        });
}

void LinearGradientStyleValue::resolve_for_size(Layout::NodeWithStyle const& node, CSSPixelSize size) const
{
    ResolvedDataCacheKey cache_key {
        .length_resolution_context = Length::ResolutionContext::for_layout_node(node),
        .size = size,
    };
    if (m_resolved_data_cache_key != cache_key) {
        m_resolved_data_cache_key = move(cache_key);
        m_resolved = Painting::resolve_linear_gradient_data(node, size, *this);
    }
}

void LinearGradientStyleValue::paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering) const
{
    VERIFY(m_resolved.has_value());
    context.display_list_recorder().fill_rect_with_linear_gradient(dest_rect.to_type<int>(), m_resolved.value());
}

}
