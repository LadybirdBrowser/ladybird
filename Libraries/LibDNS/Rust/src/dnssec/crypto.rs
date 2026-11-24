/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libcrypto_rust::{RsaSignaturePadding, ShaHashKind};

use crate::message::{Algorithm, DigestType, Dnskey, DomainName, Ds, Nsec3, Nsec3HashAlgorithm};

pub(super) fn is_supported_algorithm(algorithm: Algorithm) -> bool {
    matches!(
        algorithm,
        Algorithm::RSASHA1
            | Algorithm::RSASHA1NSEC3SHA1
            | Algorithm::RSASHA256
            | Algorithm::RSASHA512
            | Algorithm::ECDSAP256SHA256
            | Algorithm::ECDSAP384SHA384
            | Algorithm::ED25519
    )
}

pub(super) fn is_supported_digest_type(digest_type: DigestType) -> bool {
    matches!(digest_type, DigestType::SHA1 | DigestType::SHA256 | DigestType::SHA384)
}

// RFC 4034, 2.1.1. The Flags Field / 2.1.2. The Protocol Field.
// If bit 7 has value 0, then the DNSKEY record holds some other type of DNS public key and MUST NOT be used to
// verify RRSIGs that cover RRsets.
// The Protocol Field MUST have value 3, and the DNSKEY RR MUST be treated as invalid during signature verification
// if it is found to be some value other than 3.
pub(super) fn is_usable_zone_key(key: &Dnskey) -> bool {
    key.is_zone_key() && key.protocol == 3 && !key.is_revoked()
}

fn without_leading_zeros(bytes: &[u8]) -> &[u8] {
    let start = bytes.iter().position(|byte| *byte != 0).unwrap_or(bytes.len());
    &bytes[start..]
}

// RFC 3110, 3. RSA/SHA1 SIG Resource Records.
// The size of "n", including most and least significant bits (which will be 1) MUST be not less than 512 bits and not
// more than 4096 bits.
// We do not trust RSA keys with a modulus shorter than 1024 bits, nor spend time on ones longer than 8192 bits or with a
// public exponent longer than 33 bits. A signature by such a key never validates, so data signed only with one fails
// validation, unlike data signed with an unsupported algorithm, which is Insecure.
const MIN_RSA_MODULUS_BITS: usize = 1024;
const MAX_RSA_MODULUS_BITS: usize = 8192;
const MAX_RSA_PUBLIC_EXPONENT_BITS: usize = 33;

fn bit_length(bytes: &[u8]) -> usize {
    let bytes = without_leading_zeros(bytes);
    bytes
        .first()
        .map_or(0, |first| bytes.len() * 8 - first.leading_zeros() as usize)
}

pub(super) fn verify_signature(key: &Dnskey, signed_data: &[u8], signature: &[u8]) -> bool {
    match key.algorithm {
        Algorithm::RSASHA1 | Algorithm::RSASHA1NSEC3SHA1 | Algorithm::RSASHA256 | Algorithm::RSASHA512 => {
            let hash = match key.algorithm {
                Algorithm::RSASHA256 => ShaHashKind::Sha256,
                Algorithm::RSASHA512 => ShaHashKind::Sha512,
                _ => ShaHashKind::Sha1,
            };
            let Some(components) = key.rsa_public_key_components() else {
                return false;
            };
            let (modulus, exponent) = (
                without_leading_zeros(components.modulus),
                without_leading_zeros(components.exponent),
            );
            if !(MIN_RSA_MODULUS_BITS..=MAX_RSA_MODULUS_BITS).contains(&bit_length(modulus))
                || bit_length(exponent) > MAX_RSA_PUBLIC_EXPONENT_BITS
            {
                return false;
            }
            let Some(public_key) = libcrypto_rust::rsa_key_from_components(false, &[modulus, exponent]) else {
                return false;
            };
            libcrypto_rust::rsa_verify(hash, &public_key, RsaSignaturePadding::Pkcs1, signed_data, signature)
                .unwrap_or(false)
        }
        Algorithm::ECDSAP256SHA256 | Algorithm::ECDSAP384SHA384 => {
            // RFC 6605, 4. DNSKEY and RRSIG Resource Records for ECDSA.
            // ECDSA public keys consist of a single value, called "Q" in FIPS 186-3.  In DNSSEC keys, Q is a simple bit
            // string that represents the uncompressed form of a curve point, "x | y".
            // The two integers, both of which have length n bits, are concatenated to form "r | s".
            let (scalar_size, hash) = if key.algorithm == Algorithm::ECDSAP256SHA256 {
                (32, ShaHashKind::Sha256)
            } else {
                (48, ShaHashKind::Sha384)
            };
            if key.public_key.len() != scalar_size * 2 || signature.len() != scalar_size * 2 {
                return false;
            }
            let mut point = Vec::with_capacity(1 + key.public_key.len());
            point.push(0x04);
            point.extend_from_slice(&key.public_key);
            let Some(public_key) = libcrypto_rust::ec_parse_raw_public_key(&point) else {
                return false;
            };
            let Ok(digest) = libcrypto_rust::sha_digest(hash, signed_data) else {
                return false;
            };
            libcrypto_rust::ec_verify(&public_key, &digest, signature).unwrap_or(false)
        }
        Algorithm::ED25519 => {
            let Ok(public_key) =
                libcrypto_rust::okp_key_from_raw(libcrypto_rust::OkpCurve::Ed25519, false, &key.public_key)
            else {
                return false;
            };
            libcrypto_rust::okp_verify(&public_key, signature, signed_data, &[]).unwrap_or(false)
        }
        _ => false,
    }
}

fn ds_digest(owner: &DomainName, key: &Dnskey, digest_type: DigestType) -> Option<Vec<u8>> {
    // RFC 4034, 5.1.4. The Digest Field.
    // digest = digest_algorithm( DNSKEY owner name | DNSKEY RDATA);
    // DNSKEY RDATA = Flags | Protocol | Algorithm | Public Key.
    let mut input = owner.canonicalized().to_wire().ok()?;
    key.write_wire(&mut input);
    let hash = match digest_type {
        DigestType::SHA1 => ShaHashKind::Sha1,
        DigestType::SHA256 => ShaHashKind::Sha256,
        DigestType::SHA384 => ShaHashKind::Sha384,
        _ => return None,
    };
    libcrypto_rust::sha_digest(hash, &input).ok()
}

pub(super) fn ds_matches_key(owner: &DomainName, ds: &Ds, key: &Dnskey) -> bool {
    if ds.key_tag != key.calculated_key_tag() || ds.algorithm != key.algorithm {
        return false;
    }
    ds_digest(owner, key, ds.digest_type).is_some_and(|digest| digest == ds.digest)
}

// RFC 5155, 5. Calculation of the Hash.
// IH(salt, x, 0) = H(x || salt), and
// IH(salt, x, k) = H(IH(salt, x, k-1) || salt), if k > 0
pub(super) fn nsec3_hash(name: &DomainName, params: &Nsec3) -> Option<Vec<u8>> {
    if params.hash_algorithm != Nsec3HashAlgorithm::SHA1 {
        return None;
    }

    let mut input = name.canonicalized().to_wire().ok()?;
    input.extend_from_slice(&params.salt);
    let mut hash = libcrypto_rust::sha_digest(ShaHashKind::Sha1, &input).ok()?;
    for _ in 0..params.iterations {
        hash.extend_from_slice(&params.salt);
        hash = libcrypto_rust::sha_digest(ShaHashKind::Sha1, &hash).ok()?;
    }
    Some(hash)
}
