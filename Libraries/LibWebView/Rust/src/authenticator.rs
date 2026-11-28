/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The operations of a software authenticator, <https://w3c.github.io/webauthn/#sctn-authenticator-model>. The
//! authenticator of the platform and the virtual authenticators of WebDriver are of this kind. Who calls these keeps
//! the credentials.

use crate::cbor::{self, Value};
use libcrypto_rust::{
    EcCurve, KeyMaterial, ShaHashKind, ec_export_der, ec_export_public_coordinates, ec_generate_key_pair, ec_sign,
    fill_with_random, parse_private_key_info, sha_digest,
};

/// <https://www.iana.org/assignments/cose/cose.xhtml#algorithms>
pub const COSE_ALGORITHM_ES256: i32 = -7;

/// <https://w3c.github.io/webauthn/#authdata-flags>
mod flags {
    pub const USER_PRESENT: u8 = 1 << 0;
    pub const USER_VERIFIED: u8 = 1 << 2;
    pub const BACKUP_ELIGIBILITY: u8 = 1 << 3;
    pub const BACKUP_STATE: u8 = 1 << 4;
    pub const ATTESTED_CREDENTIAL_DATA_INCLUDED: u8 = 1 << 6;
}

const CREDENTIAL_ID_SIZE: usize = 32;

/// Why an operation gave no credential, named after the DOMException that the page gets.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AuthenticatorError {
    NotAllowed,
    InvalidState,
    NotSupported,
    Constraint,
}

/// <https://w3c.github.io/webauthn/#authenticator-configuration>
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AuthenticatorConfiguration {
    pub has_resident_key: bool,
    pub has_user_verification: bool,
    /// Whether the authenticator counts the assertions of a credential.
    pub has_signature_counter: bool,
    pub is_user_consenting: bool,
    pub is_user_verified: bool,
    pub default_backup_eligibility: bool,
    pub default_backup_state: bool,
}

/// <https://w3c.github.io/webauthn/#public-key-credential-source>
#[derive(Clone, Copy)]
pub struct Credential<'a> {
    pub id: &'a [u8],
    /// A P-256 key as a DER PKCS #8 PrivateKeyInfo.
    pub private_key: &'a [u8],
    pub rp_id: &'a [u8],
    pub user_handle: Option<&'a [u8]>,
    /// Whether this is a client-side discoverable public key credential source.
    pub is_resident_credential: bool,
    /// The signature counter, when the credential has one.
    pub sign_count: Option<u32>,
    pub backup_eligibility: bool,
    pub backup_state: bool,
}

pub struct MakeCredentialParameters<'a> {
    pub rp_id: &'a [u8],
    pub user_handle: &'a [u8],
    /// The COSEAlgorithmIdentifiers that the Relying Party takes, in the order of preference.
    pub public_key_algorithms: &'a [i32],
    pub exclude_credential_ids: &'a [&'a [u8]],
    pub require_resident_key: bool,
    pub require_user_verification: bool,
}

/// What the authenticator made, for who called it to keep and to give to the client.
#[derive(Debug)]
pub struct NewCredential {
    pub id: Vec<u8>,
    /// The DER PKCS #8 PrivateKeyInfo of the credential private key.
    pub private_key: Vec<u8>,
    /// The DER SubjectPublicKeyInfo of the credential public key.
    pub public_key: Vec<u8>,
    pub public_key_algorithm: i32,
    pub attestation_object: Vec<u8>,
    pub authenticator_data: Vec<u8>,
    pub backup_eligibility: bool,
    pub backup_state: bool,
    /// Where the credentials are that the new one takes the place of.
    pub displaced_credentials: Vec<usize>,
}

pub struct GetAssertionParameters<'a> {
    pub rp_id: &'a [u8],
    pub allow_credential_ids: &'a [&'a [u8]],
    pub is_conditional: bool,
    pub require_user_verification: bool,
    pub client_data_hash: &'a [u8],
}

#[derive(Debug)]
pub struct Assertion {
    /// Where the credential is that the authenticator selected.
    pub selected_credential: usize,
    /// The signature counter of the credential after the assertion.
    /// The signature counter, when the credential has one.
    pub sign_count: Option<u32>,
    pub authenticator_data: Vec<u8>,
    pub signature: Vec<u8>,
}

