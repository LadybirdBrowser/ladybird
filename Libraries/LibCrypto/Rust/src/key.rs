/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::ec::{self, EcPrivateKey, EcPublicKey};
use crate::ml::{self, MlDsaPublicKey, MlKemPublicKey, MlPrivateKey};
use crate::rsa::{self, RsaPrivateKey, RsaPublicKey};
use crate::sys;
use crate::{OkpCurve, OkpKey, okp_key_from_raw};

pub struct KeyHandle(pub(crate) Key);

pub(crate) enum Key {
    Secret(SecretBytes),
    OkpPublic(OkpKey),
    OkpPrivate(OkpKey),
    MlDsaPublic(MlDsaPublicKey),
    MlKemPublic(MlKemPublicKey),
    MlPrivate(MlPrivateKey),
    EcPublic(EcPublicKey),
    EcPrivate(EcPrivateKey),
    RsaPublic(RsaPublicKey),
    RsaPrivate(RsaPrivateKey),
}

/// The tag of a stored key, which says what its buffers hold. The values are part of the stable storage serialization
/// format (Web::Crypto::HandleTag).
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum HandleTag {
    Secret = 0,
    RsaPublic = 1,
    RsaPrivate = 2,
    EcPublic = 3,
    EcPrivate = 4,
    MlDsaPublic = 5,
    MlDsaPrivate = 6,
    MlKemPublic = 7,
    MlKemPrivate = 8,
    OkpPublic = 9,
    OkpPrivate = 10,
}

impl HandleTag {
    const ALL: [Self; 11] = [
        Self::Secret,
        Self::RsaPublic,
        Self::RsaPrivate,
        Self::EcPublic,
        Self::EcPrivate,
        Self::MlDsaPublic,
        Self::MlDsaPrivate,
        Self::MlKemPublic,
        Self::MlKemPrivate,
        Self::OkpPublic,
        Self::OkpPrivate,
    ];

    /// The number of buffers that a key of this tag stores. An EC private key stores one, followed by its public key's
    /// two when it has its public key.
    pub fn buffer_count(self) -> usize {
        match self {
            Self::Secret
            | Self::EcPrivate
            | Self::MlDsaPublic
            | Self::MlKemPublic
            | Self::OkpPublic
            | Self::OkpPrivate => 1,
            Self::RsaPublic | Self::EcPublic => 2,
            Self::MlDsaPrivate | Self::MlKemPrivate => 3,
            Self::RsaPrivate => 8,
        }
    }
}

impl TryFrom<u8> for HandleTag {
    type Error = KeyDecodeError;

    fn try_from(value: u8) -> Result<Self, KeyDecodeError> {
        Self::ALL
            .get(usize::from(value))
            .copied()
            .ok_or(KeyDecodeError::InvalidComponents)
    }
}

/// The stored form of a key: its tag and its buffers. An EC private key's buffers are its private value and its public
/// key's coordinates.
pub struct StoredKey {
    pub tag: HandleTag,
    buffers: Vec<SecretBytes>,
}

impl StoredKey {
    pub(crate) fn new(tag: HandleTag, buffers: Vec<SecretBytes>) -> Self {
        Self { tag, buffers }
    }

    pub fn buffers(&self) -> &[SecretBytes] {
        &self.buffers
    }
}

/// Bytes that are cleared when they are dropped.
pub struct SecretBytes(Vec<u8>);

impl SecretBytes {
    pub(crate) fn new(bytes: Vec<u8>) -> Self {
        Self(bytes)
    }

    pub(crate) fn truncate(&mut self, length: usize) {
        self.0.truncate(length);
    }

    /// Appends `bytes`, which must fit in the capacity reserved so that no partial copy is left behind by a reallocation.
    pub(crate) fn extend_from_slice(&mut self, bytes: &[u8]) {
        assert!(self.0.len() + bytes.len() <= self.0.capacity());
        self.0.extend_from_slice(bytes);
    }

    pub fn into_vec(mut self) -> Vec<u8> {
        core::mem::take(&mut self.0)
    }
}

