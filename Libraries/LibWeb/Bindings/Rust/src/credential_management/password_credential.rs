/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PasswordCredential, https://w3c.github.io/webappsec-credential-management/#passwordcredential

use libjs_rust::api::Trace;
use liburl_rust::origin::Origin;

use super::credential::{Credential, CredentialData};
use super::credential_user_data::CredentialUserData;
use super::password_credential_operations::{
    create_password_credential_from_data, create_password_credential_from_form,
};
use crate::bindings::conversions::{CppInterface, UsvString};
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::interface::{Inherits, Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::html_form_element::CppHTMLFormElement;

/// https://w3c.github.io/webappsec-credential-management/#dictdef-passwordcredentialdata
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

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PasswordCredential {
    credential: Credential,
    user_data: CredentialUserData,
    // TODO: Use Core::SecretString when it comes back
    password: UsvString,
    // https://w3c.github.io/webappsec-credential-management/#dom-credential-origin-slot
    #[gc(untraced)]
    #[cfg_attr(web_idl_bindings, expect(dead_code, reason = "no algorithm reads [[origin]] yet"))]
    origin: Origin,
}

impl Wrappable for PasswordCredential {}

impl Inherits<Credential> for PasswordCredential {
    fn parent(&self) -> &Credential {
        &self.credential
    }
}

fn current_settings_object_origin(cx: &CallContext) -> Origin {
    relevant_settings_object(cx.realm().global_object()).origin()
}

impl PasswordCredential {
    pub(crate) fn new(data: PasswordCredentialData, origin: Origin) -> Self {
        Self {
            credential: Credential::new(data.parent.id, "password"),
            user_data: CredentialUserData::new(
                data.name.unwrap_or_else(|| UsvString::from_ascii(b"")),
                data.icon_url.unwrap_or_else(|| UsvString::from_ascii(b"")),
            ),
            password: data.password,
            origin,
        }
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-passwordcredential-passwordcredential
    pub(crate) fn construct_from_form(cx: &mut CallContext, form: CppInterface) -> WebIdlResult<Ref<Self>> {
        // 1. Let origin be the current settings object’s origin.
        let origin = current_settings_object_origin(cx);

        // 2. Let r be the result of executing Create a PasswordCredential from an HTMLFormElement given form and origin.
        // 3. If r is an exception, throw r. Otherwise, return r.
        // SAFETY: The bindings pass the wrapper of an HTMLFormElement.
        let form = unsafe { CppHTMLFormElement::from_interface(form) };
        create_password_credential_from_form(cx, form, origin)
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-passwordcredential-passwordcredential-data
    pub(crate) fn construct_from_data(cx: &mut CallContext, data: PasswordCredentialData) -> WebIdlResult<Ref<Self>> {
        let origin = current_settings_object_origin(cx);

        // 1. Let r be the result of executing Create a PasswordCredential from PasswordCredentialData on data.
        // 2. If r is an exception, throw r.
        create_password_credential_from_data(cx.vm(), data, origin)
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-passwordcredential-password
    pub(crate) fn password(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.password.clone())
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
