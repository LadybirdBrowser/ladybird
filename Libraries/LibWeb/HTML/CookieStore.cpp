/*
 * Copyright (c) 2025, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/HTML/CookieStore.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

String CookieInit::to_string() const
{
    StringBuilder builder;
    builder.appendff("CookieInit(name={}, value={}"sv, name, value);
    if (expires.has_value())
        builder.appendff(", expires={}"sv, expires.value());
    if (domain.has_value())
        builder.appendff(", domain={}"sv, domain.value());
    if (path.has_value())
        builder.appendff(", path={}"sv, path.value());
    builder.appendff(", same_site={}, partitioned={})"sv, Bindings::idl_enum_to_string(same_site), partitioned);
    return MUST(builder.to_string());
}

String CookieStoreGetOptions::to_string() const
{
    Vector<String> parts;
    if (name.has_value())
        parts.append(MUST(String::formatted("name={}"sv, name.value())));
    if (url.has_value())
        parts.append(MUST(String::formatted("url={}"sv, url.value())));

    return MUST(String::formatted("CookieStoreGetOptions({})"sv, MUST(String::join(", "sv, parts))));
}

String CookieStoreDeleteOptions::to_string() const
{
    Vector<String> parts;
    parts.append(MUST(String::formatted("name={}"sv, name)));
    if (domain.has_value())
        parts.append(MUST(String::formatted("domain={}"sv, domain.value())));
    parts.append(MUST(String::formatted("path={}"sv, path)));
    parts.append(MUST(String::formatted("partitioned={}"sv, partitioned)));
    return MUST(String::formatted("CookieStoreDeleteOptions({})"sv, MUST(String::join(", "sv, parts))));
}

JS::Value CookieListItem::as_js_value(JS::Realm& realm) const
{
    auto& vm = realm.vm();
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto set_string = [&vm, object](FlyString const& key, String const& value) -> void {
        MUST(object->create_data_property(key, JS::PrimitiveString::create(vm, value)));
    };
    auto set_bool = [object](FlyString const& key, bool value) -> void {
        MUST(object->create_data_property(key, JS::Value(value)));
    };
    set_string("name"_fly_string, name);
    set_string("value"_fly_string, value);
    if (domain.has_value())
        set_string("domain"_fly_string, domain.value());
    set_string("path"_fly_string, path);
    if (expires.has_value())
        set_bool("expires"_fly_string, expires.value());
    set_bool("partitioned"_fly_string, partitioned);
    set_string("same_site"_fly_string, Bindings::idl_enum_to_string(same_site));
    MUST(object->create_data_property("partitioned"_fly_string, JS::Value(partitioned)));
    return object;
}

GC_DEFINE_ALLOCATOR(CookieStore);

GC::Ref<CookieStore> CookieStore::create(JS::Realm& realm, GC::Ref<Page> page)
{
    return realm.create<CookieStore>(realm, page);
}

CookieStore::CookieStore(JS::Realm& realm, GC::Ref<Page> page)
    : DOM::EventTarget(realm)
    , m_page(page)
{
}

void CookieStore::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CookieStore);
    Base::initialize(realm);
}

void CookieStore::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

// 3.1. https://wicg.github.io/cookie-store/#dom-cookiestore-get
GC::Ref<WebIDL::Promise> CookieStore::get(String const& name)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, name]() mutable {
        // 6.1 Let list be the results of running query cookies with url and name.
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
        auto const list = query_a_cookie(url, name);

        // FIXME 6.2 If list is failure, then reject p with a TypeError and abort these steps.

        // 6.3 If list is empty, then resolve p with null.
        if (list.is_empty()) {
            WebIDL::resolve_promise(realm, p, JS::js_null());
            return;
        }

        // 6.4 Otherwise, resolve p with the first item of list.
        auto first = list.get(0);
        if (first.has_value()) {
            auto cookie_list_item = first.release_value();
            WebIDL::resolve_promise(realm, p, cookie_list_item.as_js_value(realm));
        }
    }));

    // 7. Return p.
    return p;
}

// 3.1. https://wicg.github.io/cookie-store/#dom-cookiestore-get-options
GC::Ref<WebIDL::Promise> CookieStore::get(Optional<CookieStoreGetOptions> const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. If options is empty, then return a promise rejected with a TypeError.
    if (options.value().is_empty()) {
        auto& vm = realm.vm();
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("CookieStore.get: options must be provided"sv));
    }

    // 6. If options["url"] is present, then run these steps:
    if (options->url.has_value()) {
        auto& vm = realm.vm();

        // 6.1. Let parsed be the result of parsing options["url"] with settings’s API base URL.
        auto parsed = URL::Parser::basic_parse(options->url.value(), settings.api_base_url());

        // 6.2. If this’s relevant global object is a Window object and parsed does not equal url, then return a promise rejected with a TypeError.
        if (is<Window>(relevant_global_object(*this)) && !parsed.value().equals(url)) {
            return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("CookieStore.get: Invalid URL"sv));
        }

        // 6.3. If parsed’s origin and url’s origin are not the same origin, then return a promise rejected with a TypeError.
        if (parsed.value().origin() != url.origin()) {
            return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("CookieStore.get: URL origin does not match"sv));
        }

        // 6.4. Set url to parsed.
        url = parsed.value();
    }

    // 7. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 8. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, options]() mutable {
        // 8.1. Let list be the results of running query cookies with url and options["name"].
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
        auto const list = query_a_cookie(url, options->name);

        // FIXME 8.2 If list is failure, then reject p with a TypeError and abort these steps.

        // 8.3 If list is empty, then resolve p with null.
        if (list.is_empty()) {
            WebIDL::resolve_promise(realm, p, JS::js_null());
            return;
        }

        // 8.4 Otherwise, resolve p with the first item of list.
        auto first = list.get(0);
        if (first.has_value()) {
            auto cookie_list_item = first.release_value();
            WebIDL::resolve_promise(realm, p, cookie_list_item.as_js_value(realm));
        }
    }));

    // 9. Return p
    return p;
}

// 3.2. https://wicg.github.io/cookie-store/#CookieStore-getall
GC::Ref<WebIDL::Promise> CookieStore::get_all(String const& name)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, name]() mutable {
        // 6.1 Let list be the results of running query cookies with url and name.
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
        auto const list = query_a_cookie(url, name);

        // FIXME 6.2 If list is failure, then reject p with a TypeError and abort these steps.

        // 6.3 Otherwise, resolve p with list.
        JS::Array* array = MUST(JS::Array::create(realm, list.size()));
        for (u32 i = 0; i < list.size(); ++i) {
            auto& cookie_list_item = list[i];
            MUST(array->create_data_property_or_throw(i, cookie_list_item.as_js_value(realm)));
        }
        WebIDL::resolve_promise(realm, p, array);
    }));

    // 7. Return p.
    return p;
}

// 3.2. https://wicg.github.io/cookie-store/#CookieStore-getall-options
GC::Ref<WebIDL::Promise> CookieStore::get_all(Optional<CookieStoreGetOptions> const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. If options["url"] is present, then run these steps:
    if (options->url.has_value()) {
        auto& vm = realm.vm();

        // 5.1. Let parsed be the result of parsing options["url"] with settings’s API base URL.
        auto parsed = URL::Parser::basic_parse(options->url.value(), settings.api_base_url());

        // 5.2. If this’s relevant global object is a Window object and parsed does not equal url, then return a promise rejected with a TypeError.
        if (is<Window>(relevant_global_object(*this)) && !parsed.value().equals(url)) {
            return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("CookieStore.get: Invalid URL"sv));
        }

        // 5.3. If parsed’s origin and url’s origin are not the same origin, then return a promise rejected with a TypeError.
        if (parsed.value().origin() != url.origin()) {
            return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("CookieStore.get: URL origin does not match"sv));
        }

        // 5.4. Set url to parsed.
        url = parsed.value();
    }

    // 6. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 8. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, options]() mutable {
        // 8.1. Let list be the results of running query cookies with url and options["name"].
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
        auto const list = query_a_cookie(url, options->name);

        // FIXME 8.2 If list is failure, then reject p with a TypeError and abort these steps.

        // 8.3 Otherwise, resolve p with list.
        JS::Array* array = MUST(JS::Array::create(realm, list.size()));
        for (u32 i = 0; i < list.size(); ++i) {
            auto& cookie_list_item = list[i];
            MUST(array->create_data_property_or_throw(i, cookie_list_item.as_js_value(realm)));
        }
        WebIDL::resolve_promise(realm, p, array);
    }));

    // 9. Return p
    return p;
}

// 3.3. https://wicg.github.io/cookie-store/#dom-cookiestore-set
GC::Ref<WebIDL::Promise> CookieStore::set(String const& name, String const& value)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let domain be null.
    auto domain = Optional<String> {};

    // 6. Let path be "/".
    auto path = "/"_string;

    // 7.Let sameSite be strict.
    auto same_site = Bindings::CookieSameSite::Strict;

    // 8. Let partitioned be false.
    auto partitioned = false;

    // 9. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 10. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, name, value, domain, path, same_site, partitioned]() mutable {
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 10.1. Let r be the result of running set a cookie with url, name, value, domain, path, sameSite, and partitioned.
        auto const r = set_a_cookie(url, name, value, {}, domain, path, same_site, partitioned);

        // 10.2. If r is failure, then reject p with a TypeError and abort these steps.
        if (!r) {
            WebIDL::reject_promise(realm, p, JS::TypeError::create(realm));
            return;
        }

        // 10.3. Resolve p with undefined.
        WebIDL::resolve_promise(realm, p, JS::js_undefined());
    }));

    // 11. Return p.
    return p;
}

// 3.3. https://wicg.github.io/cookie-store/#dom-cookiestore-set-options
GC::Ref<WebIDL::Promise> CookieStore::set(CookieInit const& cookie_init)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, cookie_init]() mutable {
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 6.1. Let r be the result of running set a cookie with url, name, value, domain, path, sameSite, and partitioned.
        auto const r = set_a_cookie(url, cookie_init.name, cookie_init.value, cookie_init.expires, cookie_init.domain, cookie_init.path, cookie_init.same_site, cookie_init.partitioned);

        // 6.2. If r is failure, then reject p with a TypeError and abort these steps.
        if (!r) {
            WebIDL::reject_promise(realm, p, JS::TypeError::create(realm));
            return;
        }

        // 6.3. Resolve p with undefined.
        WebIDL::resolve_promise(realm, p, JS::js_undefined());
    }));

    // 7. Return p.
    return p;
}

// 3.4. https://wicg.github.io/cookie-store/#dom-cookiestore-delete
GC::Ref<WebIDL::Promise> CookieStore::delete_(String const& name)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, name]() mutable {
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 6.1. Let r be the result of running delete a cookie with url, name, null, "/", true, and "strict".
        auto r = delete_a_cookie(url, name, {}, "/"_string, false);

        // 6.2. If r is failure, then reject p with a TypeError and abort these steps.
        if (!r) {
            WebIDL::reject_promise(realm, p, JS::TypeError::create(realm));
            return;
        }

        // 6.3 Resolve p with undefined.
        WebIDL::resolve_promise(realm, p, JS::js_undefined());
    }));

    // 7. Return p.
    return p;
}

// 3.4. https://wicg.github.io/cookie-store/#dom-cookiestore-delete-options
GC::Ref<WebIDL::Promise> CookieStore::delete_(CookieStoreDeleteOptions const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be this’s relevant settings object.
    auto& settings = relevant_settings_object(*this);

    // 2. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if (origin.is_opaque()) {
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "CookieStore.set: Cannot set cookies in an opaque origin"_string));
    }

    // 4. Let url be settings’s creation URL.
    auto url = settings.creation_url;

    // 5. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 6. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, p, url, options]() mutable {
        TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 6.1. Let r be the result of running delete a cookie with url, options["name"], options["domain"], options["path"], and options["partitioned"].
        auto r = delete_a_cookie(url, options.name, options.domain, options.path, options.partitioned);

        // 6.2. If r is failure, then reject p with a TypeError and abort these steps.
        if (!r) {
            WebIDL::reject_promise(realm, p, JS::TypeError::create(realm));
            return;
        }

        // 6.3 Resolve p with undefined.
        WebIDL::resolve_promise(realm, p, JS::js_undefined());
    }));

    // 7. Return p.
    return p;
}

// 7.1. https://wicg.github.io/cookie-store/#query-cookies-algorithm
Vector<CookieListItem> CookieStore::query_a_cookie(URL::URL const& url, Optional<String> name) const
{
    // 1. Perform the steps defined in Cookies § Retrieval Model to compute the "cookie-string from a given cookie store"
    // with url as request-uri. The cookie-string itself is ignored, but the intermediate cookie-list is used in subsequent steps.
    //
    // For the purposes of the steps, the cookie-string is being generated for a "non-HTTP" API.
    auto cookie_list = m_page->client().page_did_request_all_cookies(url, Web::Cookie::Source::NonHttp);

    // 2. Let list be a new list.
    Vector<CookieListItem> list;

    // 3. For each cookie in cookie-list, run these steps.
    for (auto const& cookie : cookie_list) {
        // 3.1. If cookie’s http-only-flag is true, then continue.
        if (cookie.http_only) {
            continue;
        }

        // 3.2. If name is given, then run these steps.
        if (name.has_value()) {
            // 3.2.1. Let cookieName be the result of running UTF-8 decode without BOM on cookie’s name.
            auto cookie_name = String::from_utf8_with_replacement_character(cookie.name, String::WithBOMHandling::No);

            // 3.2.2. If cookieName does not equal name, then continue.
            if (cookie_name != name.value()) {
                continue;
            }
        }

        auto same_site = Bindings::CookieSameSite::None;
        switch (cookie.same_site) {
        case Cookie::SameSite::None:
            same_site = Bindings::CookieSameSite::None;
            break;
        case Cookie::SameSite::Strict:
            same_site = Bindings::CookieSameSite::Strict;
            break;
        case Cookie::SameSite::Lax:
            same_site = Bindings::CookieSameSite::Lax;
            break;
        case Cookie::SameSite::Default:
            break;
        }

        // 3.3. Let item be the result of running create a CookieListItem from cookie.
        CookieListItem item {
            .name = cookie.name,
            .value = cookie.value,
            .domain = cookie.domain,
            .path = cookie.path,
            .expires = cookie.expiry_time.milliseconds_since_epoch(),
            .secure = cookie.secure,
            .same_site = same_site,
            .partitioned = false
        };

        // 3.4. Append item to list.
        list.append(item);
    }

    // 4. Return list.
    return list;
}

// 7.2. https://wicg.github.io/cookie-store/#set-cookie-algorithm
bool CookieStore::set_a_cookie(URL::URL const& url, String name, String value, Optional<HighResolutionTime::DOMHighResTimeStamp> expires, Optional<String> domain, Optional<String> path, Bindings::CookieSameSite same_site, bool partitioned)
{
    // To set a cookie with url, name, value, optional expires, domain, path, sameSite, and partitioned run the following steps:
    auto is_invalid = [](String const& string) {
        // 1a. If string contains U+003B (;)
        if (string.contains(';'))
            return true;

        // 1b. any C0 control character except U+0009 TAB or U+007F DELETE, then return failure.
        if (Cookie::cookie_contains_invalid_control_character(string))
            return true;

        return false;
    };

    // 1. If name or value contain U+003B (;), any C0 control character except U+0009 TAB, or U+007F DELETE, then return failure.
    if (is_invalid(name) || is_invalid(value)) {
        return false;
    }

    // 2. If name’s length is 0:
    if (name.is_empty()) {
        // 2.1. If value contains U+003D (=), then return failure.
        if (value.contains('='))
            return false;

        // 2.2. If value’s length is 0, then return failure.
        if (value.is_empty())
            return false;

        // 2.3. If value, byte-lowercased, starts with `__host-` or `__secure-`, then return failure.
        if (value.starts_with_bytes("__host-"sv, AK::CaseSensitivity::CaseInsensitive) || value.starts_with_bytes("__secure-"sv, AK::CaseSensitivity::CaseInsensitive)) {
            return false;
        }
    }

    // 3. Let encodedName be the result of UTF-8 encoding name.
    auto encoded_name = Utf8View(name);

    // 4. Let encodedValue be the result of UTF-8 encoding value.
    auto encoded_value = Utf8View(value);

    // 5. If the byte sequence length of encodedName plus the byte sequence length of encodedValue is greater than the maximum name/value pair size, then return failure.
    if (encoded_name.byte_length() + encoded_value.byte_length() > 4096) {
        return false;
    }

    // 6. Let host be url’s host
    auto host = url.host();

    // 7. Let attributes be a new list.
    Cookie::ParsedCookie parsed_cookie {
        .name = MUST(String::from_utf8(encoded_name.as_string())),
        .value = MUST(String::from_utf8(encoded_value.as_string())),
    };

    // 8. If domain is not null, then run these steps:
    if (domain.has_value()) {
        auto domain_value = domain.value();
        // 8.1. If domain starts with U+002E (.), then return failure.
        if (domain_value.starts_with('.')) {
            return false;
        }

        // 8.2. If name, byte-lowercased, starts with `__host-`, then return failure.
        if (name.starts_with_bytes("__host-"sv, AK::CaseSensitivity::CaseInsensitive)) {
            return false;
        }

        // 8.3. If host does not equal domain and host does not end with U+002E (.) followed by domain, then return failure.
        if (!Web::Cookie::domain_matches(host->serialize(), domain_value)) {
            return false;
        }

        // 8.4. Let encodedDomain be the result of UTF-8 encoding domain.
        auto encoded_domain = Utf8View(domain_value);

        // 8.5. If the byte sequence length of encodedDomain is greater than the maximum attribute value size, then return failure.
        if (encoded_domain.byte_length() > 4096) {
            return false;
        }
        parsed_cookie.domain = MUST(String::from_utf8(encoded_domain.as_string()));
    }

    // 9. If expires is given, then append `Expires`/expires (date serialized) to attributes.
    if (expires.has_value()) {
        parsed_cookie.expiry_time_from_expires_attribute = UnixDateTime::from_milliseconds_since_epoch(expires.value());
    }

    // 10. If path is not null:
    if (path.has_value()) {
        // 10.1. If path does not start with U+002F (/), then return failure.
        if (!path->starts_with('/')) {
            return false;
        }

        // 10.2. If path is not U+002F (/), and name, byte-lowercased, starts with `__host-`, then return failure.
        if (path != "/"sv && name.starts_with_bytes("__host-"sv, AK::CaseSensitivity::CaseInsensitive)) {
            return false;
        }

        // 10.3. Let encodedPath be the result of UTF-8 encoding path.
        auto encoded_path = Utf8View(path.value());

        // 10.4. If the byte sequence length of encodedPath is greater than the maximum attribute value size, then return failure.
        if (encoded_path.byte_length() > 4096) {
            return false;
        }

        // 10.5. Append `Path`/encodedPath to attributes.
        parsed_cookie.path = MUST(String::from_utf8(encoded_path.as_string()));
    } else {
        // 11. Otherwise, append `Path`/ U+002F (/) to attributes.
        parsed_cookie.path = "/"_string;
    }

    // 12. Append `Secure`/`` to attributes.
    parsed_cookie.secure_attribute_present = true;

    // 13. Switch on sameSite:
    switch (same_site) {
    // 13.1. If sameSite is "none", then append `SameSite`/`None` to attributes.
    case Bindings::CookieSameSite::None: {
        parsed_cookie.same_site_attribute = Cookie::SameSite::None;
        break;
    }

    // 13.2. If sameSite is "strict", then append `SameSite`/`Strict` to attributes.
    case Bindings::CookieSameSite::Strict: {
        parsed_cookie.same_site_attribute = Cookie::SameSite::Strict;
        break;
    }

    // 13.3. If sameSite is "lax", then append `SameSite`/`Lax` to attributes.
    case Bindings::CookieSameSite::Lax: {
        parsed_cookie.same_site_attribute = Cookie::SameSite::Lax;
        break;
    }

    default:
        break;
    }

    // 14. If partitioned is true, then append `Partitioned`/`` to attributes.
    if (partitioned) {
        parsed_cookie.partitioned = true;
    }

    // 15. Perform the steps defined in Cookies § Storage Model for when the user agent "receives a cookie"
    // with url as request-uri, encodedName as cookie-name, encodedValue as cookie-value, and attributes as
    // cookie-attribute-list.

    // For the purposes of the steps, the newly-created cookie was received from a "non-HTTP" API.
    m_page->client().page_did_set_cookie(url, parsed_cookie, Cookie::Source::NonHttp);

    // 16. Return success.
    return true;
}

// 7.3. https://wicg.github.io/cookie-store/#delete-cookie-algorithm
bool CookieStore::delete_a_cookie(URL::URL const& url, String name, Optional<String> domain, Optional<String> path, bool partitioned)
{
    // 1. Let expires be the earliest representable date represented as a timestamp.
    auto expires = 0;

    // 2. Let value be the empty string.
    String value;

    // 3. If name’s length is 0, then set value to any non-empty implementation-defined string.
    if (name.code_points().length() == 0) {
        value = "__LadybirdNameLess"_string;
    }

    // 4. Let sameSite be "strict".
    auto same_site = Bindings::CookieSameSite::Strict;

    // 5. Return the results of running set a cookie with url, name, value, expires, domain, path, sameSite, and partitioned.
    return set_a_cookie(url, name, value, expires, domain, path, same_site, partitioned);
}

}
