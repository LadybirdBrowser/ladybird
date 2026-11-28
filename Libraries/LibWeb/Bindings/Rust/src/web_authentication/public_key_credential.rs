/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PublicKeyCredential, https://w3c.github.io/webauthn/#publickeycredential

use ak::Utf16String;
use libjs_rust::api::{ArrayBuffer, Gc, MarkedVec, Object, PropertyKey, ThrowCompletionOr, Trace, Value};

use super::authenticator_assertion_response::AuthenticatorAssertionResponse;
use super::authenticator_attestation_response::AuthenticatorAttestationResponse;
use super::authenticator_response::{array_buffer_from_bytes, bytes_of_array_buffer};
use super::ceremony::is_user_verifying_platform_authenticator_available;
use crate::bindings::conversions::{BufferSource, ToJs, record_to_js, sequence_to_js};
use crate::bindings::environment_settings_object::current_settings_object;
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Inherits, Ref, Wrappable, create_dictionary_object, set_dictionary_member, wrap};
use crate::bindings::promise::{WebIdlPromise, create_resolved_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::credential_management::credential::Credential;

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialparameters
#[derive(Clone)]
pub(crate) struct PublicKeyCredentialParameters {
    pub(crate) alg: i32,
    pub(crate) r#type: Utf16String,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialentity
#[derive(Clone)]
pub(crate) struct PublicKeyCredentialEntity {
    pub(crate) name: Utf16String,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialrpentity
#[derive(Clone)]
pub(crate) struct PublicKeyCredentialRpEntity {
    pub(crate) parent: PublicKeyCredentialEntity,
    pub(crate) id: Option<Utf16String>,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialuserentity
#[derive(Clone)]
pub(crate) struct PublicKeyCredentialUserEntity {
    pub(crate) parent: PublicKeyCredentialEntity,
    pub(crate) display_name: Utf16String,
    pub(crate) id: BufferSource,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialuserentityjson
pub(crate) struct PublicKeyCredentialUserEntityJSON {
    pub(crate) display_name: Utf16String,
    pub(crate) id: Utf16String,
    pub(crate) name: Utf16String,
}

/// https://w3c.github.io/webauthn/#dictdef-authenticatorselectioncriteria
#[derive(Clone)]
pub(crate) struct AuthenticatorSelectionCriteria {
    pub(crate) authenticator_attachment: Option<Utf16String>,
    pub(crate) require_resident_key: bool,
    pub(crate) resident_key: Option<Utf16String>,
    pub(crate) user_verification: Utf16String,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialdescriptor
#[derive(Clone, Trace)]
pub(crate) struct PublicKeyCredentialDescriptor {
    pub(crate) id: BufferSource,
    pub(crate) transports: Option<Vec<Utf16String>>,
    pub(crate) r#type: Utf16String,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialdescriptorjson
pub(crate) struct PublicKeyCredentialDescriptorJSON {
    pub(crate) id: Utf16String,
    pub(crate) transports: Option<Vec<Utf16String>>,
    pub(crate) r#type: Utf16String,
}

/// https://w3c.github.io/webauthn/#dictdef-authenticationextensionsclientinputs
// FIXME: Add the prf and largeBlob extensions.
#[expect(dead_code, reason = "the ceremonies do not read every member yet")]
#[derive(Clone)]
pub(crate) struct AuthenticationExtensionsClientInputs {
    pub(crate) appid: Option<Utf16String>,
    pub(crate) appid_exclude: Option<Utf16String>,
    pub(crate) cred_props: Option<bool>,
}

/// https://w3c.github.io/webauthn/#dictdef-authenticationextensionsclientinputsjson
pub(crate) struct AuthenticationExtensionsClientInputsJSON {
    pub(crate) appid: Option<Utf16String>,
    pub(crate) appid_exclude: Option<Utf16String>,
    pub(crate) cred_props: Option<bool>,
}

/// https://w3c.github.io/webauthn/#dictdef-credentialpropertiesoutput
#[derive(Clone)]
pub(crate) struct CredentialPropertiesOutput {
    pub(crate) rk: Option<bool>,
}

/// https://w3c.github.io/webauthn/#dictdef-authenticationextensionsclientoutputs
#[derive(Clone)]
pub(crate) struct AuthenticationExtensionsClientOutputs {
    pub(crate) appid: Option<bool>,
    pub(crate) appid_exclude: Option<bool>,
    pub(crate) cred_props: Option<CredentialPropertiesOutput>,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialcreationoptions
#[expect(dead_code, reason = "the ceremonies do not read every member yet")]
pub(crate) struct PublicKeyCredentialCreationOptions<'vm> {
    pub(crate) attestation: Utf16String,
    pub(crate) attestation_formats: Vec<Utf16String>,
    pub(crate) authenticator_selection: Option<AuthenticatorSelectionCriteria>,
    pub(crate) challenge: BufferSource,
    pub(crate) exclude_credentials: MarkedVec<'vm, PublicKeyCredentialDescriptor>,
    pub(crate) extensions: Option<AuthenticationExtensionsClientInputs>,
    pub(crate) hints: Vec<Utf16String>,
    pub(crate) pub_key_cred_params: Vec<PublicKeyCredentialParameters>,
    pub(crate) rp: PublicKeyCredentialRpEntity,
    pub(crate) timeout: Option<u32>,
    pub(crate) user: PublicKeyCredentialUserEntity,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialcreationoptionsjson
pub(crate) struct PublicKeyCredentialCreationOptionsJSON {
    pub(crate) attestation: Utf16String,
    pub(crate) attestation_formats: Vec<Utf16String>,
    pub(crate) authenticator_selection: Option<AuthenticatorSelectionCriteria>,
    pub(crate) challenge: Utf16String,
    pub(crate) exclude_credentials: Vec<PublicKeyCredentialDescriptorJSON>,
    pub(crate) extensions: Option<AuthenticationExtensionsClientInputsJSON>,
    pub(crate) hints: Vec<Utf16String>,
    pub(crate) pub_key_cred_params: Vec<PublicKeyCredentialParameters>,
    pub(crate) rp: PublicKeyCredentialRpEntity,
    pub(crate) timeout: Option<u32>,
    pub(crate) user: PublicKeyCredentialUserEntityJSON,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialrequestoptions
#[expect(dead_code, reason = "the ceremonies do not read every member yet")]
pub(crate) struct PublicKeyCredentialRequestOptions<'vm> {
    pub(crate) allow_credentials: MarkedVec<'vm, PublicKeyCredentialDescriptor>,
    pub(crate) challenge: BufferSource,
    pub(crate) extensions: Option<AuthenticationExtensionsClientInputs>,
    pub(crate) hints: Vec<Utf16String>,
    pub(crate) rp_id: Option<Utf16String>,
    pub(crate) timeout: Option<u32>,
    pub(crate) user_verification: Utf16String,
}

/// https://w3c.github.io/webauthn/#dictdef-publickeycredentialrequestoptionsjson
pub(crate) struct PublicKeyCredentialRequestOptionsJSON {
    pub(crate) allow_credentials: Vec<PublicKeyCredentialDescriptorJSON>,
    pub(crate) challenge: Utf16String,
    pub(crate) extensions: Option<AuthenticationExtensionsClientInputsJSON>,
    pub(crate) hints: Vec<Utf16String>,
    pub(crate) rp_id: Option<Utf16String>,
    pub(crate) timeout: Option<u32>,
    pub(crate) user_verification: Utf16String,
}

/// The response of a credential, an object of an interface that inherits from AuthenticatorResponse.
#[derive(Clone, Copy, Trace)]
pub(crate) enum AnyAuthenticatorResponse {
    Attestation(Ref<AuthenticatorAttestationResponse>),
    Assertion(Ref<AuthenticatorAssertionResponse>),
}

#[derive(Trace)]
pub(crate) struct PublicKeyCredential {
    credential: Credential,
    // https://w3c.github.io/webauthn/#dom-publickeycredential-identifier-slot
    raw_id: Gc<ArrayBuffer>,
    // https://w3c.github.io/webauthn/#dom-publickeycredential-response
    response: AnyAuthenticatorResponse,
    // https://w3c.github.io/webauthn/#dom-publickeycredential-authenticatorattachment
    authenticator_attachment: Option<Utf16String>,
    // https://w3c.github.io/webauthn/#dom-publickeycredential-clientextensionsresults-slot
    #[gc(untraced)]
    client_extension_results: AuthenticationExtensionsClientOutputs,
}

impl Wrappable for PublicKeyCredential {}

impl Inherits<Credential> for PublicKeyCredential {
    fn parent(&self) -> &Credential {
        &self.credential
    }
}

// https://w3c.github.io/webauthn/#base64url-encoding
fn base64url_encode(bytes: &[u8]) -> Utf16String {
    // The term Base64url Encoding refers to the base64 encoding using the URL- and filename-safe character set defined
    // in Section 5 of [RFC4648], with all trailing '=' characters omitted (as permitted by Section 3.2) and without the
    // inclusion of any line breaks, whitespace, or other additional characters.
    ak::encode_base64(bytes, ak::Base64Alphabet::Base64Url, true)
}

fn base64url_decode(string: &Utf16String, member: &str) -> WebIdlResult<Vec<u8>> {
    // NB: The decoder takes '=' padding and skips ASCII whitespace, which the encoding does not have.
    let is_other_character = |unit: u16| matches!(unit, 0x3d | 0x09 | 0x0a | 0x0c | 0x0d | 0x20);
    let has_other_characters = match string.as_units() {
        ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => {
            units.iter().any(|&unit| is_other_character(u16::from(unit)))
        }
        ak::Utf16StringUnits::Utf16(units) => units.iter().any(|&unit| is_other_character(unit)),
    };
    let result = ak::decode_base64(
        string.as_units(),
        ak::Base64Alphabet::Base64Url,
        ak::LastChunkHandling::Loose,
        None,
    );
    if has_other_characters || result.error.is_some() {
        return Err(Exception::Dom(
            "EncodingError",
            Utf16String::from_utf8(&format!("'{member}' is not a valid base64url encoding")),
        ));
    }
    Ok(result.bytes)
}

fn base64url_decode_to_array_buffer(cx: &CallContext, string: &Utf16String, member: &str) -> WebIdlResult<Value> {
    let bytes = base64url_decode(string, member)?;
    Ok(Value::from_object(
        array_buffer_from_bytes(cx.vm(), cx.realm(), &bytes).upcast::<Object>(),
    ))
}

fn strings_to_js(cx: &CallContext, strings: Vec<Utf16String>) -> ThrowCompletionOr<Value> {
    sequence_to_js(cx, strings, |string| string.to_js(cx))
}

/// The sequence of PublicKeyCredentialDescriptor that the sequence of PublicKeyCredentialDescriptorJSON describes.
fn descriptors_from_json(
    cx: &CallContext,
    descriptors: Vec<PublicKeyCredentialDescriptorJSON>,
    member: &str,
) -> WebIdlResult<Value> {
    // NB: Every identifier is decoded first, so that nothing has to keep the buffers alive in the meantime.
    let mut decoded = Vec::with_capacity(descriptors.len());
    for descriptor in descriptors {
        let id = base64url_decode(&descriptor.id, member)?;
        decoded.push((descriptor, id));
    }
    Ok(sequence_to_js(cx, decoded, |(descriptor, id)| {
        let object = create_dictionary_object(cx);
        set_member(cx, object, "type", descriptor.r#type.to_js(cx)?)?;
        set_member(
            cx,
            object,
            "id",
            Value::from_object(array_buffer_from_bytes(cx.vm(), cx.realm(), &id).upcast::<Object>()),
        )?;
        if let Some(transports) = descriptor.transports {
            set_member(cx, object, "transports", strings_to_js(cx, transports)?)?;
        }
        Ok(Value::from_object(object))
    })?)
}

/// The AuthenticationExtensionsClientInputs that the AuthenticationExtensionsClientInputsJSON describes.
fn extensions_from_json(
    cx: &CallContext,
    extensions: AuthenticationExtensionsClientInputsJSON,
) -> ThrowCompletionOr<Value> {
    let object = create_dictionary_object(cx);
    if let Some(appid) = extensions.appid {
        set_member(cx, object, "appid", appid.to_js(cx)?)?;
    }
    if let Some(appid_exclude) = extensions.appid_exclude {
        set_member(cx, object, "appidExclude", appid_exclude.to_js(cx)?)?;
    }
    if let Some(cred_props) = extensions.cred_props {
        set_member(cx, object, "credProps", cred_props.to_js(cx)?)?;
    }
    Ok(Value::from_object(object))
}

fn set_member(cx: &CallContext, object: Gc<Object>, name: &str, value: Value) -> ThrowCompletionOr<()> {
    set_dictionary_member(cx, object, &PropertyKey::from_utf8(name), value)
}

pub(crate) fn base64url_of_array_buffer(buffer: &ArrayBuffer) -> Utf16String {
    base64url_encode(&bytes_of_array_buffer(buffer))
}

fn base64url_value(cx: &CallContext, buffer: &ArrayBuffer) -> ThrowCompletionOr<Value> {
    base64url_of_array_buffer(buffer).to_js(cx)
}

impl PublicKeyCredential {
    pub(crate) fn new(
        raw_id: Gc<ArrayBuffer>,
        response: AnyAuthenticatorResponse,
        authenticator_attachment: Option<Utf16String>,
        client_extension_results: AuthenticationExtensionsClientOutputs,
    ) -> Self {
        Self {
            credential: Credential::with_identifier(raw_id, "public-key"),
            raw_id,
            response,
            authenticator_attachment,
            client_extension_results,
        }
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-rawid
    pub(crate) fn raw_id(&self, _: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        // This attribute returns the ArrayBuffer contained in the [[identifier]] internal slot.
        Ok(self.raw_id)
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-response
    pub(crate) fn response(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        Ok(Value::from_object(match self.response {
            AnyAuthenticatorResponse::Attestation(response) => wrap(cx, response),
            AnyAuthenticatorResponse::Assertion(response) => wrap(cx, response),
        }))
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-authenticatorattachment
    pub(crate) fn authenticator_attachment(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.authenticator_attachment.clone())
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-getclientextensionresults
    pub(crate) fn get_client_extension_results(
        &self,
        _: &mut CallContext,
    ) -> WebIdlResult<AuthenticationExtensionsClientOutputs> {
        // This operation returns the value of [[clientExtensionsResults]], which is a map containing extension
        // identifier → client extension output entries produced by the extension's client extension processing.
        Ok(self.client_extension_results.clone())
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-tojson
    pub(crate) fn to_json(&self, cx: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        // This operation returns RegistrationResponseJSON or AuthenticationResponseJSON, which are JSON type
        // representations mirroring PublicKeyCredential, suitable for submission to a Relying Party server as an
        // application/json payload. The client is in charge of serializing values to JSON types as usual, but MUST
        // take additional steps to first encode any ArrayBuffer values to DOMString values using base64url encoding.
        let cx = &*cx;
        let json = create_dictionary_object(cx);
        let response = create_dictionary_object(cx);
        match self.response {
            AnyAuthenticatorResponse::Attestation(attestation) => {
                set_member(
                    cx,
                    response,
                    "clientDataJSON",
                    base64url_value(cx, &attestation.parent().client_data_json_buffer())?,
                )?;
                set_member(
                    cx,
                    response,
                    "authenticatorData",
                    base64url_encode(attestation.authenticator_data_bytes()).to_js(cx)?,
                )?;
                // The AuthenticatorAttestationResponseJSON.transports member MUST be set to the output of
                // getTransports().
                set_member(
                    cx,
                    response,
                    "transports",
                    sequence_to_js(cx, attestation.transports().to_vec(), |transport| transport.to_js(cx))?,
                )?;
                // The AuthenticatorAttestationResponseJSON.publicKey member MUST be set to the output of
                // getPublicKey().
                if let Some(public_key) = attestation.public_key_bytes() {
                    set_member(cx, response, "publicKey", base64url_encode(public_key).to_js(cx)?)?;
                }
                // The AuthenticatorAttestationResponseJSON.publicKeyAlgorithm member MUST be set to the output of
                // getPublicKeyAlgorithm().
                set_member(
                    cx,
                    response,
                    "publicKeyAlgorithm",
                    attestation.public_key_algorithm().to_js(cx)?,
                )?;
                set_member(
                    cx,
                    response,
                    "attestationObject",
                    base64url_value(cx, &attestation.attestation_object_buffer())?,
                )?;
            }
            AnyAuthenticatorResponse::Assertion(assertion) => {
                set_member(
                    cx,
                    response,
                    "clientDataJSON",
                    base64url_value(cx, &assertion.parent().client_data_json_buffer())?,
                )?;
                set_member(
                    cx,
                    response,
                    "authenticatorData",
                    base64url_value(cx, &assertion.authenticator_data_buffer())?,
                )?;
                set_member(
                    cx,
                    response,
                    "signature",
                    base64url_value(cx, &assertion.signature_buffer())?,
                )?;
                if let Some(user_handle) = assertion.user_handle_buffer() {
                    set_member(cx, response, "userHandle", base64url_value(cx, &user_handle)?)?;
                }
            }
        }

        set_member(
            cx,
            json,
            "id",
            self.credential.id_string().into_utf16_string().to_js(cx)?,
        )?;
        set_member(cx, json, "rawId", base64url_value(cx, &self.raw_id)?)?;
        set_member(cx, json, "response", Value::from_object(response))?;
        if let Some(authenticator_attachment) = &self.authenticator_attachment {
            set_member(
                cx,
                json,
                "authenticatorAttachment",
                authenticator_attachment.clone().to_js(cx)?,
            )?;
        }
        // The RegistrationResponseJSON.clientExtensionResults or AuthenticationResponseJSON.clientExtensionResults
        // member MUST be set to the output of getClientExtensionResults(), with any ArrayBuffer values encoded to
        // DOMString values using base64url encoding.
        // NB: No extension that the client implements has an ArrayBuffer output.
        let client_extension_results = create_dictionary_object(cx);
        if let Some(appid) = self.client_extension_results.appid {
            set_member(cx, client_extension_results, "appid", appid.to_js(cx)?)?;
        }
        if let Some(appid_exclude) = self.client_extension_results.appid_exclude {
            set_member(cx, client_extension_results, "appidExclude", appid_exclude.to_js(cx)?)?;
        }
        if let Some(cred_props) = &self.client_extension_results.cred_props {
            let object = create_dictionary_object(cx);
            if let Some(rk) = cred_props.rk {
                set_member(cx, object, "rk", rk.to_js(cx)?)?;
            }
            set_member(cx, client_extension_results, "credProps", Value::from_object(object))?;
        }
        set_member(
            cx,
            json,
            "clientExtensionResults",
            Value::from_object(client_extension_results),
        )?;
        set_member(cx, json, "type", Utf16String::from_ascii(b"public-key").to_js(cx)?)?;
        Ok(json)
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-isconditionalmediationavailable
    pub(crate) fn is_conditional_mediation_available(cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // NB: Conditional mediation needs an authenticator whose credentials the client can list without the user,
        //     which the built-in authenticator is.
        let is_available = window_from_global_object(current_settings_object(cx.vm()).global_object())
            .is_some_and(|window| is_user_verifying_platform_authenticator_available(&window));
        Ok(create_resolved_promise(
            cx.vm(),
            cx.realm(),
            Value::from_bool(is_available),
        ))
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-isuserverifyingplatformauthenticatoravailable
    pub(crate) fn is_user_verifying_platform_authenticator_available(
        cx: &mut CallContext,
    ) -> WebIdlResult<WebIdlPromise> {
        // Upon invocation, the client employs a client platform-specific procedure to discover available
        // user-verifying platform authenticators. If any are discovered, the promise is resolved with the value of
        // true. Otherwise, the promise is resolved with the value of false.
        let is_available = window_from_global_object(current_settings_object(cx.vm()).global_object())
            .is_some_and(|window| is_user_verifying_platform_authenticator_available(&window));
        Ok(create_resolved_promise(
            cx.vm(),
            cx.realm(),
            Value::from_bool(is_available),
        ))
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-getclientcapabilities
    pub(crate) fn get_client_capabilities(cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // Keys in PublicKeyCredentialClientCapabilities MUST be sorted in ascending lexicographical order. The set of
        // keys SHOULD contain the set of enumeration values of ClientCapability, but the client MAY omit keys as it
        // deems necessary.
        // FIXME: Report the capabilities as the client gains them.
        // NB: An extension that the client implements has the key "extension:" followed by its identifier.
        // NB: The built-in authenticator verifies the user and keeps discoverable credentials, so it is a passkey
        //     platform authenticator when it is a user-verifying platform authenticator.
        let has_platform_authenticator = window_from_global_object(current_settings_object(cx.vm()).global_object())
            .is_some_and(|window| is_user_verifying_platform_authenticator_available(&window));
        let capabilities = vec![
            ("conditionalCreate", false),
            ("conditionalGet", has_platform_authenticator),
            ("extension:credProps", true),
            ("hybridTransport", false),
            ("passkeyPlatformAuthenticator", has_platform_authenticator),
            ("relatedOrigins", false),
            ("signalAllAcceptedCredentials", false),
            ("signalCurrentUserDetails", false),
            ("signalUnknownCredential", false),
            ("userVerifyingPlatformAuthenticator", has_platform_authenticator),
        ];
        let record = record_to_js(
            cx,
            capabilities
                .into_iter()
                .map(|(key, value)| (Utf16String::from_utf8(key), value))
                .collect(),
            |value| value.to_js(cx),
        )
        .map_err(Exception::from)?;
        Ok(create_resolved_promise(cx.vm(), cx.realm(), record))
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-parsecreationoptionsfromjson
    pub(crate) fn parse_creation_options_from_json(
        cx: &mut CallContext,
        options: PublicKeyCredentialCreationOptionsJSON,
    ) -> WebIdlResult<Gc<Object>> {
        // Upon invocation, the client MUST convert the options argument into a new, identically-structured
        // PublicKeyCredentialCreationOptions object, using base64url encoding to decode any DOMString attributes in
        // PublicKeyCredentialCreationOptionsJSON that correspond to buffer source type attributes in
        // PublicKeyCredentialCreationOptions. This conversion MUST also apply to any client extension inputs processed
        // by the client.
        //
        // If the client encounters any issues parsing any of the JSON type representations then it MUST throw an
        // "EncodingError" DOMException with a description of the incompatible value and terminate the operation.
        let cx = &*cx;
        let result = create_dictionary_object(cx);

        let rp = create_dictionary_object(cx);
        set_member(cx, rp, "name", options.rp.parent.name.to_js(cx)?)?;
        if let Some(id) = options.rp.id {
            set_member(cx, rp, "id", id.to_js(cx)?)?;
        }
        set_member(cx, result, "rp", Value::from_object(rp))?;

        let user = create_dictionary_object(cx);
        set_member(cx, user, "name", options.user.name.to_js(cx)?)?;
        set_member(
            cx,
            user,
            "id",
            base64url_decode_to_array_buffer(cx, &options.user.id, "user.id")?,
        )?;
        set_member(cx, user, "displayName", options.user.display_name.to_js(cx)?)?;
        set_member(cx, result, "user", Value::from_object(user))?;

        set_member(
            cx,
            result,
            "challenge",
            base64url_decode_to_array_buffer(cx, &options.challenge, "challenge")?,
        )?;
        let pub_key_cred_params = sequence_to_js(cx, options.pub_key_cred_params, |parameters| {
            let object = create_dictionary_object(cx);
            set_member(cx, object, "type", parameters.r#type.to_js(cx)?)?;
            set_member(cx, object, "alg", parameters.alg.to_js(cx)?)?;
            Ok(Value::from_object(object))
        })?;
        set_member(cx, result, "pubKeyCredParams", pub_key_cred_params)?;
        if let Some(timeout) = options.timeout {
            set_member(cx, result, "timeout", timeout.to_js(cx)?)?;
        }
        let exclude_credentials = descriptors_from_json(cx, options.exclude_credentials, "excludeCredentials.id")?;
        set_member(cx, result, "excludeCredentials", exclude_credentials)?;
        if let Some(selection) = options.authenticator_selection {
            let object = create_dictionary_object(cx);
            if let Some(authenticator_attachment) = selection.authenticator_attachment {
                set_member(
                    cx,
                    object,
                    "authenticatorAttachment",
                    authenticator_attachment.to_js(cx)?,
                )?;
            }
            if let Some(resident_key) = selection.resident_key {
                set_member(cx, object, "residentKey", resident_key.to_js(cx)?)?;
            }
            set_member(
                cx,
                object,
                "requireResidentKey",
                selection.require_resident_key.to_js(cx)?,
            )?;
            set_member(cx, object, "userVerification", selection.user_verification.to_js(cx)?)?;
            set_member(cx, result, "authenticatorSelection", Value::from_object(object))?;
        }
        set_member(cx, result, "hints", strings_to_js(cx, options.hints)?)?;
        set_member(cx, result, "attestation", options.attestation.to_js(cx)?)?;
        set_member(
            cx,
            result,
            "attestationFormats",
            strings_to_js(cx, options.attestation_formats)?,
        )?;
        if let Some(extensions) = options.extensions {
            set_member(cx, result, "extensions", extensions_from_json(cx, extensions)?)?;
        }
        Ok(result)
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-parserequestoptionsfromjson
    pub(crate) fn parse_request_options_from_json(
        cx: &mut CallContext,
        options: PublicKeyCredentialRequestOptionsJSON,
    ) -> WebIdlResult<Gc<Object>> {
        // Upon invocation, the client MUST convert the options argument into a new, identically-structured
        // PublicKeyCredentialRequestOptions object, using base64url encoding to decode any DOMString attributes in
        // PublicKeyCredentialRequestOptionsJSON that correspond to buffer source type attributes in
        // PublicKeyCredentialRequestOptions. This conversion MUST also apply to any client extension inputs processed
        // by the client.
        //
        // If the client encounters any issues parsing any of the JSON type representations then it MUST throw an
        // "EncodingError" DOMException with a description of the incompatible value and terminate the operation.
        let cx = &*cx;
        let result = create_dictionary_object(cx);
        set_member(
            cx,
            result,
            "challenge",
            base64url_decode_to_array_buffer(cx, &options.challenge, "challenge")?,
        )?;
        if let Some(timeout) = options.timeout {
            set_member(cx, result, "timeout", timeout.to_js(cx)?)?;
        }
        if let Some(rp_id) = options.rp_id {
            set_member(cx, result, "rpId", rp_id.to_js(cx)?)?;
        }
        let allow_credentials = descriptors_from_json(cx, options.allow_credentials, "allowCredentials.id")?;
        set_member(cx, result, "allowCredentials", allow_credentials)?;
        set_member(cx, result, "userVerification", options.user_verification.to_js(cx)?)?;
        set_member(cx, result, "hints", strings_to_js(cx, options.hints)?)?;
        if let Some(extensions) = options.extensions {
            set_member(cx, result, "extensions", extensions_from_json(cx, extensions)?)?;
        }
        Ok(result)
    }
}
