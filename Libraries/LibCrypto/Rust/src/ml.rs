/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ML-DSA and ML-KEM keys.

use core::ffi::CStr;

use crate::der;
use crate::key::{HandleTag, Key, KeyDecodeError, KeyHandle, KeyPair, SecretBytes, StoredKey};
use crate::openssl::{self, Pkey};
use crate::{CryptoError, KeyAlgorithm, MlDsaSize, MlKemSize, sys};

pub(crate) struct MlDsaPublicKey {
    key: Pkey,
    bytes: Vec<u8>,
    size: MlDsaSize,
}

/// Any bytes make an ML-KEM public key. Web Crypto only runs the encapsulation key check of FIPS 203, section 7.2, when
/// the key is used, so a key that fails it has no OpenSSL key or parameter set.
pub(crate) struct MlKemPublicKey {
    key: Option<Pkey>,
    bytes: Vec<u8>,
    size: Option<MlKemSize>,
}

/// An ML-DSA or ML-KEM private key, which is made from its seed.
pub(crate) struct MlPrivateKey {
    key: Pkey,
    public_key: Vec<u8>,
    parameter_set: ParameterSet,
}

/// An ML-DSA or ML-KEM parameter set.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ParameterSet {
    MlDsa(MlDsaSize),
    MlKem(MlKemSize),
}

const SEED: &CStr = c"seed";

impl MlDsaSize {
    const ALL: [Self; 3] = [Self::MlDsa44, Self::MlDsa65, Self::MlDsa87];

    fn public_key_size(self) -> usize {
        match self {
            Self::MlDsa44 => 1312,
            Self::MlDsa65 => 1952,
            Self::MlDsa87 => 2592,
        }
    }
}

impl MlKemSize {
    const ALL: [Self; 3] = [Self::MlKem512, Self::MlKem768, Self::MlKem1024];

    fn public_key_size(self) -> usize {
        match self {
            Self::MlKem512 => 800,
            Self::MlKem768 => 1184,
            Self::MlKem1024 => 1568,
        }
    }
}

impl ParameterSet {
    fn name(self) -> &'static CStr {
        match self {
            Self::MlDsa(MlDsaSize::MlDsa44) => c"ML-DSA-44",
            Self::MlDsa(MlDsaSize::MlDsa65) => c"ML-DSA-65",
            Self::MlDsa(MlDsaSize::MlDsa87) => c"ML-DSA-87",
            Self::MlKem(MlKemSize::MlKem512) => c"ML-KEM-512",
            Self::MlKem(MlKemSize::MlKem768) => c"ML-KEM-768",
            Self::MlKem(MlKemSize::MlKem1024) => c"ML-KEM-1024",
        }
    }

    fn seed_size(self) -> usize {
        match self {
            Self::MlDsa(_) => 32,
            Self::MlKem(_) => 64,
        }
    }

    fn expanded_key_size(self) -> usize {
        match self {
            Self::MlDsa(MlDsaSize::MlDsa44) => 2560,
            Self::MlDsa(MlDsaSize::MlDsa65) => 4032,
            Self::MlDsa(MlDsaSize::MlDsa87) => 4896,
            Self::MlKem(MlKemSize::MlKem512) => 1632,
            Self::MlKem(MlKemSize::MlKem768) => 2400,
            Self::MlKem(MlKemSize::MlKem1024) => 3168,
        }
    }
}

fn mldsa_public_key_from_bytes(bytes: &[u8]) -> Option<MlDsaPublicKey> {
    let size = MlDsaSize::ALL
        .into_iter()
        .find(|size| size.public_key_size() == bytes.len())?;
    let key = Pkey::from_raw_public_key(ParameterSet::MlDsa(size).name(), bytes).ok()?;
    Some(MlDsaPublicKey {
        key,
        bytes: bytes.to_vec(),
        size,
    })
}

fn mlkem_public_key_from_bytes(bytes: &[u8]) -> MlKemPublicKey {
    // NB: OpenSSL builds a key only from bytes of a parameter set's length whose coefficients are all below q, which
    //     are the two parts of the encapsulation key check.
    let checked = MlKemSize::ALL
        .into_iter()
        .find(|size| size.public_key_size() == bytes.len())
        .and_then(|size| {
            Pkey::from_raw_public_key(ParameterSet::MlKem(size).name(), bytes)
                .ok()
                .map(|key| (key, size))
        });
    let (key, size) = checked.map_or((None, None), |(key, size)| (Some(key), Some(size)));
    MlKemPublicKey {
        key,
        bytes: bytes.to_vec(),
        size,
    }
}

