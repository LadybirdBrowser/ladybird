/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The registration and authentication ceremonies, https://w3c.github.io/webauthn/#sctn-createCredential and
//! https://w3c.github.io/webauthn/#sctn-getAssertion
//!
//! The browser process holds the authenticators, and is the WebAuthn client where it counts: It takes the origin of
//! the caller from what it knows about the document, checks the Relying Party ID against it, and builds the client
//! data. The steps here that the browser process runs again are the ones whose exceptions script can see.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{EmbedderClass, EmbedderValue, NativeFunction, PropertyKey, Trace, Value, Vm};
use liburl_rust::origin::{Host, Origin};
use liburl_rust::url::HostKind;

use super::authenticator_assertion_response::AuthenticatorAssertionResponse;
use super::authenticator_attestation_response::AuthenticatorAttestationResponse;
use super::authenticator_response::array_buffer_from_bytes;
use super::public_key_credential::{
    AnyAuthenticatorResponse, AuthenticationExtensionsClientOutputs, CredentialPropertiesOutput, PublicKeyCredential,
    PublicKeyCredentialCreationOptions, PublicKeyCredentialDescriptor, PublicKeyCredentialRequestOptions,
};
use crate::bindings::conversions::BufferSource;
use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Ref, allocate, with_temporary_execution_context, wrap};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, promise_realm, queue_a_global_task, reject_promise, reject_promise_with_exception,
    resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::credential_management::credential::CredentialMediationRequirement;
use crate::dom::abort_signal::{AbortAlgorithm, AbortSignal};
use crate::dom::document::{CppDocument, is_a_registrable_domain_suffix_of_or_is_equal_to};

/// What LibWeb/WebAuthentication/Ceremony.cpp takes for a registration ceremony.
#[repr(C)]
struct MakeCredentialRequestAbi<'a> {
    challenge: ak::Span<'a, u8>,
    rp_id: ak::Span<'a, u8>,
    rp_name: usize,
    user_id: ak::Span<'a, u8>,
    user_name: usize,
    user_display_name: usize,
    public_key_algorithms: ak::Span<'a, i32>,
    exclude_credential_ids: ak::Span<'a, ak::Span<'a, u8>>,
    authenticator_attachment: u8,
    resident_key: u8,
    user_verification: u8,
    same_origin_with_ancestors: bool,
    has_timeout: bool,
    timeout: u32,
}

#[repr(C)]
struct MakeCredentialResponseAbi<'a> {
    client_data_json: ak::Span<'a, u8>,
    credential_id: ak::Span<'a, u8>,
    attestation_object: ak::Span<'a, u8>,
    authenticator_data: ak::Span<'a, u8>,
    public_key: ak::Span<'a, u8>,
    transports: ak::Span<'a, ak::Span<'a, u8>>,
    public_key_algorithm: i32,
    has_public_key: bool,
    authenticator_attachment: u8,
    is_resident_key: bool,
}

/// What LibWeb/WebAuthentication/Ceremony.cpp takes for an authentication ceremony.
#[repr(C)]
struct GetAssertionRequestAbi<'a> {
    challenge: ak::Span<'a, u8>,
    rp_id: ak::Span<'a, u8>,
    allow_credential_ids: ak::Span<'a, ak::Span<'a, u8>>,
    user_verification: u8,
    same_origin_with_ancestors: bool,
    is_conditional: bool,
    has_timeout: bool,
    timeout: u32,
}

#[repr(C)]
struct GetAssertionResponseAbi<'a> {
    client_data_json: ak::Span<'a, u8>,
    credential_id: ak::Span<'a, u8>,
    authenticator_data: ak::Span<'a, u8>,
    signature: ak::Span<'a, u8>,
    user_handle: ak::Span<'a, u8>,
    has_user_handle: bool,
    authenticator_attachment: u8,
}

unsafe extern "C" {
    fn web_authentication_is_same_origin_with_its_ancestors(document: *mut c_void) -> bool;
    fn web_authentication_is_user_verifying_platform_authenticator_available(window: *mut c_void) -> bool;
    fn web_authentication_make_credential(
        window: *mut c_void,
        request: &MakeCredentialRequestAbi<'_>,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void, Option<&MakeCredentialResponseAbi<'_>>, u8),
    ) -> u64;
    fn web_authentication_cancel_ceremony(window: *mut c_void, request_id: u64);
    fn web_authentication_get_assertion(
        window: *mut c_void,
        request: &GetAssertionRequestAbi<'_>,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void, Option<&GetAssertionResponseAbi<'_>>, u8),
    ) -> u64;
}

