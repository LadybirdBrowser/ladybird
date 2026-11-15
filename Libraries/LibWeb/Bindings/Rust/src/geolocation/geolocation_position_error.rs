/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GeolocationPositionError, https://w3c.github.io/geolocation/#dom-geolocationpositionerror

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::geolocation::geolocation_position_error_code::GeolocationPositionErrorCode;

// https://w3c.github.io/geolocation/#dom-geolocationpositionerror
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct GeolocationPositionError {
    #[gc(untraced)]
    code: GeolocationPositionErrorCode,
}

impl Wrappable for GeolocationPositionError {}

impl GeolocationPositionError {
    pub(crate) fn create(vm: &Vm, code: GeolocationPositionErrorCode) -> Ref<Self> {
        allocate(vm, |_| Self { code })
    }

    pub(crate) fn code(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        Ok(self.code as u16)
    }

    // https://w3c.github.io/geolocation/#message-attribute
    pub(crate) fn message(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The message attribute is a developer-friendly textual description of the code attribute.
        let message: &[u8] = match self.code {
            GeolocationPositionErrorCode::PositionUnavailable => b"Position unavailable",
            GeolocationPositionErrorCode::PermissionDenied => b"Permission denied",
            GeolocationPositionErrorCode::Timeout => b"Timeout",
        };
        Ok(Utf16String::from_ascii(message))
    }
}

/// A new GeolocationPositionError with the code.
///
/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_create_geolocation_position_error(
    vm: *mut c_void,
    code: u16,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(GeolocationPositionError::create(
        vm,
        GeolocationPositionErrorCode::from_u16(code),
    ))
}
