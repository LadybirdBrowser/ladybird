/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ResolutionStyleValue.h"

namespace Web::CSS {

bool ResolutionStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_resolution = other.as_resolution();
    return resolution() == other_resolution.resolution();
}

}
