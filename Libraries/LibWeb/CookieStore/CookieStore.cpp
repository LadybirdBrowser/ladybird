/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/CookieStore/CookieChangeEvent.h>
#include <LibWeb/CookieStore/CookieStore.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CookieStore {

GC_DEFINE_ALLOCATOR(CookieStore);

CookieStore::CookieStore(JS::Realm& realm, PageClient& client)
    : DOM::EventTarget(realm)
    , m_client(client)
{
}

void CookieStore::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CookieStore);
    Base::initialize(realm);
}

void CookieStore::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_client);
}

// https://cookiestore.spec.whatwg.org/#create-a-cookielistitem
static CookieListItem create_a_cookie_list_item(Cookie::Cookie const& cookie)
{
    // 1. Let name be the result of running UTF-8 decode without BOM on cookie’s name.
    // 2. Let value be the result of running UTF-8 decode without BOM on cookie’s value.
    // 3. Return «[ "name" → name, "value" → value ]»
    return CookieListItem {
        .name = cookie.name,
        .value = cookie.value,
    };
}

// https://cookiestore.spec.whatwg.org/#normalize-a-cookie-name-or-value
static String normalize(String const& input)
{
    // Remove all U+0009 TAB and U+0020 SPACE that are at the start or end of input.
    return MUST(input.trim("\t "sv));
}

// https://cookiestore.spec.whatwg.org/#query-cookies
static Vector<CookieListItem> query_cookies(PageClient& client, URL::URL const& url, Optional<String> const& name)
{
    // 1. Perform the steps defined in Cookies § Retrieval Model to compute the "cookie-string from a given cookie store"
    //    with url as request-uri. The cookie-string itself is ignored, but the intermediate cookie-list is used in subsequent steps.
    //    For the purposes of the steps, the cookie-string is being generated for a "non-HTTP" API.
    auto cookie_list = client.page_did_request_all_cookies_cookiestore(url);

    // 2. Let list be a new list.
    Vector<CookieListItem> list;

    // 3. For each cookie in cookie-list, run these steps:
    for (auto const& cookie : cookie_list) {
        // 1. Assert: cookie’s http-only-flag is false.
        VERIFY(!cookie.http_only);

        // 2. If name is non-null:
        if (name.has_value()) {
            // 1. Normalize name.
            auto normalized_name = normalize(name.value());

            // 2. Let cookieName be the result of running UTF-8 decode without BOM on cookie’s name.
            // 3. If cookieName does not equal name, then continue.
            if (cookie.name != normalized_name)
                continue;
        }
        // 3. Let item be the result of running create a CookieListItem from cookie.
        auto item = create_a_cookie_list_item(cookie);

        // 4. Append item to list.
        list.append(move(item));
    }

    // 4. Return list.
    return list;
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-get
GC::Ref<WebIDL::Promise> CookieStore::get(String name)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), name = move(name)]() {
        // 1. Let list be the results of running query cookies with url and name.
        auto list = query_cookies(client, url, name);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, list = move(list)]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If list is failure, then reject p with a TypeError and abort these steps.

            // 3. If list is empty, then resolve p with null.
            if (list.is_empty())
                WebIDL::resolve_promise(realm, promise, JS::js_null());

            // 4. Otherwise, resolve p with the first item of list.
            else
                WebIDL::resolve_promise(realm, promise, Bindings::cookie_list_item_to_value(realm, list[0]));
        }));
    }));

    // 7. Return p.
    return promise;
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-get-options
GC::Ref<WebIDL::Promise> CookieStore::get(CookieStoreGetOptions const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. If options is empty, then return a promise rejected with a TypeError.
    if (!options.name.has_value() && !options.url.has_value())
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "CookieStoreGetOptions is empty"sv));

    // 6. If options["url"] is present, then run these steps:
    if (options.url.has_value()) {
        // 1. Let parsed be the result of parsing options["url"] with settings’s API base URL.
        auto parsed = URL::Parser::basic_parse(options.url.value(), settings.api_base_url());

        // AD-HOC: This isn't explicitly mentioned in the specification, but we have to reject invalid URLs as well
        if (!parsed.has_value())
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "url is invalid"sv));

        // 2. If this’s relevant global object is a Window object and parsed does not equal url with exclude fragments
        //    set to true, then return a promise rejected with a TypeError.
        if (is<HTML::Window>(HTML::relevant_global_object(*this)) && !parsed->equals(url, URL::ExcludeFragment::Yes))
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "url does not match creation URL"sv));

        // 3. If parsed’s origin and url’s origin are not the same origin, then return a promise rejected with a TypeError.
        if (parsed->origin() != url.origin())
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "url's origin does not match creation URL's origin"sv));

        // 4. Set url to parsed.
        url = parsed.value();
    }

    // 7. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 8. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), name = options.name]() {
        // 1. Let list be the results of running query cookies with url and options["name"] with default null.
        auto list = query_cookies(client, url, name);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, list = move(list)]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If list is failure, then reject p with a TypeError and abort these steps.

            // 3. If list is empty, then resolve p with null.
            if (list.is_empty())
                WebIDL::resolve_promise(realm, promise, JS::js_null());

            // 4. Otherwise, resolve p with the first item of list.
            else
                WebIDL::resolve_promise(realm, promise, Bindings::cookie_list_item_to_value(realm, list[0]));
        }));
    }));

    // 9. Return p.
    return promise;
}

