/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OpacityValueStyleValue.h"
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> OpacityValueStyleValue::absolutized(ComputationContext const& computation_context) const
{
    if (value()->is_number() && value()->as_number().number() > 0 && value()->as_number().number() < 1)
        return *this;

    auto clamped_number_value = clamp(number_from_style_value(value()->absolutized(computation_context), 1), 0, 1);

    return OpacityValueStyleValue::create(NumberStyleValue::create(clamped_number_value));
}

GC::Ref<CSSStyleValue> OpacityValueStyleValue::reify(Utf16FlyString const& associated_property) const
{
    return value()->reify(associated_property);
}

}
