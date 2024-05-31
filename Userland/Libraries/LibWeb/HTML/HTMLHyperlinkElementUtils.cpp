/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
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
    // 1. If this element's href content attribute is absent, set this element's url to null.
    auto href_content_attribute = hyperlink_element_utils_href();
    if (!href_content_attribute.has_value()) {
        m_url = {};
        return;
    }

    // 2. Otherwise, parse this element's href content attribute value relative to this element's node document.
    //    If parsing is successful, set this element's url to the result; otherwise, set this element's url to null.
    m_url = hyperlink_element_utils_document().parse_url(*href_content_attribute);
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
    return MUST(String::from_byte_string(m_url->serialize_origin()));
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
    auto result_url = URL::Parser::basic_parse(MUST(String::formatted("{}:", protocol)), {}, m_url, URL::Parser::State::SchemeStart);
    if (result_url.is_valid())
        m_url = move(result_url);

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
    return m_url->username().release_value();
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
    MUST(url->set_username(username));

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
    return url->password().release_value();
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
    MUST(url->set_password(password));

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
    if (!url.has_value() || url->host().has<Empty>())
        return String {};

    // 4. If url's port is null, return url's host, serialized.
    if (!url->port().has_value())
        return MUST(url->serialized_host());

    // 5. Return url's host, serialized, followed by ":" and url's port, serialized.
    return MUST(String::formatted("{}:{}", MUST(url->serialized_host()), url->port().value()));
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
    auto result_url = URL::Parser::basic_parse(host, {}, url, URL::Parser::State::Host);
    if (result_url.is_valid())
        m_url = move(result_url);

    // 5. Update href.
    update_href();
}

String HTMLHyperlinkElementUtils::hostname() const
{
    // 1. Reinitialize url.
    //
    // 2. Let url be this element's url.
    URL::URL url(href());

    // 3. If url or url's host is null, return the empty string.
    if (url.host().has<Empty>())
        return String {};

    // 4. Return url's host, serialized.
    return MUST(url.serialized_host());
}

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
    auto result_url = URL::Parser::basic_parse(hostname, {}, m_url, URL::Parser::State::Hostname);
    if (result_url.is_valid())
        m_url = move(result_url);

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
    return MUST(String::number(url->port().value()));
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
    } else {
        // 5. Otherwise, basic URL parse the given value, with url as url and port state as state override.
        auto result_url = URL::Parser::basic_parse(port, {}, m_url, URL::Parser::State::Port);
        if (result_url.is_valid())
            m_url = move(result_url);
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

    // 4. If url's cannot-be-a-base-URL is true, then return url's path[0].
    // 5. If url's path is empty, then return the empty string.
    // 6. Return "/", followed by the strings in url's path (including empty strings), separated from each other by "/".
    return MUST(String::from_byte_string(m_url->serialize_path()));
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
    auto url = m_url; // We copy the URL here to follow other browser's behavior of reverting the path change if the parse failed.
    url->set_paths({});

    // 5. Basic URL parse the given value, with url as url and path start state as state override.
    auto result_url = URL::Parser::basic_parse(pathname, {}, move(url), URL::Parser::State::PathStart);
    if (result_url.is_valid())
        m_url = move(result_url);

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
        auto url_copy = m_url; // We copy the URL here to follow other browser's behavior of reverting the search change if the parse failed.
        url_copy->set_query(String {});

        //    3. Basic URL parse input, with null, this element's node document's document's character encoding, url as url, and query state as state override.
        auto result_url = URL::Parser::basic_parse(input, {}, move(url_copy), URL::Parser::State::Query);
        if (result_url.is_valid())
            m_url = move(result_url);
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
        auto url_copy = m_url; // We copy the URL here to follow other browser's behavior of reverting the hash change if the parse failed.
        url_copy->set_fragment(String {});

        //    3. Basic URL parse input, with url as url and fragment state as state override.
        auto result_url = URL::Parser::basic_parse(input, {}, move(url_copy), URL::Parser::State::Fragment);
        if (result_url.is_valid())
            m_url = move(result_url);
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
    if (!url->is_valid())
        return href_content_attribute.release_value();

    // 5. Return url, serialized.
    return MUST(String::from_byte_string(url->serialize()));
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

    // 2. Let replace be false.
    [[maybe_unused]] auto replace = false;

    // 3. Let targetAttributeValue be the empty string.
    String target_attribute_value;

    // 4. If subject is an a or area element, then set targetAttributeValue to the result of getting an element's target given subject.
    target_attribute_value = hyperlink_element_utils_get_an_elements_target();

    // 5. Let noopener be the result of getting an element's noopener with subject and targetAttributeValue.
    auto noopener = hyperlink_element_utils_get_an_elements_noopener(target_attribute_value);

    // 6. Let targetNavigable be the first return value of applying the rules for choosing a navigable given
    //    targetAttributeValue, subject's node navigable, and noopener.
    auto target_navigable = hyperlink_element_utils_document().navigable()->choose_a_navigable(target_attribute_value, noopener).navigable;

    // 7. If targetNavigable is null, then return.
    if (!target_navigable)
        return;

    // 8. Let urlString be the result of encoding-parsing-and-serializing a URL given subject's href attribute value,
    //    relative to subject's node document.
    auto url = hyperlink_element_utils_document().parse_url(href());

    // 9. If urlString is failure, then return.
    if (!url.is_valid())
        return;

    auto url_string = MUST(url.to_string());

    // 10. If hyperlinkSuffix is non-null, then append it to urlString.
    if (hyperlink_suffix.has_value()) {
        StringBuilder url_builder;
        url_builder.append(url_string);
        url_builder.append(*hyperlink_suffix);

        url_string = MUST(url_builder.to_string());
    }

    // 11. Let referrerPolicy be the current state of subject's referrerpolicy content attribute.
    auto referrer_policy = ReferrerPolicy::from_string(hyperlink_element_utils_referrerpolicy().value_or({})).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);

    // FIXME: 12. If subject's link types includes the noreferrer keyword, then set referrerPolicy to "no-referrer".

    // 13. Navigate targetNavigable to urlString using subject's node document, with referrerPolicy set to referrerPolicy and userInvolvement set to userInvolvement.
    MUST(target_navigable->navigate({ .url = url_string, .source_document = hyperlink_element_utils_document(), .referrer_policy = referrer_policy, .user_involvement = user_involvement }));
}

}
