/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::{Span, SpanMut, Utf16String};

mod ffi;
mod key;
mod key_info;

pub use key::{KeyComponents, KeyDecodeError, KeyHandle, KeyPair};
pub use key_info::{Asn1Value, KeyInfo, decode_asn1_structure};

use ffi::ReceiveBytes;

#[derive(Debug)]
pub struct BackendError;

#[repr(i32)]
pub enum ShaHashKind {
    Sha1 = 4,
    Sha256,
    Sha384,
    Sha512,
    Sha3_256,
    Sha3_384,
    Sha3_512,
}

#[repr(i32)]
pub enum CShakeKind {
    CShake128,
    CShake256,
}

#[repr(i32)]
#[derive(Clone, Copy)]
pub enum KmacKind {
    Kmac128,
    Kmac256,
}

#[repr(i32)]
pub enum Argon2Type {
    Argon2d,
    Argon2i,
    Argon2id,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum EcCurve {
    P256,
    P384,
    P521,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum OkpCurve {
    Ed25519,
    Ed448,
    X25519,
    X448,
}

#[repr(u8)]
pub enum RsaJwkInteger {
    Modulus,
    PublicExponent,
    PrivateExponent,
    Prime1,
    Prime2,
    Exponent1,
    Exponent2,
    Coefficient,
}

#[derive(Clone, Copy)]
#[repr(i32)]
pub enum MlDsaSize {
    MlDsa44,
    MlDsa65,
    MlDsa87,
}

#[derive(Clone, Copy)]
#[repr(i32)]
pub enum MlKemSize {
    MlKem512,
    MlKem768,
    MlKem1024,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum Asn1Structure {
    SubjectPublicKeyInfo,
    PrivateKeyInfo,
    OctetString,
}

pub struct Argon2Inputs<'a> {
    pub password: &'a [u8],
    pub nonce: &'a [u8],
    pub memory: u32,
    pub passes: u32,
    pub parallelism: u32,
    pub version: u32,
    pub secret_value: Option<&'a [u8]>,
    pub associated_data: Option<&'a [u8]>,
    pub tag_length: u32,
}

unsafe extern "C" fn receive_bytes(state: *mut c_void, bytes: Span<'_, u8>) {
    // SAFETY: The native call supplies the output slot and lends the bytes during the callback.
    unsafe { *state.cast::<Vec<u8>>() = bytes.as_slice().to_vec() };
}

fn receive_native_bytes<T>(call: impl FnOnce(*mut c_void, ReceiveBytes) -> T) -> (T, Vec<u8>) {
    let mut bytes = Vec::new();
    let status = call(core::ptr::from_mut(&mut bytes).cast(), receive_bytes);
    (status, bytes)
}

fn result_from_error(raw: usize) -> Result<(), Utf16String> {
    if raw == 0 {
        Ok(())
    } else {
        // SAFETY: A nonzero error returned by LibCrypto transfers an owned string.
        Err(unsafe { Utf16String::from_raw_owned(raw) })
    }
}

pub fn fill_with_random(bytes: &mut [u8]) {
    // SAFETY: The native function writes into the span during the call.
    unsafe { ffi::crypto_fill_with_random(SpanMut::new(bytes)) };
}

pub fn generate_random_uuid() -> Utf16String {
    // SAFETY: The native function invokes the receiver synchronously.
    let ((), bytes) =
        receive_native_bytes(|state, receive| unsafe { ffi::crypto_generate_random_uuid(state, receive) });
    Utf16String::from_ascii(&bytes)
}

#[derive(Debug)]
pub enum Base64DecodeError {
    OutOfMemory,
    InvalidBase64(Utf16String),
    NonAscii,
}

pub fn encode_base64url(bytes: &[u8]) -> Result<Utf16String, i32> {
    // SAFETY: The input is borrowed and the receiver is invoked synchronously.
    let (status, encoded) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_encode_base64url(Span::new(bytes), receive, state)
    });
    if status != 0 {
        return Err(status);
    }
    Ok(Utf16String::from_ascii(&encoded))
}

