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

void FilterValueListStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    bool first = true;
    for (auto& filter_function : filter_value_list()) {
        if (!first)
            builder.append(' ');
        filter_function.visit(
            [&](FilterOperation::Blur const& blur) {
                builder.append("blur("sv);
                blur.radius.serialize(builder, mode);
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
            },
            [&](FilterOperation::HueRotate const& hue_rotate) {
                builder.append("hue-rotate("sv);
                hue_rotate.angle.visit(
                    [&](AngleOrCalculated const& angle) {
                        angle.serialize(builder, mode);
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

                color.amount.serialize(builder, mode);
            },
            [&](CSS::URL const& url) {
                builder.append(url.to_string());
            });
        builder.append(')');
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

    auto absolutize_angle = [&](AngleOrCalculated const& angle) -> AngleOrCalculated {
        if (angle.is_calculated()) {
            if (auto resolved = angle.resolved(resolution_context); resolved.has_value())
                return AngleOrCalculated { Angle::make_degrees(resolved->to_degrees()) };
            return angle;
        }
        return AngleOrCalculated { Angle::make_degrees(angle.value().to_degrees()) };
    };

    Vector<FilterValue> absolutized_filter_values;
    absolutized_filter_values.ensure_capacity(m_filter_value_list.size());

    for (auto const& filter_value : m_filter_value_list) {
        filter_value.visit(
            [&](FilterOperation::Blur const& blur) {
                absolutized_filter_values.append(FilterOperation::Blur {
                    .radius = absolutize_length(blur.radius),
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
                auto absolutized_angle = hue_rotate.angle.visit(
                    [&](AngleOrCalculated const& angle) -> FilterOperation::HueRotate::AngleOrZero {
                        return absolutize_angle(angle);
                    },
                    [&](FilterOperation::HueRotate::Zero) -> FilterOperation::HueRotate::AngleOrZero {
                        return AngleOrCalculated { Angle::make_degrees(0) };
                    });
                absolutized_filter_values.append(FilterOperation::HueRotate {
                    .angle = absolutized_angle,
                });
            },
            [&](FilterOperation::Color const& color) {
                Optional<double> resolved_value;

                if (color.amount.is_calculated()) {
                    auto const& calc = color.amount.calculated();
                    if (calc->resolves_to_number()) {
                        resolved_value = calc->resolve_number(resolution_context);
                    } else if (calc->resolves_to_percentage()) {
                        if (auto resolved = calc->resolve_percentage(resolution_context); resolved.has_value())
                            resolved_value = resolved->as_fraction();
                    }
                } else if (color.amount.is_percentage()) {
                    resolved_value = color.amount.percentage().as_fraction();
                }

                if (resolved_value.has_value()) {
                    auto clamped_value = [&] {
                        switch (color.operation) {
                        case Gfx::ColorFilterType::Grayscale:
                        case Gfx::ColorFilterType::Invert:
                        case Gfx::ColorFilterType::Opacity:
                        case Gfx::ColorFilterType::Sepia:
                            return clamp(*resolved_value, 0.0, 1.0);
                        case Gfx::ColorFilterType::Brightness:
                        case Gfx::ColorFilterType::Contrast:
                        case Gfx::ColorFilterType::Saturate:
                            return max(*resolved_value, 0.0);
                        }
                        VERIFY_NOT_REACHED();
                    }();

                    absolutized_filter_values.append(FilterOperation::Color {
                        .operation = color.operation,
                        .amount = NumberPercentage { Number { Number::Type::Number, clamped_value } },
                    });
                    return;
                }

                absolutized_filter_values.append(color);
            },
            [&](CSS::URL const& url) {
                absolutized_filter_values.append(url);
            });
    }

    return FilterValueListStyleValue::create(move(absolutized_filter_values));
}

}