/// <https://w3c.github.io/webauthn/#sctn-authenticator-data>
fn authenticator_data_for(
    rp_id: &[u8],
    flags: u8,
    sign_count: u32,
    attested_credential_data: &[u8],
) -> Option<Vec<u8>> {
    // rpIdHash: SHA-256 hash of the RP ID the credential is scoped to.
    let mut data = sha_digest(ShaHashKind::Sha256, rp_id).ok()?;
    // flags
    data.push(flags);
    // signCount: Signature counter, 32-bit unsigned big-endian integer.
    data.extend_from_slice(&sign_count.to_be_bytes());
    // attestedCredentialData: attested credential data (if present).
    data.extend_from_slice(attested_credential_data);
    Some(data)
}

fn backup_flags(backup_eligibility: bool, backup_state: bool) -> u8 {
    let mut flags = 0;
    if backup_eligibility {
        flags |= flags::BACKUP_ELIGIBILITY;
    }
    if backup_state {
        flags |= flags::BACKUP_STATE;
    }
    flags
}

/// <https://w3c.github.io/webauthn/#sctn-op-make-cred>
pub fn make_credential(
    configuration: &AuthenticatorConfiguration,
    credentials: &[Credential],
    parameters: &MakeCredentialParameters,
) -> Result<NewCredential, AuthenticatorError> {
    // 2. Check if at least one of the specified combinations of PublicKeyCredentialType and cryptographic parameters
    //    in credTypesAndPubKeyAlgs is supported. If not, return an error code equivalent to "NotSupportedError" and
    //    terminate the operation.
    if !parameters.public_key_algorithms.contains(&COSE_ALGORITHM_ES256) {
        return Err(AuthenticatorError::NotSupported);
    }

    // 3. For each descriptor of excludeCredentialDescriptorList:
    for excluded_credential_id in parameters.exclude_credential_ids {
        // 1. If looking up descriptor.id in this authenticator returns non-null, and the returned item's RP ID and
        //    type match rpEntity.id and excludeCredentialDescriptorList.type respectively, then collect an
        //    authorization gesture confirming user consent for creating a new credential. The authorization gesture
        //    MUST include a test of user presence. If the user
        let is_excluded = credentials
            .iter()
            .any(|credential| credential.id == *excluded_credential_id && credential.rp_id == parameters.rp_id);
        if !is_excluded {
            continue;
        }
        // -> confirms consent to create a new credential
        //    return an error code equivalent to "InvalidStateError" and terminate the operation.
        if configuration.is_user_consenting {
            return Err(AuthenticatorError::InvalidState);
        }
        // -> does not consent to create a new credential
        //    return an error code equivalent to "NotAllowedError" and terminate the operation.
        return Err(AuthenticatorError::NotAllowed);
    }

    // 4. If requireResidentKey is true and the authenticator cannot store a client-side discoverable public key
    //    credential source, return an error code equivalent to "ConstraintError" and terminate the operation.
    if parameters.require_resident_key && !configuration.has_resident_key {
        return Err(AuthenticatorError::Constraint);
    }

    // 5. If requireUserVerification is true and the authenticator cannot perform user verification, return an error
    //    code equivalent to "ConstraintError" and terminate the operation.
    if parameters.require_user_verification && !configuration.has_user_verification {
        return Err(AuthenticatorError::Constraint);
    }

    // 6. Collect an authorization gesture confirming user consent for creating a new credential. [...] If
    //    requireUserVerification is true, the authorization gesture MUST include user verification. [...] If the user
    //    does not consent or if user verification fails, return an error code equivalent to "NotAllowedError" and
    //    terminate the operation.
    if !configuration.is_user_consenting {
        return Err(AuthenticatorError::NotAllowed);
    }
    if parameters.require_user_verification && !configuration.is_user_verified {
        return Err(AuthenticatorError::NotAllowed);
    }
    let user_verified = configuration.has_user_verification && configuration.is_user_verified;

    // 7. Once the authorization gesture has been completed and user consent has been obtained, generate a new
    //    credential object:
    // 1. Let (publicKey, privateKey) be a new pair of cryptographic keys using the combination of
    //    PublicKeyCredentialType and cryptographic parameters represented by the first item in
    //    credTypesAndPubKeyAlgs that is supported by this authenticator.
    // 8. If any error occurred while creating the new credential object, return an error code equivalent to
    //    "UnknownError" and terminate the operation.
    // NB: The client hides the details of such an error from the Relying Party, which gets a "NotAllowedError".
    let unknown_error = |_| AuthenticatorError::NotAllowed;
    let key_pair = ec_generate_key_pair(EcCurve::P256).map_err(unknown_error)?;
    let private_key = ec_export_der(&key_pair.private_key).map_err(unknown_error)?;
    let public_key = ec_export_der(&key_pair.public_key).map_err(unknown_error)?;
    let (x, y) = ec_export_public_coordinates(&key_pair.public_key).map_err(unknown_error)?;

    // 2. Let userHandle be userEntity.id.
    // 3. Let credentialSource be a new public key credential source with the fields:
    // 10. If the authenticator [...] supports a per credential signature counter: allocate the counter, associate
    //     it with the new credential, and initialize the counter value as zero.
    let sign_count = 0;
    let backup_eligibility = configuration.default_backup_eligibility;
    let backup_state = configuration.default_backup_state;

    // 4. If requireResidentKey is true or the authenticator chooses to create a client-side discoverable public key
    //    credential source:
    //    1. Let credentialId be a new credential id.
    //    2. Set credentialSource.id to credentialId.
    //    3. Let credentials be this authenticator's credentials map.
    //    4. Set credentials[(rpEntity.id, userHandle)] to credentialSource.
    // NB: The authenticator keeps every credential source, discoverable or not, in its credentials map.
    let mut id = vec![0; CREDENTIAL_ID_SIZE];
    fill_with_random(&mut id).map_err(unknown_error)?;
    let mut displaced_credentials = Vec::new();
    if parameters.require_resident_key {
        displaced_credentials = (0..credentials.len())
            .filter(|index| {
                let credential = &credentials[*index];
                credential.is_resident_credential
                    && credential.rp_id == parameters.rp_id
                    && credential.user_handle == Some(parameters.user_handle)
            })
            .collect();
    }

    // 11. Let attestedCredentialData be the attested credential data byte array including the credentialId and
    //     publicKey.
    // https://w3c.github.io/webauthn/#sctn-attested-credential-data
    // aaguid: The AAGUID of the authenticator.
    // NB: This authenticator has no AAGUID to tell, and so no attestation.
    let mut attested_credential_data = vec![0; 16];
    // credentialIdLength: Byte length L of credentialId, 16-bit unsigned big-endian integer.
    attested_credential_data.extend_from_slice(&(CREDENTIAL_ID_SIZE as u16).to_be_bytes());
    // credentialId
    attested_credential_data.extend_from_slice(&id);
    // credentialPublicKey: The credential public key encoded in COSE_Key format.
    // https://w3c.github.io/webauthn/#sctn-encoded-credPubKey-examples
    attested_credential_data.extend_from_slice(&cbor::encode(&Value::Map(vec![
        // kty: EC2 key type
        (Value::Integer(1), Value::Integer(2)),
        // alg: ES256 signature algorithm
        (Value::Integer(3), Value::Integer(i64::from(COSE_ALGORITHM_ES256))),
        // crv: P-256 curve
        (Value::Integer(-1), Value::Integer(1)),
        // x-coordinate as byte string 32 bytes in length
        (Value::Integer(-2), Value::Bytes(x)),
        // y-coordinate as byte string 32 bytes in length
        (Value::Integer(-3), Value::Bytes(y)),
    ])));

    // 13. Let authenticatorData be the byte array specified in § 6.1 Authenticator Data, including
    //     attestedCredentialData as the attestedCredentialData and processedExtensions, if any, as the extensions.
    let mut flags = flags::USER_PRESENT | flags::ATTESTED_CREDENTIAL_DATA_INCLUDED;
    if user_verified {
        flags |= flags::USER_VERIFIED;
    }
    flags |= backup_flags(backup_eligibility, backup_state);
    let authenticator_data = authenticator_data_for(parameters.rp_id, flags, sign_count, &attested_credential_data)
        .ok_or(AuthenticatorError::NotAllowed)?;

    // 12. Let attestationFormat be the first supported attestation statement format identifier from
    //     attestationFormats, taking into account enterpriseAttestationPossible. If attestationFormats contains no
    //     supported value, then let attestationFormat be the attestation statement format identifier most preferred by
    //     this authenticator.
    // 14. Create an attestation object for the new credential using the procedure specified in § 6.5.5 Generating an
    //     Attestation Object, the attestation statement format attestationFormat, and the values authenticatorData
    //     and hash, as well as taking into account the value of enterpriseAttestationPossible.
    // NB: The only format is "none", https://w3c.github.io/webauthn/#sctn-none-attestation
    let attestation_object = cbor::encode(&Value::Map(vec![
        (Value::from("fmt"), Value::from("none")),
        (Value::from("attStmt"), Value::Map(Vec::new())),
        (Value::from("authData"), Value::Bytes(authenticator_data.clone())),
    ]));

    // On successful completion of this operation, the authenticator returns the attestation object to the client.
    Ok(NewCredential {
        id,
        private_key,
        public_key,
        public_key_algorithm: COSE_ALGORITHM_ES256,
        attestation_object,
        authenticator_data,
        backup_eligibility,
        backup_state,
        displaced_credentials,
    })
}

