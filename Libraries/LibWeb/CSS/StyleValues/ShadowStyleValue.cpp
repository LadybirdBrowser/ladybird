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

// The type and placement discriminants cross the style value FFI as raw codes; the Rust
// serializer's tables depend on them.
static_assert(to_underlying(ShadowStyleValue::ShadowType::Normal) == 0);
static_assert(to_underlying(ShadowStyleValue::ShadowType::Text) == 1);
static_assert(to_underlying(ShadowPlacement::Outer) == 0);
static_assert(to_underlying(ShadowPlacement::Inner) == 1);

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::color() const
{
    if (auto color = color_or_null())
        return color.release_nonnull();
    return KeywordStyleValue::create(Keyword::Currentcolor);
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::blur_radius() const
{
    if (auto blur_radius = blur_radius_or_null())
        return blur_radius.release_nonnull();
    return LengthStyleValue::create(Length::make_px(0));
}

ValueComparingNonnullRefPtr<StyleValue const> ShadowStyleValue::spread_distance() const
{
    if (auto spread_distance = spread_distance_or_null())
        return spread_distance.release_nonnull();
    return LengthStyleValue::create(Length::make_px(0));
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
