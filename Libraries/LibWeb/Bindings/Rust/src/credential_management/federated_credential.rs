/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FederatedCredential, https://w3c.github.io/webappsec-credential-management/#federatedcredential

use ak::Utf16String;

use super::credential::CredentialData;
use crate::bindings::conversions::UsvString;

/// https://w3c.github.io/webappsec-credential-management/#dictdef-federatedcredentialrequestoptions
#[expect(dead_code, reason = "get() does not read the options yet")]
pub(crate) struct FederatedCredentialRequestOptions {
    pub(crate) protocols: Option<Vec<Utf16String>>,
    pub(crate) providers: Option<Vec<UsvString>>,
}

/// https://w3c.github.io/webappsec-credential-management/#dictdef-federatedcredentialinit
#[expect(dead_code, reason = "create() does not read the options yet")]
pub(crate) struct FederatedCredentialInit {
    pub(crate) parent: CredentialData,
    pub(crate) icon_url: Option<UsvString>,
    pub(crate) name: Option<UsvString>,
    pub(crate) protocol: Option<Utf16String>,
    pub(crate) provider: UsvString,
}
