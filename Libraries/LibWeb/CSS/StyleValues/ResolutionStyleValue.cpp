/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ResolutionStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> ResolutionStyleValue::absolutized(ComputationContext const&) const
{
    if (resolution().unit() == canonical_resolution_unit())
        return *this;
    return create(Resolution::make_dots_per_pixel(resolution().to_dots_per_pixel()));
}

bool ResolutionStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_resolution = other.as_resolution();
    return resolution() == other_resolution.resolution();
}

}
