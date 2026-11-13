/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FederatedCredential, https://w3c.github.io/webappsec-credential-management/#federatedcredential

use ak::Utf16String;
use libjs_rust::api::Trace;
use liburl_rust::origin::Origin;
use liburl_rust::{BasicParseOptions, basic_parse};

use super::credential::{Credential, CredentialData};
use super::credential_user_data::CredentialUserData;
use super::federated_credential_operations::create_federated_credential;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Inherits, Ref, Wrappable};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::domurl::url_input;

/// https://w3c.github.io/webappsec-credential-management/#dictdef-federatedcredentialrequestoptions
#[expect(dead_code, reason = "get() does not read the options yet")]
pub(crate) struct FederatedCredentialRequestOptions {
    pub(crate) protocols: Option<Vec<Utf16String>>,
    pub(crate) providers: Option<Vec<UsvString>>,
}

/// https://w3c.github.io/webappsec-credential-management/#dictdef-federatedcredentialinit
pub(crate) struct FederatedCredentialInit {
    pub(crate) parent: CredentialData,
    pub(crate) icon_url: Option<UsvString>,
    pub(crate) name: Option<UsvString>,
    pub(crate) protocol: Option<Utf16String>,
    pub(crate) provider: UsvString,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FederatedCredential {
    credential: Credential,
    user_data: CredentialUserData,
    provider: UsvString,
    protocol: Option<Utf16String>,
    // https://w3c.github.io/webappsec-credential-management/#dom-credential-origin-slot
    #[gc(untraced)]
    #[cfg_attr(web_idl_bindings, expect(dead_code, reason = "no algorithm reads [[origin]] yet"))]
    origin: Origin,
}

impl Wrappable for FederatedCredential {}

impl Inherits<Credential> for FederatedCredential {
    fn parent(&self) -> &Credential {
        &self.credential
    }
}

impl FederatedCredential {
    pub(crate) fn new(init: FederatedCredentialInit, origin: Origin) -> Self {
        Self {
            credential: Credential::new(init.parent.id, "federated"),
            user_data: CredentialUserData::new(
                init.name.unwrap_or_else(|| UsvString::from_ascii(b"")),
                init.icon_url.unwrap_or_else(|| UsvString::from_ascii(b"")),
            ),
            provider: init.provider,
            protocol: init.protocol,
            origin,
        }
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-federatedcredential-federatedcredential
    pub(crate) fn construct(cx: &mut CallContext, data: FederatedCredentialInit) -> WebIdlResult<Ref<Self>> {
        // 1. Let r be the result of executing Create a FederatedCredential from FederatedCredentialInit on data. If that
        // threw an exception, rethrow that exception.
        // 2. Return r.
        if data.parent.id.as_utf16_string().is_empty() {
            return Err(Exception::TypeError(Utf16String::from_ascii(
                b"'id' must not be empty.",
            )));
        }
        if data.provider.as_utf16_string().is_empty() {
            return Err(Exception::TypeError(Utf16String::from_ascii(
                b"'provider' must not be empty.",
            )));
        }

        // AD-HOC: Aligning with how Chromium retrieves the origin by parsing the URL from data.provider.
        let Some(url) = basic_parse(url_input(data.provider.units()), BasicParseOptions::new()) else {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_ascii(b"'provider' is not a valid url."),
            ));
        };

        Ok(create_federated_credential(cx.vm(), data, url.origin()))
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-federatedcredential-provider
    pub(crate) fn provider(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.provider.clone())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-federatedcredential-protocol
    pub(crate) fn protocol(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.protocol.clone())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialuserdata-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.user_data.name())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialuserdata-iconurl
    pub(crate) fn icon_url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.user_data.icon_url())
    }
}
