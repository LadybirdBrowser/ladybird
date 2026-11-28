/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The part of a ceremony where the client goes through the software authenticators that it can use, and asks each
//! one that fits the request, <https://w3c.github.io/webauthn/#sctn-createCredential> and
//! <https://w3c.github.io/webauthn/#sctn-getAssertion>.

use crate::authenticator::{
    self, Assertion, AuthenticatorConfiguration, AuthenticatorError, Credential, GetAssertionParameters,
    MakeCredentialParameters, NewCredential,
};
use crate::security_key::ctap::{ResidentKeyRequirement, UserVerificationRequirement};

/// <https://w3c.github.io/webauthn/#authenticator-attachment-modality>
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AuthenticatorAttachment {
    Platform,
    CrossPlatform,
}

/// An authenticator as a ceremony sees it.
pub struct Authenticator<'a> {
    pub configuration: AuthenticatorConfiguration,
    pub attachment: AuthenticatorAttachment,
    pub credentials: &'a [Credential<'a>],
}

pub struct MakeCredentialRequest<'a> {
    pub rp_id: &'a [u8],
    pub user_id: &'a [u8],
    /// The COSEAlgorithmIdentifiers of pubKeyCredParams whose type is "public-key", in the order of preference.
    pub public_key_algorithms: &'a [i32],
    pub exclude_credential_ids: &'a [&'a [u8]],
    pub authenticator_attachment: Option<AuthenticatorAttachment>,
    pub resident_key: ResidentKeyRequirement,
    pub user_verification: UserVerificationRequirement,
}

#[derive(Debug)]
pub struct MadeCredential {
    /// Where the authenticator is that made the credential.
    pub authenticator: usize,
    /// Whether the new credential is a client-side discoverable credential.
    pub is_resident_key: bool,
    pub credential: NewCredential,
}

/// <https://w3c.github.io/webauthn/#sctn-createCredential>
pub fn make_credential(
    authenticators: &[Authenticator],
    request: &MakeCredentialRequest,
) -> Result<MadeCredential, AuthenticatorError> {
    // If the length of pkOptions.user.id is not between 1 and 64 bytes (inclusive) then throw a TypeError.
    // NB: A renderer that sends such an id skipped the check that throws the TypeError.
    if request.user_id.is_empty() || request.user_id.len() > 64 {
        return Err(AuthenticatorError::NotAllowed);
    }

    // FIXME: A ceremony that no authenticator completes fails right away, without waiting for lifetimeTimer to
    //        expire.
    for (index, authenticator) in authenticators.iter().enumerate() {
        let configuration = &authenticator.configuration;

        // 2. If pkOptions.authenticatorSelection is present:
        //    1. If pkOptions.authenticatorSelection.authenticatorAttachment is present and its value is not equal to
        //       authenticator's authenticator attachment modality, continue.
        if request
            .authenticator_attachment
            .is_some_and(|attachment| attachment != authenticator.attachment)
        {
            continue;
        }

        //    2. If pkOptions.authenticatorSelection.residentKey is present and set to required: If the authenticator
        //       is not capable of storing a client-side discoverable public key credential source, continue.
        if request.resident_key == ResidentKeyRequirement::Required && !configuration.has_resident_key {
            continue;
        }

        //    3. If pkOptions.authenticatorSelection.userVerification is set to required and the authenticator is not
        //       capable of performing user verification, continue.
        if request.user_verification == UserVerificationRequirement::Required && !configuration.has_user_verification {
            continue;
        }

        // 3. Let requireResidentKey be the effective resident key requirement for credential creation, a Boolean
        //    value, as follows:
        let require_resident_key = match request.resident_key {
            // -> is present and set to required
            //    Let requireResidentKey be true.
            ResidentKeyRequirement::Required => true,
            // -> is present and set to preferred
            //    If the authenticator is capable of client-side credential storage modality, let requireResidentKey
            //    be true. [Otherwise] let requireResidentKey be false.
            ResidentKeyRequirement::Preferred => configuration.has_resident_key,
            // -> is present and set to discouraged
            //    Let requireResidentKey be false.
            ResidentKeyRequirement::Discouraged => false,
        };

        // 4. Let userVerification be the effective user verification requirement for credential creation, a Boolean
        //    value, as follows.
        let user_verification = match request.user_verification {
            // -> is set to required
            //    Let userVerification be true.
            UserVerificationRequirement::Required => true,
            // -> is set to preferred
            //    If the authenticator is capable of user verification, let userVerification be true. [Otherwise] let
            //    userVerification be false.
            UserVerificationRequirement::Preferred => configuration.has_user_verification,
            // -> is set to discouraged
            //    Let userVerification be false.
            UserVerificationRequirement::Discouraged => false,
        };

        // Invoke the authenticatorMakeCredential operation on authenticator with clientDataHash, pkOptions.rp,
        // pkOptions.user, requireResidentKey, userVerification, credTypesAndPubKeyAlgs,
        // excludeCredentialDescriptorList, enterpriseAttestationPossible, attestationFormats, and
        // authenticatorExtensions as parameters.
        let result = authenticator::make_credential(
            configuration,
            authenticator.credentials,
            &MakeCredentialParameters {
                rp_id: request.rp_id,
                user_handle: request.user_id,
                public_key_algorithms: request.public_key_algorithms,
                exclude_credential_ids: request.exclude_credential_ids,
                require_resident_key,
                require_user_verification: user_verification,
            },
        );

        match result {
            // -> If any authenticator indicates success,
            Ok(credential) => {
                return Ok(MadeCredential {
                    authenticator: index,
                    is_resident_key: require_resident_key,
                    credential,
                });
            }
            // -> If any authenticator returns an error status equivalent to "InvalidStateError",
            //    Throw an "InvalidStateError" DOMException.
            Err(AuthenticatorError::InvalidState) => return Err(AuthenticatorError::InvalidState),
            // -> If any authenticator returns an error status not equivalent to "InvalidStateError",
            //    Remove authenticator from issuedRequests.
            Err(_) => {}
        }
    }

    // Throw a "NotAllowedError" DOMException.
    Err(AuthenticatorError::NotAllowed)
}

