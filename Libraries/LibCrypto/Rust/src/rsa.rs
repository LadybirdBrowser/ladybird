/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! RSA keys.

use core::ffi::{CStr, c_int};

use crate::key::{HandleTag, Key, KeyDecodeError, KeyHandle, KeyPair, SecretBytes, StoredKey};
use crate::openssl::{
    self, BigNum, Param, ParamBuilder, Pkey, end_parameter, int_parameter, octet_string_parameter, utf8_parameter,
};
use crate::{CryptoError, KeyAlgorithm, RsaJwkInteger, ShaHashKind, der, sys};

pub(crate) struct RsaPublicKey {
    key: Pkey,
}

/// A private key of only n, e and d has no CRT components.
pub(crate) struct RsaPrivateKey {
    key: Pkey,
    has_crt_components: bool,
}

impl RsaJwkInteger {
    /// The name of the key's parameter that holds the integer, as OpenSSL takes it.
    fn name(&self) -> &'static CStr {
        match self {
            Self::Modulus => c"n",
            Self::PublicExponent => c"e",
            Self::PrivateExponent => c"d",
            Self::Prime1 => c"rsa-factor1",
            Self::Prime2 => c"rsa-factor2",
            Self::Exponent1 => c"rsa-exponent1",
            Self::Exponent2 => c"rsa-exponent2",
            Self::Coefficient => c"rsa-coefficient1",
        }
    }
}

/// The integer `integer` of `key`, without leading zeros, or nothing if the key does not have it.
fn integer(key: &Pkey, integer: RsaJwkInteger) -> Result<SecretBytes, CryptoError> {
    match key.number(integer.name()) {
        Some(number) => number.to_bytes(),
        None => Ok(SecretBytes::new(Vec::new())),
    }
}

// https://www.rfc-editor.org/rfc/rfc8017#section-3.1
// [...] the RSA public exponent e is an integer between 3 and n - 1 [...]
// NB: An odd e above 1 is at least 3.
fn public_exponent_is_valid(key: &Pkey) -> bool {
    let (Some(n), Some(e)) = (key.number(c"n"), key.number(c"e")) else {
        return false;
    };
    e.is_odd() && e.num_bits() > 1 && e.is_less_than(&n)
}

impl RsaPublicKey {
    fn from_components(modulus: &[u8], public_exponent: &[u8]) -> Result<Self, CryptoError> {
        let mut builder = ParamBuilder::new()?;
        builder.push_number(c"n", modulus, false)?;
        builder.push_number(c"e", public_exponent, false)?;
        let params = builder.build()?;
        Ok(Self {
            key: Pkey::from_data(c"RSA", sys::EVP_PKEY_PUBLIC_KEY, &params)?,
        })
    }

    fn is_valid(&self) -> Result<bool, CryptoError> {
        if !public_exponent_is_valid(&self.key) {
            return Ok(false);
        }
        openssl::check_key(&self.key, false, false)
    }
}

impl RsaPrivateKey {
    fn from_components(
        modulus: &[u8],
        public_exponent: &[u8],
        private_exponent: &[u8],
        crt_components: Option<[&[u8]; 5]>,
    ) -> Result<Self, CryptoError> {
        let mut builder = ParamBuilder::new()?;
        builder.push_number(c"n", modulus, false)?;
        builder.push_number(c"e", public_exponent, false)?;
        builder.push_number(c"d", private_exponent, true)?;
        if let Some(crt_components) = crt_components {
            let names = [
                RsaJwkInteger::Prime1,
                RsaJwkInteger::Prime2,
                RsaJwkInteger::Exponent1,
                RsaJwkInteger::Exponent2,
                RsaJwkInteger::Coefficient,
            ];
            for (integer, bytes) in names.into_iter().zip(crt_components) {
                builder.push_number(integer.name(), bytes, true)?;
            }
        }
        let params = builder.build()?;
        Ok(Self {
            key: Pkey::from_data(c"RSA", sys::EVP_PKEY_KEYPAIR, &params)?,
            has_crt_components: crt_components.is_some(),
        })
    }

    fn from_key(key: Pkey) -> Self {
        let has_crt_components = key.number(RsaJwkInteger::Prime1.name()).is_some();
        Self {
            key,
            has_crt_components,
        }
    }

    fn component(&self, component: RsaJwkInteger) -> Result<SecretBytes, CryptoError> {
        let is_crt_component = !matches!(
            component,
            RsaJwkInteger::Modulus | RsaJwkInteger::PublicExponent | RsaJwkInteger::PrivateExponent
        );
        if is_crt_component && !self.has_crt_components {
            return Ok(SecretBytes::new(Vec::new()));
        }
        integer(&self.key, component)
    }

