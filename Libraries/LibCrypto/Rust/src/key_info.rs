/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{CryptoError, EcCurve, KeyHandle, MlDsaSize, MlKemSize, OkpCurve, der, ec, ml, rsa};
use crate::key::SecretBytes;
use crate::openssl::{self, Pkey};

/// The algorithm of a SubjectPublicKeyInfo or PrivateKeyInfo.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum KeyAlgorithm {
    Rsa,
    Ec,
    Okp(OkpCurve),
    MlDsa(MlDsaSize),
    MlKem(MlKemSize),
}

impl KeyAlgorithm {
    const ALL: [Self; 12] = [
        Self::Rsa,
        Self::Ec,
        Self::Okp(OkpCurve::X25519),
        Self::Okp(OkpCurve::X448),
        Self::Okp(OkpCurve::Ed25519),
        Self::Okp(OkpCurve::Ed448),
        Self::MlDsa(MlDsaSize::MlDsa44),
        Self::MlDsa(MlDsaSize::MlDsa65),
        Self::MlDsa(MlDsaSize::MlDsa87),
        Self::MlKem(MlKemSize::MlKem512),
        Self::MlKem(MlKemSize::MlKem768),
        Self::MlKem(MlKemSize::MlKem1024),
    ];

    /// The contents of the DER encoding of the algorithm's object identifier.
    pub(crate) fn oid(self) -> &'static [u8] {
        match self {
            // rsaEncryption, 1.2.840.113549.1.1.1
            Self::Rsa => &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01],
            // id-ecPublicKey, 1.2.840.10045.2.1
            Self::Ec => &[0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01],
            // id-X25519, id-X448, id-Ed25519 and id-Ed448, 1.3.101.110 to 1.3.101.113
            Self::Okp(OkpCurve::X25519) => &[0x2b, 0x65, 0x6e],
            Self::Okp(OkpCurve::X448) => &[0x2b, 0x65, 0x6f],
            Self::Okp(OkpCurve::Ed25519) => &[0x2b, 0x65, 0x70],
            Self::Okp(OkpCurve::Ed448) => &[0x2b, 0x65, 0x71],
            // id-ml-dsa-44, id-ml-dsa-65 and id-ml-dsa-87, 2.16.840.1.101.3.4.3.17 to 2.16.840.1.101.3.4.3.19
            Self::MlDsa(MlDsaSize::MlDsa44) => &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x03, 0x11],
            Self::MlDsa(MlDsaSize::MlDsa65) => &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x03, 0x12],
            Self::MlDsa(MlDsaSize::MlDsa87) => &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x03, 0x13],
            // id-alg-ml-kem-512, id-alg-ml-kem-768 and id-alg-ml-kem-1024, 2.16.840.1.101.3.4.4.1 to 2.16.840.1.101.3.4.4.3
            Self::MlKem(MlKemSize::MlKem512) => &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x04, 0x01],
            Self::MlKem(MlKemSize::MlKem768) => &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x04, 0x02],
            Self::MlKem(MlKemSize::MlKem1024) => &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x04, 0x03],
        }
    }

    pub(crate) fn from_oid(oid: &[u8]) -> Option<Self> {
        Self::ALL.into_iter().find(|algorithm| algorithm.oid() == oid)
    }
}

/// A SubjectPublicKeyInfo or PrivateKeyInfo.
pub struct KeyInfo {
    algorithm: Option<KeyAlgorithm>,
    has_parameters: bool,
    named_curve: Option<EcCurve>,
    material: KeyMaterial,
}

pub enum KeyMaterial {
    /// The key of an RSA or EC structure, or of an ML-DSA or ML-KEM PrivateKeyInfo, which is parsed with the structure.
    Key(KeyHandle),
    /// The subjectPublicKey or privateKey contents of the other structures.
    Raw(SecretBytes),
    /// The privateKey of an ML-DSA or ML-KEM PrivateKeyInfo in the expandedKey form, which has no key.
    MlExpandedKey,
}

