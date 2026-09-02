/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LinearGradientStyleValue.h"

namespace Web::CSS {

// These discriminants cross the style value FFI as raw codes; the Rust serializer's tables
// depend on them.
static_assert(to_underlying(SideOrCorner::Top) == 0);
static_assert(to_underlying(SideOrCorner::Bottom) == 1);
static_assert(to_underlying(SideOrCorner::Left) == 2);
static_assert(to_underlying(SideOrCorner::Right) == 3);
static_assert(to_underlying(SideOrCorner::TopLeft) == 4);
static_assert(to_underlying(SideOrCorner::TopRight) == 5);
static_assert(to_underlying(SideOrCorner::BottomLeft) == 6);
static_assert(to_underlying(SideOrCorner::BottomRight) == 7);
static_assert(to_underlying(LinearGradientStyleValue::GradientType::Standard) == 0);
static_assert(to_underlying(LinearGradientStyleValue::GradientType::WebKit) == 1);
static_assert(to_underlying(ColorSyntax::Legacy) == 0);
static_assert(to_underlying(ColorSyntax::Modern) == 1);

StyleValueFFI::StyleValueData const* LinearGradientStyleValue::make_linear_gradient_data(GradientDirection const& direction, Vector<ColorStopListElement> const& color_stop_list, GradientType type, GradientRepeating repeating, RefPtr<StyleValue const> const& color_interpolation_method, ColorSyntax color_syntax)
{
    // The Rust allocation takes ownership of one strong reference to each non-null value.
    auto stops = retain_color_stops_for_rust(color_stop_list);
    bool has_direction_value = direction.has<NonnullRefPtr<StyleValue const>>();
    StyleValueFFI::StyleValueData const* direction_value = nullptr;
    u8 side_or_corner = 0;
    if (has_direction_value)
        direction_value = StyleValueFFI::rust_style_value_retain(direction.get<NonnullRefPtr<StyleValue const>>()->rust_style_value_data());
    else
        side_or_corner = to_underlying(direction.get<SideOrCorner>());
    return StyleValueFFI::rust_style_value_create_linear_gradient(
        has_direction_value, direction_value, side_or_corner,
        stops.data(), stops.size(),
        static_cast<u8>(to_underlying(type)), repeating == GradientRepeating::Yes,
        color_interpolation_method ? StyleValueFFI::rust_style_value_retain(color_interpolation_method->rust_style_value_data()) : nullptr,
        to_underlying(color_syntax));
}

LinearGradientStyleValue::LinearGradientStyleValue(StyleValueFFI::StyleValueData const* data)
    : AbstractImageStyleValue(Type::LinearGradient, data)
{
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

Optional<Painting::ImagePaint> LinearGradientStyleValue::image_paint(Painting::ImagePaintRequest const&) const
{
    return Painting::ImagePaint { Painting::ImagePaint::Gradient { *this } };
}

}
