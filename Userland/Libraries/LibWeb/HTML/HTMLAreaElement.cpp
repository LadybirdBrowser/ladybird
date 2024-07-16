/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/HTMLAreaElementPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLAreaElement);

HTMLAreaElement::HTMLAreaElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLAreaElement::~HTMLAreaElement() = default;

void HTMLAreaElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLAreaElement);
}

void HTMLAreaElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rel_list);
}

void HTMLAreaElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    HTMLElement::attribute_changed(name, old_value, value);
    if (name == HTML::AttributeNames::href) {
        set_the_url();
    } else if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.value_or(String {}));
    }
}

// https://html.spec.whatwg.org/multipage/image-maps.html#dom-area-rellist
JS::NonnullGCPtr<DOM::DOMTokenList> HTMLAreaElement::rel_list()
{
    // The IDL attribute relList must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

Optional<String> HTMLAreaElement::hyperlink_element_utils_href() const
{
    return attribute(HTML::AttributeNames::href);
}

WebIDL::ExceptionOr<void> HTMLAreaElement::set_hyperlink_element_utils_href(String href)
{
    return set_attribute(HTML::AttributeNames::href, move(href));
}

Optional<String> HTMLAreaElement::hyperlink_element_utils_referrerpolicy() const
{
    return attribute(HTML::AttributeNames::referrerpolicy);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLAreaElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

Optional<ARIA::Role> HTMLAreaElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-area-no-href
    if (!href().is_empty())
        return ARIA::Role::link;
    // https://www.w3.org/TR/html-aria/#el-area
    return ARIA::Role::generic;
}

}
