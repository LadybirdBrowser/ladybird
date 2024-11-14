/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Painting/SVGMaskPaintable.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(SVGMaskBox);

SVGMaskBox::SVGMaskBox(DOM::Document& document, SVG::SVGMaskElement& element, CSS::StyleProperties properties)
    : SVGGraphicsBox(document, element, properties)
{
}

GC::Ptr<Painting::Paintable> SVGMaskBox::create_paintable() const
{
    return Painting::SVGMaskPaintable::create(*this);
}

}
