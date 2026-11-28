/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AuthenticatorResponse, https://w3c.github.io/webauthn/#authenticatorresponse

use libjs_rust::api::{ArrayBuffer, ArrayBufferShared, Gc, Realm, Trace, Vm};

use crate::bindings::conversions::copy_of_bytes_of_buffer;
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, WebIdlResult};

/// A new ArrayBuffer of the realm that holds a copy of the bytes.
pub(crate) fn array_buffer_from_bytes(vm: &Vm, realm: Gc<Realm>, bytes: &[u8]) -> Gc<ArrayBuffer> {
    ArrayBuffer::create_from_bytes(vm, realm, bytes, ArrayBufferShared::No)
}

/// A copy of the bytes that the buffer holds now, which are none if script detached it.
pub(crate) fn bytes_of_array_buffer(buffer: &ArrayBuffer) -> Vec<u8> {
    copy_of_bytes_of_buffer(buffer, 0..buffer.byte_length()).unwrap_or_default()
}

#[derive(Trace)]
pub(crate) struct AuthenticatorResponse {
    // https://w3c.github.io/webauthn/#dom-authenticatorresponse-clientdatajson
    client_data_json: Gc<ArrayBuffer>,
}

impl Wrappable for AuthenticatorResponse {}

impl AuthenticatorResponse {
    pub(crate) fn new(client_data_json: Gc<ArrayBuffer>) -> Self {
        Self { client_data_json }
    }

    pub(crate) fn client_data_json_buffer(&self) -> Gc<ArrayBuffer> {
        self.client_data_json
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorresponse-clientdatajson
    pub(crate) fn client_data_json(&self, _: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        Ok(self.client_data_json)
    }
}
