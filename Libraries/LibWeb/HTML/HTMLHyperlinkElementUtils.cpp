/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Shannon Booth <shannon@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHyperlinkElementUtils.h>
#include <LibWeb/HTML/Navigable.h>

namespace Web::HTML {

HTMLHyperlinkElementUtils::~HTMLHyperlinkElementUtils() = default;

// https://html.spec.whatwg.org/multipage/links.html#reinitialise-url
void HTMLHyperlinkElementUtils::reinitialize_url() const
{
    // 1. If element's url is non-null, its scheme is "blob", and its cannot-be-a-basFe-URL is true, terminate these steps.
    if (m_url.has_value() && m_url->scheme() == "blob"sv && m_url->cannot_be_a_base_url())
        return;

    // 2. Set the url.
    const_cast<HTMLHyperlinkElementUtils*>(this)->set_the_url();
}

// https://html.spec.whatwg.org/multipage/links.html#concept-hyperlink-url-set
void HTMLHyperlinkElementUtils::set_the_url()
{
    ScopeGuard invalidate_style_if_needed = [old_url = m_url, this] {
        if (m_url != old_url) {
            hyperlink_element_utils_element().invalidate_style(DOM::StyleInvalidationReason::HTMLHyperlinkElementHrefChange);
        }
    };

    // 1. Set this element's url to null.
    m_url = {};

    // 2. If this element's href content attribute is absent, then return.
    auto href_content_attribute = hyperlink_element_utils_href();
    if (!href_content_attribute.has_value()) {
        return;
    }

    // 3. Let url be the result of encoding-parsing a URL given this element's href content attribute's value, relative to this element's node document.
    auto url = hyperlink_element_utils_document().encoding_parse_url(*href_content_attribute);

    // 4. If url is not failure, then set this element's url to url.
    if (url.has_value())
        m_url = url.release_value();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-origin
String HTMLHyperlinkElementUtils::origin() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. If this element's url is null, return the empty string.
    if (!m_url.has_value())
        return String {};

    // 3. Return the serialization of this element's url's origin.
    return m_url->origin().serialize();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-protocol
String HTMLHyperlinkElementUtils::protocol() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. If this element's url is null, return ":".
    if (!m_url.has_value())
        return ":"_string;

    // 3. Return this element's url's scheme, followed by ":".
    return MUST(String::formatted("{}:", m_url->scheme()));
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-protocol
void HTMLHyperlinkElementUtils::set_protocol(StringView protocol)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. If this element's url is null, terminate these steps.
    if (!m_url.has_value())
        return;

    // 3. Basic URL parse the given value, followed by ":", with this element's url as url and scheme start state as state override.
    (void)URL::Parser::basic_parse(MUST(String::formatted("{}:", protocol)), {}, &m_url.value(), URL::Parser::State::SchemeStart);

    // 4. Update href.
    update_href();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-username
String HTMLHyperlinkElementUtils::username() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. If this element's url is null, return the empty string.
    if (!m_url.has_value())
        return String {};

    // 3. Return this element's url's username.
    return m_url->username();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-username
void HTMLHyperlinkElementUtils::set_username(StringView username)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto& url = m_url;

    // 3. If url is null or url cannot have a username/password/port, then return.
    if (!url.has_value() || url->cannot_have_a_username_or_password_or_port())
        return;

    // 4. Set the username given this’s URL and the given value.
    url->set_username(username);

    // 5. Update href.
    update_href();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-password
String HTMLHyperlinkElementUtils::password() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto& url = m_url;

    // 3. If url is null, then return the empty string.
    if (!url.has_value())
        return String {};

    // 4. Return url's password.
    return url->password();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-password
void HTMLHyperlinkElementUtils::set_password(StringView password)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto& url = m_url;

    // 3. If url is null or url cannot have a username/password/port, then return.
    if (!url.has_value() || url->cannot_have_a_username_or_password_or_port())
        return;

    // 4. Set the password, given url and the given value.
    url->set_password(password);

    // 5. Update href.
    update_href();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-host
String HTMLHyperlinkElementUtils::host() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto const& url = m_url;

    // 3. If url or url's host is null, return the empty string.
    if (!url.has_value() || !url->host().has_value())
        return String {};

    // 4. If url's port is null, return url's host, serialized.
    if (!url->port().has_value())
        return url->serialized_host();

    // 5. Return url's host, serialized, followed by ":" and url's port, serialized.
    return MUST(String::formatted("{}:{}", url->serialized_host(), url->port().value()));
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-host
void HTMLHyperlinkElementUtils::set_host(StringView host)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto& url = m_url;

    // 3. If url is null or url's cannot-be-a-base-URL is true, then return.
    if (!url.has_value() || url->cannot_be_a_base_url())
        return;

    // 4. Basic URL parse the given value, with url as url and host state as state override.
    (void)URL::Parser::basic_parse(host, {}, &url.value(), URL::Parser::State::Host);

    // 5. Update href.
    update_href();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-hostname
String HTMLHyperlinkElementUtils::hostname() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto url = m_url;

    // 3. If url or url's host is null, return the empty string.
    if (!url.has_value() || !url->host().has_value())
        return String {};

    // 4. Return url's host, serialized.
    return url->serialized_host();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-hostname
void HTMLHyperlinkElementUtils::set_hostname(StringView hostname)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto& url = m_url;

    // 3. If url is null or url's cannot-be-a-base-URL is true, then return.
    if (!url.has_value() || url->cannot_be_a_base_url())
        return;

    // 4. Basic URL parse the given value, with url as url and hostname state as state override.
    (void)URL::Parser::basic_parse(hostname, {}, &url.value(), URL::Parser::State::Hostname);

    // 5. Update href.
    update_href();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-port
String HTMLHyperlinkElementUtils::port() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto& url = m_url;

    // 3. If url or url's port is null, return the empty string.
    if (!url.has_value() || !url->port().has_value())
        return String {};

    // 4. Return url's port, serialized.
    return String::number(url->port().value());
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-port
void HTMLHyperlinkElementUtils::set_port(StringView port)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.

    // 3. If url is null or url cannot have a username/password/port, then return.
    if (!m_url.has_value() || m_url->cannot_have_a_username_or_password_or_port())
        return;

    // 4. If the given value is the empty string, then set url's port to null.
    if (port.is_empty()) {
        m_url->set_port({});
    }
    // 5. Otherwise, basic URL parse the given value, with url as url and port state as state override.
    else {
        (void)URL::Parser::basic_parse(port, {}, &m_url.value(), URL::Parser::State::Port);
    }

    // 6. Update href.
    update_href();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-pathname
String HTMLHyperlinkElementUtils::pathname() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.

    // 3. If url is null, return the empty string.
    if (!m_url.has_value())
        return String {};

    // 4. Return the result of URL path serializing url.
    return m_url->serialize_path();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-pathname
void HTMLHyperlinkElementUtils::set_pathname(StringView pathname)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.

    // 3. If url is null or url's cannot-be-a-base-URL is true, then return.
    if (!m_url.has_value() || m_url->cannot_be_a_base_url())
        return;

    // 4. Set url's path to the empty list.
    m_url->set_paths({});

    // 5. Basic URL parse the given value, with url as url and path start state as state override.
    (void)URL::Parser::basic_parse(pathname, {}, &m_url.value(), URL::Parser::State::PathStart);

    // 6. Update href.
    update_href();
}

String HTMLHyperlinkElementUtils::search() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.