pub struct GetAssertionRequest<'a> {
    pub rp_id: &'a [u8],
    pub allow_credential_ids: &'a [&'a [u8]],
    pub user_verification: UserVerificationRequirement,
    pub is_conditional: bool,
    pub client_data_hash: &'a [u8],
}

#[derive(Debug)]
pub struct GotAssertion {
    /// Where the authenticator is that made the assertion.
    pub authenticator: usize,
    pub assertion: Assertion,
}

/// <https://w3c.github.io/webauthn/#sctn-getAssertion>
pub fn get_assertion(
    authenticators: &[Authenticator],
    request: &GetAssertionRequest,
) -> Result<GotAssertion, AuthenticatorError> {
    // FIXME: A ceremony that no authenticator completes fails right away, without waiting for lifetimeTimer to
    //        expire.
    for (index, authenticator) in authenticators.iter().enumerate() {
        let configuration = &authenticator.configuration;

        // If pkOptions.userVerification is set to required and the authenticator is not capable of performing user
        // verification, continue.
        if request.user_verification == UserVerificationRequirement::Required && !configuration.has_user_verification {
            continue;
        }

        // Let userVerification be the effective user verification requirement for assertion, a Boolean value.
        let user_verification = request.user_verification == UserVerificationRequirement::Required
            || (request.user_verification == UserVerificationRequirement::Preferred
                && configuration.has_user_verification);

        // NB: An authenticator with no credential for the request is not a candidate, and gives an error like one
        //     that the user did not agree with.
        let result = authenticator::get_assertion(
            configuration,
            authenticator.credentials,
            &GetAssertionParameters {
                rp_id: request.rp_id,
                allow_credential_ids: request.allow_credential_ids,
                is_conditional: request.is_conditional,
                require_user_verification: user_verification,
                client_data_hash: request.client_data_hash,
            },
        );
        if let Ok(assertion) = result {
            return Ok(GotAssertion {
                authenticator: index,
                assertion,
            });
        }
    }

    // Throw a "NotAllowedError" DOMException.
    Err(AuthenticatorError::NotAllowed)
}

#[cfg(test)]
mod tests {
    use super::*;

    const RP_ID: &[u8] = b"example.com";

    fn configuration() -> AuthenticatorConfiguration {
        AuthenticatorConfiguration {
            has_resident_key: true,
            has_user_verification: true,
            has_signature_counter: true,
            is_user_consenting: true,
            is_user_verified: true,
            default_backup_eligibility: false,
            default_backup_state: false,
        }
    }

