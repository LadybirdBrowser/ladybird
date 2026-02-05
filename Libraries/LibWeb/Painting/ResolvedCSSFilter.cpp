/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>

namespace Web::Painting {

Optional<Gfx::Filter> to_gfx_filter(ResolvedCSSFilter const& css_filter, double device_pixels_per_css_pixel)
{
    Optional<Gfx::Filter> resolved_filter;

    for (auto const& operation : css_filter.operations) {
        operation.visit(
            [&](ResolvedCSSFilter::Blur const& blur) {
                auto resolved_radius = static_cast<float>(blur.radius.to_double() * device_pixels_per_css_pixel);
                auto new_filter = Gfx::Filter::blur(resolved_radius, resolved_radius);

                resolved_filter = resolved_filter.has_value()
                    ? Gfx::Filter::compose(new_filter, *resolved_filter)
                    : new_filter;
            },
            [&](ResolvedCSSFilter::DropShadow const& drop_shadow) {
                auto offset_x = static_cast<float>(drop_shadow.offset_x.to_double() * device_pixels_per_css_pixel);
                auto offset_y = static_cast<float>(drop_shadow.offset_y.to_double() * device_pixels_per_css_pixel);
                auto radius = static_cast<float>(drop_shadow.radius.to_double() * device_pixels_per_css_pixel);
                auto new_filter = Gfx::Filter::drop_shadow(offset_x, offset_y, radius, drop_shadow.color);

                resolved_filter = resolved_filter.has_value()
                    ? Gfx::Filter::compose(new_filter, *resolved_filter)
                    : new_filter;
            },
            [&](ResolvedCSSFilter::Color const& color) {
                auto new_filter = Gfx::Filter::color(color.operation, color.amount);

                resolved_filter = resolved_filter.has_value()
                    ? Gfx::Filter::compose(new_filter, *resolved_filter)
                    : new_filter;
            },
            [&](ResolvedCSSFilter::HueRotate const& hue_rotate) {
                auto new_filter = Gfx::Filter::hue_rotate(hue_rotate.angle_degrees);

                resolved_filter = resolved_filter.has_value()
                    ? Gfx::Filter::compose(new_filter, *resolved_filter)
                    : new_filter;
            });
    }

    // SVG filters are already resolved in device pixels
    if (css_filter.svg_filter.has_value()) {
        resolved_filter = resolved_filter.has_value()
            ? Gfx::Filter::compose(*css_filter.svg_filter, *resolved_filter)
            : *css_filter.svg_filter;
    }

    return resolved_filter;
}

void ResolvedCSSFilter::dump(StringBuilder& builder) const
{
    if (!operations.is_empty()) {
        builder.append("filters=("sv);
        bool first = true;
        for (auto const& op : operations) {
            if (!first)
                builder.append(", "sv);
            first = false;
            op.visit(
                [&](Blur const& blur) {
                    builder.appendff("blur({})", blur.radius.to_float());
                },
                [&](DropShadow const& shadow) {
                    builder.appendff("drop-shadow({} {} {} {})", shadow.offset_x.to_float(), shadow.offset_y.to_float(), shadow.radius.to_float(), shadow.color.to_string());
                },
                [&](Color const& color) {
                    StringView name;
                    switch (color.operation) {
                    case Gfx::ColorFilterType::Brightness:
                        name = "brightness"sv;
                        break;
                    case Gfx::ColorFilterType::Contrast:
                        name = "contrast"sv;
                        break;
                    case Gfx::ColorFilterType::Grayscale:
                        name = "grayscale"sv;
                        break;
                    case Gfx::ColorFilterType::Invert:
                        name = "invert"sv;
                        break;
                    case Gfx::ColorFilterType::Opacity:
                        name = "opacity"sv;
                        break;
                    case Gfx::ColorFilterType::Saturate:
                        name = "saturate"sv;
                        break;
                    case Gfx::ColorFilterType::Sepia:
                        name = "sepia"sv;
                        break;
                    }
                    builder.appendff("{}({})", name, color.amount);
                },
                [&](HueRotate const& hue) {
                    builder.appendff("hue-rotate({}deg)", hue.angle_degrees);
                });
        }
        builder.append(")"sv);
    }
    if (svg_filter.has_value())
        builder.append(" svg_filter"sv);
}

}
