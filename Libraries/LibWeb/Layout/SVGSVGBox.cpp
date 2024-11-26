/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(SVGSVGBox);

SVGSVGBox::SVGSVGBox(DOM::Document& document, SVG::SVGSVGElement& element, CSS::StyleProperties properties)
    : ReplacedBox(document, element, move(properties))
{
}

GC::Ptr<Painting::Paintable> SVGSVGBox::create_paintable() const
{
    return Painting::SVGSVGPaintable::create(*this);
}

void SVGSVGBox::prepare_for_replaced_layout()
{
    auto natural_metrics = SVG::SVGSVGElement::negotiate_natural_metrics(dom_node());
    set_natural_width(natural_metrics.width);
    set_natural_height(natural_metrics.height);
    set_natural_aspect_ratio(natural_metrics.aspect_ratio);
}

}
