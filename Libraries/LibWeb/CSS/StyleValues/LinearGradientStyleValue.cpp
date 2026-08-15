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

ResolvedImage LinearGradientStyleValue::resolve_for_size(Layout::NodeWithStyle const& node, CSSPixelSize size) const
{
    return Painting::resolve_linear_gradient_data(node, size, *this);
}

void LinearGradientStyleValue::paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering, PreferredColorScheme, ResolvedImage const& resolved) const
{
    context.display_list_recorder().fill_rect_with_linear_gradient(dest_rect.to_type<int>(), resolved.get<Painting::LinearGradientData>());
}

}