/// https://w3c.github.io/webappsec-credential-management/#same-origin-with-its-ancestors
pub(crate) fn is_same_origin_with_its_ancestors(document: &CppDocument) -> bool {
    // SAFETY: The Document is live.
    unsafe { web_authentication_is_same_origin_with_its_ancestors(document.as_ptr()) }
}

/// Whether the browser process has a user-verifying platform authenticator for the window,
/// https://w3c.github.io/webauthn/#dom-publickeycredential-isuserverifyingplatformauthenticatoravailable
pub(crate) fn is_user_verifying_platform_authenticator_available(window: &CppWindow) -> bool {
    // SAFETY: The Window is live.
    unsafe { web_authentication_is_user_verifying_platform_authenticator_available(window.as_ptr()) }
}

/// The promise of a ceremony that the browser process runs, and what its result needs.
#[derive(Trace)]
struct PendingCeremony {
    promise: WebIdlPromise,
    // The signal that can abort the ceremony, and its algorithm that does so, which is there until the ceremony ends.
    signal: Option<Ref<AbortSignal>>,
    #[gc(untraced)]
    abort_algorithm_id: Cell<Option<u64>>,
    // Whether the Relying Party asked for the credProps extension, https://w3c.github.io/webauthn/#sctn-authenticator-credential-properties-extension
    #[gc(untraced)]
    wants_credential_properties: bool,
}

static PENDING_CEREMONY_CLASS: EmbedderClass<PendingCeremony> = EmbedderClass::new("PendingCeremony");

impl EmbedderValue for PendingCeremony {
    const CLASS: &'static EmbedderClass<Self> = &PENDING_CEREMONY_CLASS;
}

/// The exception that the error of the browser process stands for.
fn exception_from_authenticator_error(error: u8) -> Exception {
    let (name, message) = match error {
        0 => ("NotAllowedError", "The operation either timed out or was not allowed"),
        1 => (
            "InvalidStateError",
            "The authenticator already has a credential for this user",
        ),
        2 => ("SecurityError", "The Relying Party ID is not valid for this origin"),
        3 => (
            "NotSupportedError",
            "No authenticator supports the requested algorithms",
        ),
        4 => ("ConstraintError", "No authenticator meets the requirements"),
        _ => unreachable!("LibWeb passes an AuthenticatorError"),
    };
    Exception::Dom(name, Utf16String::from_utf8(message))
}

fn authenticator_attachment_string(attachment: u8) -> Utf16String {
    Utf16String::from_utf8(match attachment {
        0 => "platform",
        1 => "cross-platform",
        _ => unreachable!("LibWeb passes an AuthenticatorAttachment"),
    })
}

/// https://w3c.github.io/webauthn/#enumdef-userverificationrequirement
fn user_verification_requirement(value: &Utf16String) -> u8 {
    // NB: Client platforms MUST ignore unknown values, treating an unknown value as if the member does not exist, and
    //     the default of the member is "preferred".
    if *value == "required" {
        0
    } else if *value == "discouraged" {
        2
    } else {
        1
    }
}

/// Get a copy of the bytes held by the buffer source, https://webidl.spec.whatwg.org/#dfn-get-buffer-source-copy
fn copy_of(buffer_source: &BufferSource) -> Vec<u8> {
    // SAFETY: Copying the bytes neither runs script nor allocates a cell.
    unsafe { buffer_source.with_bytes(<[u8]>::to_vec) }
}

