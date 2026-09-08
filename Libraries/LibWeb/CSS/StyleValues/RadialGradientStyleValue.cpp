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

Optional<Painting::ImagePaint> RadialGradientStyleValue::image_paint(Painting::ImagePaintRequest const&) const
{
    return Painting::ImagePaint { Painting::ImagePaint::Gradient { *this } };
}

}
