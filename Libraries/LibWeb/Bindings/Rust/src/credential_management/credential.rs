/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Credential, https://w3c.github.io/webappsec-credential-management/#credential

use ak::Utf16String;
use libjs_rust::api::{ArrayBuffer, Gc, Trace, Value};

use super::federated_credential::FederatedCredential;
use super::password_credential::PasswordCredential;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::promise::{WebIdlPromise, create_resolved_promise};
use crate::bindings::{CallContext, WebIdlResult};
use crate::web_authentication::public_key_credential::{PublicKeyCredential, base64url_of_array_buffer};

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialdata
pub(crate) struct CredentialData {
    pub(crate) id: UsvString,
}

/// https://w3c.github.io/webappsec-credential-management/#enumdef-credentialmediationrequirement
#[derive(Clone, Copy)]
pub(crate) enum CredentialMediationRequirement {
    Silent,
    Optional,
    Conditional,
    Required,
}

#[derive(Trace)]
pub(crate) struct Credential {
    id: CredentialId,
    r#type: Utf16String,
}

/// Where the id of a credential comes from.
#[derive(Trace)]
enum CredentialId {
    Text(UsvString),
    /// https://w3c.github.io/webauthn/#dom-publickeycredential-identifier-slot
    Identifier(Gc<ArrayBuffer>),
}

impl Wrappable for Credential {}

impl Credential {
    pub(crate) fn new(id: UsvString, r#type: &str) -> Self {
        Self {
            id: CredentialId::Text(id),
            r#type: Utf16String::from_utf8(r#type),
        }
    }

    /// A credential whose id is the base64url encoding of the data that its [[identifier]] internal slot has.
    pub(crate) fn with_identifier(identifier: Gc<ArrayBuffer>, r#type: &str) -> Self {
        Self {
            id: CredentialId::Identifier(identifier),
            r#type: Utf16String::from_utf8(r#type),
        }
    }

    pub(crate) fn id_string(&self) -> UsvString {
        match &self.id {
            CredentialId::Text(id) => id.clone(),
            // https://w3c.github.io/webauthn/#dom-publickeycredential-id
            // This attribute is inherited from Credential, though PublicKeyCredential overrides Credential's getter,
            // instead returning the base64url encoding of the data contained in the object's [[identifier]] internal
            // slot.
            CredentialId::Identifier(identifier) => {
                UsvString::from_utf16_string(&base64url_of_array_buffer(identifier))
            }
        }
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credential-id
    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.id_string())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credential-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.r#type.clone())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credential-isconditionalmediationavailable
    pub(crate) fn is_conditional_mediation_available(cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // 1. Return a promise resolved with with false.
        Ok(create_resolved_promise(cx.vm(), cx.realm(), Value::from_bool(false)))
    }
}

#[derive(Clone, Copy, Trace)]
pub(crate) enum AnyCredential {
    Credential(Ref<Credential>),
    FederatedCredential(Ref<FederatedCredential>),
    PasswordCredential(Ref<PasswordCredential>),
    PublicKeyCredential(Ref<PublicKeyCredential>),
}
