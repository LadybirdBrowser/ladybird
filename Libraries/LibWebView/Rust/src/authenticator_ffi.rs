/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI of the software authenticators, for LibWebView's C++ Authenticator and AuthenticatorManager, which keep
//! the credentials.

use crate::authenticator::{self, AuthenticatorConfiguration, AuthenticatorError, Credential, GetAssertionParameters};
use crate::ceremony::{self, Authenticator, AuthenticatorAttachment, GetAssertionRequest, MakeCredentialRequest};
use crate::ffi_bytes::FfiBytes;
use crate::rust_panic::abort_on_panic;
use crate::security_key::ctap::{ResidentKeyRequirement, UserVerificationRequirement};
use std::ffi::c_void;

/// <https://w3c.github.io/webauthn/#public-key-credential-source>
#[repr(C)]
pub struct FfiCredential {
    pub id: FfiBytes,
    /// A P-256 key as a DER PKCS #8 PrivateKeyInfo.
    pub private_key: FfiBytes,
    pub rp_id: FfiBytes,
    pub has_user_handle: bool,
    pub user_handle: FfiBytes,
    pub is_resident_credential: bool,
    pub has_sign_count: bool,
    pub sign_count: u32,
    pub backup_eligibility: bool,
    pub backup_state: bool,
}

/// An authenticator as a ceremony sees it.
#[repr(C)]
pub struct FfiAuthenticator {
    pub configuration: AuthenticatorConfiguration,
    pub attachment: AuthenticatorAttachment,
    pub credentials: *const FfiCredential,
    pub credential_count: usize,
}

#[repr(C)]
pub struct FfiCeremonyMakeCredentialRequest {
    pub rp_id: FfiBytes,
    pub user_id: FfiBytes,
    pub public_key_algorithms: *const i32,
    pub public_key_algorithm_count: usize,
    pub exclude_credential_ids: *const FfiBytes,
    pub exclude_credential_id_count: usize,
    pub has_authenticator_attachment: bool,
    pub authenticator_attachment: AuthenticatorAttachment,
    pub resident_key: ResidentKeyRequirement,
    pub user_verification: UserVerificationRequirement,
}

/// NB: The bytes and the list live for the call that gives this.
#[repr(C)]
pub struct FfiNewCredential {
    /// Where the authenticator is that made the credential, in the list of the caller.
    pub authenticator: usize,
    /// Whether the new credential is a client-side discoverable credential.
    pub is_resident_key: bool,
    pub id: FfiBytes,
    pub private_key: FfiBytes,
    pub public_key: FfiBytes,
    pub public_key_algorithm: i32,
    pub attestation_object: FfiBytes,
    pub authenticator_data: FfiBytes,
    pub backup_eligibility: bool,
    pub backup_state: bool,
    /// Where the credentials are that the new one takes the place of, in the list of that authenticator and in
    /// rising order.
    pub displaced_credentials: *const usize,
    pub displaced_credential_count: usize,
}

#[repr(C)]
pub struct FfiCeremonyGetAssertionRequest {
    pub rp_id: FfiBytes,
    pub allow_credential_ids: *const FfiBytes,
    pub allow_credential_id_count: usize,
    pub user_verification: UserVerificationRequirement,
    pub is_conditional: bool,
    pub client_data_hash: FfiBytes,
}

/// NB: The bytes live for the call that gives this.
#[repr(C)]
pub struct FfiAuthenticatorAssertion {
    /// Where the authenticator is that made the assertion, in the list of the caller.
    pub authenticator: usize,
    /// Where the credential is that the authenticator selected, in the list of that authenticator.
    pub selected_credential: usize,
    /// The signature counter of the credential after the assertion, when it has one.
    pub has_sign_count: bool,
    pub sign_count: u32,
    pub authenticator_data: FfiBytes,
    pub signature: FfiBytes,
}

#[repr(C)]
pub struct FfiAuthenticatorResult {
    /// Whether the operation gave what it makes to the callback.
    pub is_success: bool,
    pub error: AuthenticatorError,
}

impl FfiAuthenticatorResult {
    fn of<T>(result: Result<T, AuthenticatorError>, on_success: impl FnOnce(T)) -> Self {
        match result {
            Ok(value) => {
                on_success(value);
                Self {
                    is_success: true,
                    error: AuthenticatorError::NotAllowed,
                }
            }
            Err(error) => Self {
                is_success: false,
                error,
            },
        }
    }
}

