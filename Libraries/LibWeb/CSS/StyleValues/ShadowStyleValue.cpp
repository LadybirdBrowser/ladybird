/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>

namespace Web::CSS {

String ShadowStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    if (m_properties.color)
        builder.append(m_properties.color->to_string(mode));

    if (!builder.is_empty())
        builder.append(' ');
    builder.appendff("{} {}", m_properties.offset_x->to_string(mode), m_properties.offset_y->to_string(mode));

    auto append_value = [&](ValueComparingRefPtr<CSSStyleValue const> const& value) {
        if (!value)
            return;
        if (!builder.is_empty())
            builder.append(' ');
        builder.append(value->to_string(mode));
    };
    append_value(m_properties.blur_radius);
    append_value(m_properties.spread_distance);

    if (m_properties.placement == ShadowPlacement::Inner)
        builder.append(" inset"sv);
    return MUST(builder.to_string());
}

ValueComparingNonnullRefPtr<CSSStyleValue const> ShadowStyleValue::color() const
{
    if (!m_properties.color)
        return CSSKeywordValue::create(Keyword::Currentcolor);
    return *m_properties.color;
}

ValueComparingNonnullRefPtr<CSSStyleValue const> ShadowStyleValue::blur_radius() const
{
    if (!m_properties.blur_radius)
        return LengthStyleValue::create(Length::make_px(0));
    return *m_properties.blur_radius;
}

ValueComparingNonnullRefPtr<CSSStyleValue const> ShadowStyleValue::spread_distance() const
{
    if (!m_properties.spread_distance)
        return LengthStyleValue::create(Length::make_px(0));
    return *m_properties.spread_distance;
}

ValueComparingNonnullRefPtr<CSSStyleValue const> ShadowStyleValue::absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    auto absolutized_offset_x = offset_x()->absolutized(viewport_rect, font_metrics, root_font_metrics);
    auto absolutized_offset_y = offset_y()->absolutized(viewport_rect, font_metrics, root_font_metrics);
    auto absolutized_blur_radius = blur_radius()->absolutized(viewport_rect, font_metrics, root_font_metrics);
    auto absolutized_spread_distance = spread_distance()->absolutized(viewport_rect, font_metrics, root_font_metrics);
    return create(color(), absolutized_offset_x, absolutized_offset_y, absolutized_blur_radius, absolutized_spread_distance, placement());
}

}
