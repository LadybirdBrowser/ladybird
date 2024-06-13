/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

void SVGGradientPaintStyle::set_gradient_transform(Gfx::AffineTransform transform)
{
    // Note: The scaling is removed so enough points on the gradient line are generated.
    // Otherwise, if you scale a tiny path the gradient looks pixelated.
    m_scale = 1.0f;
    if (auto inverse = transform.inverse(); inverse.has_value()) {
        auto transform_scale = transform.scale();
        m_scale = max(transform_scale.x(), transform_scale.y());
        m_inverse_transform = Gfx::AffineTransform {}.scale(m_scale, m_scale).multiply(*inverse);
    } else {
        m_inverse_transform = OptionalNone {};
    }
}

NonnullRefPtr<Gfx::SVGGradientPaintStyle> SVGLinearGradientPaintStyle::create_gfx_paint_style() const
{
    auto gfx_paint_style = adopt_ref(*new Gfx::SVGLinearGradientPaintStyle(m_start_point, m_end_point));

    Vector<Gfx::ColorStop> color_stops;
    for (auto const& color_stop : m_color_stops)
        color_stops.append({ color_stop.color, color_stop.position, color_stop.transition_hint });
    gfx_paint_style->set_color_stops(move(color_stops));

    if (m_repeat_length.has_value())
        gfx_paint_style->set_repeat_length(*m_repeat_length);

    if (m_inverse_transform.has_value())
        gfx_paint_style->set_inverse_transform(*m_inverse_transform);

    gfx_paint_style->set_scale(m_scale);

    auto spread_method = static_cast<Gfx::SVGGradientPaintStyle::SpreadMethod>(to_underlying(m_spread_method));
    gfx_paint_style->set_spread_method(spread_method);

    return gfx_paint_style;
}

NonnullRefPtr<Gfx::SVGGradientPaintStyle> SVGRadialGradientPaintStyle::create_gfx_paint_style() const
{
    auto gfx_paint_style = adopt_ref(*new Gfx::SVGRadialGradientPaintStyle(m_start_center, m_start_radius, m_end_center, m_end_radius));

    Vector<Gfx::ColorStop> color_stops;
    for (auto const& color_stop : m_color_stops)
        color_stops.append({ color_stop.color, color_stop.position, color_stop.transition_hint });
    gfx_paint_style->set_color_stops(move(color_stops));

    if (m_repeat_length.has_value())
        gfx_paint_style->set_repeat_length(*m_repeat_length);

    if (m_inverse_transform.has_value())
        gfx_paint_style->set_inverse_transform(*m_inverse_transform);

    gfx_paint_style->set_scale(m_scale);

    auto spread_method = static_cast<Gfx::SVGGradientPaintStyle::SpreadMethod>(to_underlying(m_spread_method));
    gfx_paint_style->set_spread_method(spread_method);

    return gfx_paint_style;
}

}
