/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BackgroundSizeStyleValue.h"

namespace Web::CSS {

BackgroundSizeStyleValue::BackgroundSizeStyleValue(LengthPercentageOrAuto size_x, LengthPercentageOrAuto size_y)
    : StyleValueWithDefaultOperators(Type::BackgroundSize)
    , m_properties { .size_x = move(size_x), .size_y = move(size_y) }
{
}

BackgroundSizeStyleValue::~BackgroundSizeStyleValue() = default;

String BackgroundSizeStyleValue::to_string(SerializationMode mode) const
{
    if (m_properties.size_x.is_auto() && m_properties.size_y.is_auto())
        return "auto"_string;
    return MUST(String::formatted("{} {}", m_properties.size_x.to_string(mode), m_properties.size_y.to_string(mode)));
}

ValueComparingNonnullRefPtr<StyleValue const> BackgroundSizeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutize = [&](auto& size) -> LengthPercentageOrAuto {
        if (size.is_auto())
            return size;
        return size.length_percentage().absolutized(computation_context);
    };

    auto absolutized_size_x = absolutize(m_properties.size_x);
    auto absolutized_size_y = absolutize(m_properties.size_y);

    if (absolutized_size_x == m_properties.size_x && absolutized_size_y == m_properties.size_y)
        return *this;

    return BackgroundSizeStyleValue::create(absolutized_size_x, absolutized_size_y);
}

}
