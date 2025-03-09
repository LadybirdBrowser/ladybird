/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTitleElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLTitleElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTitleElement);

HTMLTitleElement::HTMLTitleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTitleElement::~HTMLTitleElement() = default;

void HTMLTitleElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTitleElement);
}

void HTMLTitleElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    HTMLElement::children_changed(metadata);

    // This happens when children_changed is called on removal of the title element, and is about to be replaced with an empty string.
    if (m_skipTitleUpdate) {
        m_skipTitleUpdate = false;
        return;
    }

    auto navigable = this->navigable();
    if (navigable && navigable->is_traversable()) {
        navigable->traversable_navigable()->page().client().page_did_change_title(document().title().to_byte_string());
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-title-text
String HTMLTitleElement::text() const
{
    // The text attribute's getter must return this title element's child text content.
    return child_text_content();
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-title-text
void HTMLTitleElement::set_text(String const& value)
{
    // The text attribute's setter must string replace all with the given value within this title element.
    // AD-HOC: Skip the first update with an empty string, to avoid flashing an invalid window title to the user.
    m_skipTitleUpdate = true;
    string_replace_all(value);
}

}