impl MlPrivateKey {
    /// The key of `parameter_set` that `seed` makes, or a new key for an empty seed.
    fn from_seed(parameter_set: ParameterSet, seed: &[u8]) -> Result<Self, CryptoError> {
        let key = if seed.is_empty() {
            let mut seed = SecretBytes::new(vec![0; parameter_set.seed_size()]);
            openssl::fill_with_random(&mut seed)?;
            private_key_from_seed(parameter_set, &seed)?
        } else {
            private_key_from_seed(parameter_set, seed)?
        };
        let public_key = key.raw_public_key()?;
        Ok(Self {
            key,
            public_key,
            parameter_set,
        })
    }

    fn seed(&self) -> Result<SecretBytes, CryptoError> {
        self.key.octet_string(SEED)
    }

    fn expanded_key(&self) -> Result<SecretBytes, CryptoError> {
        self.key.raw_private_key()
    }

    fn public(&self) -> Option<Key> {
        match self.parameter_set {
            ParameterSet::MlDsa(_) => mldsa_public_key_from_bytes(&self.public_key).map(Key::MlDsaPublic),
            ParameterSet::MlKem(_) => Some(Key::MlKemPublic(mlkem_public_key_from_bytes(&self.public_key))),
        }
    }
}

/// The private key of `parameter_set` that `seed` makes.
// NB: The seed parameter points at the caller's buffer, and the key clears its own copy.
fn private_key_from_seed(parameter_set: ParameterSet, seed: &[u8]) -> Result<Pkey, CryptoError> {
    let mut params = [openssl::octet_string_parameter(c"seed", seed), openssl::end_parameter()];
    Pkey::from_data(parameter_set.name(), sys::EVP_PKEY_KEYPAIR, params.as_mut_ptr())
}

fn private_key(key: &KeyHandle) -> &MlPrivateKey {
    match &key.0 {
        Key::MlPrivate(private_key) => private_key,
        _ => panic!("The key is not an ML-DSA or ML-KEM private key"),
    }
}

fn generate_key_pair(parameter_set: ParameterSet, seed: &[u8]) -> Result<KeyPair, CryptoError> {
    let private_key = MlPrivateKey::from_seed(parameter_set, seed)?;
    let public_key = private_key
        .public()
        .ok_or(CryptoError::Invalid("Invalid ML-DSA key length"))?;
    Ok(KeyPair {
        public_key: KeyHandle(public_key),
        private_key: KeyHandle(Key::MlPrivate(private_key)),
    })
}

pub fn mldsa_generate_key_pair(size: MlDsaSize, seed: &[u8]) -> Result<KeyPair, CryptoError> {
    generate_key_pair(ParameterSet::MlDsa(size), seed)
}

pub fn mlkem_generate_key_pair(size: MlKemSize, seed: &[u8]) -> Result<KeyPair, CryptoError> {
    generate_key_pair(ParameterSet::MlKem(size), seed)
}

pub fn mldsa_read_public_key(size: MlDsaSize, bytes: &[u8]) -> Option<KeyHandle> {
    let public_key = mldsa_public_key_from_bytes(bytes)?;
    (public_key.size == size).then(|| KeyHandle(Key::MlDsaPublic(public_key)))
}

pub fn mlkem_read_public_key(bytes: &[u8]) -> Option<KeyHandle> {
    Some(KeyHandle(Key::MlKemPublic(mlkem_public_key_from_bytes(bytes))))
}

pub fn mldsa_public_bytes(key: &KeyHandle) -> &[u8] {
    match &key.0 {
        Key::MlDsaPublic(public_key) => &public_key.bytes,
        Key::MlPrivate(private_key) => &private_key.public_key,
        _ => panic!("The key is not an ML-DSA key"),
    }
}

pub fn mlkem_public_bytes(key: &KeyHandle) -> Vec<u8> {
    match &key.0 {
        Key::MlKemPublic(public_key) => public_key.bytes.clone(),
        _ => panic!("The key is not an ML-KEM public key"),
    }
}

pub fn ml_seed_bytes(key: &KeyHandle) -> Result<Vec<u8>, CryptoError> {
    Ok(private_key(key).seed()?.into_vec())
}

/// The SubjectPublicKeyInfo or PrivateKeyInfo of an ML-DSA or ML-KEM key of `algorithm`. A private key's privateKey is
/// in the seed-only form.
pub fn ml_export_der(key: &KeyHandle, is_public: bool, algorithm: KeyAlgorithm) -> Result<Vec<u8>, CryptoError> {
    let oid = algorithm.oid();
    match &key.0 {
        Key::MlDsaPublic(public_key) if is_public => Ok(der::subject_public_key_info(oid, None, &public_key.bytes)),
        Key::MlKemPublic(public_key) if is_public => Ok(der::subject_public_key_info(oid, None, &public_key.bytes)),
        Key::MlPrivate(private_key) if !is_public => {
            let seed = private_key.seed()?;
            let seed_header = der::header(0x80, seed.len());
            let mut encoded = SecretBytes::new(Vec::with_capacity(seed_header.len() + seed.len()));
            encoded.extend_from_slice(&seed_header);
            encoded.extend_from_slice(&seed);
            Ok(der::private_key_info(oid, None, &encoded))
        }
        _ => Err(CryptoError::Backend),
    }
}

