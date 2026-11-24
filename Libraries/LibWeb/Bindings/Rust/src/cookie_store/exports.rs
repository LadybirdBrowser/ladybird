/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points through which LibWeb's C++ code creates a CookieStore and reports cookie changes to it, declared
//! in Libraries/LibWeb/CookieStore/CookieStore.cpp.

use core::ffi::c_void;

use libjs_rust::api::{Gc, Realm, Vm};

use crate::bindings::interface::{InterfaceCell, cell_into_abi};
use crate::cookie_store::cookie_store::{Cookie, CookieStore};

/// A changed cookie, with its name and value in UTF-8 and its expiry time in milliseconds since the epoch.
#[repr(C)]
pub struct CookieAbi<'a> {
    name: ak::Span<'a, u8>,
    value: ak::Span<'a, u8>,
    expiry_time: i64,
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_cookie_store_create_cookie_store(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(CookieStore::create(vm, realm))
}

/// https://cookiestore.spec.whatwg.org/#process-cookie-changes
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_cookie_store_process_cookie_changes(
    vm: &Vm,
    cookie_store: &InterfaceCell,
    changes: ak::Span<'_, CookieAbi<'_>>,
) {
    let cookie_store = cookie_store.object::<CookieStore>();
    let changes = changes
        .as_slice()
        .iter()
        .map(|cookie| Cookie {
            name: String::from_utf8_lossy(cookie.name.as_slice()).into_owned(),
            value: String::from_utf8_lossy(cookie.value.as_slice()).into_owned(),
            expiry_time_in_milliseconds: cookie.expiry_time,
        })
        .collect();
    CookieStore::process_cookie_changes(cookie_store, vm, changes);
}
