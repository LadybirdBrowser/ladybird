/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The PIN/UV auth protocols one and two. The client and a key use them so that the PIN, and the token that the key
//! gives for it, do not cross the wire in the clear.

use super::{Host, Secret};
use crate::cbor::Value;
use libcrypto_rust::{
    EcCurve, ShaHashKind, aes_cbc_cipher_without_padding, ec_compute_coordinate, ec_export_public_coordinates,
    ec_generate_key_pair, ec_key_from_coordinates, hkdf_derive_key, hmac_sign, sha_digest,
};

const INITIALIZATION_VECTOR_SIZE: usize = 16;
const P256_COORDINATE_SIZE: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Version {
    One,
    Two,
}

impl Version {
    /// The value of the pinUvAuthProtocol parameter.
    pub fn number(self) -> i64 {
        match self {
            Self::One => 1,
            Self::Two => 2,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProtocolError;

/// A protocol with the secret that the client shares with one key.
pub struct SharedSecret {
    version: Version,
    /// The public key of the client.
    public_x: Vec<u8>,
    public_y: Vec<u8>,
    aes_key: Secret,
}

impl SharedSecret {
    /// Makes the shared secret from a new key pair of the client and the key agreement key of the security key.
    pub fn create(version: Version, authenticator_key_agreement: &Value) -> Result<Self, ProtocolError> {
        // NB: The key agreement key is a COSE EC2 key with the coordinates x (-2) and y (-3).
        let x = authenticator_key_agreement.get(-2).and_then(Value::as_bytes);
        let y = authenticator_key_agreement.get(-3).and_then(Value::as_bytes);
        let (Some(x), Some(y)) = (x, y) else {
            return Err(ProtocolError);
        };
        if x.len() != P256_COORDINATE_SIZE || y.len() != P256_COORDINATE_SIZE {
            return Err(ProtocolError);
        }
        // NB: This also refuses a point that is not on the curve.
        let authenticator_key = ec_key_from_coordinates(x, y, None).ok_or(ProtocolError)?;

        let key_pair = ec_generate_key_pair(EcCurve::P256).map_err(|_| ProtocolError)?;
        let (public_x, public_y) = ec_export_public_coordinates(&key_pair.public_key).map_err(|_| ProtocolError)?;
        let shared_point_x =
            ec_compute_coordinate(&key_pair.private_key, &authenticator_key).map_err(|_| ProtocolError)?;

        let aes_key = match version {
            // PIN/UV auth protocol one: The shared secret is the SHA-256 of the x-coordinate.
            Version::One => sha_digest(ShaHashKind::Sha256, &shared_point_x).map_err(|_| ProtocolError)?,
            // PIN/UV auth protocol two: HKDF-SHA-256 with a salt of 32 zero bytes gives a key for each use.
            Version::Two => hkdf_derive_key(ShaHashKind::Sha256, &[0; 32], &shared_point_x, b"CTAP2 AES key", 32)
                .map_err(|_| ProtocolError)?
                .into_vec(),
        };
        Ok(Self {
            version,
            public_x,
            public_y,
            aes_key: Secret::new(aes_key),
        })
    }

    pub fn version(&self) -> Version {
        self.version
    }

    /// The public key of the client as a COSE_Key.
    pub fn platform_key_agreement(&self) -> Value {
        // NB: kty (1) is EC2, alg (3) is ECDH-ES+HKDF-256, which the specification asks for though it is not what
        //     the protocol does, and crv (-1) is P-256.
        Value::Map(vec![
            (Value::Integer(1), Value::Integer(2)),
            (Value::Integer(3), Value::Integer(-25)),
            (Value::Integer(-1), Value::Integer(1)),
            (Value::Integer(-2), Value::Bytes(self.public_x.clone())),
            (Value::Integer(-3), Value::Bytes(self.public_y.clone())),
        ])
    }

    pub fn encrypt(&self, plaintext: &[u8], host: &mut dyn Host) -> Result<Vec<u8>, ProtocolError> {
        let mut initialization_vector = [0; INITIALIZATION_VECTOR_SIZE];
        // NB: Protocol one has an initialization vector of zero bytes, and protocol two sends a random one first.
        if self.version == Version::Two {
            host.fill_with_random(&mut initialization_vector);
        }
        let ciphertext = aes_cbc_cipher_without_padding(true, &self.aes_key, plaintext, &initialization_vector)
            .map_err(|_| ProtocolError)?;
        Ok(match self.version {
            Version::One => ciphertext,
            Version::Two => [&initialization_vector[..], &ciphertext].concat(),
        })
    }

    pub fn decrypt(&self, ciphertext: &[u8]) -> Result<Secret, ProtocolError> {
        let (initialization_vector, ciphertext) = match self.version {
            Version::One => (&[0; INITIALIZATION_VECTOR_SIZE][..], ciphertext),
            Version::Two => ciphertext
                .split_at_checked(INITIALIZATION_VECTOR_SIZE)
                .ok_or(ProtocolError)?,
        };
        aes_cbc_cipher_without_padding(false, &self.aes_key, ciphertext, initialization_vector)
            .map(Secret::new)
            .map_err(|_| ProtocolError)
    }
}

/// The pinUvAuthParam for a message: its HMAC-SHA-256 with the token, of which protocol one takes the first 16 bytes.
pub fn authenticate(version: Version, token: &[u8], message: &[u8]) -> Result<Vec<u8>, ProtocolError> {
    let mut digest = hmac_sign(ShaHashKind::Sha256, token, message).map_err(|_| ProtocolError)?;
    if version == Version::One {
        digest.truncate(16);
    }
    Ok(digest)
}