/// # Safety
/// `credentials` points to `count` credentials, whose bytes live for `'a`.
unsafe fn credentials_from<'a>(credentials: *const FfiCredential, count: usize) -> Vec<Credential<'a>> {
    // SAFETY: The caller says so.
    unsafe { ak::slice_from_raw_parts(credentials, count) }
        .iter()
        .map(|credential| {
            // SAFETY: The caller says so.
            unsafe {
                Credential {
                    id: credential.id.as_slice(),
                    private_key: credential.private_key.as_slice(),
                    rp_id: credential.rp_id.as_slice(),
                    user_handle: credential.has_user_handle.then(|| credential.user_handle.as_slice()),
                    is_resident_credential: credential.is_resident_credential,
                    sign_count: credential.has_sign_count.then_some(credential.sign_count),
                    backup_eligibility: credential.backup_eligibility,
                    backup_state: credential.backup_state,
                }
            }
        })
        .collect()
}

/// # Safety
/// `items` points to `count` byte strings that live for `'a`.
unsafe fn byte_slices<'a>(items: *const FfiBytes, count: usize) -> Vec<&'a [u8]> {
    // SAFETY: The caller says so.
    unsafe { ak::slice_from_raw_parts(items, count) }
        .iter()
        // SAFETY: The caller says so.
        .map(|item| unsafe { item.as_slice() })
        .collect()
}

/// # Safety
/// `authenticators` points to `count` authenticators, each with as many credentials as it says, for `'a`.
unsafe fn credentials_of<'a>(authenticators: *const FfiAuthenticator, count: usize) -> Vec<Vec<Credential<'a>>> {
    // SAFETY: The caller says so.
    unsafe { ak::slice_from_raw_parts(authenticators, count) }
        .iter()
        // SAFETY: The caller says so.
        .map(|authenticator| unsafe { credentials_from(authenticator.credentials, authenticator.credential_count) })
        .collect()
}

/// # Safety
/// `authenticators` points to `count` authenticators, and `credentials` has the credentials of each.
unsafe fn authenticators_from<'a>(
    authenticators: *const FfiAuthenticator,
    count: usize,
    credentials: &'a [Vec<Credential<'a>>],
) -> Vec<Authenticator<'a>> {
    // SAFETY: The caller says so.
    unsafe { ak::slice_from_raw_parts(authenticators, count) }
        .iter()
        .zip(credentials)
        .map(|(authenticator, credentials)| Authenticator {
            configuration: authenticator.configuration,
            attachment: authenticator.attachment,
            credentials,
        })
        .collect()
}

/// Goes through the authenticators for one that makes the credential,
/// <https://w3c.github.io/webauthn/#sctn-createCredential>.
///
/// # Safety
/// Every pointer points to as many items as the count or length beside it says. `on_credential` takes `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_ceremony_make_credential(
    authenticators: *const FfiAuthenticator,
    authenticator_count: usize,
    request: &FfiCeremonyMakeCredentialRequest,
    context: *mut c_void,
    on_credential: unsafe extern "C" fn(context: *mut c_void, credential: &FfiNewCredential),
) -> FfiAuthenticatorResult {
    abort_on_panic(|| {
        // SAFETY: The caller says so.
        let (credentials, exclude_credential_ids, public_key_algorithms) = unsafe {
            (
                credentials_of(authenticators, authenticator_count),
                byte_slices(request.exclude_credential_ids, request.exclude_credential_id_count),
                ak::slice_from_raw_parts(request.public_key_algorithms, request.public_key_algorithm_count),
            )
        };
        // SAFETY: The caller says so.
        let authenticators = unsafe { authenticators_from(authenticators, authenticator_count, &credentials) };
        let request = MakeCredentialRequest {
            // SAFETY: The caller says so.
            rp_id: unsafe { request.rp_id.as_slice() },
            // SAFETY: The caller says so.
            user_id: unsafe { request.user_id.as_slice() },
            public_key_algorithms,
            exclude_credential_ids: &exclude_credential_ids,
            authenticator_attachment: request
                .has_authenticator_attachment
                .then_some(request.authenticator_attachment),
            resident_key: request.resident_key,
            user_verification: request.user_verification,
        };
        let result = ceremony::make_credential(&authenticators, &request);
        FfiAuthenticatorResult::of(result, |made| {
            let new_credential = &made.credential;
            let new_credential = FfiNewCredential {
                authenticator: made.authenticator,
                is_resident_key: made.is_resident_key,
                id: FfiBytes::from_slice(&new_credential.id),
                private_key: FfiBytes::from_slice(&new_credential.private_key),
                public_key: FfiBytes::from_slice(&new_credential.public_key),
                public_key_algorithm: new_credential.public_key_algorithm,
                attestation_object: FfiBytes::from_slice(&new_credential.attestation_object),
                authenticator_data: FfiBytes::from_slice(&new_credential.authenticator_data),
                backup_eligibility: new_credential.backup_eligibility,
                backup_state: new_credential.backup_state,
                displaced_credentials: new_credential.displaced_credentials.as_ptr(),
                displaced_credential_count: new_credential.displaced_credentials.len(),
            };
            // SAFETY: The callback takes the context, and the bytes live for the call.
            unsafe { on_credential(context, &new_credential) };
        })
    })
}