/// The contents of the DER OCTET STRING at the start of `data`, and whether it takes all of `data`.
pub fn parse_octet_string(data: &[u8]) -> Result<(&[u8], bool), CryptoError> {
    let invalid = || CryptoError::Invalid("Invalid OCTET STRING");
    let (&tag, rest) = data.split_first().ok_or_else(invalid)?;
    let (&first_length_byte, mut rest) = rest.split_first().ok_or_else(invalid)?;
    if tag != der::OCTET_STRING {
        return Err(invalid());
    }
    let length = if first_length_byte & 0x80 == 0 {
        usize::from(first_length_byte)
    } else {
        let count = usize::from(first_length_byte & 0x7f);
        if count == 0 || count > size_of::<usize>() || rest.len() < count {
            return Err(invalid());
        }
        let (length_bytes, after) = rest.split_at(count);
        rest = after;
        length_bytes
            .iter()
            .fold(0, |length, &byte| (length << 8) | usize::from(byte))
    };
    let header_length = data.len() - rest.len();
    if rest.len() < length {
        return Err(invalid());
    }
    // NB: DER gives each length a single encoding, which is the one this writes.
    if data[..header_length] != der::header(der::OCTET_STRING, length) {
        return Err(CryptoError::Invalid("OCTET STRING is not in DER"));
    }
    Ok((&rest[..length], header_length + length == data.len()))
}

/// The DER SubjectPublicKeyInfo at the start of `data`, and whether it takes all of `data`.
pub fn parse_subject_public_key_info(data: &[u8]) -> Result<(KeyInfo, bool), CryptoError> {
    let parsed = openssl::parse_subject_public_key_info(data).map_err(CryptoError::Invalid)?;
    let key_info = KeyInfo::new(false, parsed.algorithm, SecretBytes::new(parsed.raw_key), parsed.key)?;
    Ok((key_info, parsed.consumed == data.len()))
}

/// The DER PrivateKeyInfo at the start of `data`, and whether it takes all of `data`.
pub fn parse_private_key_info(data: &[u8]) -> Result<(KeyInfo, bool), CryptoError> {
    let parsed = openssl::parse_private_key_info(data).map_err(CryptoError::Invalid)?;
    let key_info = KeyInfo::new(true, parsed.algorithm, parsed.raw_key, parsed.key)?;
    Ok((key_info, parsed.consumed == data.len()))
}

impl KeyInfo {
    // NB: OpenSSL decodes the RSA and EC keys of either structure, so those keys are taken from it.
    fn new(
        is_private: bool,
        algorithm: openssl::AlgorithmIdentifier,
        raw_key: SecretBytes,
        key: Option<Pkey>,
    ) -> Result<Self, CryptoError> {
        let named_curve = algorithm.curve.as_deref().and_then(EcCurve::from_oid);
        let has_parameters = algorithm.has_parameters();
        let algorithm = KeyAlgorithm::from_oid(&algorithm.oid);
        let material = match algorithm {
            Some(KeyAlgorithm::Rsa) => {
                let key = key.expect("OpenSSL decodes RSA keys");
                KeyMaterial::Key(if is_private {
                    rsa::from_private_key(key)
                } else {
                    rsa::from_public_key(key)
                })
            }
            Some(KeyAlgorithm::Ec) => {
                let curve = named_curve.ok_or(CryptoError::Invalid("Unsupported EC parameters"))?;
                KeyMaterial::Key(match key {
                    Some(private_key) if is_private => ec::parse_private_key_info(curve, private_key, &raw_key)?,
                    _ => ec::parse_subject_public_key(&raw_key)?,
                })
            }
            Some(algorithm @ (KeyAlgorithm::MlDsa(_) | KeyAlgorithm::MlKem(_))) if is_private => {
                match ml::parse_private_key(algorithm, &raw_key)? {
                    Some(private_key) => KeyMaterial::Key(private_key),
                    None => KeyMaterial::MlExpandedKey,
                }
            }
            _ => KeyMaterial::Raw(raw_key),
        };
        Ok(Self {
            algorithm,
            has_parameters,
            named_curve,
            material,
        })
    }

    pub fn into_material(self) -> KeyMaterial {
        self.material
    }

    /// The algorithm the structure names, if it is one of those that Web Crypto supports.
    pub fn algorithm(&self) -> Option<KeyAlgorithm> {
        self.algorithm
    }

    /// Whether the structure's AlgorithmIdentifier has parameters.
    pub fn has_parameters(&self) -> bool {
        self.has_parameters
    }

    /// The curve the structure's parameters name, if they name P-256, P-384 or P-521.
    pub fn named_curve(&self) -> Option<EcCurve> {
        self.named_curve
    }
}
