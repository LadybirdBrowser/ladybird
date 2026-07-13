/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MathMLAnchorElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/MathML/AttributeNames.h>
#include <LibWeb/MathML/MathMLAnchorElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLAnchorElement);

MathMLAnchorElement::MathMLAnchorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

MathMLAnchorElement::~MathMLAnchorElement() = default;

void MathMLAnchorElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MathMLAnchorElement);
    Base::initialize(realm);
}

// https://w3c.github.io/mathml-core/#dom-mathmlanchorelement-href
Utf16String MathMLAnchorElement::href() const
{
    // The href getter steps are:

    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this's url.
    // 3. If url is null and this has no href content attribute, then return the empty string.
    if (!m_url.has_value() && !has_attribute(MathML::AttributeNames::href))
        return {};

    // 4. Otherwise, if url is null, then return this's href content attribute's value.
    else if (!m_url.has_value())
        return get_attribute_value(MathML::AttributeNames::href);

    // 5. Return url, serialized.
    auto serialized_url = m_url->serialize();
    return Utf16String::from_ascii_without_validation(serialized_url.bytes());
}

// https://w3c.github.io/mathml-core/#dom-mathmlanchorelement-href
void MathMLAnchorElement::set_href(Utf16String const& href)
{
    set_attribute_value(HTML::AttributeNames::href, href);
}

// https://w3c.github.io/mathml-core/#dfn-set-the-url
void MathMLAnchorElement::set_the_url()
{
    // 1. If this element's href content attribute is absent, then return.
    auto href = get_attribute_value_view(MathML::AttributeNames::href);
    if (!href.has_value())
        return;

    // 2. Let url be the result of encoding-parsing a URL given this element's href content attribute's value, relative
    //    to this element's node document.
    auto url = document().encoding_parse_url(*href);

    // 3. If url is not failure, then set this's url to url.
    if (url.has_value())
        m_url = url.release_value();
}

// https://w3c.github.io/mathml-core/#dfn-update-href
void MathMLAnchorElement::update_href()
{
    // To update href for a MathMLAnchorElement, set the element's href content attribute's value to the element's url,
    // serialized.
    auto serialized_url = m_url->serialize();
    hyperlink_element_utils_element().set_attribute_value(MathML::AttributeNames::href, Utf16String::from_ascii_without_validation(serialized_url.bytes()));
}

}