    // 3. If url is null, or url's query is either null or the empty string, return the empty string.
    if (!m_url.has_value() || !m_url->query().has_value() || m_url->query()->is_empty())
        return String {};

    // 4. Return "?", followed by url's query.
    return MUST(String::formatted("?{}", m_url->query()));
}

void HTMLHyperlinkElementUtils::set_search(StringView search)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.

    // 3. If url is null, terminate these steps.
    if (!m_url.has_value())
        return;

    // 4. If the given value is the empty string, set url's query to null.
    if (search.is_empty()) {
        m_url->set_query({});
    } else {
        // 5. Otherwise:
        //    1. Let input be the given value with a single leading "?" removed, if any.
        auto input = search.substring_view(search.starts_with('?'));

        //    2. Set url's query to the empty string.
        m_url->set_query(String {});

        //    3. Basic URL parse input, with null, this element's node document's document's character encoding, url as url, and query state as state override.
        (void)URL::Parser::basic_parse(input, {}, &m_url.value(), URL::Parser::State::Query);
    }

    // 6. Update href.
    update_href();
}

String HTMLHyperlinkElementUtils::hash() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.

    // 3. If url is null, or url's fragment is either null or the empty string, return the empty string.
    if (!m_url.has_value() || !m_url->fragment().has_value() || m_url->fragment()->is_empty())
        return String {};

    // 4. Return "#", followed by url's fragment.
    return MUST(String::formatted("#{}", *m_url->fragment()));
}

