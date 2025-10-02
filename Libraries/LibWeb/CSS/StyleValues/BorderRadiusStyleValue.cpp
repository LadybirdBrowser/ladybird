/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BorderRadiusStyleValue.h"

namespace Web::CSS {

String BorderRadiusStyleValue::to_string(SerializationMode mode) const
{
    if (m_properties.horizontal_radius == m_properties.vertical_radius)
        return m_properties.horizontal_radius->to_string(mode);
    return MUST(String::formatted("{} {}", m_properties.horizontal_radius->to_string(mode), m_properties.vertical_radius->to_string(mode)));
}

ValueComparingNonnullRefPtr<StyleValue const> BorderRadiusStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_horizontal_radius = m_properties.horizontal_radius->absolutized(computation_context);
    auto absolutized_vertical_radius = m_properties.vertical_radius->absolutized(computation_context);

    if (absolutized_vertical_radius == m_properties.vertical_radius && absolutized_horizontal_radius == m_properties.horizontal_radius)
        return *this;

    return BorderRadiusStyleValue::create(absolutized_horizontal_radius, absolutized_vertical_radius);
}

}
