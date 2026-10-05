/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RandomValueSharingStyleValue.h"
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

double RandomValueSharingStyleValue::random_base_value() const
{
    return number_from_style_value(*fixed_value(), {});
}

}
