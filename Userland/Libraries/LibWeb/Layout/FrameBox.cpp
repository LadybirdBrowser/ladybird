/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/FrameBox.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/NestedBrowsingContextPaintable.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(FrameBox);

FrameBox::FrameBox(DOM::Document& document, DOM::Element& element, NonnullRefPtr<CSS::StyleProperties> style)
    : ReplacedBox(document, element, move(style))
{
}

FrameBox::~FrameBox() = default;

void FrameBox::prepare_for_replaced_layout()
{
    // FIXME: Do proper error checking, etc.
    set_natural_width(dom_node().get_attribute_value(HTML::AttributeNames::width).to_number<int>().value_or(300));
    set_natural_height(dom_node().get_attribute_value(HTML::AttributeNames::height).to_number<int>().value_or(150));
}

void FrameBox::did_set_content_size()
{
    ReplacedBox::did_set_content_size();

    if (dom_node().content_navigable())
        dom_node().content_navigable()->set_viewport_size(paintable_box()->content_size());
}

JS::GCPtr<Painting::Paintable> FrameBox::create_paintable() const
{
    return Painting::NestedBrowsingContextPaintable::create(*this);
}

}
