/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TrustedScript, https://w3c.github.io/trusted-types/dist/spec/#trusted-script

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

/// https://w3c.github.io/trusted-types/dist/spec/#trusted-script
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TrustedScript {
    data: Utf16String,
}

impl Wrappable for TrustedScript {}

impl TrustedScript {
    pub(crate) fn create(vm: &Vm, data: Utf16String) -> Ref<Self> {
        allocate(vm, |_| Self { data })
    }

    pub(crate) fn data(&self) -> &Utf16String {
        &self.data
    }

    // https://w3c.github.io/trusted-types/dist/spec/#trustedscript-stringification-behavior
    pub(crate) fn to_string(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // 1. return the associated data value.
        Ok(self.data.clone())
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedscript-tojson
    pub(crate) fn to_json(&self, cx: &mut CallContext) -> WebIdlResult<Utf16String> {
        // 1. return the associated data value.
        self.to_string(cx)
    }
}

/// A new TrustedScript with the data, a raw AK::Utf16String that C++ hands over.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_trusted_types_create_trusted_script(vm: &Vm, data: Utf16String) -> *mut c_void {
    cell_into_abi(TrustedScript::create(vm, data))
}

/// The associated data of the TrustedScript, a raw AK::Utf16String that the call lends for as long as the object lives.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_trusted_types_trusted_script_data(cell: &InterfaceCell) -> usize {
    cell.object::<TrustedScript>().data().raw_identity()
}
