/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGStyleElement.h>
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

void SVGStyleElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    update_a_style_block();
}

void SVGStyleElement::inserted()
{
    Base::inserted();
    update_a_style_block();
}

void SVGStyleElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    update_a_style_block();
}

}
