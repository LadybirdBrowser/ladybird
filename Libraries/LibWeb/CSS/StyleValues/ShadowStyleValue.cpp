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
    if (color_or_null()) {
        color_or_null()->serialize(builder, mode);
        builder.append(' ');
    }

    offset_x()->serialize(builder, mode);
    builder.append(' ');
    offset_y()->serialize(builder, mode);

    if (blur_radius_or_null()) {
        builder.append(' ');
        blur_radius_or_null()->serialize(builder, mode);
    }

    if (spread_distance_or_null() && shadow_type() == ShadowType::Normal) {
        builder.append(' ');
        spread_distance_or_null()->serialize(builder, mode);
    }

    if (placement() == ShadowPlacement::Inner)
        builder.append(" inset"sv);
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::color() const
{
    if (!color_or_null())
        return KeywordStyleValue::create(Keyword::Currentcolor);
    return *color_or_null();
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::blur_radius() const
{
    if (!blur_radius_or_null())
        return LengthStyleValue::create(Length::make_px(0));
    return *blur_radius_or_null();
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::spread_distance() const
{
    if (!spread_distance_or_null())
        return LengthStyleValue::create(Length::make_px(0));
    return *spread_distance_or_null();
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_color = color()->absolutized(computation_context);
    auto absolutized_offset_x = offset_x()->absolutized(computation_context);
    auto absolutized_offset_y = offset_y()->absolutized(computation_context);
    auto absolutized_blur_radius = blur_radius()->absolutized(computation_context);
    auto absolutized_spread_distance = spread_distance()->absolutized(computation_context);
    return create(shadow_type(), absolutized_color, absolutized_offset_x, absolutized_offset_y, absolutized_blur_radius, absolutized_spread_distance, placement());
}

}
