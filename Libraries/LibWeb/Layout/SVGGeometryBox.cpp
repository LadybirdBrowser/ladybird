/*
 * Copyright (c) 2020, Matthew Olsson <matthewcolsson@gmail.com>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/SVG/SVGPathElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(SVGGeometryBox);

SVGGeometryBox::SVGGeometryBox(DOM::Document& document, SVG::SVGGeometryElement& element, CSS::StyleProperties properties)
    : SVGGraphicsBox(document, element, properties)
{
}

GC::Ptr<Painting::Paintable> SVGGeometryBox::create_paintable() const
{
    return Painting::SVGPathPaintable::create(*this);
}

}
