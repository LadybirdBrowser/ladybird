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

BackgroundSizeStyleValue::BackgroundSizeStyleValue(LengthPercentage size_x, LengthPercentage size_y)
    : StyleValueWithDefaultOperators(Type::BackgroundSize)
    , m_properties { .size_x = move(size_x), .size_y = move(size_y) }
{
}

BackgroundSizeStyleValue::~BackgroundSizeStyleValue() = default;

String BackgroundSizeStyleValue::to_string(SerializationMode) const
{
    if (m_properties.size_x.is_auto() && m_properties.size_y.is_auto())
        return "auto"_string;
    return MUST(String::formatted("{} {}", m_properties.size_x.to_string(), m_properties.size_y.to_string()));
}

}