pub fn decode_base64url(input: &Utf16String) -> Result<Vec<u8>, Base64DecodeError> {
    use ak::Utf16StringUnits;
    let is_ascii = match input.as_units() {
        Utf16StringUnits::Ascii(_) => true,
        Utf16StringUnits::Latin1(units) => units.is_ascii(),
        Utf16StringUnits::Utf16(units) => units.iter().all(|&unit| unit <= 0x7f),
    };
    if !is_ascii {
        return Err(Base64DecodeError::NonAscii);
    }
    let mut message = 0;
    // SAFETY: The string and output slots are live during the call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_decode_base64url(input.raw_identity(), state, receive, &mut message)
    });
    match status {
        ffi::Base64Status::Success => Ok(bytes),
        ffi::Base64Status::OutOfMemory => Err(Base64DecodeError::OutOfMemory),
        ffi::Base64Status::InvalidBase64 => {
            // SAFETY: The decoder transfers an owned diagnostic string for this status.
            Err(Base64DecodeError::InvalidBase64(unsafe {
                Utf16String::from_raw_owned(message)
            }))
        }
        ffi::Base64Status::NonAscii => Err(Base64DecodeError::NonAscii),
    }
}

pub fn sha_digest(kind: ShaHashKind, data: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) =
        receive_native_bytes(|state, receive| unsafe { ffi::crypto_sha_digest(kind, Span::new(data), state, receive) });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn cshake_validate_parameters(
    output_length: u32,
    customization: Option<&[u8]>,
    function_name: Option<&[u8]>,
) -> Result<(), Utf16String> {
    // SAFETY: The input is borrowed for the call.
    result_from_error(unsafe {
        ffi::crypto_cshake_validate_parameters(output_length, customization.into(), function_name.into())
    })
}

pub fn cshake_digest(
    kind: CShakeKind,
    data: &[u8],
    output_length: u32,
    customization: Option<&[u8]>,
    function_name: Option<&[u8]>,
) -> Result<Vec<u8>, Utf16String> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_cshake_digest(
            kind,
            Span::new(data),
            output_length,
            customization.into(),
            function_name.into(),
            state,
            receive,
        )
    });
    result_from_error(status)?;
    Ok(bytes)
}

pub fn hmac_sign(kind: ShaHashKind, key: &[u8], message: &[u8]) -> Vec<u8> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let ((), bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_hmac_sign(kind, Span::new(key), Span::new(message), state, receive);
    });
    bytes
}

pub fn timing_safe_compare(left: &[u8], right: &[u8]) -> bool {
    // SAFETY: The input is borrowed for the call.
    unsafe { ffi::crypto_timing_safe_compare(Span::new(left), Span::new(right)) }
}

pub fn kmac_default_key_length(kind: KmacKind) -> u32 {
    // SAFETY: The input is borrowed for the call.
    unsafe { ffi::crypto_kmac_default_key_length(kind) }
}

pub fn kmac_validate_parameters(output_length: u32, customization: Option<&[u8]>) -> bool {
    // SAFETY: The input is borrowed for the call.
    unsafe { ffi::crypto_kmac_validate_parameters(output_length, customization.into()) }
}

