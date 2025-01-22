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

float FilterOperation::Blur::resolved_radius(Layout::Node const& node) const
{
    if (radius.has_value())
        return radius->resolved(Length::ResolutionContext::for_layout_node(node)).to_px(node).to_float();

    // Default value when omitted is 0px.
    return 0;
}

float FilterOperation::HueRotate::angle_degrees(Layout::Node const& node) const
{
    // Default value when omitted is 0deg.
    if (!angle.has_value())
        return 0.0f;
    return angle->visit([&](AngleOrCalculated const& a) { return a.resolved(node).to_degrees(); }, [&](Zero) { return 0.0; });
}

float FilterOperation::Color::resolved_amount() const
{
    // Default value when omitted is 1.
    if (!amount.has_value())
        return 1;

    if (amount->is_number())
        return amount->number().value();

    if (amount->is_percentage())
        return amount->percentage().as_fraction();

    if (amount->is_calculated()) {
        CalculationResolutionContext context {};
        if (amount->calculated()->resolves_to_number())
            return amount->calculated()->resolve_number(context).value();

        if (amount->calculated()->resolves_to_percentage())
            return amount->calculated()->resolve_percentage(context)->as_fraction();
    }

    VERIFY_NOT_REACHED();
}

String FilterValueListStyleValue::to_string(SerializationMode) const
{
    StringBuilder builder {};
    bool first = true;
    for (auto& filter_function : filter_value_list()) {
        if (!first)
            builder.append(' ');
        filter_function.visit(
            [&](FilterOperation::Blur const& blur) {
                builder.append("blur("sv);
                if (blur.radius.has_value())
                    builder.append(blur.radius->to_string());
            },
            [&](FilterOperation::DropShadow const& drop_shadow) {
                builder.appendff("drop-shadow({} {}"sv,
                    drop_shadow.offset_x, drop_shadow.offset_y);
                if (drop_shadow.radius.has_value())
                    builder.appendff(" {}", drop_shadow.radius->to_string());
                if (drop_shadow.color.has_value()) {
                    builder.append(' ');
                    serialize_a_srgb_value(builder, *drop_shadow.color);
                }
            },
            [&](FilterOperation::HueRotate const& hue_rotate) {
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
            [&](FilterOperation::Color const& color) {
                builder.appendff("{}(",
                    [&] {
                        switch (color.operation) {
                        case Gfx::ColorFilter::Type::Brightness:
                            return "brightness"sv;
                        case Gfx::ColorFilter::Type::Contrast:
                            return "contrast"sv;
                        case Gfx::ColorFilter::Type::Grayscale:
                            return "grayscale"sv;
                        case Gfx::ColorFilter::Type::Invert:
                            return "invert"sv;
                        case Gfx::ColorFilter::Type::Opacity:
                            return "opacity"sv;
                        case Gfx::ColorFilter::Type::Saturate:
                            return "saturate"sv;
                        case Gfx::ColorFilter::Type::Sepia:
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