    fn public_key(&self) -> Result<RsaPublicKey, CryptoError> {
        RsaPublicKey::from_components(
            &integer(&self.key, RsaJwkInteger::Modulus)?,
            &integer(&self.key, RsaJwkInteger::PublicExponent)?,
        )
    }

    fn is_valid(&self) -> Result<bool, CryptoError> {
        if !public_exponent_is_valid(&self.key) {
            return Ok(false);
        }
        // NB: The pairwise check needs the prime factors, which a key of only n, e and d does not have.
        openssl::check_key(&self.key, true, self.has_crt_components)
    }
}

fn public_key(key: &KeyHandle) -> &RsaPublicKey {
    match &key.0 {
        Key::RsaPublic(public_key) => public_key,
        _ => panic!("The key is not an RSA public key"),
    }
}

fn private_key(key: &KeyHandle) -> &RsaPrivateKey {
    match &key.0 {
        Key::RsaPrivate(private_key) => private_key,
        _ => panic!("The key is not an RSA private key"),
    }
}

pub fn rsa_key_is_valid(key: &KeyHandle) -> Result<bool, CryptoError> {
    match &key.0 {
        Key::RsaPublic(public_key) => public_key.is_valid(),
        Key::RsaPrivate(private_key) => private_key.is_valid(),
        _ => panic!("The key is not an RSA key"),
    }
}

pub fn rsa_validate_modulus_length(modulus_length: u32) -> bool {
    // OpenSSL refuses to generate keys with shorter moduli, and OPENSSL_RSA_MAX_MODULUS_BITS caps them.
    (512..=16384).contains(&modulus_length)
}

pub fn rsa_generate_key_pair(modulus_length: u32, exponent: &[u8]) -> Result<KeyPair, CryptoError> {
    if !rsa_validate_modulus_length(modulus_length) {
        return Err(CryptoError::Backend);
    }
    let mut builder = ParamBuilder::new()?;
    builder.push_uint(c"bits", modulus_length)?;
    builder.push_number(c"e", exponent, false)?;
    let params = builder.build()?;
    let private_key = RsaPrivateKey::from_key(Pkey::generate(c"RSA", &params)?);
    let public_key = private_key.public_key()?;
    Ok(KeyPair {
        public_key: KeyHandle(Key::RsaPublic(public_key)),
        private_key: KeyHandle(Key::RsaPrivate(private_key)),
    })
}

pub fn rsa_read_key_metadata(key: &KeyHandle) -> Result<(u32, Vec<u8>), CryptoError> {
    let key = match &key.0 {
        Key::RsaPublic(public_key) => &public_key.key,
        Key::RsaPrivate(private_key) => &private_key.key,
        _ => panic!("The key is not an RSA key"),
    };
    Ok((key.bits(), integer(key, RsaJwkInteger::PublicExponent)?.into_vec()))
}

pub fn rsa_key_from_components(is_private: bool, components: &[&[u8]]) -> Option<KeyHandle> {
    if !is_private {
        let [modulus, public_exponent] = components else {
            return None;
        };
        return Some(KeyHandle(Key::RsaPublic(
            RsaPublicKey::from_components(modulus, public_exponent).ok()?,
        )));
    }
    let crt_components = match components {
        [_, _, _] => None,
        // NB: A JWK of a key without CRT components may carry them empty.
        [_, _, _, crt @ ..] if crt.len() == 5 => crt
            .iter()
            .any(|component| !component.is_empty())
            .then(|| [crt[0], crt[1], crt[2], crt[3], crt[4]]),
        _ => return None,
    };
    let private_key =
        RsaPrivateKey::from_components(components[0], components[1], components[2], crt_components).ok()?;
    Some(KeyHandle(Key::RsaPrivate(private_key)))
}