/// Where the credentials are that getAssertion could use for the request, with the one that it selects by itself
/// last. These are the first steps of <https://w3c.github.io/webauthn/#sctn-op-get-assertion>.
pub fn credential_options(credentials: &[Credential], parameters: &GetAssertionParameters) -> Vec<usize> {
    // 2. Let credentialOptions be a new empty set of public key credential sources.
    let mut credential_options: Vec<usize> = Vec::new();
    let allow_credential_ids = parameters.allow_credential_ids;

    // NB: For a request with conditional mediation, the client has no allowCredentialDescriptorList for the
    //     authenticator, so that only discoverable credentials are used. The list of the Relying Party is then the
    //     filter of the client for what it offers the user, which is done here.
    if parameters.is_conditional {
        for (index, credential) in credentials.iter().enumerate() {
            if credential.is_resident_credential
                && (allow_credential_ids.is_empty() || allow_credential_ids.contains(&credential.id))
            {
                credential_options.push(index);
            }
        }
    }
    // 3. If allowCredentialDescriptorList was supplied, then for each descriptor of allowCredentialDescriptorList:
    else if !allow_credential_ids.is_empty() {
        for credential_id in allow_credential_ids {
            // 1. Let credSource be the result of looking up descriptor.id in this authenticator.
            // 2. If credSource is not null, append it to credentialOptions.
            // NB: credentialOptions is a set, so a credential that the list names twice is in it once.
            for (index, credential) in credentials.iter().enumerate() {
                if credential.id == *credential_id && !credential_options.contains(&index) {
                    credential_options.push(index);
                }
            }
        }
    }
    // 4. Otherwise (allowCredentialDescriptorList was not supplied), for each key → credSource of this
    //    authenticator's credentials map, append credSource to credentialOptions.
    else {
        for (index, credential) in credentials.iter().enumerate() {
            if credential.is_resident_credential {
                credential_options.push(index);
            }
        }
    }

    // 5. Remove any items from credentialOptions whose rpId is not equal to rpId.
    credential_options.retain(|index| credentials[*index].rp_id == parameters.rp_id);
    credential_options
}

