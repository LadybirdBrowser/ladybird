/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Painting/SVGImagePaintable.h>

namespace Web::Layout {

SVGImageBox::SVGImageBox(DOM::Document& document, SVG::SVGGraphicsElement& element, NonnullRefPtr<CSS::ComputedValues const> style)
    : SVGGraphicsBox(document, element, style)
{
}

RefPtr<Painting::Paintable> SVGImageBox::create_paintable() const
{
    return Painting::SVGImagePaintable::create(*this);
}

}
