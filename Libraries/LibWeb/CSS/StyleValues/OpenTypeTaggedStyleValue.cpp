/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OpenTypeTaggedStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

// The mode discriminant crosses the style value FFI as a raw code; the Rust serializer
// depends on it.
static_assert(to_underlying(OpenTypeTaggedStyleValue::Mode::FontFeatureSettings) == 0);
static_assert(to_underlying(OpenTypeTaggedStyleValue::Mode::FontVariationSettings) == 1);

ValueComparingNonnullRefPtr<StyleValue const> OpenTypeTaggedStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& absolutized_value = value()->absolutized(computation_context);

    if (absolutized_value == value())
        return *this;

    return OpenTypeTaggedStyleValue::create(mode(), tag(), absolutized_value);
}

}