    fn authenticator<'a>(
        configuration: AuthenticatorConfiguration,
        attachment: AuthenticatorAttachment,
        credentials: &'a [Credential<'a>],
    ) -> Authenticator<'a> {
        Authenticator {
            configuration,
            attachment,
            credentials,
        }
    }

    fn make_credential_request<'a>() -> MakeCredentialRequest<'a> {
        MakeCredentialRequest {
            rp_id: RP_ID,
            user_id: &[1, 2, 3],
            public_key_algorithms: &[authenticator::COSE_ALGORITHM_ES256],
            exclude_credential_ids: &[],
            authenticator_attachment: None,
            resident_key: ResidentKeyRequirement::Discouraged,
            user_verification: UserVerificationRequirement::Discouraged,
        }
    }

    fn get_assertion_request<'a>() -> GetAssertionRequest<'a> {
        GetAssertionRequest {
            rp_id: RP_ID,
            allow_credential_ids: &[],
            user_verification: UserVerificationRequirement::Discouraged,
            is_conditional: false,
            client_data_hash: &[0xcd; 32],
        }
    }

    fn credential<'a>(id: &'a [u8], private_key: &'a [u8]) -> Credential<'a> {
        Credential {
            id,
            private_key,
            rp_id: RP_ID,
            user_handle: Some(&[1, 2, 3]),
            is_resident_credential: true,
            sign_count: Some(0),
            backup_eligibility: false,
            backup_state: false,
        }
    }

    const PLATFORM: AuthenticatorAttachment = AuthenticatorAttachment::Platform;
    const CROSS_PLATFORM: AuthenticatorAttachment = AuthenticatorAttachment::CrossPlatform;

    #[test]
    fn a_user_id_of_the_wrong_size_makes_no_credential() {
        let authenticators = [authenticator(configuration(), PLATFORM, &[])];
        for user_id in [&[][..], &[0; 65][..]] {
            let request = MakeCredentialRequest {
                user_id,
                ..make_credential_request()
            };
            assert_eq!(
                make_credential(&authenticators, &request).unwrap_err(),
                AuthenticatorError::NotAllowed
            );
        }
        let request = MakeCredentialRequest {
            user_id: &[0; 64],
            ..make_credential_request()
        };
        assert!(make_credential(&authenticators, &request).is_ok());
    }

    #[test]
    fn the_first_authenticator_that_fits_makes_the_credential() {
        let without_storage = AuthenticatorConfiguration {
            has_resident_key: false,
            ..configuration()
        };
        let without_user_verification = AuthenticatorConfiguration {
            has_user_verification: false,
            ..configuration()
        };
        let authenticators = [
            authenticator(configuration(), CROSS_PLATFORM, &[]),
            authenticator(without_storage, PLATFORM, &[]),
            authenticator(without_user_verification, PLATFORM, &[]),
            authenticator(configuration(), PLATFORM, &[]),
        ];
        let made_by = |request: MakeCredentialRequest| {
            let made = make_credential(&authenticators, &request).unwrap();
            (made.authenticator, made.is_resident_key)
        };

        assert_eq!(made_by(make_credential_request()), (0, false));
        let platform = || MakeCredentialRequest {
            authenticator_attachment: Some(PLATFORM),
            ..make_credential_request()
        };
        assert_eq!(made_by(platform()), (1, false));
        // NB: A preferred discoverable credential is one where the authenticator can store it.
        let preferred = MakeCredentialRequest {
            resident_key: ResidentKeyRequirement::Preferred,
            ..platform()
        };
        assert_eq!(made_by(preferred), (1, false));
        let required = MakeCredentialRequest {
            resident_key: ResidentKeyRequirement::Required,
            ..platform()
        };
        assert_eq!(made_by(required), (2, true));
        let with_user_verification = MakeCredentialRequest {
            resident_key: ResidentKeyRequirement::Required,
            user_verification: UserVerificationRequirement::Required,
            ..platform()
        };
        assert_eq!(made_by(with_user_verification), (3, true));
    }

    #[test]
    fn an_excluded_credential_ends_the_ceremony_for_all_authenticators() {
        let credentials = [credential(&[5; 4], &[])];
        let authenticators = [
            authenticator(configuration(), PLATFORM, &credentials),
            authenticator(configuration(), PLATFORM, &[]),
        ];
        let request = MakeCredentialRequest {
            exclude_credential_ids: &[&[5; 4]],
            ..make_credential_request()
        };
        assert_eq!(
            make_credential(&authenticators, &request).unwrap_err(),
            AuthenticatorError::InvalidState
        );
    }

    #[test]
    fn an_authenticator_that_refuses_leaves_the_ceremony_to_the_next() {
        let without_consent = AuthenticatorConfiguration {
            is_user_consenting: false,
            ..configuration()
        };
        let authenticators = [
            authenticator(without_consent, PLATFORM, &[]),
            authenticator(configuration(), PLATFORM, &[]),
        ];
        assert_eq!(
            make_credential(&authenticators, &make_credential_request())
                .unwrap()
                .authenticator,
            1
        );
        assert_eq!(
            make_credential(&authenticators[..1], &make_credential_request()).unwrap_err(),
            AuthenticatorError::NotAllowed
        );
    }

    #[test]
    fn the_authenticator_with_a_credential_for_the_request_makes_the_assertion() {
        let made = make_credential(
            &[authenticator(configuration(), PLATFORM, &[])],
            &make_credential_request(),
        )
        .unwrap()
        .credential;
        let credentials = [credential(&made.id, &made.private_key)];
        let without_user_verification = AuthenticatorConfiguration {
            has_user_verification: false,
            ..configuration()
        };
        let authenticators = [
            authenticator(configuration(), PLATFORM, &[]),
            authenticator(without_user_verification, PLATFORM, &credentials),
            authenticator(configuration(), CROSS_PLATFORM, &credentials),
        ];
        let got_from = |user_verification| {
            let request = GetAssertionRequest {
                user_verification,
                ..get_assertion_request()
            };
            let got = get_assertion(&authenticators, &request).unwrap();
            // NB: The UV flag of the authenticator data.
            (got.authenticator, got.assertion.authenticator_data[32] & 0x04 != 0)
        };

        assert_eq!(got_from(UserVerificationRequirement::Discouraged), (1, false));
        assert_eq!(got_from(UserVerificationRequirement::Preferred), (1, false));
        assert_eq!(got_from(UserVerificationRequirement::Required), (2, true));

        assert_eq!(
            get_assertion(&authenticators[..1], &get_assertion_request()).unwrap_err(),
            AuthenticatorError::NotAllowed
        );
    }
}
