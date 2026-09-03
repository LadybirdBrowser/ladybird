/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

SVGGeometryElement::SVGGeometryElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGGeometryElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_path_length);
}

Layout::Node* SVGGeometryElement::create_layout_node(CSS::LayoutStyle style)
{
    return &Layout::allocate_layout_node<Layout::Box>(document(), *this, style, Layout::RustFFI::NodeKind::SVGGeometryBox);
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGGeometryElement__getTotalLength
WebIDL::ExceptionOr<float> SVGGeometryElement::get_total_length()
{
    // When getTotalLength() is called, the user agent's computed value for the total length of the path, in user units,
    // is returned.

    // NB: Update layout so that the viewport size is resolved correctly
    document().update_layout(DOM::UpdateLayoutReason::SVGPathLength);

    auto viewport_size = viewport_size_for_percentage_resolution();

    // NB: Update style for the element so that the correct computed values are used to generate the path - this is done
    //     separately from the layout update above since it may have been skipped if the element was display: none.
    document().update_style_for_element(*this);

    if (auto computed_values = computed_style())
        return get_path({ viewport_size.width(), viewport_size.height() }, *computed_values).length();

    auto transient_values = document().style_computer().materialize_style_record({ *this });
    return get_path({ viewport_size.width(), viewport_size.height() }, *transient_values).length();
}

GC::Ref<Geometry::DOMPoint> SVGGeometryElement::get_point_at_length(float distance)
{
    (void)distance;
    return Geometry::DOMPoint::create(0, 0, 0, 0);
}

GC::Ref<SVGAnimatedNumber> SVGGeometryElement::path_length()
{
    if (!m_path_length)
        m_path_length = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::pathLength, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_path_length;
}

}
