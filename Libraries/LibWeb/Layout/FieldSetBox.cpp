/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Layout/LegendBox.h>
#include <LibWeb/Painting/FieldSetPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(FieldSetBox);

FieldSetBox::FieldSetBox(DOM::Document& document, DOM::Element& element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, &element, move(style))
{
}

FieldSetBox::~FieldSetBox() = default;

bool FieldSetBox::has_rendered_legend() const
{
    // https://html.spec.whatwg.org/multipage/rendering.html#rendered-legend
    bool has_rendered_legend = false;
    if (has_children()) {
        for_each_child_of_type<Box>([&](Box const& child) {
            if (child.is_anonymous())
                return IterationDecision::Continue;

            if (!child.is_legend_box())
                return IterationDecision::Break;

            has_rendered_legend = child.computed_values().float_() == CSS::Float::None
                && child.computed_values().position() != CSS::Positioning::Absolute
                && child.computed_values().position() != CSS::Positioning::Fixed;
            return IterationDecision::Break;
        });
    }
    return has_rendered_legend;
}

GC::Ptr<Painting::Paintable> FieldSetBox::create_paintable() const
{
    return Painting::FieldSetPaintable::create(*this);
}

}
