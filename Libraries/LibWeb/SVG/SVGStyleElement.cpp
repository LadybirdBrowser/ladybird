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
    update_a_style_block_for_dynamic_change();
}

void SVGStyleElement::inserted()
{
    Base::inserted();
    update_a_style_block_for_dynamic_change();
}

void SVGStyleElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    update_a_style_block_for_dynamic_change();
}

void SVGStyleElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    style_element_attribute_changed(name, value);
}

bool SVGStyleElement::contributes_a_script_blocking_style_sheet() const
{
    return style_element_contributes_a_script_blocking_style_sheet();
}

}