static JS::Value cookie_list_to_value(JS::Realm& realm, Vector<CookieListItem> const& cookie_list)
{
    return JS::Array::create_from<CookieListItem>(realm, cookie_list, [&](auto const& cookie) {
        return Bindings::cookie_list_item_to_value(realm, cookie);
    });
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-getall
GC::Ref<WebIDL::Promise> CookieStore::get_all(String name)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), name = move(name)]() {
        // 1. Let list be the results of running query cookies with url and name.
        auto list = query_cookies(client, url, name);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, list = move(list)]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If list is failure, then reject p with a TypeError and abort these steps.

            // 3. Otherwise, resolve p with list.
            WebIDL::resolve_promise(realm, promise, cookie_list_to_value(realm, list));
        }));
    }));

    // 7. Return p.
    return promise;
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-getall-options
GC::Ref<WebIDL::Promise> CookieStore::get_all(CookieStoreGetOptions const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. If options["url"] is present, then run these steps:
    if (options.url.has_value()) {
        // 1. Let parsed be the result of parsing options["url"] with settings’s API base URL.
        auto parsed = URL::Parser::basic_parse(options.url.value(), settings.api_base_url());

        // AD-HOC: This isn't explicitly mentioned in the specification, but we have to reject invalid URLs as well
        if (!parsed.has_value())
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "url is invalid"sv));

        // 2. If this’s relevant global object is a Window object and parsed does not equal url with exclude fragments
        //    set to true, then return a promise rejected with a TypeError.
        if (is<HTML::Window>(HTML::relevant_global_object(*this)) && !parsed->equals(url, URL::ExcludeFragment::Yes))
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "url does not match creation URL"sv));

        // 3. If parsed’s origin and url’s origin are not the same origin, then return a promise rejected with a TypeError.
        if (parsed->origin() != url.origin())
            return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "url's origin does not match creation URL's origin"sv));

        // 4. Set url to parsed.
        url = parsed.value();
    }

    // 6. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 7. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), name = options.name]() {
        // 1. Let list be the results of running query cookies with url and options["name"] with default null.
        auto list = query_cookies(client, url, name);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, list = move(list)]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If list is failure, then reject p with a TypeError and abort these steps.

            // 3. Otherwise, resolve p with list.
            WebIDL::resolve_promise(realm, promise, cookie_list_to_value(realm, list));
        }));
    }));

    // 8. Return p.
    return promise;
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-layered-cookies#name-cookie-default-path
static Vector<String> cookie_default_path(Vector<String> path)
{
    // 1. Assert: path is a non-empty list.
    VERIFY(!path.is_empty());

    // 2. If path's size is greater than 1, then remove path's last item.
    if (path.size() > 1)
        path.take_last();

    // 3. Otherwise, set path[0] to the empty string.
    else
        path[0] = ""_string;

    // 4. Return path.
    return path;
}

// https://fetch.spec.whatwg.org/#serialized-cookie-default-path
static String serialized_cookie_default_path(URL::URL const& url)
{
    // 1. Let cloneURL be a clone of url.
    auto clone_url = url;

    // 2. Set cloneURL’s path to the cookie default path of cloneURL’s path.
    clone_url.set_raw_paths(cookie_default_path(clone_url.paths()));

    // 3. Return the URL path serialization of cloneURL.
    return clone_url.serialize_path();
}

static constexpr size_t maximum_name_value_pair_size = 4096;
static constexpr size_t maximum_attribute_value_size = 1024;

