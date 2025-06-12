/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WindowGlobalMixin.h>

namespace Web::HTML {

// https://w3c.github.io/csswg-drafts/cssom-view/#dictdef-scrolloptions
struct ScrollOptions {
    Bindings::ScrollBehavior behavior { Bindings::ScrollBehavior::Auto };
};

// https://drafts.csswg.org/cssom-view/#normalize-non-finite-values
[[nodiscard]] inline double normalize_non_finite_values(double value)
{
    // When asked to normalize non-finite values for a value x, if x is one of the three special floating point
    // literal values (Infinity, -Infinity or NaN), then x must be changed to the value 0. [WEBIDL]
    if (isinf(value) || isnan(value))
        return 0;

    return value;
}

// https://drafts.csswg.org/cssom-view/#normalize-non-finite-values
[[nodiscard]] inline double normalize_non_finite_values(Optional<double> const& value)
{
    if (!value.has_value())
        return 0;
    return normalize_non_finite_values(value.value());
}

}