/// The DER Ecdsa-Sig-Value of a signature that is the two integers r and s, each with the size of the curve.
fn der_signature(signature: &[u8]) -> Vec<u8> {
    fn integer(bytes: &[u8]) -> Vec<u8> {
        let first_nonzero = bytes.iter().position(|byte| *byte != 0).unwrap_or(bytes.len() - 1);
        let mut contents = bytes[first_nonzero..].to_vec();
        // NB: A DER INTEGER is signed, so a leading zero keeps a high first bit from making it negative.
        if contents[0] & 0x80 != 0 {
            contents.insert(0, 0);
        }
        let mut encoded = vec![0x02, contents.len() as u8];
        encoded.append(&mut contents);
        encoded
    }

    let (r, s) = signature.split_at(signature.len() / 2);
    let mut contents = integer(r);
    contents.append(&mut integer(s));
    // NB: The two integers of a P-256 signature take 70 bytes at most, so the length has the short form.
    let mut encoded = vec![0x30, contents.len() as u8];
    encoded.append(&mut contents);
    encoded
}

/// The assertion signature of the data, with a P-256 key given as a DER PKCS #8 PrivateKeyInfo.
fn sign(private_key: &[u8], data: &[u8]) -> Option<Vec<u8>> {
    let (key_info, is_whole_input) = parse_private_key_info(private_key).ok()?;
    if !is_whole_input || key_info.named_curve() != Some(EcCurve::P256) {
        return None;
    }
    let KeyMaterial::Key(key) = key_info.into_material() else {
        return None;
    };
    let digest = sha_digest(ShaHashKind::Sha256, data).ok()?;
    Some(der_signature(&ec_sign(&key, &digest).ok()?))
}

