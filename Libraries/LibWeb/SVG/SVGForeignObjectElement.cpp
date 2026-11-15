/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGForeignObjectElement);

SVGForeignObjectElement::SVGForeignObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGForeignObjectElement::~SVGForeignObjectElement() = default;

void SVGForeignObjectElement::initialize_element()
{
}

CSS::ElementBoxKind SVGForeignObjectElement::box_kind() const
{
    // NB: A mask or a pattern renders its content only where it is referenced, and foreign content is not part of
    //     that rendering. Thus a foreignObject in a never-rendered subtree gets no box.
    if (is_in_never_rendered_subtree())
        return CSS::ElementBoxKind::NoBox;
    return CSS::ElementBoxKind::SvgForeignObject;
}

void SVGForeignObjectElement::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_width);
    visitor.visit(m_height);
}

}
