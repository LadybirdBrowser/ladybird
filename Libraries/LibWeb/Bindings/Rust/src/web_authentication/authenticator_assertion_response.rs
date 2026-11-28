/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AuthenticatorAssertionResponse, https://w3c.github.io/webauthn/#authenticatorassertionresponse

use libjs_rust::api::{ArrayBuffer, Gc, Trace};

use super::authenticator_response::AuthenticatorResponse;
use crate::bindings::interface::{Inherits, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
pub(crate) struct AuthenticatorAssertionResponse {
    authenticator_response: AuthenticatorResponse,
    // https://w3c.github.io/webauthn/#dom-authenticatorassertionresponse-authenticatordata
    authenticator_data: Gc<ArrayBuffer>,
    // https://w3c.github.io/webauthn/#dom-authenticatorassertionresponse-signature
    signature: Gc<ArrayBuffer>,
    // https://w3c.github.io/webauthn/#dom-authenticatorassertionresponse-userhandle
    user_handle: Option<Gc<ArrayBuffer>>,
}

impl Wrappable for AuthenticatorAssertionResponse {}

impl Inherits<AuthenticatorResponse> for AuthenticatorAssertionResponse {
    fn parent(&self) -> &AuthenticatorResponse {
        &self.authenticator_response
    }
}

impl AuthenticatorAssertionResponse {
    pub(crate) fn new(
        client_data_json: Gc<ArrayBuffer>,
        authenticator_data: Gc<ArrayBuffer>,
        signature: Gc<ArrayBuffer>,
        user_handle: Option<Gc<ArrayBuffer>>,
    ) -> Self {
        Self {
            authenticator_response: AuthenticatorResponse::new(client_data_json),
            authenticator_data,
            signature,
            user_handle,
        }
    }

    pub(crate) fn authenticator_data_buffer(&self) -> Gc<ArrayBuffer> {
        self.authenticator_data
    }

    pub(crate) fn signature_buffer(&self) -> Gc<ArrayBuffer> {
        self.signature
    }

    pub(crate) fn user_handle_buffer(&self) -> Option<Gc<ArrayBuffer>> {
        self.user_handle
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorassertionresponse-authenticatordata
    pub(crate) fn authenticator_data(&self, _: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        Ok(self.authenticator_data)
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorassertionresponse-signature
    pub(crate) fn signature(&self, _: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        Ok(self.signature)
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorassertionresponse-userhandle
    pub(crate) fn user_handle(&self, _: &mut CallContext) -> WebIdlResult<Option<Gc<ArrayBuffer>>> {
        Ok(self.user_handle)
    }
}
