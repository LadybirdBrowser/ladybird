/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::Trace;

use crate::bindings::cpp_cell::CppCell;

/// https://html.spec.whatwg.org/multipage/document-sequences.html#navigable
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppNavigable(CppCell);

impl CppNavigable {
    /// # Safety
    ///
    /// `navigable` must be a live Navigable.
    pub(crate) unsafe fn from_raw(navigable: *mut c_void) -> Self {
        // SAFETY: The caller passes a live Navigable.
        Self(unsafe { CppCell::from_raw(navigable) })
    }
}
