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
    return radius.resolved({ .length_resolution_context = Length::ResolutionContext::for_layout_node(node) })->to_px(node).to_float();
}

float FilterOperation::HueRotate::angle_degrees(Layout::Node const& node) const
{
    return angle.visit([&](AngleOrCalculated const& a) { return a.resolved({ .length_resolution_context = Length::ResolutionContext::for_layout_node(node) })->to_degrees(); }, [&](Zero) { return 0.0; });
}

float FilterOperation::Color::resolved_amount() const
{
    if (amount.is_number())
        return amount.number().value();

    if (amount.is_percentage())
        return amount.percentage().as_fraction();

    if (amount.is_calculated()) {
        CalculationResolutionContext context {};
        if (amount.calculated()->resolves_to_number())
            return amount.calculated()->resolve_number(context).value();

        if (amount.calculated()->resolves_to_percentage())
            return amount.calculated()->resolve_percentage(context)->as_fraction();
    }

    VERIFY_NOT_REACHED();
}

String FilterValueListStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder {};
    bool first = true;
    for (auto& filter_function : filter_value_list()) {
        if (!first)
            builder.append(' ');
        filter_function.visit(
            [&](FilterOperation::Blur const& blur) {
                builder.appendff("blur({}", blur.radius.to_string(mode));
            },
            [&](FilterOperation::DropShadow const& drop_shadow) {
                builder.append("drop-shadow("sv);
                if (drop_shadow.color.has_value()) {
                    serialize_a_srgb_value(builder, *drop_shadow.color);
                    builder.append(' ');
                }
                builder.appendff("{} {}", drop_shadow.offset_x, drop_shadow.offset_y);
                if (drop_shadow.radius.has_value())
                    builder.appendff(" {}", drop_shadow.radius->to_string(mode));
            },
            [&](FilterOperation::HueRotate const& hue_rotate) {
                builder.append("hue-rotate("sv);
                hue_rotate.angle.visit(
                    [&](AngleOrCalculated const& angle) {
                        builder.append(angle.to_string(mode));
                    },
                    [&](FilterOperation::HueRotate::Zero const&) {
                        builder.append("0deg"sv);
                    });
            },
            [&](FilterOperation::Color const& color) {
                builder.appendff("{}(",
                    [&] {
                        switch (color.operation) {
                        case Gfx::ColorFilterType::Brightness:
                            return "brightness"sv;
                        case Gfx::ColorFilterType::Contrast:
                            return "contrast"sv;
                        case Gfx::ColorFilterType::Grayscale:
                            return "grayscale"sv;
                        case Gfx::ColorFilterType::Invert:
                            return "invert"sv;
                        case Gfx::ColorFilterType::Opacity:
                            return "opacity"sv;
                        case Gfx::ColorFilterType::Saturate:
                            return "saturate"sv;
                        case Gfx::ColorFilterType::Sepia:
                            return "sepia"sv;
                        default:
                            VERIFY_NOT_REACHED();
                        }
                    }());

                builder.append(color.amount.to_string(mode));
            },
            [&](CSS::URL const& url) {
                builder.append(url.to_string());
            });
        builder.append(')');
        first = false;
    }
    return MUST(builder.to_string());
}

bool FilterValueListStyleValue::contains_url() const
{
    for (auto const& filter_value : m_filter_value_list) {
        if (filter_value.has<URL>())
            return true;
    }
    return false;
}

}
