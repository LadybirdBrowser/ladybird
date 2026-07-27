/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConicGradientStyleValue.h"
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::CSS {

StyleValueFFI::StyleValueData const* ConicGradientStyleValue::make_conic_gradient_data(RefPtr<StyleValue const> const& from_angle, NonnullRefPtr<PositionStyleValue const> const& position, Vector<ColorStopListElement> const& color_stop_list, GradientRepeating repeating, RefPtr<StyleValue const> const& color_interpolation_method, ColorSyntax color_syntax)
{
    // The Rust allocation takes ownership of one strong reference to each non-null value.
    auto stops = retain_color_stops_for_rust(color_stop_list);
    return StyleValueFFI::rust_style_value_create_conic_gradient(
        from_angle ? StyleValueFFI::rust_style_value_retain(from_angle->rust_style_value_data()) : nullptr,
        StyleValueFFI::rust_style_value_retain(position->rust_style_value_data()),
        stops.data(), stops.size(), repeating == GradientRepeating::Yes,
        color_interpolation_method ? StyleValueFFI::rust_style_value_retain(color_interpolation_method->rust_style_value_data()) : nullptr,
        to_underlying(color_syntax));
}

ConicGradientStyleValue::ConicGradientStyleValue(StyleValueFFI::StyleValueData const* data)
    : AbstractImageStyleValue(Type::ConicGradient, data)
    , m_from_angle([&]() -> ValueComparingRefPtr<StyleValue const> {
        auto const* angle_data = static_cast<StyleValueFFI::StyleValueData const*>(data->conic_gradient.from_angle.pointer);
        if (!angle_data)
            return nullptr;
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(angle_data));
    }())
    , m_position(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                                                             static_cast<StyleValueFFI::StyleValueData const*>(data->conic_gradient.position.pointer)))
              ->as_position())
    , m_color_interpolation_method([&]() -> ValueComparingRefPtr<StyleValue const> {
        auto const* method_data = static_cast<StyleValueFFI::StyleValueData const*>(data->conic_gradient.color_interpolation_method.pointer);
        if (!method_data)
            return nullptr;
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(method_data));
    }())
{
}

void ConicGradientStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (is_repeating())
        builder.append("repeating-"sv);
    builder.append("conic-gradient("sv);
    bool has_from_angle = from_angle_value();
    bool has_at_position = !position_value()->is_center(mode);
    bool has_color_space = color_interpolation_method_value() && color_interpolation_method_value()->as_color_interpolation_method().color_interpolation_method() != ColorInterpolationMethodStyleValue::default_color_interpolation_method(gradient_color_syntax());

    if (has_from_angle) {
        builder.append("from "sv);
        from_angle_value()->serialize(builder, mode);
    }
    if (has_at_position) {
        if (has_from_angle)
            builder.append(' ');
        builder.append("at "sv);
        position_value()->serialize(builder, mode);
    }
    if (has_color_space) {
        if (has_from_angle || has_at_position)
            builder.append(' ');
        color_interpolation_method_value()->serialize(builder, mode);
    }
    if (has_from_angle || has_at_position || has_color_space)
        builder.append(", "sv);
    serialize_color_stop_list(builder, color_stop_list(), mode);
    builder.append(')');
}

void ConicGradientStyleValue::resolve_for_size(Layout::NodeWithStyle const& node, CSSPixelSize size) const
{
    if (m_resolved_size != size) {
        m_resolved_size = size;
        m_resolved = ResolvedData { Painting::resolve_conic_gradient_data(node, *this), {} };
    }
    m_resolved->position = position_value()->resolved(CSSPixelRect { { 0, 0 }, size });
}

void ConicGradientStyleValue::paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering, PreferredColorScheme) const
{
    VERIFY(m_resolved.has_value());
    auto destination_rect = dest_rect.to_type<int>();
    auto position = context.rounded_device_point(m_resolved->position).to_type<int>();
    context.display_list_recorder().fill_rect_with_conic_gradient(destination_rect, m_resolved->data, position);
}

ValueComparingNonnullRefPtr<StyleValue const> ConicGradientStyleValue::absolutized(ComputationContext const& context) const
{
    Vector<ColorStopListElement> absolutized_color_stops;
    absolutized_color_stops.ensure_capacity(color_stop_list().size());
    for (auto const& color_stop : color_stop_list()) {
        absolutized_color_stops.unchecked_append(color_stop.absolutized(context));
    }
    RefPtr<StyleValue const> absolutized_from_angle;
    if (from_angle_value())
        absolutized_from_angle = from_angle_value()->absolutized(context);
    ValueComparingNonnullRefPtr<PositionStyleValue const> absolutized_position = position_value()->absolutized(context)->as_position();

    auto absolutized_color_interpolation_method = color_interpolation_method_value() ? ValueComparingRefPtr<StyleValue const> { color_interpolation_method_value()->absolutized(context) } : nullptr;

    return create(move(absolutized_from_angle), move(absolutized_position), move(absolutized_color_stops), (is_repeating() ? GradientRepeating::Yes : GradientRepeating::No), move(absolutized_color_interpolation_method), gradient_color_syntax());
}

bool ConicGradientStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto& other_gradient = other.as_conic_gradient();
    return from_angle_value() == other_gradient.from_angle_value()
        && position_value() == other_gradient.position_value()
        && color_stop_list() == other_gradient.color_stop_list()
        && is_repeating() == other_gradient.is_repeating()
        && color_interpolation_method_value() == other_gradient.color_interpolation_method_value()
        && gradient_color_syntax() == other_gradient.gradient_color_syntax();
}

float ConicGradientStyleValue::angle_degrees() const
{
    if (!from_angle_value())
        return 0;
    return Angle::from_style_value(*from_angle_value(), {}).to_degrees();
}

}