impl core::ops::Deref for SecretBytes {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.0
    }
}

impl core::ops::DerefMut for SecretBytes {
    fn deref_mut(&mut self) -> &mut [u8] {
        &mut self.0
    }
}

impl Drop for SecretBytes {
    fn drop(&mut self) {
        clear_allocation(&mut self.0);
    }
}

/// Overwrites `bytes` with zeros in a way the compiler does not remove.
pub(crate) fn clear(bytes: &mut [u8]) {
    // SAFETY: The pointer and length come from a live mutable slice.
    unsafe { sys::OPENSSL_cleanse(bytes.as_mut_ptr().cast(), bytes.len()) };
}

/// Overwrites all of the allocation of `bytes` with zeros, including what lies past its length.
pub(crate) fn clear_allocation(bytes: &mut Vec<u8>) {
    // SAFETY: The pointer and capacity describe the vector's allocation.
    unsafe { sys::OPENSSL_cleanse(bytes.as_mut_ptr().cast(), bytes.capacity()) };
}

pub struct KeyPair {
    pub public_key: KeyHandle,
    pub private_key: KeyHandle,
}

#[derive(Debug)]
pub enum KeyDecodeError {
    InvalidComponents,
}

impl KeyHandle {
    pub fn from_secret_bytes(bytes: &[u8]) -> Self {
        Self(Key::Secret(SecretBytes::new(bytes.to_vec())))
    }

    pub fn secret_bytes(&self) -> &[u8] {
        match &self.0 {
            Key::Secret(bytes) => bytes,
            _ => panic!("The key is not a secret key"),
        }
    }

    pub fn to_stored(&self) -> StoredKey {
        match &self.0 {
            Key::Secret(bytes) => StoredKey::new(HandleTag::Secret, vec![SecretBytes::new(bytes.to_vec())]),
            Key::OkpPublic(key) => key.stored(HandleTag::OkpPublic),
            Key::OkpPrivate(key) => key.stored(HandleTag::OkpPrivate),
            Key::EcPublic(key) => key.stored(),
            Key::EcPrivate(key) => key.stored(),
            Key::RsaPublic(key) => key.stored(),
            Key::RsaPrivate(key) => key.stored(),
            Key::MlDsaPublic(key) => key.stored(),
            Key::MlKemPublic(key) => key.stored(),
            Key::MlPrivate(key) => key.stored(),
        }
    }

    /// The key that a stored form holds, as `to_stored()` gives it. An OKP key's curve is not stored with it.
    pub fn from_stored(
        tag: HandleTag,
        buffers: &[&[u8]],
        ec_parameters: Option<&[u32]>,
        has_ec_public_key: bool,
        okp_curve: Option<OkpCurve>,
    ) -> Result<Self, KeyDecodeError> {
        let is_ec_private_key = tag == HandleTag::EcPrivate;
        let expected = tag.buffer_count() + if has_ec_public_key { 2 } else { 0 };
        if buffers.len() != expected || (!is_ec_private_key && (ec_parameters.is_some() || has_ec_public_key)) {
            return Err(KeyDecodeError::InvalidComponents);
        }
        match tag {
            HandleTag::Secret => Ok(Self::from_secret_bytes(buffers[0])),
            HandleTag::OkpPublic | HandleTag::OkpPrivate => {
                let curve = okp_curve.ok_or(KeyDecodeError::InvalidComponents)?;
                okp_key_from_raw(curve, tag == HandleTag::OkpPrivate, buffers[0])
                    .map_err(|_| KeyDecodeError::InvalidComponents)
            }
            HandleTag::RsaPublic | HandleTag::RsaPrivate => rsa::decode_stored(tag, buffers),
            HandleTag::EcPublic | HandleTag::EcPrivate => {
                ec::decode_stored(tag, buffers, ec_parameters, has_ec_public_key)
            }
            HandleTag::MlDsaPublic | HandleTag::MlDsaPrivate | HandleTag::MlKemPublic | HandleTag::MlKemPrivate => {
                ml::decode_stored(tag, buffers)
            }
        }
    }
}
