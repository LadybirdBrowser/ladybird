/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::Trace;

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_fetch_body_length(body: *mut c_void, out_length: *mut u64) -> bool;
}

/// https://fetch.spec.whatwg.org/#concept-body
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppBody(CppCell);

impl CppBody {
    /// # Safety
    ///
    /// `body` must be a live body.
    pub(crate) unsafe fn from_raw(body: *mut c_void) -> Self {
        // SAFETY: The caller passes a live body.
        Self(unsafe { CppCell::from_raw(body) })
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://fetch.spec.whatwg.org/#concept-body-total-bytes
    pub(crate) fn length(&self) -> Option<u64> {
        let mut length = 0;
        // SAFETY: The body is live, and the length outlives the call.
        unsafe { web_fetch_body_length(self.as_ptr(), &raw mut length) }.then_some(length)
    }
}