// https://cookiestore.spec.whatwg.org/#set-a-cookie
static bool set_a_cookie(PageClient& client, URL::URL const& url, String name, String value, Optional<HighResolutionTime::DOMHighResTimeStamp> expires, Optional<String> const& domain, String path, Bindings::CookieSameSite same_site, bool partitioned)
{
    // 1. Normalize name.
    name = normalize(name);

    // 2. Normalize value.
    value = normalize(value);

    // 3. If name or value contain U+003B (;), any C0 control character except U+0009 TAB, or U+007F DELETE, then return failure.
    if (name.contains(';') || value.contains(';'))
        return false;
    for (auto c = '\x00'; c <= '\x1F'; ++c) {
        if (c == '\t')
            continue;
        if (name.contains(c) || value.contains(c))
            return false;
    }
    if (name.contains('\x7F') || value.contains('\x7F'))
        return false;

    // 4. If name contains U+003D (=), then return failure.
    if (name.contains('='))
        return false;

    // 5. If name’s length is 0:
    if (name.is_empty()) {
        // 1. If value contains U+003D (=), then return failure.
        if (value.contains('='))
            return false;

        // 2. If value’s length is 0, then return failure.
        if (value.is_empty())
            return false;

        // 3. If value, byte-lowercased, starts with `__host-`, `__host-http-`, `__http-`, or `__secure-`, then return failure.
        auto value_byte_lowercased = value.to_ascii_lowercase();
        if (value_byte_lowercased.starts_with_bytes("__host-"sv) || value_byte_lowercased.starts_with_bytes("__host-http-"sv) || value_byte_lowercased.starts_with_bytes("__http-"sv) || value_byte_lowercased.starts_with_bytes("__secure-"sv))
            return false;
    }

    // 6. If name, byte-lowercased, starts with `__host-http-` or `__http-`, then return failure.
    auto name_byte_lowercased = name.to_ascii_lowercase();
    if (name_byte_lowercased.starts_with_bytes("__host-http-"sv) || name_byte_lowercased.starts_with_bytes("__http-"sv))
        return false;

    // 7. Let encodedName be the result of UTF-8 encoding name.
    // 8. Let encodedValue be the result of UTF-8 encoding value.

    // 9. If the byte sequence length of encodedName plus the byte sequence length of encodedValue is greater than the
    //    maximum name/value pair size, then return failure.
    if (name.byte_count() + value.byte_count() > maximum_name_value_pair_size)
        return false;

    // 10. Let host be url’s host
    auto const& host = url.host();

    // 11. Let attributes be a new list.
    Cookie::ParsedCookie parsed_cookie {};
    parsed_cookie.name = move(name);
    parsed_cookie.value = move(value);

    // 12. If domain is not null, then run these steps:
    if (domain.has_value()) {
        // 1. If domain starts with U+002E (.), then return failure.
        if (domain->starts_with('.'))
            return false;

        // 2. If name, byte-lowercased, starts with `__host-`, then return failure.
        if (name_byte_lowercased.starts_with_bytes("__host-"sv))
            return false;

        // 3. If domain is not a registrable domain suffix of and is not equal to host, then return failure.
        if (!host.has_value() || !DOM::is_a_registrable_domain_suffix_of_or_is_equal_to(domain.value(), host.value()))
            return false;

        // 4. Let parsedDomain be the result of host parsing domain.
        auto parsed_domain = URL::Parser::parse_host(domain.value());

        // 5. Assert: parsedDomain is not failure.
        VERIFY(parsed_domain.has_value());

        // 6. Let encodedDomain be the result of UTF-8 encoding parsedDomain.
        auto encoded_domain = parsed_domain->serialize();

        // 7. If the byte sequence length of encodedDomain is greater than the maximum attribute value size, then return failure.
        if (encoded_domain.byte_count() > maximum_attribute_value_size)
            return false;

        // 8. Append `Domain`/encodedDomain to attributes.
        parsed_cookie.domain = move(encoded_domain);
    }

    // 13. If expires is given, then append `Expires`/expires (date serialized) to attributes.
    if (expires.has_value()) {
        auto expiry_time = UnixDateTime::from_milliseconds_since_epoch(expires.value());

        // https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.1
        // 3. Let cookie-age-limit be the maximum age of the cookie (which SHOULD be 400 days in the future or sooner, see
        //    Section 5.5).
        auto cookie_age_limit = UnixDateTime::now() + Cookie::maximum_cookie_age;

        // 4. If the expiry-time is more than cookie-age-limit, the user agent MUST set the expiry time to cookie-age-limit
        //    in seconds.
        if (expiry_time.seconds_since_epoch() > cookie_age_limit.seconds_since_epoch())
            expiry_time = cookie_age_limit;

        parsed_cookie.expiry_time_from_expires_attribute = expiry_time;
    }

    // 14. If path is the empty string, then set path to the serialized cookie default path of url.
    if (path.is_empty())
        path = serialized_cookie_default_path(url);

    // 15. If path does not start with U+002F (/), then return failure.
    if (!path.starts_with('/'))
        return false;

    // 16. If path is not U+002F (/), and name, byte-lowercased, starts with `__host-`, then return failure.
    if (path != "/"sv && name_byte_lowercased.starts_with_bytes("__host-"sv))
        return false;

    // 17. Let encodedPath be the result of UTF-8 encoding path.
    // 18. If the byte sequence length of encodedPath is greater than the maximum attribute value size, then return failure.
    if (path.byte_count() > maximum_attribute_value_size)
        return false;

    // 19. Append `Path`/encodedPath to attributes.
    parsed_cookie.path = path;

    // 20. Append `Secure`/`` to attributes.
    parsed_cookie.secure_attribute_present = true;

    // 21. Switch on sameSite:
    switch (same_site) {
    // -> "none"
    case Bindings::CookieSameSite::None:
        // Append `SameSite`/`None` to attributes.
        parsed_cookie.same_site_attribute = Cookie::SameSite::None;
        break;
    // -> "strict"
    case Bindings::CookieSameSite::Strict:
        // Append `SameSite`/`Strict` to attributes.
        parsed_cookie.same_site_attribute = Cookie::SameSite::Strict;
        break;
    // -> "lax"
    case Bindings::CookieSameSite::Lax:
        // Append `SameSite`/`Lax` to attributes.
        parsed_cookie.same_site_attribute = Cookie::SameSite::Lax;
        break;
    }

    // FIXME: 22. If partitioned is true, Append `Partitioned`/`` to attributes.
    (void)partitioned;

    // 23. Perform the steps defined in Cookies § Storage Model for when the user agent "receives a cookie" with url as
    //     request-uri, encodedName as cookie-name, encodedValue as cookie-value, and attributes as cookie-attribute-list.
    //     For the purposes of the steps, the newly-created cookie was received from a "non-HTTP" API.
    client.page_did_set_cookie(url, parsed_cookie, Cookie::Source::NonHttp);

    // 24. Return success.
    return true;
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-set
GC::Ref<WebIDL::Promise> CookieStore::set(String name, String value)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let domain be null.
    // 6. Let path be "/".
    // 7. Let sameSite be strict.
    // 8. Let partitioned be false.

    // 9. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 10. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), name = move(name), value = move(value)]() {
        // 1. Let r be the result of running set a cookie with url, name, value, domain, path, sameSite, and partitioned.
        auto result = set_a_cookie(client, url, move(name), move(value), {}, {}, "/"_string, Bindings::CookieSameSite::Strict, false);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, result]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If r is failure, then reject p with a TypeError and abort these steps.
            if (!result)
                return WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Name or value are malformed"sv));

            // 3. Resolve p with undefined.
            WebIDL::resolve_promise(realm, promise);
        }));
    }));

    // 11. Return p.
    return promise;
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-set-options
GC::Ref<WebIDL::Promise> CookieStore::set(CookieInit const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), options = options]() {
        // 1. Let r be the result of running set a cookie with url, options["name"], options["value"], options["expires"],
        //    options["domain"], options["path"], options["sameSite"], and options["partitioned"].
        auto result = set_a_cookie(client, url, options.name, options.value, options.expires, options.domain, options.path, options.same_site, options.partitioned);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, result]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If r is failure, then reject p with a TypeError and abort these steps.
            if (!result)
                return WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Name, value, domain or path are malformed"sv));

            // 3. Resolve p with undefined.
            WebIDL::resolve_promise(realm, promise);
        }));
    }));

    // 7. Return p.
    return promise;
}