pub fn mldsa_sign(size: MlDsaSize, key: &KeyHandle, message: &[u8], context: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let Key::MlPrivate(private_key) = &key.0 else {
        return Err(CryptoError::Backend);
    };
    let parameter_set = ParameterSet::MlDsa(size);
    if private_key.parameter_set != parameter_set {
        return Err(CryptoError::Backend);
    }
    openssl::sign_message(&private_key.key, parameter_set.name(), message, context)
}

pub fn mldsa_verify(
    size: MlDsaSize,
    key: &KeyHandle,
    message: &[u8],
    signature: &[u8],
    context: &[u8],
) -> Result<bool, CryptoError> {
    let Key::MlDsaPublic(public_key) = &key.0 else {
        return Err(CryptoError::Backend);
    };
    if public_key.size != size {
        return Err(CryptoError::Backend);
    }
    openssl::verify_message(
        &public_key.key,
        ParameterSet::MlDsa(size).name(),
        message,
        signature,
        context,
    )
}

pub fn mlkem_encapsulate(size: MlKemSize, key: &KeyHandle) -> Result<(Vec<u8>, Vec<u8>), CryptoError> {
    let Key::MlKemPublic(public_key) = &key.0 else {
        return Err(CryptoError::Invalid("The key is not an ML-KEM public key"));
    };
    let (Some(openssl_key), Some(key_size)) = (&public_key.key, public_key.size) else {
        return Err(CryptoError::Invalid("The key fails the encapsulation key check"));
    };
    if key_size != size {
        return Err(CryptoError::Invalid("The key fails the encapsulation key check"));
    }
    let (shared_key, ciphertext) = openssl::encapsulate(openssl_key)?;
    Ok((shared_key.into_vec(), ciphertext))
}

pub fn mlkem_decapsulate(size: MlKemSize, key: &KeyHandle, ciphertext: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let Key::MlPrivate(private_key) = &key.0 else {
        return Err(CryptoError::Invalid("The key is not an ML-KEM private key"));
    };
    if private_key.parameter_set != ParameterSet::MlKem(size) {
        return Err(CryptoError::Invalid("The key is for a different ML-KEM parameter set"));
    }
    Ok(openssl::decapsulate(&private_key.key, ciphertext)?.into_vec())
}

pub(crate) fn public_key_for_private_key(key: &KeyHandle) -> Option<KeyHandle> {
    let Key::MlPrivate(private_key) = &key.0 else {
        return None;
    };
    private_key.public().map(KeyHandle)
}

struct ModuleLatticePrivateKey<'a> {
    seed: Option<&'a [u8]>,
    expanded_key: Option<&'a [u8]>,
}

// https://datatracker.ietf.org/doc/html/draft-ietf-lamps-kyber-certificates-11#autoid-7
// https://www.rfc-editor.org/rfc/rfc9881.html#section-6
fn parse_module_lattice_private_key(
    der: &[u8],
    seed_size: usize,
    expanded_key_size: usize,
) -> Result<ModuleLatticePrivateKey<'_>, CryptoError> {
    // PrivateKey ::= CHOICE {
    //      seed [0] IMPLICIT OCTET STRING (SIZE (seed_size)),
    //      expandedKey OCTET STRING (SIZE (expanded_key_size)),
    //      both SEQUENCE {
    //           seed OCTET STRING (SIZE (seed_size)),
    //           expandedKey OCTET STRING (SIZE (expanded_key_size))
    //      }
    // }
    // NB: The sizes are fixed, so each choice has a single DER layout, and the input is a choice when its headers are
    //     those of that layout.
    let seed_header = der::header(0x80, seed_size);
    if der.len() == seed_header.len() + seed_size
        && let Some(seed) = der.strip_prefix(seed_header.as_slice())
    {
        return Ok(ModuleLatticePrivateKey {
            seed: Some(seed),
            expanded_key: None,
        });
    }

    let expanded_key_header = der::header(der::OCTET_STRING, expanded_key_size);
    if der.len() == expanded_key_header.len() + expanded_key_size
        && let Some(expanded_key) = der.strip_prefix(expanded_key_header.as_slice())
    {
        return Ok(ModuleLatticePrivateKey {
            seed: None,
            expanded_key: Some(expanded_key),
        });
    }

    let both_seed_header = der::header(der::OCTET_STRING, seed_size);
    let contents_length = both_seed_header.len() + seed_size + expanded_key_header.len() + expanded_key_size;
    let sequence_header = der::header(der::SEQUENCE, contents_length);
    if der.len() == sequence_header.len() + contents_length
        && let Some(rest) = der.strip_prefix(sequence_header.as_slice())
        && let Some(rest) = rest.strip_prefix(both_seed_header.as_slice())
    {
        let (seed, rest) = rest.split_at(seed_size);
        if let Some(expanded_key) = rest.strip_prefix(expanded_key_header.as_slice()) {
            return Ok(ModuleLatticePrivateKey {
                seed: Some(seed),
                expanded_key: Some(expanded_key),
            });
        }
    }
    Err(CryptoError::Invalid("Invalid module-lattice private key"))
}

