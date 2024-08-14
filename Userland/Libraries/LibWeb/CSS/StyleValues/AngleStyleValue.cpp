/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AngleStyleValue.h"

namespace Web::CSS {

AngleStyleValue::AngleStyleValue(Angle angle)
    : CSSUnitValue(Type::Angle)
    , m_angle(move(angle))
{
}

AngleStyleValue::~AngleStyleValue() = default;

String AngleStyleValue::to_string() const
{
    return m_angle.to_string();
}

bool AngleStyleValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_angle = other.as_angle();
    return m_angle == other_angle.m_angle;
}

}
