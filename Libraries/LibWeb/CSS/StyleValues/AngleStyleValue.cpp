/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AngleStyleValue.h"

namespace Web::CSS {

AngleStyleValue::AngleStyleValue(Angle angle)
    : DimensionStyleValue(Type::Angle)
    , m_angle(move(angle))
{
}

AngleStyleValue::~AngleStyleValue() = default;

void AngleStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append(m_angle.to_string(mode));
}

bool AngleStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_angle = other.as_angle();
    return m_angle == other_angle.m_angle;
}

}
