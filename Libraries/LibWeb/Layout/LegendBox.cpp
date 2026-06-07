/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/LegendBox.h>

namespace Web::Layout {

LegendBox::LegendBox(DOM::Document& document, DOM::Element& element, CSS::ComputedProperties const& style)
    : BlockContainer(document, &element, style)
{
}

LegendBox::~LegendBox() = default;

}
