/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConicGradientStyleValue.h"
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>

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
{
}

Optional<Painting::ImagePaint> ConicGradientStyleValue::image_paint(Painting::ImagePaintRequest const&) const
{
    return Painting::ImagePaint { Painting::ImagePaint::Gradient { *this } };
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

}
