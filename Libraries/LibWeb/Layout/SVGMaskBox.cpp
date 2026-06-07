/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Painting/SVGMaskPaintable.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::Layout {

SVGMaskBox::SVGMaskBox(DOM::Document& document, SVG::SVGMaskElement& element, CSS::ComputedProperties const& style)
    : SVGGraphicsBox(document, element, style)
{
}

RefPtr<Painting::Paintable> SVGMaskBox::create_paintable() const
{
    return Painting::SVGMaskPaintable::create(*this);
}

}
