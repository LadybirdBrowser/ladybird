/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Handles to the C++ environment settings objects.

use core::ffi::c_void;

use core::ptr::NonNull;

use libjs_rust::api::{Gc, Object, Trace};

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_bindings_relevant_settings_object(global_object: *mut c_void) -> *mut c_void;
    fn web_bindings_environment_settings_object_global_object(settings_object: *mut c_void) -> *mut c_void;
    fn web_bindings_environment_settings_object_id(
        settings_object: *mut c_void,
        id: *mut c_void,
        on_id: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
    );
}

unsafe extern "C" fn on_id(id: *mut c_void, bytes: ak::Span<'_, u8>) {
    // SAFETY: id is the string of the caller, and LibWeb lends the id, which is ASCII, for the call.
    unsafe { *id.cast::<String>() = String::from_utf8_lossy(bytes.as_slice()).into_owned() };
}

/// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppEnvironmentSettingsObject(CppCell);

/// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-settings-object
pub(crate) fn relevant_settings_object(global_object: Gc<Object>) -> CppEnvironmentSettingsObject {
    // SAFETY: The global object is live, and LibWeb returns its live environment settings object.
    CppEnvironmentSettingsObject(unsafe {
        CppCell::from_raw(web_bindings_relevant_settings_object(global_object.as_ptr().cast()))
    })
}

impl CppEnvironmentSettingsObject {
    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
    pub(crate) fn id(&self) -> String {
        let mut id = String::new();
        // SAFETY: The settings object is live, and the id outlives the call.
        unsafe { web_bindings_environment_settings_object_id(self.as_ptr(), (&raw mut id).cast(), on_id) };
        id
    }

    /// https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-global
    pub(crate) fn global_object(&self) -> Gc<Object> {
        // SAFETY: The settings object is live, and LibWeb returns its live global object.
        let global_object = unsafe { web_bindings_environment_settings_object_global_object(self.as_ptr()) };
        // SAFETY: The global object is live.
        unsafe { Gc::from_non_null(NonNull::new(global_object.cast()).expect("a settings object has a global object")) }
    }
}
