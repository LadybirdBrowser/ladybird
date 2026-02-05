/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Painting/SVGClipPaintable.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(SVGClipBox);

SVGClipBox::SVGClipBox(DOM::Document& document, SVG::SVGClipPathElement& element, GC::Ref<CSS::ComputedProperties> style)
    : SVGBox(document, element, style)
{
}

GC::Ptr<Painting::Paintable> SVGClipBox::create_paintable() const
{
    return Painting::SVGClipPaintable::create(*this);
}

}
