/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_dom_is_valid_attribute_local_name(name: usize) -> bool;
    fn web_dom_element_attribute(
        element: *mut c_void,
        name: ak::Span<'_, u8>,
        value: *mut c_void,
        on_value: unsafe extern "C" fn(*mut c_void, usize),
    );
}

unsafe extern "C" fn on_value(value: *mut c_void, raw_value: usize) {
    // SAFETY: value is the result slot of the caller, and LibWeb hands over a reference to the string.
    unsafe { *value.cast::<Option<Utf16String>>() = Some(Utf16String::from_raw_owned(raw_value)) };
}

/// https://dom.spec.whatwg.org/#concept-element
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppElement(CppCell);

impl CppElement {
    /// # Safety
    ///
    /// `element` must be a live Element.
    pub(crate) unsafe fn from_raw(element: *mut c_void) -> Self {
        // SAFETY: The caller passes a live Element.
        Self(unsafe { CppCell::from_raw(element) })
    }

    /// https://dom.spec.whatwg.org/#concept-element-attributes-get-value
    pub(crate) fn attribute(&self, name: &str) -> Option<Utf16String> {
        let mut value = None;
        // SAFETY: The element is live, the name is lent for the call, and the value outlives it.
        unsafe {
            web_dom_element_attribute(
                self.0.as_ptr(),
                ak::Span::new(name.as_bytes()),
                (&raw mut value).cast(),
                on_value,
            );
        }
        value
    }
}

// https://dom.spec.whatwg.org/#valid-attribute-local-name
pub(crate) fn is_valid_attribute_local_name(name: &Utf16String) -> bool {
    // SAFETY: The name is live for the call.
    unsafe { web_dom_is_valid_attribute_local_name(name.raw_identity()) }
}
