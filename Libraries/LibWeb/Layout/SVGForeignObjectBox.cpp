/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGForeignObjectBox.h>
#include <LibWeb/Painting/SVGForeignObjectPaintable.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

SVGForeignObjectBox::SVGForeignObjectBox(DOM::Document& document, SVG::SVGForeignObjectElement& element, CSS::ComputedProperties const& style)
    : BlockContainer(document, &element, style)
{
}

RefPtr<Painting::Paintable> SVGForeignObjectBox::create_paintable() const
{
    return Painting::SVGForeignObjectPaintable::create(*this);
}

}
