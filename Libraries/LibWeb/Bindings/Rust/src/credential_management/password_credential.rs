/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PasswordCredential, https://w3c.github.io/webappsec-credential-management/#passwordcredential

use super::credential::CredentialData;
use crate::bindings::conversions::{CppInterface, UsvString};

/// https://w3c.github.io/webappsec-credential-management/#dictdef-passwordcredentialdata
#[expect(dead_code, reason = "create() does not read the options yet")]
pub(crate) struct PasswordCredentialData {
    pub(crate) parent: CredentialData,
    pub(crate) icon_url: Option<UsvString>,
    pub(crate) name: Option<UsvString>,
    pub(crate) password: UsvString,
}

/// https://w3c.github.io/webappsec-credential-management/#typedefdef-passwordcredentialinit
#[expect(dead_code, reason = "create() does not read the options yet")]
pub(crate) enum PasswordCredentialInit {
    PasswordCredentialData(PasswordCredentialData),
    HTMLFormElement(CppInterface),
}
