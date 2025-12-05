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

Optional<CSSPixels> CanvasBox::compute_natural_width() const
{
    return dom_node().width();
}

Optional<CSSPixels> CanvasBox::compute_natural_height() const
{
    return dom_node().height();
}

Optional<CSSPixelFraction> CanvasBox::compute_natural_aspect_ratio() const
{
    if (auto height = natural_height(); height.has_value() && height.value() != 0)
        if (auto width = natural_width(); width.has_value())
            return CSSPixelFraction(width.value(), height.value());
    return {};
}

GC::Ptr<Painting::Paintable> CanvasBox::create_paintable() const
{
    return Painting::CanvasPaintable::create(*this);
}

}