/// The ids of the descriptors whose type is one that this implementation supports.
fn credential_ids_of(descriptors: &[PublicKeyCredentialDescriptor]) -> Vec<Vec<u8>> {
    descriptors
        .iter()
        .filter(|descriptor| descriptor.r#type == "public-key")
        .map(|descriptor| copy_of(&descriptor.id))
        .collect()
}

/// The Relying Party ID of a ceremony, as both ceremonies determine it from the origin of the caller.
fn determine_rp_id(caller_origin: &Origin, requested_rp_id: Option<&Utf16String>) -> WebIdlResult<String> {
    // Let callerOrigin be origin. If callerOrigin is an opaque origin, throw a "NotAllowedError" DOMException.
    let Origin::Tuple { host, domain, .. } = caller_origin else {
        return Err(Exception::Dom(
            "NotAllowedError",
            Utf16String::from_utf8("An opaque origin cannot use public key credentials"),
        ));
    };

    // Let effectiveDomain be the callerOrigin's effective domain. If effective domain is not a valid domain, then
    // throw a "SecurityError" DOMException.
    // NOTE: An effective domain may resolve to a host, which can be represented in various manners, such as domain,
    //       ipv4 address, ipv6 address, opaque host, or empty host. Only the domain format of host is allowed here.
    let effective_domain: &Host = domain.as_ref().unwrap_or(host);
    if effective_domain.kind != HostKind::Domain || effective_domain.serialization.is_empty() {
        return Err(Exception::Dom(
            "SecurityError",
            Utf16String::from_utf8("The effective domain of the origin is not a valid domain"),
        ));
    }

    match requested_rp_id {
        // -> is present
        //    If rp.id is not a registrable domain suffix of and is not equal to effectiveDomain, and if the client
        //    -> does not support related origin requests
        //       throw a "SecurityError" DOMException.
        // FIXME: Support related origin requests, https://w3c.github.io/webauthn/#sctn-related-origins
        Some(rp_id) => {
            if !is_a_registrable_domain_suffix_of_or_is_equal_to(rp_id, effective_domain) {
                return Err(Exception::Dom(
                    "SecurityError",
                    Utf16String::from_utf8("The Relying Party ID is not a registrable domain suffix of the origin"),
                ));
            }
            Ok(String::from_utf16_lossy(&rp_id.to_utf16()))
        }
        // -> is not present
        //    Set rp.id to effectiveDomain.
        None => Ok(effective_domain.serialization.clone()),
    }
}

/// Rejects the promise with the abort reason of the signal as soon as the signal is aborted, and tells the browser
/// process to give up on the ceremony. A result that still comes after that settles nothing. The result is the ID of
/// the abort algorithm, to remove it with when the ceremony ends.
fn cancel_when_aborted(
    vm: &Vm,
    window: &CppWindow,
    request_id: u64,
    promise: WebIdlPromise,
    signal: Option<Ref<AbortSignal>>,
) -> Option<u64> {
    let signal = signal?;
    let algorithm = NativeFunction::create(
        vm,
        (promise, signal, window.clone(), request_id),
        |vm, (promise, signal, window, request_id): &(WebIdlPromise, Ref<AbortSignal>, CppWindow, u64)| {
            reject_promise(vm, *promise, signal.reason());
            // SAFETY: The Window is live.
            unsafe { web_authentication_cancel_ceremony(window.as_ptr(), *request_id) };
            Ok(Value::UNDEFINED)
        },
        0,
        &PropertyKey::from_utf8(""),
        Some(promise_realm(promise)),
        None,
        None,
    );
    signal.add_abort_algorithm(vm, AbortAlgorithm::Rust(algorithm))
}

/// Takes the pending ceremony that LibWeb hands back with the result of the browser process.
///
/// # Safety
///
/// The pointers are the VM and the cell that the ceremony gave to LibWeb, which kept the cell alive.
unsafe fn pending_ceremony<'a>(vm: *mut c_void, cell: *mut c_void) -> (&'a Vm, WebIdlPromise, bool) {
    // SAFETY: The caller passes the VM and a live PendingCeremony.
    let (vm, ceremony) = unsafe {
        (
            &*vm.cast::<Vm>(),
            PENDING_CEREMONY_CLASS
                .downcast(NonNull::new(cell).expect("LibWeb passes the cell"))
                .expect("LibWeb passes a PendingCeremony"),
        )
    };
    // NB: A signal that outlives the ceremony does not keep the promise alive, or take back a request that is over.
    if let (Some(signal), Some(id)) = (&ceremony.signal, ceremony.abort_algorithm_id.take()) {
        signal.remove_abort_algorithm(vm, id);
    }
    (vm, ceremony.promise, ceremony.wants_credential_properties)
}

/// Settles the promise of a ceremony that the browser process failed.
fn reject_with_authenticator_error(vm: &Vm, promise: WebIdlPromise, error: u8) {
    let realm = promise_realm(promise);
    // Queue a task on global's DOM manipulation task source to run the following substeps:
    queue_a_global_task(
        vm,
        realm.global_object(),
        TaskSource::DOMManipulation,
        (promise, error),
        |vm, (promise, error)| {
            // 1. Reject p with e.
            with_temporary_execution_context(promise_realm(promise), || {
                reject_promise_with_exception(vm, promise, exception_from_authenticator_error(error));
            });
        },
    );
}

