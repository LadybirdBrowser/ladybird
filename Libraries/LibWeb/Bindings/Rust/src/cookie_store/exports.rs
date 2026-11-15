/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points through which LibWeb's C++ code creates a CookieStore and reports cookie changes to it, declared
//! in Libraries/LibWeb/CookieStore/CookieStore.cpp.

use core::ffi::c_void;

use libjs_rust::api::Vm;

use crate::bindings::interface::{cell_from_abi, cell_into_abi, realm_arguments};
use crate::cookie_store::cookie_store::{Cookie, CookieStore};

/// A changed cookie, with its name and value in UTF-8 and its expiry time in milliseconds since the epoch.
#[repr(C)]
pub struct CookieAbi<'a> {
    name: ak::Span<'a, u8>,
    value: ak::Span<'a, u8>,
    expiry_time: i64,
}

/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_cookie_store_create_cookie_store(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(CookieStore::create(vm, realm))
}

/// https://cookiestore.spec.whatwg.org/#process-cookie-changes
///
/// # Safety
///
/// The pointers must be the live VM and the live cell of a CookieStore object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_cookie_store_process_cookie_changes(
    vm: *mut c_void,
    cookie_store: *mut c_void,
    changes: ak::Span<'_, CookieAbi<'_>>,
) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    // SAFETY: The caller passes a live CookieStore object.
    let cookie_store = unsafe { cell_from_abi::<CookieStore>(cookie_store) };
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
