/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/Layout/FieldSetBox.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(FieldSetBox);

FieldSetBox::FieldSetBox(DOM::Document& document, DOM::Element& element, CSS::StyleProperties style)
    : BlockContainer(document, &element, move(style))
{
}

FieldSetBox::~FieldSetBox() = default;

bool FieldSetBox::has_rendered_legend() const
{
    // https://html.spec.whatwg.org/#rendered-legend
    if (this->has_children() && this->first_child()->is_legend_box()) {
        auto* first_child = this->first_child();
        return first_child->computed_values().float_() == CSS::Float::None
            && first_child->computed_values().position() != CSS::Positioning::Absolute
            && first_child->computed_values().position() != CSS::Positioning::Fixed;
    }
    return false;
}

}
