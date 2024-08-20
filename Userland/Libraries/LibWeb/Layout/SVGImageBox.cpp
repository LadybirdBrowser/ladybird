/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::Layout {

SVGImageBox::SVGImageBox(DOM::Document& document, SVG::SVGGraphicsElement& element, NonnullRefPtr<CSS::StyleProperties> properties)
    : SVGGraphicsBox(document, element, properties)
{
}

JS::GCPtr<Painting::Paintable> SVGImageBox::create_paintable() const
{
    return Painting::ImagePaintable::create(*this);
}

}