/// Settles the promise of a ceremony with the credential that it made.
fn resolve_with_credential(vm: &Vm, promise: WebIdlPromise, credential: Ref<PublicKeyCredential>) {
    let realm = promise_realm(promise);
    // Queue a task on global's DOM manipulation task source to run the following substeps:
    queue_a_global_task(
        vm,
        realm.global_object(),
        TaskSource::DOMManipulation,
        (promise, credential),
        |vm, (promise, credential)| {
            // 1. Resolve p with the result of promise-calling r given global.
            let realm = promise_realm(promise);
            with_temporary_execution_context(realm, || {
                let wrapper = wrap(&CallContext::new(vm, realm), credential);
                resolve_promise(vm, promise, Value::from_object(wrapper));
            });
        },
    );
}

unsafe extern "C" fn make_credential_completed(
    vm: *mut c_void,
    cell: *mut c_void,
    response: Option<&MakeCredentialResponseAbi<'_>>,
    error: u8,
) {
    // SAFETY: LibWeb passes the VM and the cell of create(), which it kept alive.
    let (vm, promise, wants_credential_properties) = unsafe { pending_ceremony(vm, cell) };
    let Some(response) = response else {
        reject_with_authenticator_error(vm, promise, error);
        return;
    };
    let realm = promise_realm(promise);

    // If any authenticator indicates success,
    // 3. Let credentialCreationData be a struct whose items are:
    //    attestationObjectResult: whose value is the bytes returned from the successful authenticatorMakeCredential
    //        operation.
    //    clientDataJSONResult: whose value is the bytes of clientDataJSON.
    //    attestationConveyancePreferenceOption: whose value is the value of pkOptions.attestation.
    //    clientExtensionResults: whose value is an AuthenticationExtensionsClientOutputs object containing extension
    //        identifier → client extension output entries.
    // 4. Let constructCredentialAlg be an algorithm that takes a global object global, and whose steps are:
    // NB: The authenticator of the browser process gives no attestation ("none"), so there is no attestation
    //     statement to replace for an attestation conveyance preference.
    //    3. Let id be attestationObject.authData.attestedCredentialData.credentialId.
    //    4. Let pubKeyCred be a new PublicKeyCredential object associated with global whose fields are:
    //       [[identifier]]: id
    //       authenticatorAttachment: The AuthenticatorAttachment value matching the current authenticator attachment
    //           modality of authenticator.
    //       response: A new AuthenticatorAttestationResponse object associated with global whose fields are:
    //           clientDataJSON: A new ArrayBuffer, created using global's %ArrayBuffer%, containing the bytes of
    //               credentialCreationData.clientDataJSONResult.
    //           attestationObject: attestationObject
    //           [[transports]]: A sequence of zero or more unique DOMStrings, in lexicographical order, that the
    //               authenticator is believed to support.
    //       [[clientExtensionsResults]]: A new ArrayBuffer, created using global's %ArrayBuffer%, containing the
    //           bytes of credentialCreationData.clientExtensionResults.
    //    5. Return pubKeyCred.
    let mut transports: Vec<String> = response
        .transports
        .as_slice()
        .iter()
        .map(|transport| String::from_utf8_lossy(transport.as_slice()).into_owned())
        .collect();
    transports.sort();
    transports.dedup();
    let attestation_response = allocate(vm, |_| {
        AuthenticatorAttestationResponse::new(
            array_buffer_from_bytes(vm, realm, response.client_data_json.as_slice()),
            array_buffer_from_bytes(vm, realm, response.attestation_object.as_slice()),
            transports
                .iter()
                .map(|transport| Utf16String::from_utf8(transport))
                .collect(),
            response.authenticator_data.as_slice().to_vec(),
            response.has_public_key.then(|| response.public_key.as_slice().to_vec()),
            response.public_key_algorithm,
        )
    });
    let client_extension_results = AuthenticationExtensionsClientOutputs {
        appid: None,
        appid_exclude: None,
        // https://w3c.github.io/webauthn/#sctn-authenticator-credential-properties-extension
        // Set clientExtensionResults["credProps"]["rk"] to the value of the requireResidentKey parameter that was
        // used in the invocation of the authenticatorMakeCredential operation.
        cred_props: wants_credential_properties.then_some(CredentialPropertiesOutput {
            rk: Some(response.is_resident_key),
        }),
    };
    let raw_id = array_buffer_from_bytes(vm, realm, response.credential_id.as_slice());
    let credential = allocate(vm, |_| {
        PublicKeyCredential::new(
            raw_id,
            AnyAuthenticatorResponse::Attestation(attestation_response),
            Some(authenticator_attachment_string(response.authenticator_attachment)),
            client_extension_results,
        )
    });
    resolve_with_credential(vm, promise, credential);
}