/// Whether the bytes are a P-256 private key as a DER PKCS #8 PrivateKeyInfo, which is what a credential can have.
pub fn is_credential_private_key(private_key: &[u8]) -> bool {
    parse_private_key_info(private_key).is_ok_and(|(key_info, is_whole_input)| {
        is_whole_input
            && key_info.named_curve() == Some(EcCurve::P256)
            && matches!(key_info.into_material(), KeyMaterial::Key(_))
    })
}

/// <https://w3c.github.io/webauthn/#sctn-op-get-assertion>
pub fn get_assertion(
    configuration: &AuthenticatorConfiguration,
    credentials: &[Credential],
    parameters: &GetAssertionParameters,
) -> Result<Assertion, AuthenticatorError> {
    let credential_options = credential_options(credentials, parameters);

    // 6. If credentialOptions is now empty, return an error code equivalent to "NotAllowedError" and terminate the
    //    operation.
    // NB: The authenticator has nobody to ask, so it selects the credential that it got last.
    let Some(&selected_credential) = credential_options.last() else {
        return Err(AuthenticatorError::NotAllowed);
    };

    // 7. Prompt the user to select a public key credential source selectedCredential from credentialOptions. Collect
    //    an authorization gesture confirming user consent for using selectedCredential. [...] If
    //    requireUserVerification is true, the authorization gesture MUST include user verification. [...] If the user
    //    does not consent, return an error code equivalent to "NotAllowedError" and terminate the operation.
    if !configuration.is_user_consenting {
        return Err(AuthenticatorError::NotAllowed);
    }
    let user_verified = configuration.has_user_verification && configuration.is_user_verified;
    if parameters.require_user_verification && !user_verified {
        return Err(AuthenticatorError::NotAllowed);
    }
    let credential = &credentials[selected_credential];

    // 9. Increment the credential associated signature counter or the global signature counter value, depending on
    //    which approach is implemented by the authenticator, by some positive value. If the authenticator does not
    //    implement a signature counter, let the signature counter value remain constant at zero.
    // NB: A credential of a virtual authenticator can be one without a signature counter.
    let sign_count = match credential.sign_count {
        // NB: A counter that went back to zero would look like a cloned credential to the Relying Party.
        Some(sign_count) if configuration.has_signature_counter => {
            Some(sign_count.checked_add(1).ok_or(AuthenticatorError::NotAllowed)?)
        }
        sign_count => sign_count,
    };

    // 10. Let authenticatorData be the byte array specified in § 6.1 Authenticator Data including processedExtensions,
    //     if any, as the extensions and excluding attestedCredentialData.
    let mut flags = flags::USER_PRESENT;
    if user_verified {
        flags |= flags::USER_VERIFIED;
    }
    flags |= backup_flags(credential.backup_eligibility, credential.backup_state);
    let authenticator_data = authenticator_data_for(parameters.rp_id, flags, sign_count.unwrap_or(0), &[])
        .ok_or(AuthenticatorError::NotAllowed)?;

    // 11. Let signature be the assertion signature of the concatenation authenticatorData || hash using the privateKey
    //     of selectedCredential. A simple, undelimited concatenation is safe to use here because the authenticator
    //     data describes its own length. The hash of the serialized client data (which potentially has a variable
    //     length) is always the last element.
    let signed_data = [&authenticator_data[..], parameters.client_data_hash].concat();

    // 12. If any error occurred while generating the assertion signature, return an error code equivalent to
    //     "UnknownError" and terminate the operation.
    // NB: The client hides the details of such an error from the Relying Party, which gets a "NotAllowedError".
    let signature = sign(credential.private_key, &signed_data).ok_or(AuthenticatorError::NotAllowed)?;

    // 13. Return to the user agent:
    //     - selectedCredential.id, if either a list of credentials (i.e., allowCredentialDescriptorList) of length 2
    //       or greater was supplied by the client, or no such list was supplied.
    //     - authenticatorData
    //     - signature
    //     - selectedCredential.userHandle
    Ok(Assertion {
        selected_credential,
        sign_count,
        authenticator_data,
        signature,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcrypto_rust::{ec_verify, parse_subject_public_key_info};

    const RP_ID: &[u8] = b"example.com";
    const CLIENT_DATA_HASH: &[u8] = &[0xcd; 32];

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

    fn make_credential_parameters<'a>() -> MakeCredentialParameters<'a> {
        MakeCredentialParameters {
            rp_id: RP_ID,
            user_handle: &[1, 2, 3],
            public_key_algorithms: &[-257, COSE_ALGORITHM_ES256],
            exclude_credential_ids: &[],
            require_resident_key: false,
            require_user_verification: false,
        }
    }

    fn get_assertion_parameters<'a>() -> GetAssertionParameters<'a> {
        GetAssertionParameters {
            rp_id: RP_ID,
            allow_credential_ids: &[],
            is_conditional: false,
            require_user_verification: false,
            client_data_hash: CLIENT_DATA_HASH,
        }
    }

    fn credential<'a>(id: &'a [u8], rp_id: &'a [u8], is_resident_credential: bool) -> Credential<'a> {
        Credential {
            id,
            private_key: &[],
            rp_id,
            user_handle: Some(&[1, 2, 3]),
            is_resident_credential,
            sign_count: Some(0),
            backup_eligibility: false,
            backup_state: false,
        }
    }

    /// The two integers of a DER Ecdsa-Sig-Value, each with the 32 bytes of P-256.
    fn raw_signature(der: &[u8]) -> Vec<u8> {
        assert_eq!(der[0], 0x30);
        assert_eq!(usize::from(der[1]), der.len() - 2);
        let mut raw = Vec::new();
        let mut rest = &der[2..];
        for _ in 0..2 {
            assert_eq!(rest[0], 0x02);
            let (integer, after) = rest[2..].split_at(usize::from(rest[1]));
            let integer = if integer.len() > 32 { &integer[1..] } else { integer };
            raw.extend(std::iter::repeat_n(0, 32 - integer.len()));
            raw.extend_from_slice(integer);
            rest = after;
        }
        assert!(rest.is_empty());
        raw
    }

    #[test]
    fn makes_a_credential_that_says_what_it_is_for() {
        let new_credential = make_credential(&configuration(), &[], &make_credential_parameters()).unwrap();

        assert_eq!(new_credential.id.len(), 32);
        assert_eq!(new_credential.public_key_algorithm, COSE_ALGORITHM_ES256);
        assert!(is_credential_private_key(&new_credential.private_key));
        assert!(new_credential.displaced_credentials.is_empty());

        let data = &new_credential.authenticator_data;
        assert_eq!(data[..32], sha_digest(ShaHashKind::Sha256, RP_ID).unwrap());
        // NB: The flags UP, UV and AT, and a signature counter of zero.
        assert_eq!(data[32], 0x45);
        assert_eq!(data[33..37], [0; 4]);
        assert_eq!(data[37..53], [0; 16]);
        assert_eq!(data[53..55], [0, 32]);
        assert_eq!(data[55..87], new_credential.id);

        let (cose_key, size) = cbor::decode(&data[87..]).unwrap();
        assert_eq!(size, data.len() - 87);
        assert_eq!(cose_key.get(1).and_then(Value::as_integer), Some(2));
        assert_eq!(cose_key.get(3).and_then(Value::as_integer), Some(-7));
        assert_eq!(cose_key.get(-1).and_then(Value::as_integer), Some(1));
        let x = cose_key.get(-2).and_then(Value::as_bytes).unwrap();
        let y = cose_key.get(-3).and_then(Value::as_bytes).unwrap();
        // NB: The SubjectPublicKeyInfo ends with the uncompressed point.
        assert_eq!(new_credential.public_key.len(), 91);
        assert_eq!(new_credential.public_key[27..], [x, y].concat());

        let (attestation_object, _) = cbor::decode(&new_credential.attestation_object).unwrap();
        assert_eq!(
            attestation_object.get_text("fmt").and_then(Value::as_text),
            Some("none")
        );
        assert_eq!(
            attestation_object.get_text("authData").and_then(Value::as_bytes),
            Some(&data[..])
        );
    }

    #[test]
    fn make_credential_refuses_what_the_authenticator_cannot_do() {
        let make = |configuration: AuthenticatorConfiguration, parameters: MakeCredentialParameters| {
            make_credential(&configuration, &[], &parameters).unwrap_err()
        };

        let error = make(
            configuration(),
            MakeCredentialParameters {
                public_key_algorithms: &[-257],
                ..make_credential_parameters()
            },
        );
        assert_eq!(error, AuthenticatorError::NotSupported);

        let error = make(
            AuthenticatorConfiguration {
                has_resident_key: false,
                ..configuration()
            },
            MakeCredentialParameters {
                require_resident_key: true,
                ..make_credential_parameters()
            },
        );
        assert_eq!(error, AuthenticatorError::Constraint);

        let requires_user_verification = || MakeCredentialParameters {
            require_user_verification: true,
            ..make_credential_parameters()
        };
        let error = make(
            AuthenticatorConfiguration {
                has_user_verification: false,
                ..configuration()
            },
            requires_user_verification(),
        );
        assert_eq!(error, AuthenticatorError::Constraint);

        let error = make(
            AuthenticatorConfiguration {
                is_user_verified: false,
                ..configuration()
            },
            requires_user_verification(),
        );
        assert_eq!(error, AuthenticatorError::NotAllowed);

        let error = make(
            AuthenticatorConfiguration {
                is_user_consenting: false,
                ..configuration()
            },
            make_credential_parameters(),
        );
        assert_eq!(error, AuthenticatorError::NotAllowed);
    }

    #[test]
    fn make_credential_refuses_a_credential_of_the_exclude_list() {
        let credentials = [
            credential(&[5; 4], RP_ID, false),
            credential(&[6; 4], b"other.example", false),
        ];
        let make = |configuration: AuthenticatorConfiguration, excluded: &[u8]| {
            let exclude_credential_ids = [excluded];
            make_credential(
                &configuration,
                &credentials,
                &MakeCredentialParameters {
                    exclude_credential_ids: &exclude_credential_ids,
                    ..make_credential_parameters()
                },
            )
            .map(|_| ())
        };

        assert_eq!(make(configuration(), &[5; 4]), Err(AuthenticatorError::InvalidState));
        let without_consent = AuthenticatorConfiguration {
            is_user_consenting: false,
            ..configuration()
        };
        assert_eq!(make(without_consent, &[5; 4]), Err(AuthenticatorError::NotAllowed));
        // NB: The credential with this ID is one of another Relying Party.
        assert_eq!(make(configuration(), &[6; 4]), Ok(()));
    }

    #[test]
    fn a_discoverable_credential_takes_the_place_of_the_one_for_the_same_user() {
        let mut other_user = credential(&[7; 4], RP_ID, true);
        other_user.user_handle = Some(&[9]);
        let credentials = [
            credential(&[5; 4], RP_ID, true),
            credential(&[6; 4], RP_ID, false),
            other_user,
            credential(&[8; 4], b"other.example", true),
            credential(&[9; 4], RP_ID, true),
        ];
        let make = |require_resident_key| {
            let parameters = MakeCredentialParameters {
                require_resident_key,
                ..make_credential_parameters()
            };
            make_credential(&configuration(), &credentials, &parameters)
                .unwrap()
                .displaced_credentials
        };

        assert_eq!(make(true), [0, 4]);
        assert!(make(false).is_empty());
    }

    #[test]
    fn the_credential_options_are_those_that_the_request_can_use() {
        let credentials = [
            credential(&[5; 4], RP_ID, true),
            credential(&[6; 4], RP_ID, false),
            credential(&[7; 4], b"other.example", true),
            credential(&[8; 4], RP_ID, true),
        ];
        let options = |allow_credential_ids: &[&[u8]], is_conditional| {
            credential_options(
                &credentials,
                &GetAssertionParameters {
                    allow_credential_ids,
                    is_conditional,
                    ..get_assertion_parameters()
                },
            )
        };

        // NB: Without a list, only the discoverable credentials of the Relying Party.
        assert_eq!(options(&[], false), [0, 3]);
        // NB: With a list, what it names, each one time, and also when not discoverable.
        assert_eq!(options(&[&[6; 4], &[5; 4], &[6; 4], &[7; 4], &[1; 4]], false), [1, 0]);
        // NB: With conditional mediation, the list only narrows the discoverable credentials down.
        assert_eq!(options(&[], true), [0, 3]);
        assert_eq!(options(&[&[6; 4], &[8; 4]], true), [3]);
    }

    #[test]
    fn gets_an_assertion_that_the_public_key_verifies() {
        let new_credential = make_credential(&configuration(), &[], &make_credential_parameters()).unwrap();
        let mut stored = credential(&new_credential.id, RP_ID, true);
        stored.private_key = &new_credential.private_key;
        stored.sign_count = Some(41);
        stored.backup_eligibility = true;
        let credentials = [credential(&[5; 4], RP_ID, true), stored];

        let assertion = get_assertion(&configuration(), &credentials, &get_assertion_parameters()).unwrap();
        // NB: The authenticator selects the credential that it got last.
        assert_eq!(assertion.selected_credential, 1);
        assert_eq!(assertion.sign_count, Some(42));
        let data = &assertion.authenticator_data;
        assert_eq!(data.len(), 37);
        assert_eq!(data[..32], sha_digest(ShaHashKind::Sha256, RP_ID).unwrap());
        // NB: The flags UP, UV and BE.
        assert_eq!(data[32], 0x0d);
        assert_eq!(data[33..], [0, 0, 0, 42]);

        let (public_key_info, _) = parse_subject_public_key_info(&new_credential.public_key).unwrap();
        let KeyMaterial::Key(public_key) = public_key_info.into_material() else {
            panic!("no public key");
        };
        let digest = sha_digest(ShaHashKind::Sha256, &[&data[..], CLIENT_DATA_HASH].concat()).unwrap();
        assert!(ec_verify(&public_key, &digest, &raw_signature(&assertion.signature)).is_ok_and(|is_valid| is_valid));
    }

    #[test]
    fn get_assertion_refuses_what_the_user_did_not_agree_to() {
        let new_credential = make_credential(&configuration(), &[], &make_credential_parameters()).unwrap();
        let mut stored = credential(&new_credential.id, RP_ID, true);
        stored.private_key = &new_credential.private_key;
        let credentials = [stored];
        let get = |configuration: AuthenticatorConfiguration, parameters: GetAssertionParameters| {
            get_assertion(&configuration, &credentials, &parameters).map(|assertion| assertion.sign_count)
        };

        let other_relying_party = GetAssertionParameters {
            rp_id: b"other.example",
            ..get_assertion_parameters()
        };
        assert_eq!(
            get(configuration(), other_relying_party),
            Err(AuthenticatorError::NotAllowed)
        );

        let without_consent = AuthenticatorConfiguration {
            is_user_consenting: false,
            ..configuration()
        };
        assert_eq!(
            get(without_consent, get_assertion_parameters()),
            Err(AuthenticatorError::NotAllowed)
        );

        let without_user_verification = AuthenticatorConfiguration {
            is_user_verified: false,
            has_signature_counter: false,
            ..configuration()
        };
        let requires_user_verification = GetAssertionParameters {
            require_user_verification: true,
            ..get_assertion_parameters()
        };
        assert_eq!(
            get(without_user_verification, requires_user_verification),
            Err(AuthenticatorError::NotAllowed)
        );
        // NB: An authenticator without a signature counter leaves it at zero.
        assert_eq!(get(without_user_verification, get_assertion_parameters()), Ok(Some(0)));
    }

    #[test]
    fn a_credential_without_a_signature_counter_gets_none() {
        let new_credential = make_credential(&configuration(), &[], &make_credential_parameters()).unwrap();
        let mut stored = credential(&new_credential.id, RP_ID, true);
        stored.private_key = &new_credential.private_key;
        stored.sign_count = None;
        let credentials = [stored];

        let assertion = get_assertion(&configuration(), &credentials, &get_assertion_parameters()).unwrap();
        assert_eq!(assertion.sign_count, None);
        // NB: The signature counter of the authenticator data comes after the hash of the RP ID and the flags.
        assert_eq!(assertion.authenticator_data[33..37], [0; 4]);
    }

    #[test]
    fn a_key_that_is_no_p256_private_key_gives_no_assertion() {
        let mut stored = credential(&[5; 4], RP_ID, true);
        stored.private_key = &[0x30, 0x03, 0x02, 0x01, 0x00];
        assert!(!is_credential_private_key(stored.private_key));
        assert_eq!(
            get_assertion(&configuration(), &[stored], &get_assertion_parameters()).map(|_| ()),
            Err(AuthenticatorError::NotAllowed)
        );
    }

    #[test]
    fn der_signatures_have_the_shortest_integers() {
        let mut signature = [0; 64];
        signature[31] = 0x7f;
        signature[32] = 0x80;
        let der = der_signature(&signature);
        assert_eq!(der[..5], [0x30, 0x26, 0x02, 0x01, 0x7f]);
        assert_eq!(der[5..8], [0x02, 0x21, 0x00]);
        assert_eq!(raw_signature(&der), signature);
    }
}
