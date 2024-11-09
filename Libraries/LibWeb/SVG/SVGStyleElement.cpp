/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGStyleElementPrototype.h>
#include <LibWeb/SVG/SVGStyleElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGStyleElement);

SVGStyleElement::SVGStyleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

SVGStyleElement::~SVGStyleElement() = default;

void SVGStyleElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGStyleElement);
}

void SVGStyleElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_style_element_utils.visit_edges(visitor);
}

void SVGStyleElement::children_changed()
{
    m_style_element_utils.update_a_style_block(*this);
    Base::children_changed();
}

void SVGStyleElement::inserted()
{
    m_style_element_utils.update_a_style_block(*this);
    Base::inserted();
}

void SVGStyleElement::removed_from(Node* old_parent)
{
    m_style_element_utils.update_a_style_block(*this);
    Base::removed_from(old_parent);
}

// https://www.w3.org/TR/cssom/#dom-linkstyle-sheet
CSS::CSSStyleSheet* SVGStyleElement::sheet()
{
    // The sheet attribute must return the associated CSS style sheet for the node or null if there is no associated CSS style sheet.
    return m_style_element_utils.sheet();
}

// https://www.w3.org/TR/cssom/#dom-linkstyle-sheet
CSS::CSSStyleSheet const* SVGStyleElement::sheet() const
{
    // The sheet attribute must return the associated CSS style sheet for the node or null if there is no associated CSS style sheet.
    return m_style_element_utils.sheet();
}

}
