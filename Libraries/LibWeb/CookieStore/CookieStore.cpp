/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/URL.h>
#include <LibWeb/CookieStore/CookieStore.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>

namespace Web::CookieStore {

struct CookieABI {
    ReadonlyBytes name;
    ReadonlyBytes value;
    i64 expiry_time;
};

struct ParsedCookieABI {
    ReadonlyBytes name;
    ReadonlyBytes value;
    bool has_expiry_time;
    i64 expiry_time;
    bool has_max_age;
    ReadonlyBytes max_age;
    bool has_domain;
    ReadonlyBytes domain;
    ReadonlyBytes path;
    bool secure_attribute_present;
    HTTP::Cookie::SameSite same_site_attribute;
};

extern "C" {
// Defined in Bindings/Rust/src/cookie_store/exports.rs.
void* web_rust_cookie_store_create_cookie_store(JSVM*, void* realm);
void web_rust_cookie_store_process_cookie_changes(JSVM*, CookieStore*, ReadonlySpan<CookieABI>);

// Called by Bindings/Rust/src/cookie_store/cookie_store.rs.
void web_cookie_store_request_all_cookies(HTML::EnvironmentSettingsObject*, URL::FFI::RustUrl const*, void* ctx, void (*on_cookie)(void*, ReadonlyBytes name, ReadonlyBytes value, bool http_only));
void web_cookie_store_set_cookie(HTML::EnvironmentSettingsObject*, URL::FFI::RustUrl const*, ParsedCookieABI const*);
}

GC::Ref<CookieStore> CookieStore::create(JS::Realm& realm)
{
    return *static_cast<CookieStore*>(web_rust_cookie_store_create_cookie_store(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

void CookieStore::process_cookie_changes(Vector<HTTP::Cookie::Cookie> const& cookies)
{
    Vector<CookieABI> changes;
    changes.ensure_capacity(cookies.size());
    for (auto const& cookie : cookies)
        changes.unchecked_append({ cookie.name.bytes(), cookie.value.bytes(), cookie.expiry_time.milliseconds_since_epoch() });

    web_rust_cookie_store_process_cookie_changes(JS::HostABI::vm_to_abi(JS::VM::the()), this, changes);
}

static PageClient& page_client(HTML::EnvironmentSettingsObject& settings_object)
{
    auto& global_object = settings_object.global_object();
    if (auto* window = HTML::window_from_global_object(global_object))
        return window->page().client();
    return Bindings::service_worker_global_scope_from_global_object(global_object)->page()->client();
}

void web_cookie_store_request_all_cookies(HTML::EnvironmentSettingsObject* settings_object, URL::FFI::RustUrl const* rust_url, void* ctx, void (*on_cookie)(void*, ReadonlyBytes name, ReadonlyBytes value, bool http_only))
{
    URL::URL url;
    url.set_from_rust(*rust_url);

    for (auto const& cookie : page_client(*settings_object).page_did_request_all_cookies_cookiestore(settings_object->id, url))
        on_cookie(ctx, cookie.name.bytes(), cookie.value.bytes(), cookie.http_only);
}

void web_cookie_store_set_cookie(HTML::EnvironmentSettingsObject* settings_object, URL::FFI::RustUrl const* rust_url, ParsedCookieABI const* cookie)
{
    URL::URL url;
    url.set_from_rust(*rust_url);

    HTTP::Cookie::ParsedCookie parsed_cookie {};
    parsed_cookie.name = MUST(String::from_utf8(StringView { cookie->name }));
    parsed_cookie.value = MUST(String::from_utf8(StringView { cookie->value }));
    if (cookie->has_expiry_time)
        parsed_cookie.expiry_time_from_expires_attribute = UnixDateTime::from_milliseconds_since_epoch(cookie->expiry_time);
    if (cookie->has_max_age)
        HTTP::Cookie::on_max_age_attribute(parsed_cookie, StringView { cookie->max_age });
    if (cookie->has_domain)
        parsed_cookie.domain = MUST(String::from_utf8(StringView { cookie->domain }));
    parsed_cookie.path = MUST(String::from_utf8(StringView { cookie->path }));
    parsed_cookie.secure_attribute_present = cookie->secure_attribute_present;
    parsed_cookie.same_site_attribute = cookie->same_site_attribute;

    page_client(*settings_object).page_did_set_cookie(settings_object->id, url, parsed_cookie, HTTP::Cookie::Source::NonHttp);
}

}
