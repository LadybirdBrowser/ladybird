/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::{Gc, Object, Realm, Trace};

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_fetch_controller_create(global_object: *mut c_void) -> *mut c_void;
    fn web_fetch_controller_terminate(controller: *mut c_void);
    fn web_fetch_controller_abort(controller: *mut c_void, realm: *mut c_void);
    fn web_fetch_controller_has_report_timing_steps(controller: *mut c_void) -> bool;
    fn web_fetch_controller_report_timing(controller: *mut c_void, global_object: *mut c_void);
}

/// https://fetch.spec.whatwg.org/#fetch-controller
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppFetchController(CppCell);

impl PartialEq for CppFetchController {
    fn eq(&self, other: &Self) -> bool {
        self.as_ptr() == other.as_ptr()
    }
}

impl CppFetchController {
    pub(crate) fn create(global_object: Gc<Object>) -> Self {
        // SAFETY: The global object is live, and LibWeb returns a new, live fetch controller.
        Self(unsafe { CppCell::from_raw(web_fetch_controller_create(global_object.as_ptr().cast())) })
    }

    /// # Safety
    ///
    /// `controller` must be a live fetch controller.
    pub(crate) unsafe fn from_raw(controller: *mut c_void) -> Self {
        // SAFETY: The caller passes a live fetch controller.
        Self(unsafe { CppCell::from_raw(controller) })
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://fetch.spec.whatwg.org/#fetch-controller-terminate
    pub(crate) fn terminate(&self) {
        // SAFETY: The fetch controller is live.
        unsafe { web_fetch_controller_terminate(self.as_ptr()) };
    }

    /// https://fetch.spec.whatwg.org/#fetch-controller-abort
    pub(crate) fn abort(&self, realm: Gc<Realm>) {
        // SAFETY: The fetch controller and the realm are live.
        unsafe { web_fetch_controller_abort(self.as_ptr(), realm.as_ptr().cast()) };
    }

    pub(crate) fn has_report_timing_steps(&self) -> bool {
        // SAFETY: The fetch controller is live.
        unsafe { web_fetch_controller_has_report_timing_steps(self.as_ptr()) }
    }

    /// https://fetch.spec.whatwg.org/#finalize-and-report-timing
    pub(crate) fn report_timing(&self, global_object: Gc<Object>) {
        // SAFETY: The fetch controller and the global object are live.
        unsafe { web_fetch_controller_report_timing(self.as_ptr(), global_object.as_ptr().cast()) };
    }
}
