/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Credential, https://w3c.github.io/webappsec-credential-management/#credential

use ak::Utf16String;
use libjs_rust::api::{Trace, Value};

use super::federated_credential::FederatedCredential;
use super::password_credential::PasswordCredential;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::promise::{WebIdlPromise, create_resolved_promise};
use crate::bindings::{CallContext, WebIdlResult};

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
#[gc(crate = libjs_rust)]
pub(crate) struct Credential {
    id: UsvString,
    r#type: Utf16String,
}

impl Wrappable for Credential {}

impl Credential {
    pub(crate) fn new(id: UsvString, r#type: &str) -> Self {
        Self {
            id,
            r#type: Utf16String::from_utf8(r#type),
        }
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credential-id
    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.id.clone())
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
#[gc(crate = libjs_rust)]
pub(crate) enum AnyCredential {
    Credential(Ref<Credential>),
    FederatedCredential(Ref<FederatedCredential>),
    PasswordCredential(Ref<PasswordCredential>),
}
