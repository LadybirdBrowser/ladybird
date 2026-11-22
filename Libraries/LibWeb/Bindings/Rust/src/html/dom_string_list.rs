/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#domstringlist

use core::ffi::c_void;

use ak::{Span, Utf16String};
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMStringList {
    strings: Vec<Utf16String>,
}

impl Wrappable for DOMStringList {}

impl DOMStringList {
    pub(crate) fn create(vm: &Vm, strings: Vec<Utf16String>) -> Ref<Self> {
        allocate(vm, |_| Self { strings })
    }

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The length getter steps are to return this's associated list's size.
        Ok(u32::try_from(self.strings.len()).expect("a DOMStringList has fewer than 2^32 strings"))
    }

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Utf16String>> {
        // The item(index) method steps are to return the indexth item in this's associated list, or null if index
        // plus one is greater than this's associated list's size.
        Ok(self.strings.get(index as usize).cloned())
    }

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-domstringlist-contains
    pub(crate) fn contains(&self, _: &mut CallContext, string: Utf16String) -> WebIdlResult<bool> {
        // The contains(string) method steps are to return true if this's associated list contains string, and
        // false otherwise.
        Ok(self.strings.contains(&string))
    }
}

/// # Safety
///
/// The caller passes the live VM and transfers ownership of each string in the span.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_string_list_create(vm: &Vm, strings: Span<'_, usize>) -> *mut c_void {
    let strings = strings
        .as_slice()
        .iter()
        // SAFETY: The caller transfers one reference for each string.
        .map(|&string| unsafe { Utf16String::from_raw_owned(string) })
        .collect();
    cell_into_abi(DOMStringList::create(vm, strings))
}
