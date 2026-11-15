/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GeolocationPositionError, https://w3c.github.io/geolocation/#dom-geolocationpositionerror

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
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
