/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FilterValueListStyleValue.h"
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

float Filter::Blur::resolved_radius(Layout::Node const& node) const
{
    // Default value when omitted is 0px.
    auto sigma = 0;
    if (radius.has_value())
        sigma = radius->to_px(node).to_int();
    // Note: The radius/sigma of the blur needs to be doubled for LibGfx's blur functions.
    return sigma * 2;
}

float Filter::HueRotate::angle_degrees() const
{
    // Default value when omitted is 0deg.
    if (!angle.has_value())
        return 0.0f;
    return angle->visit([&](Angle const& a) { return a.to_degrees(); }, [&](auto) { return 0.0; });
}

float Filter::Color::resolved_amount() const
{
    if (amount.has_value()) {
        if (amount->is_percentage())
            return amount->percentage().as_fraction();
        return amount->number().value();
    }
    // All color filters (brightness, sepia, etc) have a default amount of 1.
    return 1.0f;
}

String FilterValueListStyleValue::to_string() const
{
    StringBuilder builder {};
    bool first = true;
    for (auto& filter_function : filter_value_list()) {
        if (!first)
            builder.append(' ');
        filter_function.visit(
            [&](Filter::Blur const& blur) {
                builder.append("blur("sv);
                if (blur.radius.has_value())
                    builder.append(blur.radius->to_string());
            },
            [&](Filter::DropShadow const& drop_shadow) {
                builder.appendff("drop-shadow({} {}"sv,
                    drop_shadow.offset_x, drop_shadow.offset_y);
                if (drop_shadow.radius.has_value())
                    builder.appendff(" {}", drop_shadow.radius->to_string());
                if (drop_shadow.color.has_value()) {
                    builder.append(' ');
                    serialize_a_srgb_value(builder, *drop_shadow.color);
                }
            },
            [&](Filter::HueRotate const& hue_rotate) {
                builder.append("hue-rotate("sv);
                if (hue_rotate.angle.has_value()) {
                    hue_rotate.angle->visit(
                        [&](Angle const& angle) {
                            return builder.append(angle.to_string());
                        },
                        [&](auto&) {
                            return builder.append('0');
                        });
                }
            },
            [&](Filter::Color const& color) {
                builder.appendff("{}(",
                    [&] {
                        switch (color.operation) {
                        case Filter::Color::Operation::Brightness:
                            return "brightness"sv;
                        case Filter::Color::Operation::Contrast:
                            return "contrast"sv;
                        case Filter::Color::Operation::Grayscale:
                            return "grayscale"sv;
                        case Filter::Color::Operation::Invert:
                            return "invert"sv;
                        case Filter::Color::Operation::Opacity:
                            return "opacity"sv;
                        case Filter::Color::Operation::Saturate:
                            return "saturate"sv;
                        case Filter::Color::Operation::Sepia:
                            return "sepia"sv;
                        default:
                            VERIFY_NOT_REACHED();
                        }
                    }());
                if (color.amount.has_value())
                    builder.append(color.amount->to_string());
            });
        builder.append(')');
        first = false;
    }
    return MUST(builder.to_string());
}

}
