/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>

namespace Web::CSS {

void ShadowStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (m_properties.color) {
        m_properties.color->serialize(builder, mode);
        builder.append(' ');
    }

    m_properties.offset_x->serialize(builder, mode);
    builder.append(' ');
    m_properties.offset_y->serialize(builder, mode);

    if (m_properties.blur_radius) {
        builder.append(' ');
        m_properties.blur_radius->serialize(builder, mode);
    }

    if (m_properties.spread_distance && m_properties.shadow_type == ShadowType::Normal) {
        builder.append(' ');
        m_properties.spread_distance->serialize(builder, mode);
    }

    if (m_properties.placement == ShadowPlacement::Inner)
        builder.append(" inset"sv);
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::color() const
{
    if (!m_properties.color)
        return KeywordStyleValue::create(Keyword::Currentcolor);
    return *m_properties.color;
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::blur_radius() const
{
    if (!m_properties.blur_radius)
        return LengthStyleValue::create(Length::make_px(0));
    return *m_properties.blur_radius;
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::spread_distance() const
{
    if (!m_properties.spread_distance)
        return LengthStyleValue::create(Length::make_px(0));
    return *m_properties.spread_distance;
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_offset_x = offset_x()->absolutized(computation_context);
    auto absolutized_offset_y = offset_y()->absolutized(computation_context);
    auto absolutized_blur_radius = blur_radius()->absolutized(computation_context);
    auto absolutized_spread_distance = spread_distance()->absolutized(computation_context);
    return create(m_properties.shadow_type, color(), absolutized_offset_x, absolutized_offset_y, absolutized_blur_radius, absolutized_spread_distance, placement());
}

}
