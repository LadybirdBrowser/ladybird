/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Handles to the C++ environment settings objects.

use core::ffi::c_void;

use libjs_rust::api::{Gc, Object, Trace};

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_bindings_relevant_settings_object(global_object: *mut c_void) -> *mut c_void;
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
}
