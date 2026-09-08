/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Ratio.h"
#include <math.h>

namespace Web::CSS {

Ratio::Ratio(double first, double second)
    : m_first_value(first)
    , m_second_value(second)
{
}

// https://www.w3.org/TR/css-values-4/#degenerate-ratio
bool Ratio::is_degenerate() const
{
    return !isfinite(m_first_value) || m_first_value == 0
        || !isfinite(m_second_value) || m_second_value == 0;
}

}
