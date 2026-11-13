/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CredentialUserData, https://w3c.github.io/webappsec-credential-management/#credentialuserdata

use libjs_rust::api::Trace;

use crate::bindings::conversions::UsvString;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CredentialUserData {
    name: UsvString,
    icon_url: UsvString,
}

impl CredentialUserData {
    pub(crate) fn new(name: UsvString, icon_url: UsvString) -> Self {
        Self { name, icon_url }
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialuserdata-name
    pub(crate) fn name(&self) -> UsvString {
        self.name.clone()
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialuserdata-iconurl
    pub(crate) fn icon_url(&self) -> UsvString {
        self.icon_url.clone()
    }
}
