/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AuthenticatorAttestationResponse, https://w3c.github.io/webauthn/#authenticatorattestationresponse

use ak::Utf16String;
use libjs_rust::api::{ArrayBuffer, Gc, Trace};

use super::authenticator_response::{AuthenticatorResponse, array_buffer_from_bytes};
use crate::bindings::interface::{Inherits, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
pub(crate) struct AuthenticatorAttestationResponse {
    authenticator_response: AuthenticatorResponse,
    // https://w3c.github.io/webauthn/#dom-authenticatorattestationresponse-attestationobject
    attestation_object: Gc<ArrayBuffer>,
    // https://w3c.github.io/webauthn/#dom-authenticatorattestationresponse-transports-slot
    transports: Vec<Utf16String>,
    /// The authenticator data contained within attestationObject.
    authenticator_data: Vec<u8>,
    /// The DER SubjectPublicKeyInfo of the new credential, or None if the public key algorithm is not one that the
    /// user agent understands.
    public_key: Option<Vec<u8>>,
    /// The COSEAlgorithmIdentifier of the new credential.
    public_key_algorithm: i32,
}

impl Wrappable for AuthenticatorAttestationResponse {}

impl Inherits<AuthenticatorResponse> for AuthenticatorAttestationResponse {
    fn parent(&self) -> &AuthenticatorResponse {
        &self.authenticator_response
    }
}

impl AuthenticatorAttestationResponse {
    pub(crate) fn new(
        client_data_json: Gc<ArrayBuffer>,
        attestation_object: Gc<ArrayBuffer>,
        transports: Vec<Utf16String>,
        authenticator_data: Vec<u8>,
        public_key: Option<Vec<u8>>,
        public_key_algorithm: i32,
    ) -> Self {
        Self {
            authenticator_response: AuthenticatorResponse::new(client_data_json),
            attestation_object,
            transports,
            authenticator_data,
            public_key,
            public_key_algorithm,
        }
    }

    pub(crate) fn attestation_object_buffer(&self) -> Gc<ArrayBuffer> {
        self.attestation_object
    }

    pub(crate) fn transports(&self) -> &[Utf16String] {
        &self.transports
    }

    pub(crate) fn authenticator_data_bytes(&self) -> &[u8] {
        &self.authenticator_data
    }

    pub(crate) fn public_key_bytes(&self) -> Option<&[u8]> {
        self.public_key.as_deref()
    }

    pub(crate) fn public_key_algorithm(&self) -> i32 {
        self.public_key_algorithm
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorattestationresponse-attestationobject
    pub(crate) fn attestation_object(&self, _: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        Ok(self.attestation_object)
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorattestationresponse-gettransports
    pub(crate) fn get_transports(&self, _: &mut CallContext) -> WebIdlResult<Vec<Utf16String>> {
        // This operation returns the value of [[transports]].
        Ok(self.transports.clone())
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorattestationresponse-getauthenticatordata
    pub(crate) fn get_authenticator_data(&self, cx: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        // This operation returns the authenticator data contained within attestationObject.
        Ok(array_buffer_from_bytes(cx.vm(), cx.realm(), &self.authenticator_data))
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorattestationresponse-getpublickey
    pub(crate) fn get_public_key(&self, cx: &mut CallContext) -> WebIdlResult<Option<Gc<ArrayBuffer>>> {
        // This operation returns the DER SubjectPublicKeyInfo of the new credential, or null if this is not available.
        Ok(self
            .public_key
            .as_ref()
            .map(|public_key| array_buffer_from_bytes(cx.vm(), cx.realm(), public_key)))
    }

    // https://w3c.github.io/webauthn/#dom-authenticatorattestationresponse-getpublickeyalgorithm
    pub(crate) fn get_public_key_algorithm(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        // This operation returns the COSEAlgorithmIdentifier of the new credential.
        Ok(self.public_key_algorithm)
    }
}
