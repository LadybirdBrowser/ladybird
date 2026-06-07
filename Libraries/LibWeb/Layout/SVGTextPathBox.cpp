/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGTextPathBox.h>
#include <LibWeb/Painting/SVGPathPaintable.h>

namespace Web::Layout {

SVGTextPathBox::SVGTextPathBox(DOM::Document& document, SVG::SVGTextPathElement& element, CSS::ComputedProperties const& style)
    : SVGGraphicsBox(document, element, style)
{
}

RefPtr<Painting::Paintable> SVGTextPathBox::create_paintable() const
{
    return Painting::SVGPathPaintable::create(*this);
}

}
