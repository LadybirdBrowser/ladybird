/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CookieStore, https://cookiestore.spec.whatwg.org/#cookiestore

use core::ffi::c_void;
use core::ptr::NonNull;
use std::time::{SystemTime, UNIX_EPOCH};

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, Trace, Value, Vm};
use liburl_rust::origin::Host;
use liburl_rust::url::{ExcludeFragment, UrlInput, parse_host};
use liburl_rust::{BasicParseOptions, RustUrl, Url, basic_parse};

use crate::bindings::conversions::UsvString;
#[cfg(web_idl_bindings)]
use crate::bindings::conversions::{ToJs, sequence_to_js};
use crate::bindings::environment_settings_object::{CppEnvironmentSettingsObject, relevant_settings_object};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, create_rejected_promise_from_exception, deferred_invoke, promise_realm,
    queue_a_global_task, reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::cookie_store::cookie_change_event::{CookieChangeEvent, CookieChangeEventInit};
use crate::dom::document::is_a_registrable_domain_suffix_of_or_is_equal_to;
use crate::dom::event::EventInit;

#[repr(C)]
struct ParsedCookieAbi<'a> {
    name: ak::Span<'a, u8>,
    value: ak::Span<'a, u8>,
    has_expiry_time: bool,
    expiry_time: i64,
    has_max_age: bool,
    max_age: ak::Span<'a, u8>,
    has_domain: bool,
    domain: ak::Span<'a, u8>,
    path: ak::Span<'a, u8>,
    secure_attribute_present: bool,
    same_site_attribute: u8,
}

/// The values of HTTP::Cookie::SameSite.
const SAME_SITE_NONE: u8 = 1;
const SAME_SITE_STRICT: u8 = 2;
const SAME_SITE_LAX: u8 = 3;

unsafe extern "C" {
    fn web_cookie_store_request_all_cookies(
        settings_object: *mut c_void,
        url: *const RustUrl,
        ctx: *mut c_void,
        on_cookie: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, ak::Span<'_, u8>, bool),
    );
    fn web_cookie_store_set_cookie(settings_object: *mut c_void, url: *const RustUrl, cookie: *const ParsedCookieAbi);
}

/// The dictionary CookieListItem of CookieStore/CookieStore.idl.
#[derive(Clone)]
pub(crate) struct CookieListItem {
    pub(crate) name: Option<UsvString>,
    pub(crate) value: Option<UsvString>,
}

/// The dictionary CookieStoreGetOptions of CookieStore/CookieStore.idl.
#[derive(Default)]
pub(crate) struct CookieStoreGetOptions {
    pub(crate) name: Option<UsvString>,
    pub(crate) url: Option<UsvString>,
}

/// The enumeration CookieSameSite of CookieStore/CookieStore.idl.
#[derive(Clone, Copy, Default)]
pub(crate) enum CookieSameSite {
    #[default]
    Strict,
    Lax,
    None,
}

/// The dictionary CookieInit of CookieStore/CookieStore.idl.
pub(crate) struct CookieInit {
    pub(crate) name: UsvString,
    pub(crate) value: UsvString,
    pub(crate) expires: Option<f64>,
    pub(crate) domain: Option<UsvString>,
    pub(crate) path: UsvString,
    pub(crate) same_site: CookieSameSite,
    pub(crate) partitioned: bool,
    pub(crate) max_age: Option<i64>,
}

/// The dictionary CookieStoreDeleteOptions of CookieStore/CookieStore.idl.
pub(crate) struct CookieStoreDeleteOptions {
    pub(crate) name: UsvString,
    pub(crate) domain: Option<UsvString>,
    pub(crate) path: UsvString,
    pub(crate) partitioned: bool,
}

