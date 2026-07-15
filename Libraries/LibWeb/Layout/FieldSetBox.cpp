/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Painting/FieldSetPaintable.h>

namespace Web::Layout {

FieldSetBox::FieldSetBox(DOM::Document& document, DOM::Element& element, NonnullRefPtr<CSS::ComputedValues const> style)
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

GC::Ptr<LegendBox const> FieldSetBox::rendered_legend() const
{
    // https://html.spec.whatwg.org/multipage/rendering.html#rendered-legend
    // If the element's box has a child box that matches the conditions in the list below, then the first such child box
    // is the 'fieldset' element's rendered legend:
    //   * The child is a legend element.
    //   * The child's used value of 'float' is 'none'.
    //   * The child's used value of 'position' is not 'absolute' or 'fixed'.
    GC::Ptr<LegendBox const> legend;
    for_each_child_of_type<Box>([&](Box const& child) {
        if (!child.is_legend_box() || !child.is_in_flow())
            return IterationDecision::Continue;
        legend = static_cast<LegendBox const&>(child);
        return IterationDecision::Break;
    });
    return legend;
}

RefPtr<Painting::Paintable> FieldSetBox::create_paintable() const
{
    return Painting::FieldSetPaintable::create(*this);
}

}