// https://cookiestore.spec.whatwg.org/#delete-a-cookie
static bool delete_a_cookie(PageClient& client, URL::URL const& url, String name, Optional<String> const& domain, String path, bool partitioned)
{
    // 1. Let expires be the earliest representable date represented as a timestamp.
    // NOTE: The exact value of expires is not important for the purposes of this algorithm, as long as it is in the past.
    HighResolutionTime::DOMHighResTimeStamp expires = UnixDateTime::earliest().milliseconds_since_epoch();

    // 2. Normalize name.
    name = normalize(name);

    // 3. Let value be the empty string.
    String value;

    // 4. If name’s length is 0, then set value to any non-empty implementation-defined string.
    if (name.is_empty())
        value = "ladybird"_string;

    // 5. Return the results of running set a cookie with url, name, value, expires, domain, path, "strict", and partitioned.
    return set_a_cookie(client, url, move(name), move(value), expires, move(domain), move(path), Bindings::CookieSameSite::Strict, partitioned);
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-delete
GC::Ref<WebIDL::Promise> CookieStore::delete_(String name)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), name = move(name)]() {
        // 1. Let r be the result of running delete a cookie with url, name, null, "/", and true.
        auto result = delete_a_cookie(client, url, move(name), {}, "/"_string, true);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, result]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If r is failure, then reject p with a TypeError and abort these steps.
            if (!result)
                return WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Name is malformed"sv));

            // 3. Resolve p with undefined.
            WebIDL::resolve_promise(realm, promise);
        }));
    }));

    // 7. Return p.
    return promise;
}

