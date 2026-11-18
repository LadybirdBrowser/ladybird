/*
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! EC keys of the NIST prime curves. A key given by its coordinates or scalar is on the curve whose coordinates have
//! that size: 32 bytes for P-256, 48 for P-384 and 66 for P-521.

use core::ffi::{CStr, c_int};

use crate::der;
use crate::key::{HandleTag, Key, KeyDecodeError, KeyHandle, KeyPair, SecretBytes, StoredKey};
use crate::openssl::{self, ParamBuilder, Pkey, end_parameter, octet_string_parameter, utf8_parameter};
use crate::{CryptoError, EcCurve, KeyAlgorithm, sys};

#[derive(Clone)]
pub(crate) struct EcPublicKey {
    key: Pkey,
    curve: EcCurve,
}

pub(crate) struct EcPrivateKey {
    key: Pkey,
    curve: EcCurve,
}

impl EcCurve {
    fn from_scalar_size(scalar_size: usize) -> Result<Self, CryptoError> {
        match scalar_size {
            32 => Ok(Self::P256),
            48 => Ok(Self::P384),
            66 => Ok(Self::P521),
            _ => Err(CryptoError::Invalid("Unsupported EC curve")),
        }
    }

    fn scalar_size(self) -> usize {
        match self {
            Self::P256 => 32,
            Self::P384 => 48,
            Self::P521 => 66,
        }
    }

    /// The name of the curve's group, as OpenSSL takes it.
    fn name(self) -> &'static CStr {
        match self {
            Self::P256 => c"P-256",
            Self::P384 => c"P-384",
            Self::P521 => c"P-521",
        }
    }

    fn nid(self) -> c_int {
        match self {
            Self::P256 => sys::NID_X9_62_PRIME256V1,
            Self::P384 => sys::NID_SECP384R1,
            Self::P521 => sys::NID_SECP521R1,
        }
    }

    /// The contents of the DER encoding of the curve's object identifier.
    fn oid(self) -> &'static [u8] {
        match self {
            // secp256r1, 1.2.840.10045.3.1.7
            Self::P256 => &[0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07],
            // secp384r1, 1.3.132.0.34
            Self::P384 => &[0x2b, 0x81, 0x04, 0x00, 0x22],
            // secp521r1, 1.3.132.0.35
            Self::P521 => &[0x2b, 0x81, 0x04, 0x00, 0x23],
        }
    }

    pub(crate) fn from_oid(oid: &[u8]) -> Option<Self> {
        [Self::P256, Self::P384, Self::P521]
            .into_iter()
            .find(|curve| curve.oid() == oid)
    }

    /// The arcs of the curve's object identifier, as a stored key gives them.
    fn arcs(self) -> &'static [u32] {
        match self {
            Self::P256 => &[1, 2, 840, 10045, 3, 1, 7],
            Self::P384 => &[1, 3, 132, 0, 34],
            Self::P521 => &[1, 3, 132, 0, 35],
        }
    }
}

/// The coordinates of the public point of `key`, a key on `curve`.
fn coordinates(key: &Pkey, curve: EcCurve) -> Result<(Vec<u8>, Vec<u8>), CryptoError> {
    let coordinate = |name| -> Result<Vec<u8>, CryptoError> {
        let number = key.number(name).ok_or(CryptoError::Backend)?;
        Ok(number.to_padded(curve.scalar_size())?.into_vec())
    };
    Ok((coordinate(c"qx")?, coordinate(c"qy")?))
}

fn uncompressed_point(key: &Pkey, curve: EcCurve) -> Result<Vec<u8>, CryptoError> {
    let (x, y) = coordinates(key, curve)?;
    let mut octets = Vec::with_capacity(1 + x.len() + y.len());
    octets.push(0x04);
    octets.extend_from_slice(&x);
    octets.extend_from_slice(&y);
    Ok(octets)
}

impl EcPublicKey {
    /// A public key from its SEC 1 encoding, uncompressed or compressed.
    pub(crate) fn from_octets(data: &[u8]) -> Result<Self, CryptoError> {
        let scalar_size = match data.first() {
            None => return Err(CryptoError::Invalid("Empty EC point")),
            Some(0x04) if data.len() % 2 == 1 => (data.len() - 1) / 2,
            Some(0x02 | 0x03) => data.len() - 1,
            Some(_) => return Err(CryptoError::Invalid("Unsupported EC point format")),
        };
        let curve = EcCurve::from_scalar_size(scalar_size)?;
        let invalid = |_| CryptoError::Invalid("Invalid EC public key");
        let params = [
            utf8_parameter(c"group", curve.name()),
            octet_string_parameter(c"pub", data),
            end_parameter(),
        ];
        let key = Pkey::from_data(c"EC", sys::EVP_PKEY_PUBLIC_KEY, &params).map_err(invalid)?;
        if !openssl::check_key(&key, false, false).map_err(invalid)? {
            return Err(invalid(CryptoError::Backend));
        }
        Ok(Self { key, curve })
    }

    fn from_coordinates(x: &[u8], y: &[u8]) -> Result<Self, CryptoError> {
        if x.len() != y.len() {
            return Err(CryptoError::Invalid("EC coordinates have different sizes"));
        }
        let mut octets = Vec::with_capacity(1 + x.len() + y.len());
        octets.push(0x04);
        octets.extend_from_slice(x);
        octets.extend_from_slice(y);
        Self::from_octets(&octets)
    }

    fn coordinates(&self) -> Result<(Vec<u8>, Vec<u8>), CryptoError> {
        coordinates(&self.key, self.curve)
    }

    fn to_uncompressed(&self) -> Result<Vec<u8>, CryptoError> {
        uncompressed_point(&self.key, self.curve)
    }
}

impl EcPrivateKey {
    fn generate(curve: EcCurve) -> Result<Self, CryptoError> {
        let params = [utf8_parameter(c"group", curve.name()), end_parameter()];
        Ok(Self {
            key: Pkey::generate(c"EC", &params)?,
            curve,
        })
    }

    /// The key of the private value `d`, whose public key, when given, must be the one `d` gives.
    fn from_scalar(d: &[u8], public_key: Option<&EcPublicKey>) -> Result<Self, CryptoError> {
        let curve = EcCurve::from_scalar_size(d.len())?;
        let point =
            openssl::ec_public_point(curve.nid(), d)?.ok_or(CryptoError::Invalid("EC private key is out of range"))?;
        if let Some(public_key) = public_key
            && public_key.to_uncompressed()? != point
        {
            return Err(CryptoError::Invalid("EC public key does not match the private key"));
        }

        let mut builder = ParamBuilder::new()?;
        builder.push_utf8(c"group", curve.name())?;
        builder.push_number(c"priv", d, true)?;
        builder.push_octets(c"pub", &point)?;
        let params = builder.build()?;
        Ok(Self {
            key: Pkey::from_data(c"EC", sys::EVP_PKEY_KEYPAIR, &params)?,
            curve,
        })
    }

    fn coordinates(&self) -> Result<(Vec<u8>, Vec<u8>), CryptoError> {
        coordinates(&self.key, self.curve)
    }

    fn to_uncompressed(&self) -> Result<Vec<u8>, CryptoError> {
        uncompressed_point(&self.key, self.curve)
    }

    fn public_key(&self) -> Result<EcPublicKey, CryptoError> {
        EcPublicKey::from_octets(&self.to_uncompressed()?)
    }

    fn d_bytes(&self) -> Result<SecretBytes, CryptoError> {
        let d = self.key.number(c"priv").ok_or(CryptoError::Backend)?;
        d.to_padded(self.curve.scalar_size())
    }
}

// https://www.rfc-editor.org/rfc/rfc5915#section-3
fn encode_private_key(d: &[u8], parameters: Option<EcCurve>, public_point: Option<&[u8]>) -> Vec<u8> {
    let version = [der::INTEGER, 1, 1];
    let d_header = der::header(der::OCTET_STRING, d.len());
    let parameters = parameters.map(|curve| der::element(0xa0, &[&der::object_identifier(curve.oid())]));
    let public_key = public_point.map(|point| der::element(0xa1, &[&der::element(der::BIT_STRING, &[&[0], point])]));
    der::element(
        der::SEQUENCE,
        &[
            &version,
            &d_header,
            d,
            parameters.as_deref().unwrap_or_default(),
            public_key.as_deref().unwrap_or_default(),
        ],
    )
}

/// The private key `key` of a PKCS#8 PrivateKeyInfo with the named curve `curve`, whose privateKey is `encoded`.
pub(crate) fn parse_private_key_info(curve: EcCurve, key: Pkey, encoded: &[u8]) -> Result<KeyHandle, CryptoError> {
    let scalar_size = curve.scalar_size();
    let d = key
        .number(c"priv")
        .ok_or(CryptoError::Backend)?
        .to_padded(scalar_size)?;
    let private_key = EcPrivateKey::from_scalar(&d, None)?;
    let uncompressed = private_key.to_uncompressed()?;
    let mut compressed = uncompressed[..=scalar_size].to_vec();
    compressed[0] = 0x02 | (uncompressed.last().copied().unwrap_or_default() & 1);

    // NB: OpenSSL accepts BER, and a privateKey of any width. The parameters and the public key are optional, and the
    //     public key may be compressed, so the encoding must be the DER encoding of the key in one of those forms.
    for parameters in [None, Some(curve)] {
        for public_point in [None, Some(uncompressed.as_slice()), Some(compressed.as_slice())] {
            let mut candidate = encode_private_key(&d, parameters, public_point);
            let matches = candidate == encoded;
            crate::key::clear(&mut candidate);
            if matches {
                return Ok(KeyHandle(Key::EcPrivate(private_key)));
            }
        }
    }
    Err(CryptoError::Invalid("Invalid ECPrivateKey"))
}

/// The public key of a SubjectPublicKeyInfo, whose subjectPublicKey is the SEC 1 encoding of the point.
pub(crate) fn parse_subject_public_key(encoded: &[u8]) -> Result<KeyHandle, CryptoError> {
    Ok(KeyHandle(Key::EcPublic(EcPublicKey::from_octets(encoded)?)))
}

fn public_key(key: &KeyHandle) -> &EcPublicKey {
    match &key.0 {
        Key::EcPublic(public_key) => public_key,
        _ => panic!("The key is not an EC public key"),
    }
}

fn private_key(key: &KeyHandle) -> &EcPrivateKey {
    match &key.0 {
        Key::EcPrivate(private_key) => private_key,
        _ => panic!("The key is not an EC private key"),
    }
}

pub fn ec_generate_key_pair(curve: EcCurve) -> Result<KeyPair, CryptoError> {
    let private_key = EcPrivateKey::generate(curve).map_err(|_| CryptoError::Backend)?;
    let public_key = private_key.public_key()?;
    Ok(KeyPair {
        public_key: KeyHandle(Key::EcPublic(public_key)),
        private_key: KeyHandle(Key::EcPrivate(private_key)),
    })
}

pub fn ec_sign(key: &KeyHandle, digest: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let private_key = private_key(key);
    openssl::ecdsa_sign(&private_key.key, digest, private_key.curve.scalar_size())
}

pub fn ec_verify(key: &KeyHandle, digest: &[u8], signature: &[u8]) -> Result<bool, CryptoError> {
    let public_key = public_key(key);
    if signature.len() != public_key.curve.scalar_size() * 2 {
        return Ok(false);
    }
    openssl::ecdsa_verify(&public_key.key, digest, signature)
}

pub fn ec_public_key_scalar_size(key: &KeyHandle) -> usize {
    public_key(key).curve.scalar_size()
}

pub fn ec_key_from_coordinates(x: &[u8], y: &[u8], d: Option<&[u8]>) -> Option<KeyHandle> {
    let public_key = EcPublicKey::from_coordinates(x, y).ok()?;
    let Some(d) = d else {
        return Some(KeyHandle(Key::EcPublic(public_key)));
    };
    let private_key = EcPrivateKey::from_scalar(d, Some(&public_key)).ok()?;
    Some(KeyHandle(Key::EcPrivate(private_key)))
}

pub fn ec_parse_raw_public_key(bytes: &[u8]) -> Option<KeyHandle> {
    Some(KeyHandle(Key::EcPublic(EcPublicKey::from_octets(bytes).ok()?)))
}

// NB: Every EC key is a valid key for its own curve, since building it checks that.
pub fn ec_key_is_valid(curve: EcCurve, key: &KeyHandle) -> bool {
    let key_curve = match &key.0 {
        Key::EcPublic(public_key) => public_key.curve,
        Key::EcPrivate(private_key) => private_key.curve,
        _ => panic!("The key is not an EC key"),
    };
    key_curve == curve
}

pub fn ec_export_der(key: &KeyHandle) -> Result<Vec<u8>, CryptoError> {
    let private_key = match &key.0 {
        Key::EcPublic(public_key) => {
            return Ok(der::subject_public_key_info(
                KeyAlgorithm::Ec.oid(),
                Some(&der::object_identifier(public_key.curve.oid())),
                &public_key.to_uncompressed()?,
            ));
        }
        Key::EcPrivate(private_key) => private_key,
        _ => panic!("The key is not an EC key"),
    };
    let parameters = der::object_identifier(private_key.curve.oid());
    // NB: The ECPrivateKey has its public key and not its parameters, which the PrivateKeyInfo already has.
    let public_point = private_key.to_uncompressed()?;
    let encoded = SecretBytes::new(encode_private_key(&private_key.d_bytes()?, None, Some(&public_point)));
    Ok(der::private_key_info(
        KeyAlgorithm::Ec.oid(),
        Some(&parameters),
        &encoded,
    ))
}

pub fn ec_export_uncompressed(key: &KeyHandle) -> Result<Vec<u8>, CryptoError> {
    public_key(key).to_uncompressed()
}

pub fn ec_export_public_coordinates(key: &KeyHandle) -> Result<(Vec<u8>, Vec<u8>), CryptoError> {
    match &key.0 {
        Key::EcPublic(public_key) => public_key.coordinates(),
        Key::EcPrivate(private_key) => private_key.coordinates(),
        _ => panic!("The key is not an EC key"),
    }
}

pub fn ec_export_private_scalar(key: &KeyHandle) -> Result<Vec<u8>, CryptoError> {
    Ok(private_key(key).d_bytes()?.into_vec())
}

pub fn ec_compute_coordinate(key: &KeyHandle, public_key: &KeyHandle) -> Result<SecretBytes, CryptoError> {
    let private_key = private_key(key);
    let peer = self::public_key(public_key);
    if private_key.curve != peer.curve {
        return Err(CryptoError::Invalid("EC keys are on different curves"));
    }
    openssl::derive(&private_key.key, &peer.key, true)
}

pub(crate) fn public_key_for_private_key(key: &KeyHandle) -> Option<KeyHandle> {
    let Key::EcPrivate(private_key) = &key.0 else {
        return None;
    };
    Some(KeyHandle(Key::EcPublic(private_key.public_key().ok()?)))
}

impl EcPublicKey {
    /// The stored form of the key: its coordinates.
    pub(crate) fn stored(&self) -> StoredKey {
        let (x, y) = self.coordinates().expect("OpenSSL gives an EC key's coordinates");
        StoredKey::new(HandleTag::EcPublic, vec![SecretBytes::new(x), SecretBytes::new(y)])
    }
}

impl EcPrivateKey {
    /// The stored form of the key: its private value, then its public key's coordinates.
    pub(crate) fn stored(&self) -> StoredKey {
        let (x, y) = self.coordinates().expect("OpenSSL gives an EC key's coordinates");
        StoredKey::new(
            HandleTag::EcPrivate,
            vec![
                self.d_bytes().expect("OpenSSL gives an EC key's private value"),
                SecretBytes::new(x),
                SecretBytes::new(y),
            ],
        )
    }
}

/// The key that a stored handle of an EC tag holds.
pub(crate) fn decode_stored(
    tag: HandleTag,
    buffers: &[&[u8]],
    parameters: Option<&[u32]>,
    has_public_key: bool,
) -> Result<KeyHandle, KeyDecodeError> {
    let invalid = |_| KeyDecodeError::InvalidComponents;
    match tag {
        HandleTag::EcPublic => Ok(KeyHandle(Key::EcPublic(
            EcPublicKey::from_coordinates(buffers[0], buffers[1]).map_err(invalid)?,
        ))),
        HandleTag::EcPrivate => {
            let public_key = if has_public_key {
                Some(EcPublicKey::from_coordinates(buffers[1], buffers[2]).map_err(invalid)?)
            } else {
                None
            };
            let private_key = EcPrivateKey::from_scalar(buffers[0], public_key.as_ref()).map_err(invalid)?;
            // NB: A stored key may have its own curve's parameters, and may not have its public key.
            if parameters.is_some_and(|parameters| parameters != private_key.curve.arcs()) {
                return Err(KeyDecodeError::InvalidComponents);
            }
            Ok(KeyHandle(Key::EcPrivate(private_key)))
        }
        _ => Err(KeyDecodeError::InvalidComponents),
    }
}