/// Gives `on_option` where each credential is that getAssertion could use for the request, with the one that it
/// selects by itself last.
///
/// # Safety
/// Every pointer points to as many items as the count or length beside it says. `on_option` takes `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_authenticator_credential_options(
    credentials: *const FfiCredential,
    credential_count: usize,
    request: &FfiCeremonyGetAssertionRequest,
    context: *mut c_void,
    on_option: unsafe extern "C" fn(context: *mut c_void, credential: usize),
) {
    abort_on_panic(|| {
        // SAFETY: The caller says so.
        let (credentials, allow_credential_ids) = unsafe {
            (
                credentials_from(credentials, credential_count),
                byte_slices(request.allow_credential_ids, request.allow_credential_id_count),
            )
        };
        let parameters = GetAssertionParameters {
            // SAFETY: The caller says so.
            rp_id: unsafe { request.rp_id.as_slice() },
            allow_credential_ids: &allow_credential_ids,
            is_conditional: request.is_conditional,
            require_user_verification: false,
            client_data_hash: &[],
        };
        for option in authenticator::credential_options(&credentials, &parameters) {
            // SAFETY: The callback takes the context.
            unsafe { on_option(context, option) };
        }
    });
}

/// Goes through the authenticators for one that makes the assertion,
/// <https://w3c.github.io/webauthn/#sctn-getAssertion>.
///
/// # Safety
/// Every pointer points to as many items as the count or length beside it says. `on_assertion` takes `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_ceremony_get_assertion(
    authenticators: *const FfiAuthenticator,
    authenticator_count: usize,
    request: &FfiCeremonyGetAssertionRequest,
    context: *mut c_void,
    on_assertion: unsafe extern "C" fn(context: *mut c_void, assertion: &FfiAuthenticatorAssertion),
) -> FfiAuthenticatorResult {
    abort_on_panic(|| {
        // SAFETY: The caller says so.
        let (credentials, allow_credential_ids) = unsafe {
            (
                credentials_of(authenticators, authenticator_count),
                byte_slices(request.allow_credential_ids, request.allow_credential_id_count),
            )
        };
        // SAFETY: The caller says so.
        let authenticators = unsafe { authenticators_from(authenticators, authenticator_count, &credentials) };
        let request = GetAssertionRequest {
            // SAFETY: The caller says so.
            rp_id: unsafe { request.rp_id.as_slice() },
            allow_credential_ids: &allow_credential_ids,
            user_verification: request.user_verification,
            is_conditional: request.is_conditional,
            // SAFETY: The caller says so.
            client_data_hash: unsafe { request.client_data_hash.as_slice() },
        };
        let result = ceremony::get_assertion(&authenticators, &request);
        FfiAuthenticatorResult::of(result, |got| {
            let assertion = FfiAuthenticatorAssertion {
                authenticator: got.authenticator,
                selected_credential: got.assertion.selected_credential,
                has_sign_count: got.assertion.sign_count.is_some(),
                sign_count: got.assertion.sign_count.unwrap_or(0),
                authenticator_data: FfiBytes::from_slice(&got.assertion.authenticator_data),
                signature: FfiBytes::from_slice(&got.assertion.signature),
            };
            // SAFETY: The callback takes the context, and the bytes live for the call.
            unsafe { on_assertion(context, &assertion) };
        })
    })
}

/// Whether the bytes are a P-256 private key as a DER PKCS #8 PrivateKeyInfo, which is what a credential can have.
///
/// # Safety
/// `private_key` points to `private_key.length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_authenticator_is_credential_private_key(private_key: FfiBytes) -> bool {
    // SAFETY: The caller says so.
    abort_on_panic(|| authenticator::is_credential_private_key(unsafe { private_key.as_slice() }))
}
