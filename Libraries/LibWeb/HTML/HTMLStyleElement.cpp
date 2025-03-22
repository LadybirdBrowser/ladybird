/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLStyleElementPrototype.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLStyleElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLStyleElement);

HTMLStyleElement::HTMLStyleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLStyleElement::~HTMLStyleElement() = default;

void HTMLStyleElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLStyleElement);
}

void HTMLStyleElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_style_element_utils.visit_edges(visitor);
}

void HTMLStyleElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    Base::children_changed(metadata);
    m_style_element_utils.update_a_style_block(*this);
}

void HTMLStyleElement::inserted()
{
    m_style_element_utils.update_a_style_block(*this);
    Base::inserted();
}

void HTMLStyleElement::removed_from(Node* old_parent, Node& old_root)
{
    m_style_element_utils.update_a_style_block(*this);
    Base::removed_from(old_parent, old_root);
}

void HTMLStyleElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::media) {
        if (auto* sheet = m_style_element_utils.sheet())
            sheet->set_media(value.value_or({}));
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-style-disabled
bool HTMLStyleElement::disabled()
{
    // 1. If this does not have an associated CSS style sheet, return false.
    if (!sheet())
        return false;

    // 2. If this's associated CSS style sheet's disabled flag is set, return true.
    if (sheet()->disabled())
        return true;

    // 3. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-style-disabled
void HTMLStyleElement::set_disabled(bool disabled)
{
    // 1. If this does not have an associated CSS style sheet, return.
    if (!sheet())
        return;

    // 2. If the given value is true, set this's associated CSS style sheet's disabled flag.
    //    Otherwise, unset this's associated CSS style sheet's disabled flag.
    sheet()->set_disabled(disabled);
}

// https://www.w3.org/TR/cssom/#dom-linkstyle-sheet
CSS::CSSStyleSheet* HTMLStyleElement::sheet()
{
    // The sheet attribute must return the associated CSS style sheet for the node or null if there is no associated CSS style sheet.
    return m_style_element_utils.sheet();
}

// https://www.w3.org/TR/cssom/#dom-linkstyle-sheet
CSS::CSSStyleSheet const* HTMLStyleElement::sheet() const
{
    // The sheet attribute must return the associated CSS style sheet for the node or null if there is no associated CSS style sheet.
    return m_style_element_utils.sheet();
}

}
