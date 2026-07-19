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

StyleValueFFI::StyleValueData* RadialGradientStyleValue::make_radial_gradient_data(EndingShape ending_shape, NonnullRefPtr<StyleValue const> const& size, NonnullRefPtr<PositionStyleValue const> const& position, Vector<ColorStopListElement> const& color_stop_list, GradientRepeating repeating, RefPtr<StyleValue const> const& color_interpolation_method, ColorSyntax color_syntax)
{
    // The Rust allocation takes ownership of one strong reference to each non-null value.
    auto stops = retain_color_stops_for_rust(color_stop_list);
    return StyleValueFFI::rust_style_value_create_radial_gradient(
        static_cast<u8>(to_underlying(ending_shape)),
        retain_style_value_for_rust(size.ptr()), retain_style_value_for_rust(position.ptr()),
        stops.data(), stops.size(), repeating == GradientRepeating::Yes,
        retain_style_value_for_rust(color_interpolation_method.ptr()), to_underlying(color_syntax));
}

void RadialGradientStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.append("radial-gradient("sv);

    // AD-HOC: We need to check the serialized size to determine if it should be included.
    auto const& serialized_size = size_value()->to_string(mode);

    bool has_size = serialized_size != "farthest-corner"sv;
    bool has_position = !position_value()->is_center(mode);
    bool has_color_space = color_interpolation_method_value() && color_interpolation_method_value()->as_color_interpolation_method().color_interpolation_method() != ColorInterpolationMethodStyleValue::default_color_interpolation_method(gradient_color_syntax());

    if (has_size)
        size_value()->serialize(builder, mode);

    if (has_position) {
        if (has_size)
            builder.append(' ');

        builder.append("at "sv);
        position_value()->serialize(builder, mode);
    }

    if (has_color_space) {
        if (has_size || has_position)
            builder.append(' ');

        color_interpolation_method_value()->serialize(builder, mode);
    }

    if (has_size || has_position || has_color_space)
        builder.append(", "sv);

    serialize_color_stop_list(builder, color_stop_list(), mode);
    builder.append(')');
}

CSSPixelSize RadialGradientStyleValue::resolve_size(CSSPixelPoint center, CSSPixelRect const& reference_box) const
{
    if (ending_shape() == EndingShape::Circle) {
        auto radius = size_value()->as_radial_size().resolve_circle_size(center, reference_box);
        return CSSPixelSize { radius, radius };
    }

    return size_value()->as_radial_size().resolve_ellipse_size(center, reference_box);
}

void RadialGradientStyleValue::resolve_for_size(Layout::NodeWithStyle const& node, CSSPixelSize paint_size) const
{
    CSSPixelRect gradient_box { { 0, 0 }, paint_size };
    auto center = position_value()->resolved(gradient_box);
    auto gradient_size = resolve_size(center, gradient_box);

    if (m_resolved_size != paint_size) {
        m_resolved_size = move(paint_size);
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
    absolutized_color_stops.ensure_capacity(color_stop_list().size());
    for (auto const& color_stop : color_stop_list()) {
        absolutized_color_stops.unchecked_append(color_stop.absolutized(context));
    }

    auto absolutized_size = size_value()->absolutized(context);
    NonnullRefPtr absolutized_position = position_value()->absolutized(context)->as_position();

    auto absolutized_color_interpolation_method = color_interpolation_method_value() ? ValueComparingRefPtr<StyleValue const> { color_interpolation_method_value()->absolutized(context) } : nullptr;

    return create(ending_shape(), move(absolutized_size), move(absolutized_position), move(absolutized_color_stops), (is_repeating() ? GradientRepeating::Yes : GradientRepeating::No), move(absolutized_color_interpolation_method));
}

bool RadialGradientStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto& other_gradient = other.as_radial_gradient();
    return ending_shape() == other_gradient.ending_shape()
        && size_value() == other_gradient.size_value()
        && position_value() == other_gradient.position_value()
        && color_stop_list() == other_gradient.color_stop_list()
        && is_repeating() == other_gradient.is_repeating()
        && color_interpolation_method_value() == other_gradient.color_interpolation_method_value()
        && gradient_color_syntax() == other_gradient.gradient_color_syntax();
}

void RadialGradientStyleValue::paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering) const
{
    VERIFY(m_resolved.has_value());
    auto center = context.rounded_device_point(m_resolved->center).to_type<int>();
    auto size = context.rounded_device_size(m_resolved->gradient_size).to_type<int>();
    context.display_list_recorder().fill_rect_with_radial_gradient(dest_rect.to_type<int>(), m_resolved->data, center, size);
}

}
