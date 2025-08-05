/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
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

        // 2. If name is given, then run these steps:
        if (name.has_value()) {
            // 1. Let cookieName be the result of running UTF-8 decode without BOM on cookie’s name.
            // 2. If cookieName does not equal name, then continue.
            if (cookie.name != name.value())
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
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_string));

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
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_string));

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
        // 1. Let list be the results of running query cookies with url and options["name"] (if present).
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
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Document origin is opaque"_string));

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

}
