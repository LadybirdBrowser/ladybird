/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGTextPathBox.h>
#include <LibWeb/Painting/SVGPathPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(SVGTextPathBox);

SVGTextPathBox::SVGTextPathBox(DOM::Document& document, SVG::SVGTextPathElement& element, GC::Ref<CSS::ComputedProperties> style)
    : SVGGraphicsBox(document, element, style)
{
}

GC::Ptr<Painting::Paintable> SVGTextPathBox::create_paintable() const
{
    return Painting::SVGPathPaintable::create(*this);
}

}
