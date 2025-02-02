/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LinearGradientStyleValue.h"
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
            return "top left"sv;
        case SideOrCorner::TopRight:
            return "top right"sv;
        case SideOrCorner::BottomLeft:
            return "bottom left"sv;
        case SideOrCorner::BottomRight:
            return "bottom right"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    if (m_properties.gradient_type == GradientType::WebKit)
        builder.append("-webkit-"sv);
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.append("linear-gradient("sv);
    m_properties.direction.visit(
        [&](SideOrCorner side_or_corner) {
            return builder.appendff("{}{}, "sv, m_properties.gradient_type == GradientType::Standard ? "to "sv : ""sv, side_or_corner_to_string(side_or_corner));
        },
        [&](Angle const& angle) {
            return builder.appendff("{}, "sv, angle.to_string());
        });

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

void LinearGradientStyleValue::resolve_for_size(Layout::NodeWithStyleAndBoxModelMetrics const& node, CSSPixelSize size) const
{
    if (m_resolved.has_value() && m_resolved->size == size)
        return;
    m_resolved = ResolvedData { Painting::resolve_linear_gradient_data(node, size, *this), size };
}

void LinearGradientStyleValue::paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering) const
{
    VERIFY(m_resolved.has_value());
    context.display_list_recorder().fill_rect_with_linear_gradient(dest_rect.to_type<int>(), m_resolved->data);
}

}
