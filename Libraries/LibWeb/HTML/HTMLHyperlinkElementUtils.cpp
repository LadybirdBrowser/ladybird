/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/LinkInvalidator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLHyperlinkElementUtils.h>

namespace Web::HTML {

HTMLHyperlinkElementUtils::~HTMLHyperlinkElementUtils() = default;

// https://html.spec.whatwg.org/multipage/links.html#api-for-a-and-area-elements:concept-hyperlink-url-set-2
void HTMLHyperlinkElementUtils::set_the_url()
{
    ScopeGuard invalidate_style_if_needed = [old_url = m_url, this] {
        if (m_url != old_url)
            CSS::Invalidation::invalidate_style_after_hyperlink_state_change(hyperlink_element_utils_element());
    };

    auto& element = hyperlink_element_utils_element();

    // 1. Set this element's url to null.
    m_url = {};

    // 2. If this element's href content attribute is absent, then return.
    auto href_content_attribute = element.attribute(HTML::AttributeNames::href);
    if (!href_content_attribute.has_value()) {
        return;
    }

    // 3. Let url be the result of encoding-parsing a URL given this element's href content attribute's value, relative to this element's node document.
    auto url = element.document().encoding_parse_url(*href_content_attribute);

    // 4. If url is not failure, then set this element's url to url.
    if (url.has_value())
        m_url = url.release_value();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-href
String HTMLHyperlinkElementUtils::href() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto const& url = m_url;

    // 3. If url is null and this element has no href content attribute, return the empty string.
    auto href_content_attribute = hyperlink_element_utils_element().attribute(HTML::AttributeNames::href);
    if (!url.has_value() && !href_content_attribute.has_value())
        return String {};

    // 4. Otherwise, if url is null, return this element's href content attribute's value.
    if (!url.has_value())
        return href_content_attribute.release_value();

    // 5. Return url, serialized.
    return url->serialize();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-href
void HTMLHyperlinkElementUtils::set_href(String href)
{
    // The href attribute's setter must set this element's href content attribute's value to the given value.
    hyperlink_element_utils_element().set_attribute_value(HTML::AttributeNames::href, move(href));
}

// https://html.spec.whatwg.org/multipage/links.html#update-href
void HTMLHyperlinkElementUtils::update_href()
{
    // To update href for an HTMLAnchorElement or HTMLAreaElement element, set the element's href content attribute's
    // value to the element's url, serialized.
    hyperlink_element_utils_element().set_attribute_value(HTML::AttributeNames::href, m_url->serialize());
}

// https://html.spec.whatwg.org/multipage/links.html#dom-a-target
String HTMLHyperlinkElementUtils::target() const
{
    return hyperlink_element_utils_element().get_attribute_value(HTML::AttributeNames::target);
}

// https://html.spec.whatwg.org/multipage/links.html#dom-a-target
void HTMLHyperlinkElementUtils::set_target(String value)
{
    hyperlink_element_utils_element().set_attribute_value(HTML::AttributeNames::target, value);
}

}
