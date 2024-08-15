/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>

namespace Web::CSS {

String ShadowStyleValue::to_string() const
{
    StringBuilder builder;
    builder.appendff("{} {} {} {} {}", m_properties.color->to_string(), m_properties.offset_x->to_string(), m_properties.offset_y->to_string(), m_properties.blur_radius->to_string(), m_properties.spread_distance->to_string());
    if (m_properties.placement == ShadowPlacement::Inner)
        builder.append(" inset"sv);
    return MUST(builder.to_string());
}

ValueComparingNonnullRefPtr<CSSStyleValue const> ShadowStyleValue::absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    auto absolutized_offset_x = m_properties.offset_x->absolutized(viewport_rect, font_metrics, root_font_metrics);
    auto absolutized_offset_y = m_properties.offset_y->absolutized(viewport_rect, font_metrics, root_font_metrics);
    auto absolutized_blur_radius = m_properties.blur_radius->absolutized(viewport_rect, font_metrics, root_font_metrics);
    auto absolutized_spread_distance = m_properties.spread_distance->absolutized(viewport_rect, font_metrics, root_font_metrics);
    return ShadowStyleValue::create(m_properties.color, absolutized_offset_x, absolutized_offset_y, absolutized_blur_radius, absolutized_spread_distance, m_properties.placement);
}

}
