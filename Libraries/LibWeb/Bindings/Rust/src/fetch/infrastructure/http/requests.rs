/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

unsafe extern "C" {
    fn web_fetch_request_done(request: *mut c_void) -> bool;
    fn web_fetch_request_set_header(request: *mut c_void, name: ak::Span<'_, u8>, value: ak::Span<'_, u8>);
    fn web_fetch_request_delete_header(request: *mut c_void, name: ak::Span<'_, u8>);
    fn web_fetch_request_unref(request: *mut c_void);
}

/// https://fetch.spec.whatwg.org/#concept-request
pub(crate) struct CppRequest(NonNull<c_void>);

impl CppRequest {
    /// # Safety
    ///
    /// `request` must be a request that LibWeb hands over a reference to.
    pub(crate) unsafe fn adopt(request: *mut c_void) -> Self {
        Self(NonNull::new(request).expect("LibWeb hands over a request"))
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://fetch.spec.whatwg.org/#done-flag
    pub(crate) fn done(&self) -> bool {
        // SAFETY: This holds a reference to the request.
        unsafe { web_fetch_request_done(self.0.as_ptr()) }
    }

    /// https://fetch.spec.whatwg.org/#concept-header-list-set
    pub(crate) fn set_header(&self, name: &[u8], value: &[u8]) {
        // SAFETY: This holds a reference to the request, and the name and the value are lent for the call.
        unsafe { web_fetch_request_set_header(self.0.as_ptr(), ak::Span::new(name), ak::Span::new(value)) };
    }

    /// https://fetch.spec.whatwg.org/#concept-header-list-delete
    pub(crate) fn delete_header(&self, name: &[u8]) {
        // SAFETY: This holds a reference to the request, and the name is lent for the call.
        unsafe { web_fetch_request_delete_header(self.0.as_ptr(), ak::Span::new(name)) };
    }
}

impl Drop for CppRequest {
    fn drop(&mut self) {
        // SAFETY: This holds a reference to the request, which it lets go of.
        unsafe { web_fetch_request_unref(self.0.as_ptr()) };
    }
}