/// The private key in the privateKey of a PKCS#8 PrivateKeyInfo of `algorithm`, an ML-DSA or ML-KEM algorithm, or None
/// for a privateKey in the expandedKey form.
// NB: A key in the expandedKey form, which Web Crypto rejects, has no seed to make an OpenSSL key from, and OpenSSL
//     would refuse an expanded key that is not valid before Web Crypto can reject the form.
pub(crate) fn parse_private_key(algorithm: KeyAlgorithm, der: &[u8]) -> Result<Option<KeyHandle>, CryptoError> {
    let parameter_set = match algorithm {
        KeyAlgorithm::MlDsa(size) => ParameterSet::MlDsa(size),
        KeyAlgorithm::MlKem(size) => ParameterSet::MlKem(size),
        _ => unreachable!("The algorithm is ML-DSA or ML-KEM"),
    };
    let encoded = parse_module_lattice_private_key(der, parameter_set.seed_size(), parameter_set.expanded_key_size())?;
    let Some(seed) = encoded.seed else {
        return Ok(None);
    };
    let private_key = MlPrivateKey::from_seed(parameter_set, seed)?;
    if let Some(expected) = encoded.expanded_key {
        let expanded_key = private_key.expanded_key()?;
        if *expanded_key != *expected {
            return Err(CryptoError::Invalid("The expanded key does not match the seed"));
        }
    }
    Ok(Some(KeyHandle(Key::MlPrivate(private_key))))
}

impl MlDsaPublicKey {
    pub(crate) fn stored(&self) -> StoredKey {
        StoredKey::new(HandleTag::MlDsaPublic, vec![SecretBytes::new(self.bytes.clone())])
    }
}

impl MlKemPublicKey {
    pub(crate) fn stored(&self) -> StoredKey {
        StoredKey::new(HandleTag::MlKemPublic, vec![SecretBytes::new(self.bytes.clone())])
    }
}

impl MlPrivateKey {
    /// The stored form of the key: its seed, public key and expanded key.
    pub(crate) fn stored(&self) -> StoredKey {
        let tag = match self.parameter_set {
            ParameterSet::MlDsa(_) => HandleTag::MlDsaPrivate,
            ParameterSet::MlKem(_) => HandleTag::MlKemPrivate,
        };
        StoredKey::new(
            tag,
            vec![
                self.seed().expect("OpenSSL gives the seed of a key made from one"),
                SecretBytes::new(self.public_key.clone()),
                self.expanded_key().expect("OpenSSL gives the expanded key"),
            ],
        )
    }
}

/// The key that a stored handle of an ML-DSA or ML-KEM tag holds. A private key is rebuilt from its seed, with the
/// parameter set that its public key's length names, and must give back that public key.
pub(crate) fn decode_stored(tag: HandleTag, buffers: &[&[u8]]) -> Result<KeyHandle, KeyDecodeError> {
    let invalid = || KeyDecodeError::InvalidComponents;
    let parameter_set = match tag {
        HandleTag::MlDsaPublic => {
            return Ok(KeyHandle(Key::MlDsaPublic(
                mldsa_public_key_from_bytes(buffers[0]).ok_or_else(invalid)?,
            )));
        }
        HandleTag::MlKemPublic => return Ok(KeyHandle(Key::MlKemPublic(mlkem_public_key_from_bytes(buffers[0])))),
        HandleTag::MlDsaPrivate => {
            ParameterSet::MlDsa(mldsa_public_key_from_bytes(buffers[1]).ok_or_else(invalid)?.size)
        }
        HandleTag::MlKemPrivate => {
            ParameterSet::MlKem(mlkem_public_key_from_bytes(buffers[1]).size.ok_or_else(invalid)?)
        }
        _ => return Err(invalid()),
    };
    if buffers[0].is_empty() {
        return Err(invalid());
    }
    let private_key = MlPrivateKey::from_seed(parameter_set, buffers[0]).map_err(|_| invalid())?;
    if private_key.public_key != buffers[1] {
        return Err(invalid());
    }
    Ok(KeyHandle(Key::MlPrivate(private_key)))
}