// https://cookiestore.spec.whatwg.org/#dom-cookiestore-delete-options
GC::Ref<WebIDL::Promise> CookieStore::delete_(CookieStoreDeleteOptions const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto const& settings = HTML::relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto const& origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque())
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_utf16));

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, client = m_client, promise, url = move(url), options = options]() {
        // 1. Let r be the result of running delete a cookie with url, options["name"], options["domain"], options["path"],
        //    and options["partitioned"].
        auto result = delete_a_cookie(client, url, options.name, options.domain, options.path, options.partitioned);

        // AD-HOC: Queue a global task to perform the next steps
        // Spec issue: https://github.com/whatwg/cookiestore/issues/239
        queue_global_task(HTML::Task::Source::Unspecified, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, result]() {
            HTML::TemporaryExecutionContext execution_context { realm };
            // 2. If r is failure, then reject p with a TypeError and abort these steps.
            if (!result)
                return WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Name is malformed"sv));

            // 3. Resolve p with undefined.
            WebIDL::resolve_promise(realm, promise);
        }));
    }));

    // 7. Return p.
    return promise;
}

void CookieStore::set_onchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

WebIDL::CallbackType* CookieStore::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

// https://cookiestore.spec.whatwg.org/#cookie-change
struct CookieChange {
    enum class Type {
        Changed,
        Deleted,
    };

    Cookie::Cookie cookie;
    Type type;
};

// https://cookiestore.spec.whatwg.org/#observable-changes
static Vector<CookieChange> observable_changes(URL::URL const& url, Vector<Cookie::Cookie> const& changes)
{
    // The observable changes for url are the set of cookie changes to cookies in a cookie store which meet the
    // requirements in step 1 of Cookies § Retrieval Algorithm’s steps to compute the "cookie-string from a given
    // cookie store" with url as request-uri, for a "non-HTTP" API.
    // https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-14#name-retrieval-algorithm
    auto canonicalized_domain = Cookie::canonicalize_domain(url);
    if (!canonicalized_domain.has_value())
        return {};

    // FIXME: The retrieval's same-site status is "same-site" if the Document's "site for cookies" is same-site with the
    //        top-level origin as defined in Section 5.2.1 (otherwise it is "cross-site"), and the retrieval's type is "non-HTTP".
    auto is_same_site_retrieval = true;

    auto now = UnixDateTime::now();

    // 1. Let cookie-list be the set of cookies from the cookie store that meets all of the following requirements:
    Vector<CookieChange> observable_changes;
    for (auto const& cookie : changes) {
        // * Either:
        //     The cookie's host-only-flag is true and the canonicalized host of the retrieval's URI is identical to
        //     the cookie's domain.
        bool is_host_only_and_has_identical_domain = cookie.host_only && (canonicalized_domain.value() == cookie.domain);
        // Or:
        //     The cookie's host-only-flag is false and the canonicalized host of the retrieval's URI domain-matches
        //     the cookie's domain.
        bool is_not_host_only_and_domain_matches = !cookie.host_only && Web::Cookie::domain_matches(canonicalized_domain.value(), cookie.domain);

        if (!is_host_only_and_has_identical_domain && !is_not_host_only_and_domain_matches)
            continue;

        // * The retrieval's URI's path path-matches the cookie's path.
        if (!Cookie::path_matches(url.serialize_path(), cookie.path))
            continue;

        // * If the cookie's secure-only-flag is true, then the retrieval's URI must denote a "secure" connection (as
        //   defined by the user agent).
        if (cookie.secure && url.scheme() != "https"sv && url.scheme() != "wss"sv)
            continue;

        // * If the cookie's http-only-flag is true, then exclude the cookie if the retrieval's type is "non-HTTP".
        if (cookie.http_only)
            continue;

        // * If the cookie's same-site-flag is not "None" and the retrieval's same-site status is "cross-site", then
        //   exclude the cookie unless all of the following conditions are met:
        //     * The retrieval's type is "HTTP".
        //     * The same-site-flag is "Lax" or "Default".
        //     * The HTTP request associated with the retrieval uses a "safe" method.
        //     * The target browsing context of the HTTP request associated with the retrieval is the active browsing context
        //       or a top-level traversable.
        if (cookie.same_site != Cookie::SameSite::None && !is_same_site_retrieval)
            continue;

        // A cookie change is a cookie and a type (either changed or deleted):
        // - A cookie which is removed due to an insertion of another cookie with the same name, domain, and path is ignored.
        // - A newly-created cookie which is not immediately evicted is considered changed.
        // - A newly-created cookie which is immediately evicted is considered deleted.
        // - A cookie which is otherwise evicted or removed is considered deleted
        observable_changes.append({ cookie, cookie.expiry_time < now ? CookieChange::Type::Deleted : CookieChange::Type::Changed });
    }

    return observable_changes;
}

