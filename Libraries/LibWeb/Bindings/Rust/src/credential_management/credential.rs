/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Credential, https://w3c.github.io/webappsec-credential-management/#credential

use crate::bindings::conversions::UsvString;

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialdata
#[expect(dead_code, reason = "create() does not read the options yet")]
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
