/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BorderRadiusStyleValue.h"

namespace Web::CSS {

String BorderRadiusStyleValue::to_string() const
{
    if (m_properties.horizontal_radius == m_properties.vertical_radius)
        return m_properties.horizontal_radius.to_string();
    return MUST(String::formatted("{} / {}", m_properties.horizontal_radius.to_string(), m_properties.vertical_radius.to_string()));
}

ValueComparingNonnullRefPtr<CSSStyleValue const> BorderRadiusStyleValue::absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    if (m_properties.horizontal_radius.is_percentage() && m_properties.vertical_radius.is_percentage())
        return *this;
    auto absolutized_horizontal_radius = m_properties.horizontal_radius;
    auto absolutized_vertical_radius = m_properties.vertical_radius;
    if (m_properties.horizontal_radius.is_length())
        absolutized_horizontal_radius = m_properties.horizontal_radius.length().absolutized(viewport_rect, font_metrics, root_font_metrics);
    if (m_properties.vertical_radius.is_length())
        absolutized_vertical_radius = m_properties.vertical_radius.length().absolutized(viewport_rect, font_metrics, root_font_metrics);
    return BorderRadiusStyleValue::create(absolutized_horizontal_radius, absolutized_vertical_radius);
}

}