void HTMLHyperlinkElementUtils::set_hash(StringView hash)
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.

    // 3. If url is null, then return.
    if (!m_url.has_value())
        return;

    // 4. If the given value is the empty string, set url's fragment to null.
    if (hash.is_empty()) {
        m_url->set_fragment({});
    } else {
        // 5. Otherwise:
        //    1. Let input be the given value with a single leading "#" removed, if any.
        auto input = hash.substring_view(hash.starts_with('#'));

        //    2. Set url's fragment to the empty string.
        m_url->set_fragment(String {});

        //    3. Basic URL parse input, with url as url and fragment state as state override.
        (void)URL::Parser::basic_parse(input, {}, &m_url.value(), URL::Parser::State::Fragment);
    }

    // 6. Update href.
    update_href();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-href
String HTMLHyperlinkElementUtils::href() const
{
    // 1. Reinitialize url.
    reinitialize_url();

    // 2. Let url be this element's url.
    auto const& url = m_url;

    // 3. If url is null and this element has no href content attribute, return the empty string.
    auto href_content_attribute = hyperlink_element_utils_href();
    if (!url.has_value() && !href_content_attribute.has_value())
        return String {};

    // 4. Otherwise, if url is null, return this element's href content attribute's value.
    if (!url.has_value())
        return href_content_attribute.release_value();

    // 5. Return url, serialized.
    return url->serialize();
}

// https://html.spec.whatwg.org/multipage/links.html#dom-hyperlink-href
WebIDL::ExceptionOr<void> HTMLHyperlinkElementUtils::set_href(String href)
{
    // The href attribute's setter must set this element's href content attribute's value to the given value.
    return set_hyperlink_element_utils_href(move(href));
}

// https://html.spec.whatwg.org/multipage/links.html#update-href
void HTMLHyperlinkElementUtils::update_href()
{
    // To update href, set the element's href content attribute's value to the element's url, serialized.
    MUST(set_hyperlink_element_utils_href(m_url->serialize()));
}

bool HTMLHyperlinkElementUtils::cannot_navigate() const
{
    // An element element cannot navigate if one of the following is true:

    // 1. element's node document is not fully active
    auto const& document = const_cast<HTMLHyperlinkElementUtils*>(this)->hyperlink_element_utils_document();
    if (!document.is_fully_active())
        return true;

    // 2. element is not an a element and is not connected.
    if (!hyperlink_element_utils_is_html_anchor_element() && !hyperlink_element_utils_is_connected())
        return true;

    return false;
}

// https://html.spec.whatwg.org/multipage/links.html#following-hyperlinks-2
void HTMLHyperlinkElementUtils::follow_the_hyperlink(Optional<String> hyperlink_suffix, UserNavigationInvolvement user_involvement)
{
    // 1. If subject cannot navigate, then return.
    if (cannot_navigate())
        return;

    // 2. Let targetAttributeValue be the empty string.
    String target_attribute_value;

    // 3. If subject is an a or area element, then set targetAttributeValue to the result of getting an element's target given subject.
    target_attribute_value = hyperlink_element_utils_get_an_elements_target();

    // 4. Let urlRecord be the result of encoding-parsing a URL given subject's href attribute value, relative to subject's node document.
    auto url_record = hyperlink_element_utils_document().encoding_parse_url(href());

    // 5. If urlRecord is failure, then return.
    if (!url_record.has_value())
        return;

    // 6. Let noopener be the result of getting an element's noopener with subject, urlRecord, and targetAttributeValue.
    auto noopener = hyperlink_element_utils_get_an_elements_noopener(*url_record, target_attribute_value);

    // 7. Let targetNavigable be the first return value of applying the rules for choosing a navigable given
    //    targetAttributeValue, subject's node navigable, and noopener.
    auto target_navigable = hyperlink_element_utils_document().navigable()->choose_a_navigable(target_attribute_value, noopener).navigable;

    // 8. If targetNavigable is null, then return.
    if (!target_navigable)
        return;

    // 9. Let urlString be the result of applying the URL serializer to urlRecord.
    auto url_string = url_record->serialize();

    // 10. If hyperlinkSuffix is non-null, then append it to urlString.
    if (hyperlink_suffix.has_value())
        url_string = MUST(String::formatted("{}{}", url_string, *hyperlink_suffix));

    // 11. Let referrerPolicy be the current state of subject's referrerpolicy content attribute.
    auto referrer_policy = ReferrerPolicy::from_string(hyperlink_element_utils_referrerpolicy().value_or({})).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);

    // FIXME: 12. If subject's link types includes the noreferrer keyword, then set referrerPolicy to "no-referrer".

    // 13. Navigate targetNavigable to urlString using subject's node document, with referrerPolicy set to referrerPolicy and userInvolvement set to userInvolvement.
    MUST(target_navigable->navigate({ .url = url_string, .source_document = hyperlink_element_utils_document(), .referrer_policy = referrer_policy, .user_involvement = user_involvement }));
}

}