// NB: OpenSSL writes a SubjectPublicKeyInfo of rsaEncryption with NULL parameters.
pub fn rsa_export_der(key: &KeyHandle) -> Result<Vec<u8>, CryptoError> {
    let private_key = match &key.0 {
        Key::RsaPublic(public_key) => return public_key.key.public_key_info(),
        Key::RsaPrivate(private_key) => private_key,
        _ => panic!("The key is not an RSA key"),
    };
    if !private_key.has_crt_components {
        return Err(CryptoError::Backend);
    }
    let integers = [
        RsaJwkInteger::Modulus,
        RsaJwkInteger::PublicExponent,
        RsaJwkInteger::PrivateExponent,
        RsaJwkInteger::Prime1,
        RsaJwkInteger::Prime2,
        RsaJwkInteger::Exponent1,
        RsaJwkInteger::Exponent2,
        RsaJwkInteger::Coefficient,
    ]
    .map(|integer| self::integer(&private_key.key, integer));
    let integers = integers.into_iter().collect::<Result<Vec<_>, _>>()?;
    let integers: Vec<&[u8]> = integers.iter().map(|integer| &**integer).collect();
    let encoded = SecretBytes::new(der::rsa_private_key(&integers));
    // https://www.rfc-editor.org/rfc/rfc3279#section-2.3.1
    // The parameters field MUST have ASN.1 type NULL for this algorithm identifier.
    Ok(der::private_key_info(
        KeyAlgorithm::Rsa.oid(),
        Some(&der::NULL),
        &encoded,
    ))
}

/// The integer of an RSA key, or none for a CRT value of a private key that has none.
pub fn rsa_export_jwk_integer(key: &KeyHandle, component: RsaJwkInteger) -> Result<Option<Vec<u8>>, CryptoError> {
    let (key, has_crt_components) = match &key.0 {
        Key::RsaPublic(public_key) => {
            assert!(
                matches!(component, RsaJwkInteger::Modulus | RsaJwkInteger::PublicExponent),
                "A public key has only n and e"
            );
            (&public_key.key, false)
        }
        Key::RsaPrivate(private_key) => (&private_key.key, private_key.has_crt_components),
        _ => panic!("The key is not an RSA key"),
    };
    let is_crt_component = !matches!(
        component,
        RsaJwkInteger::Modulus | RsaJwkInteger::PublicExponent | RsaJwkInteger::PrivateExponent
    );
    if is_crt_component && !has_crt_components {
        return Ok(None);
    }
    Ok(Some(integer(key, component)?.into_vec()))
}

pub fn rsa_oaep_cipher(
    encrypt: bool,
    hash_kind: ShaHashKind,
    key: &KeyHandle,
    label: &[u8],
    input: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    let mut params = vec![
        utf8_parameter(c"pad-mode", c"oaep"),
        utf8_parameter(c"digest", hash_kind.name()),
        utf8_parameter(c"mgf1-digest", hash_kind.name()),
    ];
    // NB: OpenSSL copies the label.
    if !label.is_empty() {
        params.push(octet_string_parameter(c"oaep-label", label));
    }
    params.push(end_parameter());
    if encrypt {
        return openssl::encrypt(&public_key(key).key, &params, input);
    }

    let key = &private_key(key).key;
    if input.len() != key.size() {
        return Err(CryptoError::Backend);
    }
    openssl::decrypt(key, &params, input)
}

/// The padding of an RSA signature.
#[derive(Clone, Copy)]
pub enum RsaSignaturePadding {
    Pkcs1,
    Pss { salt_length: u32 },
}

impl RsaSignaturePadding {
    // NB: OpenSSL takes the salt length as an int and gives negative values special meanings, such as salt length
    //     auto-detection, so a length that does not fit in an int must never reach it. No RSA key is large enough for
    //     such a salt.
    fn pss_salt_length(self) -> Result<Option<c_int>, ()> {
        match self {
            Self::Pkcs1 => Ok(None),
            Self::Pss { salt_length } => c_int::try_from(salt_length).map(Some).map_err(|_| ()),
        }
    }
}

/// The signature parameters of the padding, which point at `salt_length`.
fn signature_parameters(hash_kind: ShaHashKind, salt_length: Option<&mut c_int>) -> Vec<Param<'_>> {
    match salt_length {
        Some(salt_length) => vec![
            utf8_parameter(c"pad-mode", c"pss"),
            utf8_parameter(c"mgf1-digest", hash_kind.name()),
            int_parameter(c"saltlen", salt_length),
            end_parameter(),
        ],
        None => vec![utf8_parameter(c"pad-mode", c"pkcs1"), end_parameter()],
    }
}

pub fn rsa_sign(
    hash_kind: ShaHashKind,
    key: &KeyHandle,
    padding: RsaSignaturePadding,
    message: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    let mut salt_length = padding.pss_salt_length().map_err(|_| CryptoError::Backend)?;
    let params = signature_parameters(hash_kind, salt_length.as_mut());
    openssl::digest_sign(&private_key(key).key, Some(hash_kind.name()), &params, message)
}