pub fn kmac_sign(
    kind: KmacKind,
    key: &[u8],
    message: &[u8],
    output_length: u32,
    customization: Option<&[u8]>,
) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_kmac_sign(
            kind,
            Span::new(key),
            Span::new(message),
            output_length,
            customization.into(),
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn hash_digest_size(kind: ShaHashKind) -> usize {
    // SAFETY: The input is borrowed for the call.
    unsafe { ffi::crypto_hash_digest_size(kind) }
}

pub fn hkdf_derive_key(
    kind: ShaHashKind,
    salt: &[u8],
    key: &[u8],
    info: &[u8],
    length: u32,
) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_hkdf_derive_key(
            kind,
            Span::new(salt),
            Span::new(key),
            Span::new(info),
            length,
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn pbkdf2_derive_key(
    kind: ShaHashKind,
    password: &[u8],
    salt: &[u8],
    iterations: u32,
    length: u32,
) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_pbkdf2_derive_key(
            kind,
            Span::new(password),
            Span::new(salt),
            iterations,
            length,
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn argon2_validate_parameters(nonce: &[u8]) -> Result<(), Utf16String> {
    // SAFETY: The input is borrowed for the call.
    result_from_error(unsafe { ffi::crypto_argon2_validate_parameters(Span::new(nonce)) })
}

pub fn argon2_derive_key(kind: Argon2Type, inputs: &Argon2Inputs<'_>) -> Result<Vec<u8>, Utf16String> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_argon2_derive_key(kind, &inputs.into(), state, receive)
    });
    result_from_error(status)?;
    Ok(bytes)
}

pub fn aes_cbc_cipher(encrypt: bool, key: &[u8], input: &[u8], iv: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_aes_cbc_cipher(encrypt, Span::new(key), Span::new(input), Span::new(iv), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

/// Increments only the low `counter_length` bits of the counter block, modulo 2^counter_length, as the standard
/// incrementing function of NIST SP 800-38A Appendix B.1 does.
pub fn aes_ctr_cipher(
    encrypt: bool,
    key: &[u8],
    input: &[u8],
    counter: &[u8],
    counter_length: u8,
) -> Result<Vec<u8>, BackendError> {
    let counter_block: [u8; 16] = counter.try_into().map_err(|_| BackendError)?;
    if counter_length == 0 || counter_length > 128 {
        return Err(BackendError);
    }

    // The native cipher increments the whole block, so split the input where the low bits wrap around.
    let mask = u128::MAX >> (128 - u32::from(counter_length));
    let mut block = u128::from_be_bytes(counter_block);
    let mut output = Vec::with_capacity(input.len());
    let mut remaining = input;
    while !remaining.is_empty() {
        let blocks_until_wrap = (mask - (block & mask)).checked_add(1);
        let segment_length = match blocks_until_wrap.and_then(|blocks| blocks.checked_mul(16)) {
            Some(bytes) if bytes < remaining.len() as u128 => bytes as usize,
            _ => remaining.len(),
        };
        let (segment, rest) = remaining.split_at(segment_length);
        output.extend(aes_ctr_cipher_whole_block_counter(
            encrypt,
            key,
            segment,
            &block.to_be_bytes(),
        )?);
        remaining = rest;
        block &= !mask;
    }
    Ok(output)
}

fn aes_ctr_cipher_whole_block_counter(
    encrypt: bool,
    key: &[u8],
    input: &[u8],
    counter: &[u8],
) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_aes_ctr_cipher(
            encrypt,
            Span::new(key),
            Span::new(input),
            Span::new(counter),
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn aes_kw_cipher(wrap: bool, key: &[u8], input: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_aes_kw_cipher(wrap, Span::new(key), Span::new(input), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn aes_gcm_encrypt(
    key: &[u8],
    plaintext: &[u8],
    iv: &[u8],
    additional_data: &[u8],
    tag_length: usize,
) -> Result<(Vec<u8>, Vec<u8>), BackendError> {
    let mut first = Vec::new();
    let mut second = Vec::new();
    // SAFETY: Inputs and both output slots are borrowed for the call.
    let status = unsafe {
        ffi::crypto_aes_gcm_encrypt(
            Span::new(key),
            Span::new(plaintext),
            Span::new(iv),
            Span::new(additional_data),
            tag_length,
            core::ptr::from_mut(&mut first).cast(),
            core::ptr::from_mut(&mut second).cast(),
            receive_bytes,
        )
    };
    status.then_some(()).ok_or(BackendError)?;
    Ok((first, second))
}

pub fn aes_gcm_decrypt(
    key: &[u8],
    ciphertext: &[u8],
    iv: &[u8],
    additional_data: &[u8],
    tag: &[u8],
) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_aes_gcm_decrypt(
            Span::new(key),
            Span::new(ciphertext),
            Span::new(iv),
            Span::new(additional_data),
            Span::new(tag),
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn aes_ocb_cipher(
    encrypt: bool,
    key: &[u8],
    input: &[u8],
    iv: &[u8],
    additional_data: &[u8],
    tag_length: usize,
) -> Result<Vec<u8>, Utf16String> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_aes_ocb_cipher(
            encrypt,
            Span::new(key),
            Span::new(input),
            Span::new(iv),
            Span::new(additional_data),
            tag_length,
            state,
            receive,
        )
    });
    result_from_error(status)?;
    Ok(bytes)
}

pub fn chacha20_poly1305_cipher(
    encrypt: bool,
    key: &[u8],
    iv: &[u8],
    input: &[u8],
    additional_data: &[u8],
) -> Result<Vec<u8>, Utf16String> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_chacha20_poly1305_cipher(
            encrypt,
            Span::new(key),
            Span::new(iv),
            Span::new(input),
            Span::new(additional_data),
            state,
            receive,
        )
    });
    result_from_error(status)?;
    Ok(bytes)
}

pub fn rsa_key_is_valid(key: &KeyHandle) -> Result<bool, BackendError> {
    let mut value = false;
    // SAFETY: Inputs and output are borrowed for the call.
    let status = unsafe { ffi::crypto_rsa_key_is_valid(key.as_raw(), &mut value) };
    status.then_some(()).ok_or(BackendError)?;
    Ok(value)
}

pub fn rsa_validate_public_exponent(exponent: &[u8], modulus_length: u32) -> bool {
    // SAFETY: The input is borrowed for the call.
    unsafe { ffi::crypto_rsa_validate_public_exponent(Span::new(exponent), modulus_length) }
}

pub fn rsa_validate_modulus_length(modulus_length: u32) -> bool {
    // SAFETY: The input is borrowed for the call.
    unsafe { ffi::crypto_rsa_validate_modulus_length(modulus_length) }
}

pub fn rsa_generate_key_pair(modulus_length: u32, exponent: &[u8]) -> Result<KeyPair, BackendError> {
    let mut public_key = core::ptr::null_mut();
    let mut private_key = core::ptr::null_mut();
    // SAFETY: Inputs and output slots are borrowed for the call.
    let status = unsafe {
        ffi::crypto_rsa_generate_key_pair(modulus_length, Span::new(exponent), &mut public_key, &mut private_key)
    };
    status.then_some(()).ok_or(BackendError)?;
    // SAFETY: Success transfers ownership of both keys.
    Ok(unsafe { KeyPair::from_raw(public_key, private_key) })
}

pub fn rsa_export_public_exponent(exponent: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_rsa_export_public_exponent(Span::new(exponent), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn rsa_export_der(key: &KeyHandle, is_public: bool) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_rsa_export_der(key.as_raw(), is_public, state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn rsa_export_jwk_integer(key: &KeyHandle, component: RsaJwkInteger) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_rsa_export_jwk_integer(key.as_raw(), component, state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn rsa_oaep_cipher(
    encrypt: bool,
    hash_kind: ShaHashKind,
    key: &KeyHandle,
    label: &[u8],
    input: &[u8],
) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_rsa_oaep_cipher(
            encrypt,
            hash_kind,
            key.as_raw(),
            Span::new(label),
            Span::new(input),
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn rsa_pss_sign(
    hash_kind: ShaHashKind,
    key: &KeyHandle,
    salt_length: u32,
    message: &[u8],
) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_rsa_pss_sign(hash_kind, key.as_raw(), salt_length, Span::new(message), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn rsa_pss_verify(
    hash_kind: ShaHashKind,
    key: &KeyHandle,
    salt_length: u32,
    message: &[u8],
    signature: &[u8],
) -> Result<bool, BackendError> {
    let mut value = false;
    // SAFETY: Inputs and output are borrowed for the call.
    let status = unsafe {
        ffi::crypto_rsa_pss_verify(
            hash_kind,
            key.as_raw(),
            salt_length,
            Span::new(message),
            Span::new(signature),
            &mut value,
        )
    };
    status.then_some(()).ok_or(BackendError)?;
    Ok(value)
}

pub fn rsa_pkcs1_sign(hash_kind: ShaHashKind, key: &KeyHandle, message: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_rsa_pkcs1_sign(hash_kind, key.as_raw(), Span::new(message), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn rsa_pkcs1_verify(
    hash_kind: ShaHashKind,
    key: &KeyHandle,
    message: &[u8],
    signature: &[u8],
) -> Result<bool, BackendError> {
    let mut value = false;
    // SAFETY: Inputs and output are borrowed for the call.
    let status = unsafe {
        ffi::crypto_rsa_pkcs1_verify(
            hash_kind,
            key.as_raw(),
            Span::new(message),
            Span::new(signature),
            &mut value,
        )
    };
    status.then_some(()).ok_or(BackendError)?;
    Ok(value)
}

pub fn ec_generate_key_pair(curve: EcCurve) -> Result<KeyPair, BackendError> {
    let mut public_key = core::ptr::null_mut();
    let mut private_key = core::ptr::null_mut();
    // SAFETY: Inputs and output slots are borrowed for the call.
    let status = unsafe { ffi::crypto_ec_generate_key_pair(curve, &mut public_key, &mut private_key) };
    status.then_some(()).ok_or(BackendError)?;
    // SAFETY: Success transfers ownership of both keys.
    Ok(unsafe { KeyPair::from_raw(public_key, private_key) })
}

pub fn ec_verify(curve: EcCurve, key: &KeyHandle, digest: &[u8], r: &[u8], s: &[u8]) -> Result<bool, Utf16String> {
    let mut value = false;
    // SAFETY: Inputs and output are borrowed for the call.
    let status = unsafe {
        ffi::crypto_ec_verify(
            curve,
            key.as_raw(),
            Span::new(digest),
            Span::new(r),
            Span::new(s),
            &mut value,
        )
    };
    result_from_error(status)?;
    Ok(value)
}

pub fn ec_public_key_scalar_size(key: &KeyHandle) -> usize {
    // SAFETY: The input is borrowed for the call.
    unsafe { ffi::crypto_ec_public_key_scalar_size(key.as_raw()) }
}

pub fn ec_key_from_coordinates(x: &[u8], y: &[u8], coord_size: usize, d: Option<&[u8]>) -> KeyHandle {
    // SAFETY: The input is borrowed for the call.
    let raw = unsafe { ffi::crypto_ec_key_from_coordinates(Span::new(x), Span::new(y), coord_size, d.into()) };
    // SAFETY: The native operation transfers ownership of the key.
    unsafe { KeyHandle::from_raw(raw) }.expect("LibCrypto returns a key")
}

pub fn ec_parse_raw_public_key(bytes: &[u8]) -> Option<KeyHandle> {
    // SAFETY: The input is borrowed for the call.
    let raw = unsafe { ffi::crypto_ec_parse_raw_public_key(Span::new(bytes)) };
    // SAFETY: The native operation transfers ownership of the key.
    unsafe { KeyHandle::from_raw(raw) }
}

pub fn ec_key_is_valid(curve: EcCurve, key: &KeyHandle) -> Result<bool, BackendError> {
    let mut value = false;
    // SAFETY: Inputs and output are borrowed for the call.
    let status = unsafe { ffi::crypto_ec_key_is_valid(curve, key.as_raw(), &mut value) };
    status.then_some(()).ok_or(BackendError)?;
    Ok(value)
}

pub fn ec_export_der(curve: EcCurve, key: &KeyHandle, is_public: bool) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_ec_export_der(curve, key.as_raw(), is_public, state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn ec_export_uncompressed(key: &KeyHandle) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_ec_export_uncompressed(key.as_raw(), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn ec_export_public_coordinates(curve: EcCurve, key: &KeyHandle) -> Result<(Vec<u8>, Vec<u8>), BackendError> {
    let mut first = Vec::new();
    let mut second = Vec::new();
    // SAFETY: Inputs and both output slots are borrowed for the call.
    let status = unsafe {
        ffi::crypto_ec_export_public_coordinates(
            curve,
            key.as_raw(),
            core::ptr::from_mut(&mut first).cast(),
            core::ptr::from_mut(&mut second).cast(),
            receive_bytes,
        )
    };
    status.then_some(()).ok_or(BackendError)?;
    Ok((first, second))
}

pub fn ec_export_private_scalar(key: &KeyHandle) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_ec_export_private_scalar(key.as_raw(), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn okp_generate_private_key(curve: OkpCurve) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) =
        receive_native_bytes(|state, receive| unsafe { ffi::crypto_okp_generate_private_key(curve, state, receive) });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn okp_generate_public_key(curve: OkpCurve, private_key: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_okp_generate_public_key(curve, Span::new(private_key), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn okp_compute_coordinate(curve: OkpCurve, private_key: &[u8], public_key: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_okp_compute_coordinate(curve, Span::new(private_key), Span::new(public_key), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn okp_sign(curve: OkpCurve, private_key: &[u8], message: &[u8], context: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_okp_sign(
            curve,
            Span::new(private_key),
            Span::new(message),
            Span::new(context),
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn okp_verify(
    curve: OkpCurve,
    public_key: &[u8],
    signature: &[u8],
    message: &[u8],
    context: &[u8],
) -> Result<bool, Utf16String> {
    let mut value = false;
    // SAFETY: Inputs and output are borrowed for the call.
    let status = unsafe {
        ffi::crypto_okp_verify(
            curve,
            Span::new(public_key),
            Span::new(signature),
            Span::new(message),
            Span::new(context),
            &mut value,
        )
    };
    result_from_error(status)?;
    Ok(value)
}

pub fn okp_export_der(curve: OkpCurve, is_public: bool, key: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_okp_export_der(curve, is_public, Span::new(key), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn mldsa_sign(size: MlDsaSize, key: &KeyHandle, message: &[u8], context: &[u8]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_mldsa_sign(
            size,
            key.as_raw(),
            Span::new(message),
            Span::new(context),
            state,
            receive,
        )
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn mldsa_verify(
    size: MlDsaSize,
    key: &KeyHandle,
    message: &[u8],
    signature: &[u8],
    context: &[u8],
) -> Result<bool, BackendError> {
    let mut value = false;
    // SAFETY: Inputs and output are borrowed for the call.
    let status = unsafe {
        ffi::crypto_mldsa_verify(
            size,
            key.as_raw(),
            Span::new(message),
            Span::new(signature),
            Span::new(context),
            &mut value,
        )
    };
    status.then_some(()).ok_or(BackendError)?;
    Ok(value)
}

pub fn mldsa_generate_key_pair(size: MlDsaSize, seed: &[u8]) -> Result<KeyPair, Utf16String> {
    let mut public_key = core::ptr::null_mut();
    let mut private_key = core::ptr::null_mut();
    // SAFETY: Inputs and output slots are borrowed for the call.
    let status =
        unsafe { ffi::crypto_mldsa_generate_key_pair(size, Span::new(seed), &mut public_key, &mut private_key) };
    result_from_error(status)?;
    // SAFETY: Success transfers ownership of both keys.
    Ok(unsafe { KeyPair::from_raw(public_key, private_key) })
}

pub fn mldsa_read_public_key(bytes: &[u8]) -> KeyHandle {
    // SAFETY: The input is borrowed for the call.
    let raw = unsafe { ffi::crypto_mldsa_read_public_key(Span::new(bytes)) };
    // SAFETY: The native operation transfers ownership of the key.
    unsafe { KeyHandle::from_raw(raw) }.expect("LibCrypto returns a key")
}

pub fn mldsa_public_bytes(key: &KeyHandle) -> &[u8] {
    let mut bytes = Span::default();
    // SAFETY: The key owns the bytes returned by the native call.
    unsafe { ffi::crypto_mldsa_public_bytes(key.as_raw(), &mut bytes) };
    bytes.as_slice()
}

pub fn mldsa_seed_bytes(key: &KeyHandle) -> &[u8] {
    let mut bytes = Span::default();
    // SAFETY: The key owns the bytes returned by the native call.
    unsafe { ffi::crypto_mldsa_seed_bytes(key.as_raw(), &mut bytes) };
    bytes.as_slice()
}

pub fn mldsa_export_der(key: &KeyHandle, is_public: bool, oid: &[u32]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_mldsa_export_der(key.as_raw(), is_public, Span::new(oid), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn mlkem_generate_key_pair(size: MlKemSize, seed: &[u8]) -> Result<KeyPair, Utf16String> {
    let mut public_key = core::ptr::null_mut();
    let mut private_key = core::ptr::null_mut();
    // SAFETY: Inputs and output slots are borrowed for the call.
    let status =
        unsafe { ffi::crypto_mlkem_generate_key_pair(size, Span::new(seed), &mut public_key, &mut private_key) };
    result_from_error(status)?;
    // SAFETY: Success transfers ownership of both keys.
    Ok(unsafe { KeyPair::from_raw(public_key, private_key) })
}

pub fn mlkem_read_public_key(bytes: &[u8]) -> KeyHandle {
    // SAFETY: The input is borrowed for the call.
    let raw = unsafe { ffi::crypto_mlkem_read_public_key(Span::new(bytes)) };
    // SAFETY: The native operation transfers ownership of the key.
    unsafe { KeyHandle::from_raw(raw) }.expect("LibCrypto returns a key")
}

pub fn mlkem_public_bytes(key: &KeyHandle) -> Vec<u8> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let ((), bytes) =
        receive_native_bytes(|state, receive| unsafe { ffi::crypto_mlkem_public_bytes(key.as_raw(), state, receive) });
    bytes
}

pub fn mlkem_seed_bytes(key: &KeyHandle) -> &[u8] {
    let mut bytes = Span::default();
    // SAFETY: The key owns the bytes returned by the native call.
    unsafe { ffi::crypto_mlkem_seed_bytes(key.as_raw(), &mut bytes) };
    bytes.as_slice()
}

pub fn mlkem_export_der(key: &KeyHandle, is_public: bool, oid: &[u32]) -> Result<Vec<u8>, BackendError> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_mlkem_export_der(key.as_raw(), is_public, Span::new(oid), state, receive)
    });
    status.then_some(()).ok_or(BackendError)?;
    Ok(bytes)
}

pub fn mlkem_encapsulate(size: MlKemSize, key: &KeyHandle) -> Result<(Vec<u8>, Vec<u8>), Utf16String> {
    let mut first = Vec::new();
    let mut second = Vec::new();
    // SAFETY: Inputs and both output slots are borrowed for the call.
    let status = unsafe {
        ffi::crypto_mlkem_encapsulate(
            size,
            key.as_raw(),
            core::ptr::from_mut(&mut first).cast(),
            core::ptr::from_mut(&mut second).cast(),
            receive_bytes,
        )
    };
    result_from_error(status)?;
    Ok((first, second))
}

pub fn mlkem_decapsulate(size: MlKemSize, key: &KeyHandle, ciphertext: &[u8]) -> Result<Vec<u8>, Utf16String> {
    // SAFETY: Inputs and output are borrowed for the synchronous native call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_mlkem_decapsulate(size, key.as_raw(), Span::new(ciphertext), state, receive)
    });
    result_from_error(status)?;
    Ok(bytes)
}

pub fn public_key_for_private_key(
    key: &KeyHandle,
    name: &Utf16String,
    named_curve: Option<&Utf16String>,
) -> Option<KeyHandle> {
    let named_curve = named_curve.map_or(0, Utf16String::raw_identity);
    // SAFETY: The inputs are borrowed for the call.
    let raw = unsafe { ffi::crypto_public_key_for_private_key(key.as_raw(), name.raw_identity(), named_curve) };
    // SAFETY: A non-null result transfers ownership of the key.
    unsafe { KeyHandle::from_raw(raw) }
}

pub fn rsa_read_key_metadata(key: &KeyHandle) -> Result<(u32, Vec<u8>), BackendError> {
    let mut modulus_length = 0;
    // SAFETY: The key and output slots live until the native call returns.
    let (success, exponent) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_rsa_read_key_metadata(key.as_raw(), &mut modulus_length, state, receive)
    });
    success.then_some((modulus_length, exponent)).ok_or(BackendError)
}

pub fn rsa_key_from_components(is_private: bool, components: &[&[u8]]) -> Option<KeyHandle> {
    if (is_private && !matches!(components.len(), 3 | 8)) || (!is_private && components.len() != 2) {
        return None;
    }
    let mut spans = [Span::default(); 8];
    for (span, component) in spans.iter_mut().zip(components) {
        *span = Span::new(component);
    }
    // SAFETY: The spans are live for the call and contain the required components.
    let raw = unsafe { ffi::crypto_rsa_key_from_jwk_components(is_private, Span::new(&spans[..components.len()])) };
    // SAFETY: A non-null result transfers ownership of the key.
    unsafe { KeyHandle::from_raw(raw) }
}

#[derive(Debug)]
pub enum EcError {
    Operation(Utf16String),
    CoordinateEncoding,
}

fn ec_result(status: u8, error: usize) -> Result<(), EcError> {
    match status {
        0 => Ok(()),
        1 => result_from_error(error).map_err(EcError::Operation),
        _ => Err(EcError::CoordinateEncoding),
    }
}

pub fn ec_sign(curve: EcCurve, key: &KeyHandle, digest: &[u8]) -> Result<(Vec<u8>, Vec<u8>), EcError> {
    let mut error = 0;
    let mut r = Vec::new();
    let mut s = Vec::new();
    // SAFETY: Inputs and both output slots are live for the synchronous call.
    let status = unsafe {
        ffi::crypto_ec_sign(
            curve,
            key.as_raw(),
            Span::new(digest),
            &mut error,
            core::ptr::from_mut(&mut r).cast(),
            core::ptr::from_mut(&mut s).cast(),
            receive_bytes,
        )
    };
    ec_result(status, error)?;
    Ok((r, s))
}

pub fn ec_compute_coordinate(curve: EcCurve, key: &KeyHandle, public_key: &KeyHandle) -> Result<Vec<u8>, EcError> {
    let mut error = 0;
    // SAFETY: The key handles and output slots are live for the synchronous call.
    let (status, bytes) = receive_native_bytes(|state, receive| unsafe {
        ffi::crypto_ec_compute_coordinate(curve, key.as_raw(), public_key.as_raw(), &mut error, state, receive)
    });
    ec_result(status, error)?;
    Ok(bytes)
}
