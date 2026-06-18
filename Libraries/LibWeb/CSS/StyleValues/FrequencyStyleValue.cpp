/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FrequencyStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> FrequencyStyleValue::absolutized(ComputationContext const&) const
{
    if (m_frequency.unit() == canonical_frequency_unit())
        return *this;
    return create(Frequency::make_hertz(m_frequency.to_hertz()));
}

bool FrequencyStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_frequency = other.as_frequency();
    return m_frequency == other_frequency.m_frequency;
}

}
