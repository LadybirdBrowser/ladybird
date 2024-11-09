/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGDescElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGDescElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGDescElement);

SVGDescElement::SVGDescElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGDescElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGDescElement);
}

JS::GCPtr<Layout::Node> SVGDescElement::create_layout_node(CSS::StyleProperties)
{
    return nullptr;
}

}