struct PreparedLists {
    Vector<CookieListItem> changed_list;
    Vector<CookieListItem> deleted_list;
};

// https://cookiestore.spec.whatwg.org/#prepare-lists
static PreparedLists prepare_lists(Vector<CookieChange> const& changes)
{
    // 1. Let changedList be a new list.
    Vector<CookieListItem> changed_list;

    // 2. Let deletedList be a new list.
    Vector<CookieListItem> deleted_list;

    // 3. For each change in changes, run these steps:
    for (auto const& change : changes) {
        // 1. Let item be the result of running create a CookieListItem from change’s cookie.
        auto item = create_a_cookie_list_item(change.cookie);

        // 2. If change’s type is changed, then append item to changedList.
        if (change.type == CookieChange::Type::Changed)
            changed_list.append(move(item));

        // 3. Otherwise, run these steps:
        else {
            // 1. Set item["value"] to undefined.
            item.value.clear();

            // 2. Append item to deletedList.
            deleted_list.append(move(item));
        }
    }

    // 4. Return changedList and deletedList.
    return { move(changed_list), move(deleted_list) };
}

// https://cookiestore.spec.whatwg.org/#process-cookie-changes
void CookieStore::process_cookie_changes(Vector<Cookie::Cookie> const& all_changes)
{
    auto& realm = this->realm();

    // 1. Let url be window’s relevant settings object’s creation URL.
    auto url = HTML::relevant_settings_object(*this).creation_url;

    // 2. Let changes be the observable changes for url.
    auto changes = observable_changes(url, all_changes);

    // 3. If changes is empty, then continue.
    if (changes.is_empty())
        return;

    // 4. Queue a global task on the DOM manipulation task source given window to fire a change event named "change"
    //    with changes at window’s CookieStore.
    queue_global_task(HTML::Task::Source::DOMManipulation, realm.global_object(), GC::create_function(realm.heap(), [this, &realm, changes = move(changes)]() {
        HTML::TemporaryExecutionContext execution_context { realm };
        // https://cookiestore.spec.whatwg.org/#fire-a-change-event
        // 4. Let changedList and deletedList be the result of running prepare lists from changes.
        auto [changed_list, deleted_list] = prepare_lists(changes);

        CookieChangeEventInit event_init = {};
        // 5. Set event’s changed attribute to changedList.
        event_init.changed = move(changed_list);

        // 6. Set event’s deleted attribute to deletedList.
        event_init.deleted = move(deleted_list);

        // 1. Let event be the result of creating an Event using CookieChangeEvent.
        // 2. Set event’s type attribute to type.
        auto event = CookieChangeEvent::create(realm, HTML::EventNames::change, event_init);

        // 3. Set event’s bubbles and cancelable attributes to false.
        event->set_bubbles(false);
        event->set_cancelable(false);

        // 7. Dispatch event at target.
        this->dispatch_event(event);
    }));
}

}
