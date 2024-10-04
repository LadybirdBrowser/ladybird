/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/CanvasBox.h>
#include <LibWeb/Painting/CanvasPaintable.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(CanvasBox);

CanvasBox::CanvasBox(DOM::Document& document, HTML::HTMLCanvasElement& element, NonnullRefPtr<CSS::StyleProperties> style)
    : ReplacedBox(document, element, move(style))
{
}

CanvasBox::~CanvasBox() = default;

void CanvasBox::prepare_for_replaced_layout()
{
    set_natural_width(dom_node().width());
    set_natural_height(dom_node().height());
}

JS::GCPtr<Painting::Paintable> CanvasBox::create_paintable() const
{
    return Painting::CanvasPaintable::create(*this);
}

}
