/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGStyleElementPrototype.h>
#include <LibWeb/SVG/SVGStyleElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGStyleElement);

SVGStyleElement::SVGStyleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

SVGStyleElement::~SVGStyleElement() = default;

void SVGStyleElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGStyleElement);
    Base::initialize(realm);
}

void SVGStyleElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visit_style_element_edges(visitor);
}

void SVGStyleElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    Base::children_changed(metadata);
    update_a_style_block();
}

void SVGStyleElement::inserted()
{
    update_a_style_block();
    Base::inserted();
}

void SVGStyleElement::removed_from(Node* old_parent, Node& old_root)
{
    update_a_style_block();
    Base::removed_from(old_parent, old_root);
}

}