pub fn rsa_verify(
    hash_kind: ShaHashKind,
    key: &KeyHandle,
    padding: RsaSignaturePadding,
    message: &[u8],
    signature: &[u8],
) -> Result<bool, CryptoError> {
    let Ok(mut salt_length) = padding.pss_salt_length() else {
        return Ok(false);
    };
    let public_key = public_key(key);
    if signature.len() != public_key.key.size() {
        return Ok(false);
    }
    // NB: Keys are checked in full when they are imported, so this only keeps the rule on e, which is cheap.
    if !public_exponent_is_valid(&public_key.key) {
        return Ok(false);
    }
    let params = signature_parameters(hash_kind, salt_length.as_mut());
    openssl::digest_verify(&public_key.key, Some(hash_kind.name()), &params, message, signature)
}

/// Whether the big-endian `exponent` is at least 3, is odd, and is less than 2^modulus_length - 1.
pub fn rsa_validate_public_exponent(exponent: &[u8], modulus_length: u32) -> bool {
    let Ok(exponent) = BigNum::from_bytes(exponent, false) else {
        return false;
    };
    if !exponent.is_odd() || exponent.num_bits() < 2 {
        return false;
    }
    // NB: exponent < 2^modulus_length - 1 exactly when exponent + 1 has at most modulus_length bits.
    exponent
        .incremented()
        .is_ok_and(|incremented| u32::try_from(incremented.num_bits()).is_ok_and(|bits| bits <= modulus_length))
}

/// The big-endian `exponent` with no leading zero bytes.
pub fn rsa_export_public_exponent(exponent: &[u8]) -> Vec<u8> {
    let leading_zeros = exponent.iter().take_while(|&&byte| byte == 0).count();
    exponent[leading_zeros..].to_vec()
}

pub(crate) fn public_key_for_private_key(key: &KeyHandle) -> Option<KeyHandle> {
    let Key::RsaPrivate(private_key) = &key.0 else {
        return None;
    };
    Some(KeyHandle(Key::RsaPublic(private_key.public_key().ok()?)))
}

pub(crate) fn from_public_key(key: Pkey) -> KeyHandle {
    KeyHandle(Key::RsaPublic(RsaPublicKey { key }))
}

pub(crate) fn from_private_key(key: Pkey) -> KeyHandle {
    KeyHandle(Key::RsaPrivate(RsaPrivateKey::from_key(key)))
}

impl RsaPublicKey {
    /// The stored form of the key: n and e.
    pub(crate) fn stored(&self) -> StoredKey {
        let integer = |component| integer(&self.key, component).expect("OpenSSL gives an RSA key's components");
        StoredKey::new(
            HandleTag::RsaPublic,
            vec![integer(RsaJwkInteger::Modulus), integer(RsaJwkInteger::PublicExponent)],
        )
    }
}

impl RsaPrivateKey {
    /// The stored form of the key: n, d, e and then its CRT components, which a key without them stores empty.
    pub(crate) fn stored(&self) -> StoredKey {
        let buffers = [
            RsaJwkInteger::Modulus,
            RsaJwkInteger::PrivateExponent,
            RsaJwkInteger::PublicExponent,
            RsaJwkInteger::Prime1,
            RsaJwkInteger::Prime2,
            RsaJwkInteger::Exponent1,
            RsaJwkInteger::Exponent2,
            RsaJwkInteger::Coefficient,
        ]
        .into_iter()
        .map(|integer| self.component(integer).expect("OpenSSL gives an RSA key's components"))
        .collect();
        StoredKey::new(HandleTag::RsaPrivate, buffers)
    }
}

/// The key that a stored handle of an RSA tag holds.
pub(crate) fn decode_stored(tag: HandleTag, buffers: &[&[u8]]) -> Result<KeyHandle, KeyDecodeError> {
    let invalid = |_| KeyDecodeError::InvalidComponents;
    match tag {
        HandleTag::RsaPublic => Ok(KeyHandle(Key::RsaPublic(
            RsaPublicKey::from_components(buffers[0], buffers[1]).map_err(invalid)?,
        ))),
        HandleTag::RsaPrivate => {
            // NB: A key without CRT components stores them empty.
            let crt_components =
                (!buffers[3].is_empty()).then(|| [buffers[3], buffers[4], buffers[5], buffers[6], buffers[7]]);
            let private_key =
                RsaPrivateKey::from_components(buffers[0], buffers[2], buffers[1], crt_components).map_err(invalid)?;
            Ok(KeyHandle(Key::RsaPrivate(private_key)))
        }
        _ => Err(KeyDecodeError::InvalidComponents),
    }
}
