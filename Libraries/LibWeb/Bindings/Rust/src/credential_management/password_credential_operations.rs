/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::Vm;
use liburl_rust::origin::Origin;

use super::credential::CredentialData;
use super::password_credential::{PasswordCredential, PasswordCredentialData};
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::html_form_element::CppHTMLFormElement;
use crate::xhr::form_data::{FormData, FormDataEntryValue};

/// https://w3c.github.io/webappsec-credential-management/#abstract-opdef-create-a-passwordcredential-from-an-htmlformelement
pub(crate) fn create_password_credential_from_form(
    cx: &mut CallContext,
    form: CppHTMLFormElement,
    origin: Origin,
) -> WebIdlResult<Ref<PasswordCredential>> {
    // 1. Let data be a new PasswordCredentialData dictionary.
    let mut data = PasswordCredentialData {
        parent: CredentialData {
            id: UsvString::from_ascii(b""),
        },
        icon_url: None,
        name: None,
        password: UsvString::from_ascii(b""),
    };

    // 2. Set data’s origin member’s value to origin’s value.

    // 3. Let formData be the result of executing the FormData constructor on form.
    let form_data = FormData::construct(cx, Some(form.interface()), None)?;

    // 4. Let elements be a list of all the submittable elements whose form owner is form, in tree order.
    let elements = form.submittable_elements(cx.vm());

    // 5. Let newPasswordObserved be false.
    let mut new_password_observed = false;

    // 6. For each field in elements, run the following steps:
    for index in 0..elements.len() {
        let field = elements.get(index).expect("the index is in range");

        // 1. If field does not have an autocomplete attribute, then skip to the next field.
        let Some(autocomplete_attribute) = field.attribute("autocomplete").filter(|value| !value.is_empty()) else {
            continue;
        };

        // 2. Let name be the value of field’s name attribute.
        // 3. If formData’s has() method returns false when executed on name, then skip to the next field.
        let Some(name) = field.attribute("name") else {
            continue;
        };
        let name = UsvString::from_utf16_string(&name);
        if !form_data.has(cx, name.clone())? {
            continue;
        }

        let string_value_of = |cx: &mut CallContext| -> WebIdlResult<Option<UsvString>> {
            Ok(match form_data.get(cx, name.clone())? {
                Some(FormDataEntryValue::USVString(value)) => Some(value),
                _ => None,
            })
        };

        // 4. If field’s autocomplete attribute’s value contains one or more autofill detail tokens (tokens), then:
        // 1. For each token in tokens:
        let autocomplete_attribute = String::from_utf16_lossy(&autocomplete_attribute.to_utf16());
        for token in autocomplete_attribute.split(' ').filter(|token| !token.is_empty()) {
            // 1. If token is an ASCII case-insensitive match for one of the following strings, run the associated steps:
            //    - "new-password"
            //       Set data’s password member’s value to the result of executing formData’s get() method on name,
            //       and newPasswordObserved to true.
            if token.eq_ignore_ascii_case("new-password")
                && let Some(password) = string_value_of(cx)?
            {
                data.password = password;
                new_password_observed = true;
            }
            //    - "current-password"
            //       If newPasswordObserved is false, set data’s password member’s value to the result of executing
            //       formData’s get() method on name.
            //       Note: By checking that newPasswordObserved is false, new-password fields take precedence over
            //             current-password fields.
            if !new_password_observed
                && token.eq_ignore_ascii_case("current-password")
                && let Some(password) = string_value_of(cx)?
            {
                data.password = password;
            }
            //    - "photo"
            //      Set data’s iconURL member’s value to the result of executing formData’s get() method on name.
            if token.eq_ignore_ascii_case("photo")
                && let Some(photo) = string_value_of(cx)?
            {
                data.icon_url = Some(photo);
            }
            //    - "name"
            //    - "nickname"
            //      Set data’s name member’s value to the result of executing formData’s get() method on name.
            if token.eq_ignore_ascii_case("name")
                && let Some(name) = string_value_of(cx)?
            {
                data.name = Some(name);
            }
            if token.eq_ignore_ascii_case("nickname")
                && let Some(nickname) = string_value_of(cx)?
            {
                data.name = Some(nickname);
            }
            //    - "username"
            //      Set data’s id member’s value to the result of executing formData’s get() method on name.
            if token.eq_ignore_ascii_case("username")
                && let Some(username) = string_value_of(cx)?
            {
                data.parent.id = username;
            }
        }
    }

    // 7. Let c be the result of executing Create a PasswordCredential from PasswordCredentialData on data.
    //    If that threw an exception, rethrow that exception.
    // 8. Assert: c is a PasswordCredential.
    // 9. Return c.
    create_password_credential_from_data(cx.vm(), data, origin)
}

/// https://w3c.github.io/webappsec-credential-management/#abstract-opdef-create-a-passwordcredential-from-passwordcredentialdata
pub(crate) fn create_password_credential_from_data(
    vm: &Vm,
    data: PasswordCredentialData,
    origin: Origin,
) -> WebIdlResult<Ref<PasswordCredential>> {
    // 1. Let c be a new PasswordCredential object.
    // 2. If any of the following are the empty string, throw a TypeError exception:
    //    - data’s id member’s value
    //    - data’s origin member’s value
    //      NOTE: origin cannot be an empty string at this time since it is retrieved from the current settings object
    //            in the constructor.
    //    - data’s password member’s value
    if data.parent.id.as_utf16_string().is_empty() {
        return Err(Exception::TypeError(Utf16String::from_ascii(
            b"'id' must not be empty.",
        )));
    }
    if data.password.as_utf16_string().is_empty() {
        return Err(Exception::TypeError(Utf16String::from_ascii(
            b"'password' must not be empty.",
        )));
    }

    // 3. Set c’s properties as follows:
    //    - password
    //      - data’s password member’s value
    //    - id
    //      - data’s id member’s value
    //    - iconUrl
    //      - data’s iconURL member’s value
    //    - name
    //      - data’s name member’s value
    //    - [[origin]]
    //      - data’s origin member’s value.
    //        NOTE: origin is retrieved from the current settings object in the constructor.
    // 4. Return c.
    Ok(allocate(vm, |_| PasswordCredential::new(data, origin)))
}
