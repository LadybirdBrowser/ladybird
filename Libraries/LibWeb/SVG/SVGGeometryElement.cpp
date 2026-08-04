/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGGeometryElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

SVGGeometryElement::SVGGeometryElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGGeometryElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGGeometryElement);
    Base::initialize(realm);
}

void SVGGeometryElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_path_length);
}

RefPtr<Layout::Node> SVGGeometryElement::create_layout_node(NonnullRefPtr<CSS::ComputedValues const> style)
{
    return make_ref_counted<Layout::SVGGeometryBox>(document(), *this, style);
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
    //     separately from the layout update above since it may have been skipped if the element was display: none or
    //     disconnected.
    document().update_style_for_element(*this);

    VERIFY(computed_values());

    return get_path({ viewport_size.width(), viewport_size.height() }).length();
}

GC::Ref<Geometry::DOMPoint> SVGGeometryElement::get_point_at_length(float distance)
{
    (void)distance;
    return Geometry::DOMPoint::construct_impl(realm(), 0, 0, 0, 0);
}

GC::Ref<SVGAnimatedNumber> SVGGeometryElement::path_length()
{
    if (!m_path_length)
        m_path_length = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::pathLength, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_path_length;
}

}
