/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGTextPathBox.h>
#include <LibWeb/Painting/SVGPathPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(SVGTextPathBox);

SVGTextPathBox::SVGTextPathBox(DOM::Document& document, SVG::SVGTextPathElement& element, CSS::StyleProperties properties)
    : SVGGraphicsBox(document, element, properties)
{
}

GC::Ptr<Painting::Paintable> SVGTextPathBox::create_paintable() const
{
    return Painting::SVGPathPaintable::create(*this);
}

}
