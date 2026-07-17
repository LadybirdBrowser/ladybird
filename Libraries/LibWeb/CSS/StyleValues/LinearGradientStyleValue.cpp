/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LinearGradientStyleValue.h"
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::CSS {

StyleValueFFI::StyleValueData* LinearGradientStyleValue::make_linear_gradient_data(GradientDirection const& direction, Vector<ColorStopListElement> const& color_stop_list, GradientType type, GradientRepeating repeating, RefPtr<StyleValue const> const& color_interpolation_method, ColorSyntax color_syntax)
{
    // The Rust allocation takes ownership of one strong reference to each non-null value.
    auto stops = retain_color_stops_for_rust(color_stop_list);
    bool has_direction_value = direction.has<NonnullRefPtr<StyleValue const>>();
    void const* direction_value = nullptr;
    u8 side_or_corner = 0;
    if (has_direction_value)
        direction_value = retain_style_value_for_rust(direction.get<NonnullRefPtr<StyleValue const>>().ptr());
    else
        side_or_corner = to_underlying(direction.get<SideOrCorner>());
    return StyleValueFFI::rust_style_value_create_linear_gradient(
        has_direction_value, direction_value, side_or_corner,
        stops.data(), stops.size(),
        static_cast<u8>(to_underlying(type)), repeating == GradientRepeating::Yes,
        retain_style_value_for_rust(color_interpolation_method.ptr()), to_underlying(color_syntax));
}

void LinearGradientStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
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

    // NB: Materialize the direction and interpolation method once instead of per use.
    auto direction = this->direction();
    auto color_interpolation_method_value = this->color_interpolation_method_value();

    auto default_direction = gradient_type() == GradientType::WebKit ? SideOrCorner::Top : SideOrCorner::Bottom;
    bool has_direction = direction != default_direction;
    bool has_color_space = color_interpolation_method_value && color_interpolation_method_value->as_color_interpolation_method().color_interpolation_method() != ColorInterpolationMethodStyleValue::default_color_interpolation_method(gradient_color_syntax());

    if (gradient_type() == GradientType::WebKit)
        builder.append("-webkit-"sv);
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.append("linear-gradient("sv);
    if (has_direction) {
        direction.visit(
            [&](SideOrCorner side_or_corner) {
                builder.appendff("{}{}", gradient_type() == GradientType::Standard ? "to "sv : ""sv, side_or_corner_to_string(side_or_corner));
            },
            [&](NonnullRefPtr<StyleValue const> const& angle) {
                angle->serialize(builder, mode);
            });

        if (has_color_space)
            builder.append(' ');
    }

    if (has_color_space)
        color_interpolation_method_value->serialize(builder, mode);

    if (has_direction || has_color_space)
        builder.append(", "sv);

    serialize_color_stop_list(builder, color_stop_list(), mode);
    builder.append(")"sv);
}

ValueComparingNonnullRefPtr<StyleValue const> LinearGradientStyleValue::absolutized(ComputationContext const& context) const
{
    Vector<ColorStopListElement> absolutized_color_stops;
    absolutized_color_stops.ensure_capacity(color_stop_list().size());
    for (auto const& color_stop : color_stop_list()) {
        absolutized_color_stops.unchecked_append(color_stop.absolutized(context));
    }

    auto color_interpolation_method_value = this->color_interpolation_method_value();
    auto absolutized_color_interpolation_method = color_interpolation_method_value ? ValueComparingRefPtr<StyleValue const> { color_interpolation_method_value->absolutized(context) } : nullptr;

    return create(direction(), move(absolutized_color_stops), gradient_type(), (is_repeating() ? GradientRepeating::Yes : GradientRepeating::No), move(absolutized_color_interpolation_method));
}

bool LinearGradientStyleValue::equals(StyleValue const& other_) const
{
    if (type() != other_.type())
        return false;
    auto const& other_gradient = other_.as_linear_gradient();
    return direction() == other_gradient.direction()
        && color_stop_list() == other_gradient.color_stop_list()
        && gradient_type() == other_gradient.gradient_type()
        && is_repeating() == other_gradient.is_repeating()
        && color_interpolation_method_value() == other_gradient.color_interpolation_method_value()
        && gradient_color_syntax() == other_gradient.gradient_color_syntax();
}

float LinearGradientStyleValue::angle_degrees(CSSPixelSize gradient_size) const
{
    auto corner_angle_degrees = [&] {
        return AK::to_degrees(atan2(gradient_size.height().to_double(), gradient_size.width().to_double()));
    };
    return direction().visit(
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
            if (gradient_type() == GradientType::WebKit)
                return angle + 180.0;
            return angle;
        },
        [&](NonnullRefPtr<StyleValue const> const& style_value) {
            auto angle = Angle::from_style_value(style_value, {}).to_degrees();
            // Note: With -webkit-linear-gradient, 0deg points to the right instead of top,
            // and the direction is reversed (counter-clockwise instead of clockwise)
            if (gradient_type() == GradientType::WebKit)
                return 90.0 - angle;
            return angle;
        });
}

void LinearGradientStyleValue::resolve_for_size(Layout::NodeWithStyle const& node, CSSPixelSize size) const
{
    if (m_resolved_size != size) {
        m_resolved_size = move(size);
        m_resolved = Painting::resolve_linear_gradient_data(node, size, *this);
    }
}

void LinearGradientStyleValue::paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering) const
{
    VERIFY(m_resolved.has_value());
    context.display_list_recorder().fill_rect_with_linear_gradient(dest_rect.to_type<int>(), m_resolved.value());
}

}
