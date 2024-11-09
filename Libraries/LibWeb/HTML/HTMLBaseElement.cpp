/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLBaseElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLBaseElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLBaseElement);

HTMLBaseElement::HTMLBaseElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLBaseElement::~HTMLBaseElement() = default;

void HTMLBaseElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLBaseElement);
}

void HTMLBaseElement::inserted()
{
    HTMLElement::inserted();

    document().update_base_element({});

    // The frozen base URL must be immediately set for an element whenever any of the following situations occur:
    // - The base element becomes the first base element in tree order with an href content attribute in its Document.

    // NOTE: inserted() is called after this element has been inserted into the document.
    auto first_base_element_with_href_in_document = document().first_base_element_with_href_in_tree_order();
    if (first_base_element_with_href_in_document.ptr() == this)
        set_the_frozen_base_url();
}

void HTMLBaseElement::removed_from(Node* parent)
{
    HTMLElement::removed_from(parent);
    document().update_base_element({});
}

void HTMLBaseElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    HTMLElement::attribute_changed(name, old_value, value);

    // The frozen base URL must be immediately set for an element whenever any of the following situations occur:
    // - The base element is the first base element in tree order with an href content attribute in its Document, and its href content attribute is changed.
    if (name != AttributeNames::href)
        return;

    document().update_base_element({});

    auto first_base_element_with_href_in_document = document().first_base_element_with_href_in_tree_order();
    if (first_base_element_with_href_in_document.ptr() == this)
        set_the_frozen_base_url();
}

// https://html.spec.whatwg.org/multipage/semantics.html#set-the-frozen-base-url
void HTMLBaseElement::set_the_frozen_base_url()
{
    // 1. Let document be element's node document.
    auto& document = this->document();

    // 2. Let urlRecord be the result of parsing the value of element's href content attribute with document's fallback base URL, and document's character encoding. (Thus, the base element isn't affected by itself.)
    auto href = get_attribute_value(AttributeNames::href);
    auto url_record = document.fallback_base_url().complete_url(href);

    // 3. Set element's frozen base URL to document's fallback base URL, if urlRecord is failure or running Is base allowed for Document? on the resulting URL record and document returns "Blocked", and to urlRecord otherwise.
    // FIXME: Apply "Is base allowed for Document?" CSP
    if (!url_record.is_valid()) {
        m_frozen_base_url = document.fallback_base_url();
        return;
    }

    m_frozen_base_url = move(url_record);
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-base-href
String HTMLBaseElement::href() const
{
    // 1. Let document be element's node document.
    auto const& document = this->document();

    // 2. Let url be the value of the href attribute of this element, if it has one, and the empty string otherwise.
    auto url = attribute(AttributeNames::href).value_or(String {});

    // 3. Let urlRecord be the result of parsing url with document's fallback base URL, and document's character encoding. (Thus, the base element isn't affected by other base elements or itself.)
    // FIXME: Pass in document's character encoding.
    auto url_record = document.fallback_base_url().complete_url(url);

    // 4. If urlRecord is failure, return url.
    if (!url_record.is_valid())
        return url;

    // 5. Return the serialization of urlRecord.
    return MUST(url_record.to_string());
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-base-href
WebIDL::ExceptionOr<void> HTMLBaseElement::set_href(String const& href)
{
    // The href IDL attribute, on setting, must set the href content attribute to the given new value.
    return set_attribute(AttributeNames::href, href);
}

}
