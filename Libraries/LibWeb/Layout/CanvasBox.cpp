/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/CanvasBox.h>
#include <LibWeb/Painting/CanvasPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(CanvasBox);

CanvasBox::CanvasBox(DOM::Document& document, HTML::HTMLCanvasElement& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, move(style))
{
}

CanvasBox::~CanvasBox() = default;

CSS::SizeWithAspectRatio CanvasBox::compute_auto_content_box_size() const
{
    auto width = dom_node().width();
    auto height = dom_node().height();
    if (width == 0 || height == 0)
        return { width, height, {} };
    return { width, height, CSSPixelFraction(width, height) };
}

GC::Ptr<Painting::Paintable> CanvasBox::create_paintable() const
{
    return Painting::CanvasPaintable::create(*this);
}

}
