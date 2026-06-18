/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TimeStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> TimeStyleValue::absolutized(ComputationContext const&) const
{
    if (m_time.unit() == canonical_time_unit())
        return *this;
    return create(Time::make_seconds(m_time.to_seconds()));
}

bool TimeStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_time = other.as_time();
    return m_time == other_time.m_time;
}

}
