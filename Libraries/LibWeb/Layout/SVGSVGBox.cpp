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

SVGSVGBox::SVGSVGBox(DOM::Document& document, SVG::SVGSVGElement& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, style)
{
}

GC::Ptr<Painting::Paintable> SVGSVGBox::create_paintable() const
{
    return Painting::SVGSVGPaintable::create(*this);
}

CSS::SizeWithAspectRatio SVGSVGBox::natural_size() const
{
    auto metrics = SVG::SVGSVGElement::negotiate_natural_metrics(dom_node());
    return { metrics.width, metrics.height, metrics.aspect_ratio };
}

}