unsafe extern "C" fn get_assertion_completed(
    vm: *mut c_void,
    cell: *mut c_void,
    response: Option<&GetAssertionResponseAbi<'_>>,
    error: u8,
) {
    // SAFETY: LibWeb passes the VM and the cell of get(), which it kept alive.
    let (vm, promise, _) = unsafe { pending_ceremony(vm, cell) };
    let Some(response) = response else {
        reject_with_authenticator_error(vm, promise, error);
        return;
    };
    let realm = promise_realm(promise);

    // If any authenticator indicates success,
    // 3. Let constructAssertionAlg be an algorithm that takes a global object global, and whose steps are:
    //    1. Let pubKeyCred be a new PublicKeyCredential object associated with global whose fields are:
    //       [[identifier]]: A new ArrayBuffer, created using global's %ArrayBuffer%, containing the bytes of
    //           assertionCreationData.credentialIdResult.
    //       authenticatorAttachment: The AuthenticatorAttachment value matching the current authenticator attachment
    //           modality of authenticator.
    //       response: A new AuthenticatorAssertionResponse object associated with global whose fields are:
    //           clientDataJSON, authenticatorData, signature, userHandle
    //       [[clientExtensionsResults]]: A new ArrayBuffer, created using global's %ArrayBuffer%, containing the
    //           bytes of assertionCreationData.clientExtensionResults.
    //    2. Return pubKeyCred.
    let assertion_response = allocate(vm, |_| {
        AuthenticatorAssertionResponse::new(
            array_buffer_from_bytes(vm, realm, response.client_data_json.as_slice()),
            array_buffer_from_bytes(vm, realm, response.authenticator_data.as_slice()),
            array_buffer_from_bytes(vm, realm, response.signature.as_slice()),
            response
                .has_user_handle
                .then(|| array_buffer_from_bytes(vm, realm, response.user_handle.as_slice())),
        )
    });
    let raw_id = array_buffer_from_bytes(vm, realm, response.credential_id.as_slice());
    let credential = allocate(vm, |_| {
        PublicKeyCredential::new(
            raw_id,
            AnyAuthenticatorResponse::Assertion(assertion_response),
            Some(authenticator_attachment_string(response.authenticator_attachment)),
            AuthenticationExtensionsClientOutputs {
                appid: None,
                appid_exclude: None,
                cred_props: None,
            },
        )
    });
    resolve_with_credential(vm, promise, credential);
}

/// What create() knows about its caller.
pub(crate) struct Caller<'a> {
    pub(crate) window: &'a CppWindow,
    pub(crate) origin: &'a Origin,
    pub(crate) same_origin_with_ancestors: bool,
}

