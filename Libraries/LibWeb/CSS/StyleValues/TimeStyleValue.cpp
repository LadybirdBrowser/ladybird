/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TimeStyleValue.h"

namespace Web::CSS {

bool TimeStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_time = other.as_time();
    return time() == other_time.time();
}

}
