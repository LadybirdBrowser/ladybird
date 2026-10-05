/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RatioStyleValue.h"
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

Ratio RatioStyleValue::resolved() const
{
    return { number_from_style_value(numerator(), {}), number_from_style_value(denominator(), {}) };
}

}