/// PublicKeyCredential's [[Create]](origin, options, sameOriginWithAncestors) internal method,
/// https://w3c.github.io/webauthn/#sctn-createCredential
///
/// The promise settles with the result of the ceremony, unless the steps here throw first.
pub(crate) fn create(
    vm: &Vm,
    caller: &Caller<'_>,
    promise: WebIdlPromise,
    pk_options: &PublicKeyCredentialCreationOptions<'_>,
    mediation: CredentialMediationRequirement,
    signal: Option<Ref<AbortSignal>>,
) -> WebIdlResult<()> {
    // 1. Assert: options.publicKey is present.

    // 2. If sameOriginWithAncestors is false:
    if !caller.same_origin_with_ancestors {
        // 1. If options.mediation is present with the value conditional:
        //    1. Throw a "NotAllowedError" DOMException
        if matches!(mediation, CredentialMediationRequirement::Conditional) {
            return Err(Exception::Dom(
                "NotAllowedError",
                Utf16String::from_utf8("A cross-origin frame cannot create a credential with conditional mediation"),
            ));
        }

        // 2. If the relevant global object, as determined by the calling create() implementation, does not have
        //    transient activation:
        //    1. Throw a "NotAllowedError" DOMException.
        if !caller.window.has_transient_activation() {
            return Err(Exception::Dom(
                "NotAllowedError",
                Utf16String::from_utf8("A cross-origin frame needs a user activation to create a credential"),
            ));
        }

        // 3. Consume user activation of the relevant global object.
        caller.window.consume_user_activation();

        // FIXME: 4. If the origin that is creating a credential is different from the top-level origin of the relevant
        //           global object (i.e., is a different origin than the user can see in the address bar), the client
        //           SHOULD make this fact clear to the user.
    }

    // NB: A later step throws a "NotAllowedError" DOMException for conditional mediation when the user agent has not
    //     recently mediated an authentication for the origin. This client keeps no record of one, and says with
    //     getClientCapabilities() that it has no conditional mediation for registration ceremonies, so that is always
    //     the case. The check is here, before anything goes to the browser process.
    // FIXME: Support conditional mediation for registration ceremonies.
    if matches!(mediation, CredentialMediationRequirement::Conditional) {
        return Err(Exception::Dom(
            "NotAllowedError",
            Utf16String::from_utf8("Conditional mediation is not supported for the creation of a credential"),
        ));
    }

    // 3. Let pkOptions be the value of options.publicKey.

    // 4. If pkOptions.timeout is present, check if its value lies within a reasonable range as defined by the client
    //    and if not, correct it to the closest value lying within that range. Set a timer lifetimeTimer to this
    //    adjusted value. If pkOptions.timeout is not present, then set lifetimeTimer to a client-specific default.
    // NB: The browser process corrects the value and runs the timer, so that a page cannot keep a ceremony open.

    // 5. If the length of pkOptions.user.id is not between 1 and 64 bytes (inclusive) then throw a TypeError.
    // NB: All BufferSource objects used in this algorithm MUST be snapshotted when the algorithm begins.
    let user_id = copy_of(&pk_options.user.id);
    if user_id.is_empty() || user_id.len() > 64 {
        return Err(Exception::TypeError(Utf16String::from_utf8(
            "The length of user.id must be between 1 and 64 bytes",
        )));
    }
    let challenge = copy_of(&pk_options.challenge);
    let exclude_credential_ids = credential_ids_of(&pk_options.exclude_credentials.values());

    // 6. Let callerOrigin be origin. If callerOrigin is an opaque origin, throw a "NotAllowedError" DOMException.
    // 7. Let effectiveDomain be the callerOrigin's effective domain. If effective domain is not a valid domain, then
    //    throw a "SecurityError" DOMException.
    // 8. If pkOptions.rp.id [...]
    let rp_id = determine_rp_id(caller.origin, pk_options.rp.id.as_ref())?;

    // 9. Let credTypesAndPubKeyAlgs be a new list whose items are pairs of PublicKeyCredentialType and a
    //    COSEAlgorithmIdentifier.
    let mut public_key_algorithms: Vec<i32> = Vec::new();

    // 10. If pkOptions.pubKeyCredParams's size
    // -> is zero
    //    Append the following pairs of PublicKeyCredentialType and COSEAlgorithmIdentifier values to
    //    credTypesAndPubKeyAlgs:
    //    - public-key and -7 ("ES256").
    //    - public-key and -257 ("RS256").
    if pk_options.pub_key_cred_params.is_empty() {
        public_key_algorithms.extend([-7, -257]);
    }
    // -> is non-zero
    //    For each current of pkOptions.pubKeyCredParams:
    else {
        for current in &pk_options.pub_key_cred_params {
            // 1. If current.type does not contain a PublicKeyCredentialType supported by this implementation, then
            //    continue.
            if current.r#type != "public-key" {
                continue;
            }

            // 2. Let alg be current.alg.
            // 3. Append the pair of current.type and alg to credTypesAndPubKeyAlgs.
            public_key_algorithms.push(current.alg);
        }

        // If credTypesAndPubKeyAlgs is empty, throw a "NotSupportedError" DOMException.
        if public_key_algorithms.is_empty() {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_utf8("None of pubKeyCredParams has a supported credential type"),
            ));
        }
    }

    // 11. Let clientExtensions be a new map and let authenticatorExtensions be a new map.
    // 12. If pkOptions.extensions is present, then for each extensionId → clientExtensionInput of
    //     pkOptions.extensions:
    //     1. If extensionId is not supported by this client platform or is not a registration extension, then
    //        continue.
    //     2. Set clientExtensions[extensionId] to clientExtensionInput.
    // NB: The one supported registration extension is credProps, a client extension with no authenticator input.
    let wants_credential_properties = pk_options
        .extensions
        .as_ref()
        .is_some_and(|extensions| extensions.cred_props == Some(true));
    // INTEROP: The appid extension is an authentication extension. A registration that names it is an error of the
    //          Relying Party, which the browsers report and do not ignore.
    if pk_options
        .extensions
        .as_ref()
        .is_some_and(|extensions| extensions.appid.is_some())
    {
        return Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("The appid extension is only valid when getting a credential"),
        ));
    }

    // 13. Let collectedClientData be a new CollectedClientData instance [...]
    // 14. Let clientDataJSON be the JSON-compatible serialization of client data constructed from
    //     collectedClientData.
    // 15. Let clientDataHash be the hash of the serialized client data represented by clientDataJSON.
    // NB: The browser process builds these, with the origin and the top-level origin that it knows the document by.

    // 16. If options.signal is present and aborted, throw the options.signal's abort reason.
    if let Some(signal) = &signal
        && signal.aborted()
    {
        reject_promise(vm, promise, signal.reason());
        return Ok(());
    }

    // 20. For each authenticator that becomes available on this client device during the lifetime of lifetimeTimer
    //     [...]
    // NB: The browser process runs the rest of the algorithm with its authenticators. What it needs from
    //     pkOptions.authenticatorSelection are the three values below.
    let selection = pk_options.authenticator_selection.as_ref();
    // NB: Zero is no attachment. An unknown value is treated as if the member does not exist.
    let authenticator_attachment = match selection.and_then(|selection| selection.authenticator_attachment.as_ref()) {
        Some(attachment) if *attachment == "platform" => 1,
        Some(attachment) if *attachment == "cross-platform" => 2,
        _ => 0,
    };
    // https://w3c.github.io/webauthn/#dom-authenticatorselectioncriteria-residentkey
    // If no value is given then the effective value is required if requireResidentKey is true or discouraged if it
    // is false or absent.
    let resident_key = match selection {
        Some(selection) => match &selection.resident_key {
            Some(resident_key) if *resident_key == "discouraged" => 0,
            Some(resident_key) if *resident_key == "preferred" => 1,
            Some(resident_key) if *resident_key == "required" => 2,
            _ if selection.require_resident_key => 2,
            _ => 0,
        },
        None => 0,
    };
    let user_verification = selection.map_or(1, |selection| {
        user_verification_requirement(&selection.user_verification)
    });

    let ceremony = PENDING_CEREMONY_CLASS.allocate(vm, |_| PendingCeremony {
        promise,
        signal,
        abort_algorithm_id: Cell::new(None),
        wants_credential_properties,
    });
    let exclude_credential_ids: Vec<ak::Span<'_, u8>> =
        exclude_credential_ids.iter().map(|id| ak::Span::new(id)).collect();
    let request = MakeCredentialRequestAbi {
        challenge: ak::Span::new(&challenge),
        rp_id: ak::Span::new(rp_id.as_bytes()),
        rp_name: pk_options.rp.parent.name.raw_identity(),
        user_id: ak::Span::new(&user_id),
        user_name: pk_options.user.parent.name.raw_identity(),
        user_display_name: pk_options.user.display_name.raw_identity(),
        public_key_algorithms: ak::Span::new(&public_key_algorithms),
        exclude_credential_ids: ak::Span::new(&exclude_credential_ids),
        authenticator_attachment,
        resident_key,
        user_verification,
        same_origin_with_ancestors: caller.same_origin_with_ancestors,
        has_timeout: pk_options.timeout.is_some(),
        timeout: pk_options.timeout.unwrap_or(0),
    };
    // SAFETY: The window and the cell are live, and the request is lent for the call.
    let request_id = unsafe {
        web_authentication_make_credential(
            caller.window.as_ptr(),
            &request,
            ceremony.as_ptr().cast(),
            make_credential_completed,
        )
    };
    ceremony
        .abort_algorithm_id
        .set(cancel_when_aborted(vm, caller.window, request_id, promise, signal));
    Ok(())
}

