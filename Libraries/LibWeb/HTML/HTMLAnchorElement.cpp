/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAnchorElement);

HTMLAnchorElement::HTMLAnchorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLAnchorElement::~HTMLAnchorElement() = default;

void HTMLAnchorElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rel_list);
}

void HTMLAnchorElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::href) {
        set_the_url();
    } else if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.has_value() ? value->utf16_view() : u""sv);
    }
}

bool HTMLAnchorElement::has_activation_behavior() const
{
    return creates_a_hyperlink();
}

void HTMLAnchorElement::activation_behavior(Web::DOM::Event const& event)
{
    activate_the_hyperlink(event);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLAnchorElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

Optional<ARIA::Role> HTMLAnchorElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-a-no-href
    if (!href().is_empty())
        return ARIA::Role::link;
    // https://www.w3.org/TR/html-aria/#el-a
    return ARIA::Role::generic;
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-rellist
GC::Ref<DOM::DOMTokenList> HTMLAnchorElement::rel_list()
{
    // The IDL attribute relList must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-text
Utf16String HTMLAnchorElement::text() const
{
    // The text attribute's getter must return this element's descendant text content.
    return descendant_text_content();
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-text
void HTMLAnchorElement::set_text(Utf16View text)
{
    // The text attribute's setter must string replace all with the given value within this element.
    string_replace_all(text);
}

}
