/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OpacityValueStyleValue.h"
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

GC::Ref<CSSStyleValue> OpacityValueStyleValue::reify(Utf16FlyString const& associated_property) const
{
    return value()->reify(associated_property);
}

}
