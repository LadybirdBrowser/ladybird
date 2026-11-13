/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TrustedHTML, https://w3c.github.io/trusted-types/dist/spec/#trusted-html

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

/// https://w3c.github.io/trusted-types/dist/spec/#trusted-html
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TrustedHTML {
    data: Utf16String,
}

impl Wrappable for TrustedHTML {}

impl TrustedHTML {
    pub(crate) fn create(vm: &Vm, data: Utf16String) -> Ref<Self> {
        allocate(vm, |_| Self { data })
    }

    pub(crate) fn data(&self) -> &Utf16String {
        &self.data
    }

    // https://w3c.github.io/trusted-types/dist/spec/#trustedhtml-stringification-behavior
    pub(crate) fn to_string(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // 1. return the associated data value.
        Ok(self.data.clone())
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedhtml-tojson
    pub(crate) fn to_json(&self, cx: &mut CallContext) -> WebIdlResult<Utf16String> {
        // 1. return the associated data value.
        self.to_string(cx)
    }
}

/// A new TrustedHTML with the data, a raw AK::Utf16String that C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, and `data` a raw string that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_create_trusted_html(vm: *mut c_void, data: usize) -> *mut c_void {
    // SAFETY: The caller passes the live VM and hands over the string.
    let (vm, data) = unsafe { (&*vm.cast::<Vm>(), Utf16String::from_raw_owned(data)) };
    cell_into_abi(TrustedHTML::create(vm, data))
}

/// The associated data of the TrustedHTML, a raw AK::Utf16String that the call lends for as long as the object lives.
///
/// # Safety
///
/// `cell` must be the live cell of a TrustedHTML.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_trusted_html_data(cell: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell.
    unsafe { cell_from_abi::<TrustedHTML>(cell) }.data().raw_identity()
}
