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

StyleValueFFI::StyleValueData const* RadialGradientStyleValue::make_radial_gradient_data(EndingShape ending_shape, NonnullRefPtr<StyleValue const> const& size, NonnullRefPtr<PositionStyleValue const> const& position, Vector<ColorStopListElement> const& color_stop_list, GradientRepeating repeating, RefPtr<StyleValue const> const& color_interpolation_method, ColorSyntax color_syntax)
{
    // The Rust allocation takes ownership of one strong reference to each non-null value.
    auto stops = retain_color_stops_for_rust(color_stop_list);
    return StyleValueFFI::rust_style_value_create_radial_gradient(
        static_cast<u8>(to_underlying(ending_shape)),
        StyleValueFFI::rust_style_value_retain(size->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(position->rust_style_value_data()),
        stops.data(), stops.size(), repeating == GradientRepeating::Yes,
        color_interpolation_method ? StyleValueFFI::rust_style_value_retain(color_interpolation_method->rust_style_value_data()) : nullptr,
        to_underlying(color_syntax));
}

RadialGradientStyleValue::RadialGradientStyleValue(StyleValueFFI::StyleValueData const* data)
    : AbstractImageStyleValue(Type::RadialGradient, data)
{
}

CSSPixelSize RadialGradientStyleValue::resolve_size(CSSPixelPoint center, CSSPixelRect const& reference_box) const
{
    if (ending_shape() == EndingShape::Circle) {
        auto radius = size_value()->as_radial_size().resolve_circle_size(center, reference_box);
        return CSSPixelSize { radius, radius };
    }

    return size_value()->as_radial_size().resolve_ellipse_size(center, reference_box);
}

ResolvedImage RadialGradientStyleValue::resolve_for_size(Layout::NodeWithStyle const& node, CSSPixelSize paint_size) const
{
    CSSPixelRect gradient_box { { 0, 0 }, paint_size };
    auto center = position_value()->resolved(gradient_box);
    auto gradient_size = resolve_size(center, gradient_box);

    return Painting::ResolvedRadialGradient {
        Painting::resolve_radial_gradient_data(node, gradient_size, *this),
        gradient_size,
        center,
    };
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

void RadialGradientStyleValue::paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering, PreferredColorScheme, ResolvedImage const& resolved_image) const
{
    auto const& resolved = resolved_image.get<Painting::ResolvedRadialGradient>();
    auto center = context.rounded_device_point(resolved.center).to_type<int>();
    auto size = context.rounded_device_size(resolved.gradient_size).to_type<int>();
    context.display_list_recorder().fill_rect_with_radial_gradient(dest_rect.to_type<int>(), resolved.data, center, size);
}

}