/// PublicKeyCredential's [[DiscoverFromExternalSource]](origin, options, sameOriginWithAncestors) internal method,
/// https://w3c.github.io/webauthn/#sctn-discover-from-external-source
///
/// The promise settles with the result of the ceremony, unless the steps here throw first.
pub(crate) fn discover_from_external_source(
    vm: &Vm,
    caller: &Caller<'_>,
    promise: WebIdlPromise,
    pk_options: &PublicKeyCredentialRequestOptions<'_>,
    mediation: CredentialMediationRequirement,
    signal: Option<Ref<AbortSignal>>,
) -> WebIdlResult<()> {
    // 1. Assert: options.publicKey is present.
    // 2. Let pkOptions be the value of options.publicKey.

    // 3. If options.mediation is present with the value conditional:
    //    1. Let credentialIdFilter be the value of pkOptions.allowCredentials.
    //    2. Set pkOptions.allowCredentials to empty.
    //       NOTE: This prevents non-discoverable credentials from being used during conditional requests.
    //    3. Set a timer lifetimeTimer to a value of infinity.
    // NB: The browser process gets the list for a conditional request too, and uses it there as credentialIdFilter:
    //     It offers the user only discoverable credentials, and of those only the ones that the list names.
    let is_conditional = matches!(mediation, CredentialMediationRequirement::Conditional);
    // 4. Else:
    //    1. Let credentialIdFilter be an empty list.
    //    2. If pkOptions.timeout is present, check if its value lies within a reasonable range as defined by the
    //       client and if not, correct it to the closest value lying within that range. Set a timer lifetimeTimer to
    //       this adjusted value. If pkOptions.timeout is not present, then set lifetimeTimer to a client-specific
    //       default.
    // NB: The browser process corrects the value and runs the timer, so that a page cannot keep a ceremony open.

    // NB: All BufferSource objects used in this algorithm MUST be snapshotted when the algorithm begins.
    let challenge = copy_of(&pk_options.challenge);
    let allow_credentials = pk_options.allow_credentials.values();
    let allow_credential_ids = credential_ids_of(&allow_credentials);
    // NB: A list that names only credentials of types that this client does not know is not an empty list: No
    //     credential can satisfy it, and it must not become a request for any discoverable credential.
    if !allow_credentials.is_empty() && allow_credential_ids.is_empty() {
        return Err(Exception::Dom(
            "NotAllowedError",
            Utf16String::from_utf8("The allowCredentials list names no credential of a supported type"),
        ));
    }

    // 5. Let callerOrigin be origin. If callerOrigin is an opaque origin, throw a "NotAllowedError" DOMException.
    // 6. Let effectiveDomain be the callerOrigin's effective domain. If effective domain is not a valid domain, then
    //    throw a "SecurityError" DOMException.
    // 7. If pkOptions.rpId [...]
    let rp_id = determine_rp_id(caller.origin, pk_options.rp_id.as_ref())?;

    // 8. Let clientExtensions be a new map and let authenticatorExtensions be a new map.
    // 9. If pkOptions.extensions is present, then for each extensionId → clientExtensionInput of
    //    pkOptions.extensions:
    //    1. If extensionId is not supported by this client platform or is not an authentication extension, then
    //       continue.
    // FIXME: Support the appid extension, https://w3c.github.io/webauthn/#sctn-appid-extension
    // INTEROP: The credProps extension is a registration extension. An authentication that names it is an error of
    //          the Relying Party, which the browsers report and do not ignore.
    if pk_options
        .extensions
        .as_ref()
        .is_some_and(|extensions| extensions.cred_props == Some(true))
    {
        return Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("The credProps extension is only valid when creating a credential"),
        ));
    }

    // 10. Let collectedClientData be a new CollectedClientData instance [...]
    // 11. Let clientDataJSON be the JSON-compatible serialization of client data constructed from
    //     collectedClientData.
    // 12. Let clientDataHash be the hash of the serialized client data represented by clientDataJSON.
    // NB: The browser process builds these, with the origin and the top-level origin that it knows the document by.

    // 13. If options.signal is present and aborted, throw the options.signal's abort reason.
    if let Some(signal) = &signal
        && signal.aborted()
    {
        reject_promise(vm, promise, signal.reason());
        return Ok(());
    }

    // NB: The browser process runs the rest of the algorithm with its authenticators.
    let ceremony = PENDING_CEREMONY_CLASS.allocate(vm, |_| PendingCeremony {
        promise,
        signal,
        abort_algorithm_id: Cell::new(None),
        wants_credential_properties: false,
    });
    let allow_credential_ids: Vec<ak::Span<'_, u8>> = allow_credential_ids.iter().map(|id| ak::Span::new(id)).collect();
    let request = GetAssertionRequestAbi {
        challenge: ak::Span::new(&challenge),
        rp_id: ak::Span::new(rp_id.as_bytes()),
        allow_credential_ids: ak::Span::new(&allow_credential_ids),
        user_verification: user_verification_requirement(&pk_options.user_verification),
        same_origin_with_ancestors: caller.same_origin_with_ancestors,
        is_conditional,
        has_timeout: pk_options.timeout.is_some(),
        timeout: pk_options.timeout.unwrap_or(0),
    };
    // SAFETY: The window and the cell are live, and the request is lent for the call.
    let request_id = unsafe {
        web_authentication_get_assertion(
            caller.window.as_ptr(),
            &request,
            ceremony.as_ptr().cast(),
            get_assertion_completed,
        )
    };
    ceremony
        .abort_algorithm_id
        .set(cancel_when_aborted(vm, caller.window, request_id, promise, signal));
    Ok(())
}
