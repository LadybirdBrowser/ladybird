/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGViewElement.h"
#include <LibWeb/Bindings/SVGViewElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGViewElement);

SVGViewElement::SVGViewElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGViewElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGViewElement);
    Base::initialize(realm);
    SVGFitToViewBox::initialize(realm);
}

void SVGViewElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
}

void SVGViewElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);
}

}