/// A cookie of the cookie store, as the UI process reports its changes.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Cookie {
    pub(crate) name: String,
    pub(crate) value: String,
    pub(crate) expiry_time_in_milliseconds: i64,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum EmptyOptionsAllowed {
    No,
    Yes,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum FirstItemOnly {
    No,
    Yes,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct CookieListRequest {
    environment: CppEnvironmentSettingsObject,
    #[gc(untraced)]
    url: Url,
    name: Option<String>,
    global: Gc<Object>,
    promise: WebIdlPromise,
    #[gc(untraced)]
    first_item_only: FirstItemOnly,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct CookieMutation {
    environment: CppEnvironmentSettingsObject,
    #[gc(untraced)]
    url: Url,
    name: String,
    value: String,
    #[gc(untraced)]
    expires: Option<f64>,
    domain: Option<String>,
    path: String,
    #[gc(untraced)]
    same_site: CookieSameSite,
    partitioned: bool,
    max_age: Option<i64>,
    global: Gc<Object>,
    promise: WebIdlPromise,
    failure_message: &'static str,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct CookieDeletion {
    environment: CppEnvironmentSettingsObject,
    #[gc(untraced)]
    url: Url,
    name: String,
    domain: Option<String>,
    path: String,
    partitioned: bool,
    global: Gc<Object>,
    promise: WebIdlPromise,
    failure_message: &'static str,
}

fn cookie_list_item_to_js(cx: &CallContext, item: CookieListItem) -> Value {
    // NB: The generated bindings convert the dictionary, which a build of the crate on its own leaves out.
    #[cfg(web_idl_bindings)]
    return ToJs::to_js(item, cx).expect("a cookie list item converts to a JS value");
    #[cfg(not(web_idl_bindings))]
    {
        let _ = (item, cx);
        unreachable!("only the generated bindings resolve a promise with the dictionary")
    }
}

fn cookie_list_to_js(cx: &CallContext, cookie_list: Vec<CookieListItem>) -> Value {
    #[cfg(web_idl_bindings)]
    return sequence_to_js(cx, cookie_list, |item| ToJs::to_js(item, cx))
        .expect("a cookie list converts to a JS value");
    #[cfg(not(web_idl_bindings))]
    {
        let _ = (cookie_list, cx);
        unreachable!("only the generated bindings resolve a promise with the dictionary")
    }
}

fn resolve_cookie_list_item_or_null_promise(vm: &Vm, promise: WebIdlPromise, cookie_list: Vec<CookieListItem>) {
    let Some(first_item) = cookie_list.into_iter().next() else {
        resolve_promise(vm, promise, Value::NULL);
        return;
    };

    let value = cookie_list_item_to_js(&CallContext::new(vm, promise_realm(promise)), first_item);
    resolve_promise(vm, promise, value);
}

fn resolve_cookie_list_promise(vm: &Vm, promise: WebIdlPromise, cookie_list: Vec<CookieListItem>) {
    let result = cookie_list_to_js(&CallContext::new(vm, promise_realm(promise)), cookie_list);
    resolve_promise(vm, promise, result);
}

fn validate_cookie_store_get_options(
    realm: Gc<Realm>,
    options: &CookieStoreGetOptions,
    empty_options_allowed: EmptyOptionsAllowed,
) -> Result<Url, Exception> {
    // 1. Let settings be this's relevant settings object.
    let settings = relevant_settings_object(realm.global_object());

    // 2. Let origin be settings's origin.
    let origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if origin.is_opaque() {
        return Err(Exception::Dom(
            "SecurityError",
            Utf16String::from_utf8("Document origin is opaque"),
        ));
    }

    // 4. Let url be settings's creation URL.
    let mut url = settings.creation_url();

    // 5. If options is empty, then return a promise rejected with a TypeError.
    if empty_options_allowed == EmptyOptionsAllowed::No && options.name.is_none() && options.url.is_none() {
        return Err(Exception::type_error("CookieStoreGetOptions is empty"));
    }

    // 6. If options["url"] exists:
    if let Some(options_url) = &options.url {
        // 1. Let parsed be the result of parsing options["url"] with settings's API base URL.
        let api_base_url = settings.api_base_url();
        let options_url = options_url.to_utf16();
        let parsed = basic_parse(
            UrlInput::from(&*options_url),
            BasicParseOptions::new().base_url(&api_base_url),
        );

        // 2. If parsed is failure or parsed does not equal url with exclude fragments set to true, then return a promise
        //    rejected with a TypeError.
        let Some(parsed) = parsed else {
            return Err(Exception::type_error("url is invalid"));
        };
        if parsed.serialize(ExcludeFragment::Yes) != url.serialize(ExcludeFragment::Yes) {
            return Err(Exception::type_error("url does not match creation URL"));
        }

        // 3. If parsed's origin and url's origin are not the same origin, then return a promise rejected with a TypeError.
        if !parsed.origin().is_same_origin(&url.origin()) {
            return Err(Exception::type_error(
                "url's origin does not match creation URL's origin",
            ));
        }

        // 4. Set url to parsed.
        url = parsed;
    }

    Ok(url)
}

fn validate_cookie_store_mutation(realm: Gc<Realm>) -> Result<Url, Exception> {
    // 1. Let settings be this's relevant settings object.
    let settings = relevant_settings_object(realm.global_object());

    // 2. Let origin be settings's origin.
    let origin = settings.origin();

    // 3. If origin is an opaque origin, then return a promise rejected with a "SecurityError" DOMException.
    if origin.is_opaque() {
        return Err(Exception::Dom(
            "SecurityError",
            Utf16String::from_utf8("Document origin is opaque"),
        ));
    }

    // 4. Let url be settings's creation URL.
    Ok(settings.creation_url())
}

fn complete_cookie_list_request(vm: &Vm, request: CookieListRequest, cookie_list: Vec<CookieListItem>) {
    // AD-HOC: Queue a global task to perform the next steps
    // Spec issue: https://github.com/whatwg/cookiestore/issues/239
    queue_a_global_task(
        vm,
        request.global,
        TaskSource::Unspecified,
        (
            request.promise,
            CookieListResult { list: cookie_list },
            request.first_item_only == FirstItemOnly::Yes,
        ),
        |vm, (promise, cookie_list, first_item_only)| {
            with_temporary_execution_context(promise_realm(promise), || {
                if first_item_only {
                    resolve_cookie_list_item_or_null_promise(vm, promise, cookie_list.list);
                } else {
                    resolve_cookie_list_promise(vm, promise, cookie_list.list);
                }
            });
        },
    );
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct CookieListResult {
    #[gc(untraced)]
    list: Vec<CookieListItem>,
}

fn complete_cookie_mutation(
    vm: &Vm,
    global: Gc<Object>,
    promise: WebIdlPromise,
    failure_message: &'static str,
    result: bool,
) {
    // NB: The UI's notification of this mutation's cookie changes is handled from a deferred invocation scheduled while
    //     waiting for its reply. Queue the task below after that, so the change event is fired first.
    deferred_invoke(
        vm,
        (global, promise, result, failure_message),
        |vm, (global, promise, result, failure_message)| {
            // AD-HOC: Queue a global task to perform the next steps
            // Spec issue: https://github.com/whatwg/cookiestore/issues/239
            queue_a_global_task(
                vm,
                global,
                TaskSource::Unspecified,
                (promise, result, failure_message),
                |vm, (promise, result, failure_message)| {
                    with_temporary_execution_context(promise_realm(promise), || {
                        if !result {
                            reject_promise_with_exception(vm, promise, Exception::type_error(failure_message));
                            return;
                        }

                        resolve_promise(vm, promise, Value::UNDEFINED);
                    });
                },
            );
        },
    );
}

// https://cookiestore.spec.whatwg.org/#create-a-cookielistitem
fn create_a_cookie_list_item(name: &str, value: &str) -> CookieListItem {
    // 1. Let name be the result of running UTF-8 decode without BOM on cookie’s name.
    // 2. Let value be the result of running UTF-8 decode without BOM on cookie’s value.
    // 3. Return «[ "name" → name, "value" → value ]».
    CookieListItem {
        name: Some(UsvString::from_utf8(name)),
        value: Some(UsvString::from_utf8(value)),
    }
}

// https://cookiestore.spec.whatwg.org/#normalize-a-cookie-name-or-value
fn normalize(input: &str) -> &str {
    // Remove all U+0009 TAB and U+0020 SPACE that are at the start or end of input.
    input.trim_matches(['\t', ' '])
}

// https://cookiestore.spec.whatwg.org/#query-cookies
fn query_cookies(environment: &CppEnvironmentSettingsObject, url: &Url, name: Option<&str>) -> Vec<CookieListItem> {
    struct QueriedCookie {
        name: String,
        value: String,
        http_only: bool,
    }

    unsafe extern "C" fn on_cookie(ctx: *mut c_void, name: ak::Span<'_, u8>, value: ak::Span<'_, u8>, http_only: bool) {
        // SAFETY: ctx is the list below, and LibWeb lends the name and the value, which are UTF-8, for the call.
        unsafe { &mut *ctx.cast::<Vec<QueriedCookie>>() }.push(QueriedCookie {
            name: String::from_utf8_lossy(name.as_slice()).into_owned(),
            value: String::from_utf8_lossy(value.as_slice()).into_owned(),
            http_only,
        });
    }

    // 1. Perform the steps defined in Cookies § Retrieval Model to compute the "cookie-string from a given cookie store"
    //    with url as request-uri. The cookie-string itself is ignored, but the intermediate cookie-list is used in subsequent steps.
    //    For the purposes of the steps, the cookie-string is being generated for a "non-HTTP" API.
    let mut cookie_list: Vec<QueriedCookie> = Vec::new();
    let rust_url = RustUrl::from_url(url);
    // SAFETY: The settings object is live, and the URL and the list outlive the call.
    unsafe {
        web_cookie_store_request_all_cookies(
            environment.as_ptr(),
            &raw const rust_url,
            (&raw mut cookie_list).cast(),
            on_cookie,
        );
    }

    // 2. Let list be « ».
    let mut list = Vec::new();

    // 3. For each cookie of cookie-list:
    for cookie in &cookie_list {
        // 1. Assert: cookie’s http-only-flag is false.
        assert!(!cookie.http_only);

        // 2. If name is non-null:
        if let Some(name) = name {
            // 1. Normalize name.
            let normalized_name = normalize(name);

            // 2. Let cookieName be the result of running UTF-8 decode without BOM on cookie’s name.
            // 3. If cookieName does not equal name, then continue.
            if cookie.name != normalized_name {
                continue;
            }
        }

        // 3. Let item be the result of running create a CookieListItem from cookie.
        let item = create_a_cookie_list_item(&cookie.name, &cookie.value);

        // 4. Append item to list.
        list.push(item);
    }

    // 4. Return list.
    list
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-layered-cookies#name-cookie-default-path
fn cookie_default_path(mut path: Vec<&str>) -> Vec<&str> {
    // 1. Assert: path is a non-empty list.
    assert!(!path.is_empty());

    // 2. If path's size is greater than 1, then remove path's last item.
    if path.len() > 1 {
        path.pop();
    }
    // 3. Otherwise, set path[0] to the empty string.
    else {
        path[0] = "";
    }

    // 4. Return path.
    path
}

// https://fetch.spec.whatwg.org/#serialized-cookie-default-path
fn serialized_cookie_default_path(url: &Url) -> String {
    // 1. Let cloneURL be a clone of url.
    let mut clone_url = url.clone();

    // 2. Set cloneURL’s path to the cookie default path of cloneURL’s path.
    clone_url.set_path(cookie_default_path(url.path_segments().collect()));

    // 3. Return the URL path serialization of cloneURL.
    clone_url.serialize_path().to_owned()
}

const MAXIMUM_NAME_VALUE_PAIR_SIZE: usize = 4096;
const MAXIMUM_ATTRIBUTE_VALUE_SIZE: usize = 1024;

/// LibHTTP's MAXIMUM_COOKIE_AGE: 400 days.
const MAXIMUM_COOKIE_AGE_IN_MILLISECONDS: i64 = 400 * 24 * 60 * 60 * 1000;

fn unix_now_in_milliseconds() -> i64 {
    let since_epoch = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    i64::try_from(since_epoch.as_millis()).unwrap_or(i64::MAX)
}

// https://cookiestore.spec.whatwg.org/#set-a-cookie
#[allow(clippy::too_many_arguments, reason = "the algorithm has these arguments")]
fn set_a_cookie(
    environment: &CppEnvironmentSettingsObject,
    url: &Url,
    name: &str,
    value: &str,
    expires: Option<f64>,
    domain: Option<&str>,
    path: &str,
    same_site: CookieSameSite,
    partitioned: bool,
    max_age: Option<i64>,
) -> bool {
    // 1. Normalize name.
    let name = normalize(name);

    // 2. Normalize value.
    let value = normalize(value);

    // 3. If name or value contain U+003B (;), any C0 control character except U+0009 TAB, or U+007F DELETE, then return failure.
    let is_forbidden = |code_point: char| {
        code_point == ';' || (code_point <= '\u{1F}' && code_point != '\t') || code_point == '\u{7F}'
    };
    if name.contains(is_forbidden) || value.contains(is_forbidden) {
        return false;
    }

    // 4. If name contains U+003D (=), then return failure.
    if name.contains('=') {
        return false;
    }

    // 5. If name’s length is 0:
    if name.is_empty() {
        // 1. If value contains U+003D (=), then return failure.
        if value.contains('=') {
            return false;
        }

        // 2. If value’s length is 0, then return failure.
        if value.is_empty() {
            return false;
        }

        // 3. If value, byte-lowercased, starts with `__host-`, `__host-http-`, `__http-`, or `__secure-`, then return failure.
        let value_byte_lowercased = value.to_ascii_lowercase();
        if value_byte_lowercased.starts_with("__host-")
            || value_byte_lowercased.starts_with("__host-http-")
            || value_byte_lowercased.starts_with("__http-")
            || value_byte_lowercased.starts_with("__secure-")
        {
            return false;
        }
    }

    // 6. If name, byte-lowercased, starts with `__host-http-` or `__http-`, then return failure.
    let name_ascii_lowercased = name.to_ascii_lowercase();
    if name_ascii_lowercased.starts_with("__host-http-") || name_ascii_lowercased.starts_with("__http-") {
        return false;
    }

    // 7. Let encodedName be the result of UTF-8 encoding name.
    // 8. Let encodedValue be the result of UTF-8 encoding value.
    let encoded_name = name.as_bytes();
    let encoded_value = value.as_bytes();

    // 9. If the byte sequence length of encodedName plus the byte sequence length of encodedValue is greater than the
    //    maximum name/value pair size, then return failure.
    if encoded_name.len() + encoded_value.len() > MAXIMUM_NAME_VALUE_PAIR_SIZE {
        return false;
    }

    // 10. Let host be url’s host
    let host = url.serialized_host().map(|serialization| Host {
        kind: url.host_kind(),
        serialization: serialization.to_owned(),
    });

    // 11. Let attributes be « ».
    let mut encoded_domain = None;

    // 12. If domain is non-null:
    if let Some(domain) = domain {
        // 1. If domain starts with U+002E (.), then return failure.
        if domain.starts_with('.') {
            return false;
        }

        // 2. If name, byte-lowercased, starts with `__host-`, then return failure.
        if name_ascii_lowercased.starts_with("__host-") {
            return false;
        }

        // 3. If domain is not a registrable domain suffix of and is not equal to host, then return failure.
        let Some(host) = &host else {
            return false;
        };
        if !is_a_registrable_domain_suffix_of_or_is_equal_to(&Utf16String::from_utf8(domain), host) {
            return false;
        }

        // 4. Let parsedDomain be the result of host parsing domain.
        let parsed_domain = parse_host(domain);

        // 5. Assert: parsedDomain is not failure.
        let parsed_domain = parsed_domain.expect("a registrable domain suffix of a host parses");

        // 6. Let encodedDomain be the result of UTF-8 encoding parsedDomain.
        let domain = parsed_domain.serialization;

        // 7. If the byte sequence length of encodedDomain is greater than the maximum attribute value size, then return failure.
        if domain.len() > MAXIMUM_ATTRIBUTE_VALUE_SIZE {
            return false;
        }

        // 8. Append (`Domain`, encodedDomain) to attributes.
        encoded_domain = Some(domain);
    }

    let mut expiry_time = None;
    let mut encoded_max_age = None;

    // 13. If expires is non-null:
    if let Some(expires) = expires {
        // 1. If maxAge is non-null, then return failure.
        if max_age.is_some() {
            return false;
        }

        // 2. Append (`Expires`, expires (date serialized)) to attributes.
        #[allow(
            clippy::cast_possible_truncation,
            reason = "a timestamp is milliseconds since the epoch"
        )]
        let mut expires = expires as i64;

        // https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.6.1
        // 3. Let cookie-age-limit be the maximum age of the cookie (which SHOULD be 400 days in the future or sooner, see
        //    Section 5.5).
        let cookie_age_limit = unix_now_in_milliseconds().saturating_add(MAXIMUM_COOKIE_AGE_IN_MILLISECONDS);

        // 4. If the expiry-time is more than cookie-age-limit, the user agent MUST set the expiry time to cookie-age-limit
        //    in seconds.
        if expires.div_euclid(1000) > cookie_age_limit.div_euclid(1000) {
            expires = cookie_age_limit;
        }

        expiry_time = Some(expires);
    }
    // 14. Otherwise, if maxAge is non-null, then append (`Max-Age`, ToString(maxAge)) to attributes.
    else if let Some(max_age) = max_age {
        encoded_max_age = Some(max_age.to_string());
    }

    // 15. If path is the empty string, then set path to the serialized cookie default path of url.
    let path = if path.is_empty() {
        serialized_cookie_default_path(url)
    } else {
        path.to_owned()
    };

    // 16. If path does not start with U+002F (/), then return failure.
    if !path.starts_with('/') {
        return false;
    }

    // 17. If path is not U+002F (/), and name, byte-lowercased, starts with `__host-`, then return failure.
    if path != "/" && name_ascii_lowercased.starts_with("__host-") {
        return false;
    }

    // 18. Let encodedPath be the result of UTF-8 encoding path.
    let encoded_path = path.as_bytes();

    // 19. If the byte sequence length of encodedPath is greater than the maximum attribute value size, then return failure.
    if encoded_path.len() > MAXIMUM_ATTRIBUTE_VALUE_SIZE {
        return false;
    }

    // 20. Append (`Path`, encodedPath) to attributes.
    // 21. Append (`Secure`, ``) to attributes.
    let secure_attribute_present = true;

    // 22. Switch on sameSite:
    let same_site_attribute = match same_site {
        // -> "none"
        //    Append (`SameSite`, `None`) to attributes.
        CookieSameSite::None => SAME_SITE_NONE,
        // -> "strict"
        //    Append (`SameSite`, `Strict`) to attributes.
        CookieSameSite::Strict => SAME_SITE_STRICT,
        // -> "lax"
        //    Append (`SameSite`, `Lax`) to attributes.
        CookieSameSite::Lax => SAME_SITE_LAX,
    };

    // FIXME: 23. If partitioned is true, Append (`Partitioned`, ``) to attributes.
    let _ = partitioned;

    // 24. Perform the steps defined in Cookies § Storage Model for when the user agent "receives a cookie" with url as
    //     request-uri, encodedName as cookie-name, encodedValue as cookie-value, and attributes as cookie-attribute-list.
    //     For the purposes of the steps, the newly-created cookie was received from a "non-HTTP" API.
    let attributes = ParsedCookieAbi {
        name: ak::Span::new(encoded_name),
        value: ak::Span::new(encoded_value),
        has_expiry_time: expiry_time.is_some(),
        expiry_time: expiry_time.unwrap_or_default(),
        has_max_age: encoded_max_age.is_some(),
        max_age: ak::Span::new(encoded_max_age.as_deref().unwrap_or_default().as_bytes()),
        has_domain: encoded_domain.is_some(),
        domain: ak::Span::new(encoded_domain.as_deref().unwrap_or_default().as_bytes()),
        path: ak::Span::new(encoded_path),
        secure_attribute_present,
        same_site_attribute,
    };
    let rust_url = RustUrl::from_url(url);
    // SAFETY: The settings object is live, and the URL and the attributes outlive the call.
    unsafe { web_cookie_store_set_cookie(environment.as_ptr(), &raw const rust_url, &raw const attributes) };

    // 25. Return success.
    true
}

// https://cookiestore.spec.whatwg.org/#delete-a-cookie
fn delete_a_cookie(
    environment: &CppEnvironmentSettingsObject,
    url: &Url,
    name: &str,
    domain: Option<&str>,
    path: &str,
    partitioned: bool,
) -> bool {
    // 1. Normalize name.
    let name = normalize(name);

    // 2. Let value be the empty string.
    let mut value = "";

    // 3. If name’s length is 0, then set value to any non-empty implementation-defined string.
    if name.is_empty() {
        value = "ladybird";
    }

    // 4. Return the results of running set a cookie with url, name, value, null, domain, path, "strict", partitioned,
    //    and 0.
    set_a_cookie(
        environment,
        url,
        name,
        value,
        None,
        domain,
        path,
        CookieSameSite::Strict,
        partitioned,
        Some(0),
    )
}

// https://cookiestore.spec.whatwg.org/#cookie-change
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct CookieChange {
    cookie: Cookie,
    #[gc(untraced)]
    r#type: CookieChangeType,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum CookieChangeType {
    Changed,
    Deleted,
}

// https://cookiestore.spec.whatwg.org/#observable-changes
fn observable_changes(changes: Vec<Cookie>) -> Vec<CookieChange> {
    // The observable changes for url are the set of cookie changes to cookies in a cookie store which meet the
    // requirements in step 1 of Cookies § Retrieval Algorithm’s steps to compute the "cookie-string from a given
    // cookie store" with url as request-uri, for a "non-HTTP" API.
    let now = unix_now_in_milliseconds();

    changes
        .into_iter()
        .map(|cookie| {
            // A cookie change is a cookie and a type (either changed or deleted):
            // - A cookie which is removed due to an insertion of another cookie with the same name, domain, and path is ignored.
            // - A newly-created cookie which is not immediately evicted is considered changed.
            // - A newly-created cookie which is immediately evicted is considered deleted.
            // - A cookie which is otherwise evicted or removed is considered deleted
            let r#type = if cookie.expiry_time_in_milliseconds < now {
                CookieChangeType::Deleted
            } else {
                CookieChangeType::Changed
            };
            CookieChange { cookie, r#type }
        })
        .collect()
}

// https://cookiestore.spec.whatwg.org/#prepare-lists
fn prepare_lists(changes: &[CookieChange]) -> (Vec<CookieListItem>, Vec<CookieListItem>) {
    // 1. Let changedList be « ».
    let mut changed_list = Vec::new();

    // 2. Let deletedList be « ».
    let mut deleted_list = Vec::new();

    // 3. For each change in changes, run these steps:
    for change in changes {
        // 1. Let item be the result of running create a CookieListItem from change’s cookie.
        let mut item = create_a_cookie_list_item(&change.cookie.name, &change.cookie.value);

        // 2. If change’s type is changed, then append item to changedList.
        if change.r#type == CookieChangeType::Changed {
            changed_list.push(item);
        }
        // 3. Otherwise, run these steps:
        else {
            // 1. Set item["value"] to undefined.
            item.value = None;

            // 2. Append item to deletedList.
            deleted_list.push(item);
        }
    }

    // 4. Return changedList and deletedList.
    (changed_list, deleted_list)
}

// https://cookiestore.spec.whatwg.org/#cookiestore
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CookieStore {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
}

impl Wrappable for CookieStore {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for CookieStore {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl CookieStore {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        let cookie_store = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            realm,
        });
        cookie_store.event_target.set_owner(cookie_store);
        cookie_store
    }

    fn request_cookie_list(
        &self,
        vm: &Vm,
        options: &CookieStoreGetOptions,
        empty_options_allowed: EmptyOptionsAllowed,
        first_item_only: FirstItemOnly,
        in_parallel: fn(&Vm, CookieListRequest),
    ) -> WebIdlPromise {
        let url = match validate_cookie_store_get_options(self.realm, options, empty_options_allowed) {
            Ok(url) => url,
            Err(exception) => return create_rejected_promise_from_exception(vm, self.realm, exception),
        };

        let promise = create_promise(vm, self.realm);
        in_parallel(
            vm,
            CookieListRequest {
                environment: relevant_settings_object(self.realm.global_object()),
                url,
                name: options.name.as_ref().map(UsvString::to_utf8),
                global: self.realm.global_object(),
                promise,
                first_item_only,
            },
        );
        promise
    }

    pub(crate) fn get_with_name(&self, cx: &mut CallContext, name: UsvString) -> WebIdlResult<WebIdlPromise> {
        let options = CookieStoreGetOptions {
            name: Some(name),
            url: None,
        };
        Ok(self.request_cookie_list(
            cx.vm(),
            &options,
            EmptyOptionsAllowed::No,
            FirstItemOnly::Yes,
            Self::get_in_parallel,
        ))
    }

    // https://cookiestore.spec.whatwg.org/#dom-cookiestore-get-options
    pub(crate) fn get(&self, cx: &mut CallContext, options: CookieStoreGetOptions) -> WebIdlResult<WebIdlPromise> {
        Ok(self.request_cookie_list(
            cx.vm(),
            &options,
            EmptyOptionsAllowed::No,
            FirstItemOnly::Yes,
            Self::get_in_parallel,
        ))
    }

    fn get_in_parallel(vm: &Vm, request: CookieListRequest) {
        // 8. Run the following steps in parallel:
        deferred_invoke(vm, request, |vm, request| {
            // 1. Let list be the results of running query cookies with url and options["name"] with default null.
            let list = query_cookies(&request.environment, &request.url, request.name.as_deref());
            complete_cookie_list_request(vm, request, list);
        });
    }

    pub(crate) fn get_all_with_name(&self, cx: &mut CallContext, name: UsvString) -> WebIdlResult<WebIdlPromise> {
        let options = CookieStoreGetOptions {
            name: Some(name),
            url: None,
        };
        Ok(self.request_cookie_list(
            cx.vm(),
            &options,
            EmptyOptionsAllowed::Yes,
            FirstItemOnly::No,
            Self::get_all_in_parallel,
        ))
    }

    // https://cookiestore.spec.whatwg.org/#dom-cookiestore-getall-options
    pub(crate) fn get_all(&self, cx: &mut CallContext, options: CookieStoreGetOptions) -> WebIdlResult<WebIdlPromise> {
        Ok(self.request_cookie_list(
            cx.vm(),
            &options,
            EmptyOptionsAllowed::Yes,
            FirstItemOnly::No,
            Self::get_all_in_parallel,
        ))
    }

    fn get_all_in_parallel(vm: &Vm, request: CookieListRequest) {
        // 7. Run the following steps in parallel:
        deferred_invoke(vm, request, |vm, request| {
            // 1. Let list be the results of running query cookies with url and options["name"] with default null.
            let list = query_cookies(&request.environment, &request.url, request.name.as_deref());
            complete_cookie_list_request(vm, request, list);
        });
    }

    fn request_cookie_mutation(&self, vm: &Vm, options: &CookieInit, failure_message: &'static str) -> WebIdlPromise {
        let url = match validate_cookie_store_mutation(self.realm) {
            Ok(url) => url,
            Err(exception) => return create_rejected_promise_from_exception(vm, self.realm, exception),
        };

        let promise = create_promise(vm, self.realm);
        let mutation = CookieMutation {
            environment: relevant_settings_object(self.realm.global_object()),
            url,
            name: options.name.to_utf8(),
            value: options.value.to_utf8(),
            expires: options.expires,
            domain: options.domain.as_ref().map(UsvString::to_utf8),
            path: options.path.to_utf8(),
            same_site: options.same_site,
            partitioned: options.partitioned,
            max_age: options.max_age,
            global: self.realm.global_object(),
            promise,
            failure_message,
        };

        // 6. Run the following steps in parallel:
        deferred_invoke(vm, mutation, |vm, mutation| {
            // 1. Let r be the result of running set a cookie with url, options["name"], options["value"], options["expires"],
            //    options["domain"], options["path"], options["sameSite"], options["partitioned"], and options["maxAge"].
            let result = set_a_cookie(
                &mutation.environment,
                &mutation.url,
                &mutation.name,
                &mutation.value,
                mutation.expires,
                mutation.domain.as_deref(),
                &mutation.path,
                mutation.same_site,
                mutation.partitioned,
                mutation.max_age,
            );
            complete_cookie_mutation(vm, mutation.global, mutation.promise, mutation.failure_message, result);
        });
        promise
    }

    pub(crate) fn set_with_name_and_value(
        &self,
        cx: &mut CallContext,
        name: UsvString,
        value: UsvString,
    ) -> WebIdlResult<WebIdlPromise> {
        let options = CookieInit {
            name,
            value,
            expires: None,
            domain: None,
            path: UsvString::from_ascii(b"/"),
            same_site: CookieSameSite::Strict,
            partitioned: false,
            max_age: None,
        };
        Ok(self.request_cookie_mutation(cx.vm(), &options, "Name or value are malformed"))
    }

    // https://cookiestore.spec.whatwg.org/#dom-cookiestore-set-options
    pub(crate) fn set(&self, cx: &mut CallContext, options: CookieInit) -> WebIdlResult<WebIdlPromise> {
        Ok(self.request_cookie_mutation(cx.vm(), &options, "Name, value, domain or path are malformed"))
    }

    fn request_cookie_deletion(&self, vm: &Vm, options: &CookieStoreDeleteOptions) -> WebIdlPromise {
        let url = match validate_cookie_store_mutation(self.realm) {
            Ok(url) => url,
            Err(exception) => return create_rejected_promise_from_exception(vm, self.realm, exception),
        };

        let promise = create_promise(vm, self.realm);
        let deletion = CookieDeletion {
            environment: relevant_settings_object(self.realm.global_object()),
            url,
            name: options.name.to_utf8(),
            domain: options.domain.as_ref().map(UsvString::to_utf8),
            path: options.path.to_utf8(),
            partitioned: options.partitioned,
            global: self.realm.global_object(),
            promise,
            failure_message: "Name is malformed",
        };

        // 6. Run the following steps in parallel:
        deferred_invoke(vm, deletion, |vm, deletion| {
            // 1. Let r be the result of running delete a cookie with url, options["name"], options["domain"], options["path"],
            //    and options["partitioned"].
            let result = delete_a_cookie(
                &deletion.environment,
                &deletion.url,
                &deletion.name,
                deletion.domain.as_deref(),
                &deletion.path,
                deletion.partitioned,
            );
            complete_cookie_mutation(vm, deletion.global, deletion.promise, deletion.failure_message, result);
        });
        promise
    }

    pub(crate) fn delete_with_name(&self, cx: &mut CallContext, name: UsvString) -> WebIdlResult<WebIdlPromise> {
        let options = CookieStoreDeleteOptions {
            name,
            domain: None,
            path: UsvString::from_ascii(b"/"),
            partitioned: true,
        };
        Ok(self.request_cookie_deletion(cx.vm(), &options))
    }

    // https://cookiestore.spec.whatwg.org/#dom-cookiestore-delete-options
    pub(crate) fn delete(
        &self,
        cx: &mut CallContext,
        options: CookieStoreDeleteOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        Ok(self.request_cookie_deletion(cx.vm(), &options))
    }

    // https://cookiestore.spec.whatwg.org/#process-cookie-changes
    pub(crate) fn process_cookie_changes(this: Ref<Self>, vm: &Vm, all_changes: Vec<Cookie>) {
        // 1. Let url be window’s relevant settings object’s creation URL.
        // 2. Let changes be the observable changes for url.
        // 3. If changes is empty, then continue.
        // NB: We perform the URL-based filtering in the UI process so that we don't have to send all changed cookies over
        //     IPC to every tab.
        let changes = observable_changes(all_changes);

        // 4. Queue a global task on the DOM manipulation task source given window to fire a change event named "change"
        //    with changes at window’s CookieStore.
        queue_a_global_task(
            vm,
            this.realm.global_object(),
            TaskSource::DOMManipulation,
            (this, changes),
            |vm, (this, changes)| {
                with_temporary_execution_context(this.realm, || {
                    // https://cookiestore.spec.whatwg.org/#fire-a-change-event
                    // 4. Let changedList and deletedList be the result of running prepare lists from changes.
                    let (changed_list, deleted_list) = prepare_lists(&changes);

                    // 1. Let event be the result of creating an Event using CookieChangeEvent.
                    // 2. Set event’s type attribute to type.
                    // 3. Set event’s bubbles and cancelable attributes to false.
                    // 5. Set event’s changed attribute to changedList.
                    // 6. Set event’s deleted attribute to deletedList.
                    let event_init = CookieChangeEventInit {
                        parent: EventInit {
                            bubbles: false,
                            cancelable: false,
                            composed: false,
                        },
                        changed: Some(changed_list),
                        deleted: Some(deleted_list),
                    };
                    let event =
                        CookieChangeEvent::create(vm, this.realm, &Utf16String::from_utf8("change"), event_init);

                    // 7. Dispatch event at target.
                    this.event_target.dispatch_event(event.event());
                });
            },
        );
    }
}
