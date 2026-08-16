/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/FieldSetBox.h>

namespace Web::Layout {

FieldSetBox::FieldSetBox(DOM::Document& document, DOM::Element& element, CSS::LayoutStyle style)
    : BlockContainer(document, &element, style)
{
    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    // If the computed outer display type is inline, the fieldset is expected to behave as inline-block. Otherwise, it
    // is expected to behave as flow-root. This does not change the computed value.
    if (display().is_flow_inside())
        modify_computed_values([&](auto& values) {
            values.set_display(CSS::Display { display().outside(), CSS::DisplayInside::FlowRoot });
        });
}

FieldSetBox::~FieldSetBox() = default;

}
