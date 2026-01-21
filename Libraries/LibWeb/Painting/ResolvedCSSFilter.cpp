/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

}
