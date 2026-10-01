/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGViewElement.h"
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGViewElement);

SVGViewElement::SVGViewElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGViewElement::initialize_element()
{
    SVGFitToViewBox::initialize_fit_to_view_box();
}

void SVGViewElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
}

void SVGViewElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);

    // An <svg> that has made this element its active view takes its view box from here, so the change has to reach
    // that element's published facts as well as this one's.
    if (name.equals_ignoring_ascii_case(AttributeNames::viewBox)) {
        document().for_each_in_subtree_of_type<SVGSVGElement>([](auto& svg_element) {
            svg_element.publish_svg_attribute_facts();
            return TraversalDecision::Continue;
        });
    }
}

}
