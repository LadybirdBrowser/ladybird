/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FilterValueListStyleValue.h"
#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

float FilterOperation::Blur::resolved_radius() const
{
    return Length::from_style_value(radius, {}).absolute_length_to_px_without_rounding();
}

float FilterOperation::HueRotate::angle_degrees() const
{
    return Angle::from_style_value(angle, {}).to_degrees();
}

float FilterOperation::Color::resolved_amount() const
{
    return number_from_style_value(amount, 1);
}

void FilterValueListStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    bool first = true;
    for (auto& filter_function : filter_value_list()) {
        if (!first)
            builder.append(' ');
        filter_function.visit(
            [&](FilterOperation::Blur const& blur) {
                builder.append("blur("sv);
                blur.radius->serialize(builder, mode);
                builder.append(')');
            },
            [&](FilterOperation::DropShadow const& drop_shadow) {
                builder.append("drop-shadow("sv);
                if (drop_shadow.color) {
                    drop_shadow.color->serialize(builder, mode);
                    builder.append(' ');
                }
                builder.appendff("{} {}", drop_shadow.offset_x, drop_shadow.offset_y);
                if (drop_shadow.radius.has_value()) {
                    builder.append(' ');
                    drop_shadow.radius->serialize(builder, mode);
                }
                builder.append(')');
            },
            [&](FilterOperation::HueRotate const& hue_rotate) {
                builder.append("hue-rotate("sv);
                hue_rotate.angle->serialize(builder, mode);
                builder.append(')');
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

                color.amount->serialize(builder, mode);
                builder.append(')');
            },
            [&](CSS::URL const& url) {
                builder.append(url.to_string());
            });
        first = false;
    }
}

bool FilterValueListStyleValue::contains_url() const
{
    for (auto const& filter_value : m_filter_value_list) {
        if (filter_value.has<URL>())
            return true;
    }
    return false;
}

ValueComparingNonnullRefPtr<StyleValue const> FilterValueListStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
    auto const& length_resolution_context = computation_context.length_resolution_context;

    auto absolutize_length = [&](LengthOrCalculated const& length) -> LengthOrCalculated {
        if (length.is_calculated()) {
            if (auto resolved = length.resolved(resolution_context); resolved.has_value())
                return LengthOrCalculated { Length::make_px(resolved->to_px(length_resolution_context)) };
            return length;
        }
        if (auto absolutized = length.value().absolutize(length_resolution_context); absolutized.has_value())
            return LengthOrCalculated { absolutized.release_value() };
        return length;
    };

    Vector<FilterValue> absolutized_filter_values;
    absolutized_filter_values.ensure_capacity(m_filter_value_list.size());

    for (auto const& filter_value : m_filter_value_list) {
        filter_value.visit(
            [&](FilterOperation::Blur const& blur) {
                absolutized_filter_values.append(FilterOperation::Blur {
                    .radius = blur.radius->absolutized(computation_context),
                });
            },
            [&](FilterOperation::DropShadow const& drop_shadow) {
                absolutized_filter_values.append(FilterOperation::DropShadow {
                    .offset_x = absolutize_length(drop_shadow.offset_x),
                    .offset_y = absolutize_length(drop_shadow.offset_y),
                    .radius = drop_shadow.radius.map([&](auto const& r) { return absolutize_length(r); }),
                    .color = drop_shadow.color ? ValueComparingRefPtr<StyleValue const> { drop_shadow.color->absolutized(computation_context) } : nullptr,
                });
            },
            [&](FilterOperation::HueRotate const& hue_rotate) {
                absolutized_filter_values.append(FilterOperation::HueRotate {
                    .angle = hue_rotate.angle->absolutized(computation_context),
                });
            },
            [&](FilterOperation::Color const& color) {
                absolutized_filter_values.append(FilterOperation::Color {
                    .operation = color.operation,
                    .amount = NumberStyleValue::create(number_from_style_value(color.amount->absolutized(computation_context), 1)),
                });
            },
            [&](CSS::URL const& url) {
                absolutized_filter_values.append(url);
            });
    }

    return FilterValueListStyleValue::create(move(absolutized_filter_values));
}

}
