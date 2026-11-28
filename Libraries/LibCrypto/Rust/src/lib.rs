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

use core::ffi::{CStr, c_int, c_uint};

use subtle::ConstantTimeEq;

use key::Key;
use openssl::{Cipher, CipherContext, Pkey, end_parameter, octet_string_parameter, uint_parameter, utf8_parameter};

mod der;
mod ec;
mod key;
mod key_info;
mod ml;
mod openssl;
mod rsa;
mod sys;

pub use ec::{
    ec_compute_coordinate, ec_export_der, ec_export_private_scalar, ec_export_public_coordinates,
    ec_export_uncompressed, ec_generate_key_pair, ec_key_from_coordinates, ec_key_is_valid, ec_parse_raw_public_key,
    ec_public_key_scalar_size, ec_sign, ec_verify,
};
pub use key::{HandleTag, KeyDecodeError, KeyHandle, KeyPair, SecretBytes, StoredKey};
pub use key_info::{
    KeyAlgorithm, KeyInfo, KeyMaterial, parse_octet_string, parse_private_key_info, parse_subject_public_key_info,
};
pub use ml::{
    ml_export_der, ml_seed_bytes, mldsa_generate_key_pair, mldsa_private_key_from_seed, mldsa_public_bytes,
    mldsa_read_public_key, mldsa_sign, mldsa_verify, mlkem_decapsulate, mlkem_encapsulate, mlkem_generate_key_pair,
    mlkem_private_key_from_seed, mlkem_public_bytes, mlkem_read_public_key,
};
pub use rsa::{
    RsaSignaturePadding, rsa_export_der, rsa_export_jwk_integer, rsa_export_public_exponent, rsa_generate_key_pair,
    rsa_key_from_components, rsa_key_is_valid, rsa_oaep_cipher, rsa_read_key_metadata, rsa_sign,
    rsa_validate_modulus_length, rsa_validate_public_exponent, rsa_verify,
};

/// The error of an operation: an input that it rejects, for the reason given, or a failure of OpenSSL.
#[derive(Debug)]
pub enum CryptoError {
    Invalid(&'static str),
    Backend,
}

impl CryptoError {
    pub fn message(&self) -> &'static str {
        match self {
            Self::Invalid(reason) => reason,
            Self::Backend => "The cryptographic operation failed",
        }
    }
}

#[derive(Clone, Copy)]
pub enum ShaHashKind {
    Sha1,
    Sha256,
    Sha384,
    Sha512,
    Sha3_256,
    Sha3_384,
    Sha3_512,
}

pub enum CShakeKind {
    CShake128,
    CShake256,
}

#[derive(Clone, Copy)]
pub enum KmacKind {
    Kmac128,
    Kmac256,
}

pub enum Argon2Type {
    Argon2d,
    Argon2i,
    Argon2id,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum EcCurve {
    P256,
    P384,
    P521,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum OkpCurve {
    Ed25519,
    Ed448,
    X25519,
    X448,
}

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

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MlDsaSize {
    MlDsa44,
    MlDsa65,
    MlDsa87,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MlKemSize {
    MlKem512,
    MlKem768,
    MlKem1024,
}

pub struct Argon2Inputs<'a> {
    pub password: &'a [u8],
    pub nonce: &'a [u8],
    pub memory: u32,
    pub passes: u32,
    pub parallelism: u32,
    pub secret_value: Option<&'a [u8]>,
    pub associated_data: Option<&'a [u8]>,
    pub tag_length: u32,
}

/// Overwrites `bytes` with zeros in a way the compiler does not remove.
pub fn clear_secret(bytes: &mut [u8]) {
    key::clear(bytes);
}

pub fn fill_with_random(bytes: &mut [u8]) -> Result<(), CryptoError> {
    openssl::fill_with_random(bytes)
}

impl ShaHashKind {
    /// The name of the digest, as OpenSSL takes it.
    pub(crate) fn name(&self) -> &'static CStr {
        match self {
            Self::Sha1 => c"SHA1",
            Self::Sha256 => c"SHA2-256",
            Self::Sha384 => c"SHA2-384",
            Self::Sha512 => c"SHA2-512",
            Self::Sha3_256 => c"SHA3-256",
            Self::Sha3_384 => c"SHA3-384",
            Self::Sha3_512 => c"SHA3-512",
        }
    }
}

pub fn sha_digest(kind: ShaHashKind, data: &[u8]) -> Result<Vec<u8>, CryptoError> {
    openssl::digest(kind.name(), data)
}

pub fn hash_digest_size(kind: ShaHashKind) -> usize {
    match kind {
        ShaHashKind::Sha1 => 20,
        ShaHashKind::Sha256 | ShaHashKind::Sha3_256 => 32,
        ShaHashKind::Sha384 | ShaHashKind::Sha3_384 => 48,
        ShaHashKind::Sha512 | ShaHashKind::Sha3_512 => 64,
    }
}

pub fn hmac_sign(kind: ShaHashKind, key: &[u8], message: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let params = [utf8_parameter(c"digest", kind.name()), end_parameter()];
    openssl::mac(c"HMAC", key, message, hash_digest_size(kind), &params)
}

pub fn hkdf_derive_key(
    kind: ShaHashKind,
    salt: &[u8],
    key: &[u8],
    info: &[u8],
    length: u32,
) -> Result<SecretBytes, CryptoError> {
    let params = [
        utf8_parameter(c"digest", kind.name()),
        octet_string_parameter(c"key", key),
        octet_string_parameter(c"salt", salt),
        octet_string_parameter(c"info", info),
        end_parameter(),
    ];
    let mut derived = SecretBytes::new(vec![0; length as usize]);
    openssl::kdf(c"HKDF", &params, &mut derived)?;
    Ok(derived)
}

pub fn pbkdf2_derive_key(
    kind: ShaHashKind,
    password: &[u8],
    salt: &[u8],
    iterations: u32,
    length: u32,
) -> Result<SecretBytes, CryptoError> {
    // NB: OpenSSL's PBKDF2 takes the iteration count and the lengths as int.
    let fits_in_int = |value: usize| c_int::try_from(value).is_ok();
    if ![iterations as usize, password.len(), salt.len(), length as usize]
        .into_iter()
        .all(fits_in_int)
    {
        return Err(CryptoError::Backend);
    }
    let mut iterations: c_uint = iterations;
    // NB: Web Crypto does not impose SP 800-132's lower bounds on the salt, iterations and key length, which OpenSSL
    //     checks unless asked for PKCS #5 behaviour.
    let mut pkcs5 = 1;
    let params = [
        utf8_parameter(c"digest", kind.name()),
        octet_string_parameter(c"pass", password),
        octet_string_parameter(c"salt", salt),
        uint_parameter(c"iter", &mut iterations),
        openssl::int_parameter(c"pkcs5", &mut pkcs5),
        end_parameter(),
    ];
    let mut derived = SecretBytes::new(vec![0; length as usize]);
    openssl::kdf(c"PBKDF2", &params, &mut derived)?;
    Ok(derived)
}

pub fn cshake_validate_parameters(output_length: u32) -> Result<(), CryptoError> {
    if !output_length.is_multiple_of(8) {
        return Err(CryptoError::Invalid("SHAKE output length must be a multiple of 8 bits"));
    }
    Ok(())
}

// https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-185.pdf
fn left_encode(output: &mut Vec<u8>, value: u64) {
    let bytes = value.to_be_bytes();
    let leading_zeros = bytes.iter().take_while(|&&byte| byte == 0).count().min(bytes.len() - 1);
    output.push((bytes.len() - leading_zeros) as u8);
    output.extend_from_slice(&bytes[leading_zeros..]);
}

// https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-185.pdf
fn encode_string(output: &mut Vec<u8>, string: &[u8]) {
    left_encode(output, string.len() as u64 * 8);
    output.extend_from_slice(string);
}

pub fn cshake_digest(
    kind: CShakeKind,
    data: &[u8],
    output_length: u32,
    customization: Option<&[u8]>,
    function_name: Option<&[u8]>,
) -> Result<Vec<u8>, CryptoError> {
    cshake_validate_parameters(output_length)?;
    let mut output = vec![0; (output_length / 8) as usize];
    cshake(
        kind,
        data,
        function_name.unwrap_or_default(),
        customization.unwrap_or_default(),
        &mut output,
    )?;
    Ok(output)
}

fn cshake(
    kind: CShakeKind,
    data: &[u8],
    name: &[u8],
    customization: &[u8],
    output: &mut [u8],
) -> Result<(), CryptoError> {
    // If N = "" and S = "", return SHAKE128(X, L) (or SHAKE256(X, L) for cSHAKE256).
    if name.is_empty() && customization.is_empty() {
        let shake = match kind {
            CShakeKind::CShake128 => c"SHAKE128",
            CShakeKind::CShake256 => c"SHAKE256",
        };
        return openssl::extendable_output(shake, &[data], output);
    }

    // Else return KECCAK[256](bytepad(encode_string(N) || encode_string(S), 168) || X || 00, L) for cSHAKE128, or
    // KECCAK[512](bytepad(encode_string(N) || encode_string(S), 136) || X || 00, L) for cSHAKE256.
    // NB: OpenSSL's KECCAK-KMAC digests are KECCAK[c](M || 00, L), the function that KMAC builds on.
    let (rate, keccak_name) = match kind {
        CShakeKind::CShake128 => (168, c"KECCAK-KMAC-128"),
        CShakeKind::CShake256 => (136, c"KECCAK-KMAC-256"),
    };
    let mut prefix = Vec::new();
    left_encode(&mut prefix, rate as u64);
    encode_string(&mut prefix, name);
    encode_string(&mut prefix, customization);
    prefix.resize(prefix.len().div_ceil(rate) * rate, 0);
    openssl::extendable_output(keccak_name, &[&prefix, data], output)
}

pub fn timing_safe_compare(left: &[u8], right: &[u8]) -> bool {
    left.ct_eq(right).into()
}

pub fn kmac_validate_parameters(output_length: u32, customization: Option<&[u8]>) -> bool {
    // OpenSSL's KMAC implementation rejects longer outputs and customization strings.
    const MAXIMUM_OUTPUT_LENGTH: u32 = 0xFF_FFFF / 8;
    const MAXIMUM_CUSTOMIZATION_LENGTH: usize = 512;

    output_length != 0
        && output_length.is_multiple_of(8)
        && output_length / 8 <= MAXIMUM_OUTPUT_LENGTH
        && customization.is_none_or(|customization| customization.len() <= MAXIMUM_CUSTOMIZATION_LENGTH)
}

// https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-sign
pub fn kmac_sign(
    kind: KmacKind,
    key: &[u8],
    message: &[u8],
    output_length: u32,
    customization: Option<&[u8]>,
) -> Result<Vec<u8>, CryptoError> {
    if !kmac_validate_parameters(output_length, customization) {
        return Err(CryptoError::Backend);
    }
    let name = match kind {
        KmacKind::Kmac128 => c"KMAC-128",
        KmacKind::Kmac256 => c"KMAC-256",
    };
    let output_size = (output_length / 8) as usize;
    let mut size = c_uint::try_from(output_size).map_err(|_| CryptoError::Backend)?;
    let mut params = vec![uint_parameter(c"size", &mut size)];
    if let Some(customization) = customization {
        params.push(octet_string_parameter(c"custom", customization));
    }
    params.push(end_parameter());
    openssl::mac(name, key, message, output_size, &params)
}

pub fn argon2_validate_parameters(nonce: &[u8]) -> Result<(), CryptoError> {
    // OpenSSL's Argon2 implementation rejects shorter salts.
    const MINIMUM_NONCE_LENGTH: usize = 8;

    if nonce.len() < MINIMUM_NONCE_LENGTH {
        return Err(CryptoError::Invalid("Argon2 nonce must be at least 8 bytes"));
    }
    Ok(())
}

/// Argon2 with inputs that `argon2_validate_parameters()` accepts.
pub fn argon2_derive_key(kind: Argon2Type, inputs: &Argon2Inputs<'_>) -> Result<SecretBytes, CryptoError> {
    let name = match kind {
        Argon2Type::Argon2d => c"ARGON2D",
        Argon2Type::Argon2i => c"ARGON2I",
        Argon2Type::Argon2id => c"ARGON2ID",
    };
    // NB: OpenSSL takes version 0x13, the only version Web Crypto allows, and one thread when none are given.
    let mut output = SecretBytes::new(vec![0; inputs.tag_length as usize]);
    let (mut passes, mut lanes, mut memory) = (inputs.passes, inputs.parallelism, inputs.memory);
    let mut size = c_uint::try_from(output.len()).map_err(|_| CryptoError::Backend)?;
    let mut params = vec![
        octet_string_parameter(c"pass", inputs.password),
        octet_string_parameter(c"salt", inputs.nonce),
        uint_parameter(c"iter", &mut passes),
        uint_parameter(c"lanes", &mut lanes),
        uint_parameter(c"memcost", &mut memory),
        uint_parameter(c"size", &mut size),
    ];
    if let Some(secret) = inputs.secret_value {
        params.push(octet_string_parameter(c"secret", secret));
    }
    if let Some(associated_data) = inputs.associated_data {
        params.push(octet_string_parameter(c"ad", associated_data));
    }
    params.push(end_parameter());
    openssl::kdf(name, &params, &mut output)?;
    Ok(output)
}

enum AesMode {
    Cbc,
    Ctr,
    Gcm,
    Ocb,
    Kw,
}

fn aes_cipher(mode: AesMode, key: &[u8]) -> Result<Cipher, CryptoError> {
    let ciphers = match mode {
        AesMode::Cbc => [sys::EVP_aes_128_cbc, sys::EVP_aes_192_cbc, sys::EVP_aes_256_cbc],
        AesMode::Ctr => [sys::EVP_aes_128_ctr, sys::EVP_aes_192_ctr, sys::EVP_aes_256_ctr],
        AesMode::Gcm => [sys::EVP_aes_128_gcm, sys::EVP_aes_192_gcm, sys::EVP_aes_256_gcm],
        AesMode::Ocb => [sys::EVP_aes_128_ocb, sys::EVP_aes_192_ocb, sys::EVP_aes_256_ocb],
        AesMode::Kw => [sys::EVP_aes_128_wrap, sys::EVP_aes_192_wrap, sys::EVP_aes_256_wrap],
    };
    let getter = match key.len() {
        16 => ciphers[0],
        24 => ciphers[1],
        32 => ciphers[2],
        _ => return Err(CryptoError::Backend),
    };
    Ok(Cipher::from_static(getter))
}

/// Encrypts or decrypts `input` whole with a context set up with the cipher, key and IV.
fn cipher(
    encrypt: bool,
    cipher: Cipher,
    key: &[u8],
    iv: Option<&[u8]>,
    input: &[u8],
    allow_key_wrapping: bool,
) -> Result<Vec<u8>, CryptoError> {
    let mut context = CipherContext::create()?;
    if allow_key_wrapping {
        context.allow_key_wrapping();
    }
    context.init(encrypt, Some(cipher), Some(key), iv)?;
    let mut output = Vec::with_capacity(input.len());
    // NB: Key wrapping takes its input whole.
    let result = if allow_key_wrapping {
        context.update_whole(input, &mut output)
    } else {
        context.update(input, &mut output)
    };
    // NB: OpenSSL writes a plaintext out before it checks its padding.
    result
        .and_then(|()| context.finalize(&mut output))
        .inspect_err(|_| key::clear_allocation(&mut output))?;
    Ok(output)
}

pub fn aes_cbc_cipher(encrypt: bool, key: &[u8], input: &[u8], iv: &[u8]) -> Result<Vec<u8>, CryptoError> {
    const IV_LENGTH: usize = 16;

    let aes = aes_cipher(AesMode::Cbc, key)?;
    if iv.len() != IV_LENGTH {
        return Err(CryptoError::Backend);
    }
    cipher(encrypt, aes, key, Some(iv), input, false)
}

/// AES-CBC over whole blocks, with no padding added or taken away. The input must be a multiple of the block size.
pub fn aes_cbc_cipher_without_padding(
    encrypt: bool,
    key: &[u8],
    input: &[u8],
    iv: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    const BLOCK_LENGTH: usize = 16;

    let aes = aes_cipher(AesMode::Cbc, key)?;
    if iv.len() != BLOCK_LENGTH || !input.len().is_multiple_of(BLOCK_LENGTH) {
        return Err(CryptoError::Backend);
    }
    let mut context = CipherContext::create()?;
    context.init(encrypt, Some(aes), Some(key), Some(iv))?;
    context.disable_padding()?;
    let mut output = Vec::with_capacity(input.len());
    context
        .update(input, &mut output)
        .and_then(|()| context.finalize(&mut output))
        .inspect_err(|_| key::clear_allocation(&mut output))?;
    Ok(output)
}

/// Increments only the low `counter_length` bits of the counter block, modulo 2^counter_length, as the standard
/// incrementing function of NIST SP 800-38A Appendix B.1 does.
pub fn aes_ctr_cipher(
    encrypt: bool,
    key: &[u8],
    input: &[u8],
    counter: &[u8],
    counter_length: u8,
) -> Result<Vec<u8>, CryptoError> {
    let counter_block: [u8; 16] = counter.try_into().map_err(|_| CryptoError::Backend)?;
    if counter_length == 0 || counter_length > 128 {
        return Err(CryptoError::Backend);
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
        output.extend_from_slice(&SecretBytes::new(aes_ctr_cipher_whole_block_counter(
            encrypt,
            key,
            segment,
            &block.to_be_bytes(),
        )?));
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
) -> Result<Vec<u8>, CryptoError> {
    let aes = aes_cipher(AesMode::Ctr, key)?;
    cipher(encrypt, aes, key, Some(counter), input, false)
}

pub fn aes_kw_cipher(wrap: bool, key: &[u8], input: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let aes = aes_cipher(AesMode::Kw, key)?;
    cipher(wrap, aes, key, None, input, true)
}

/// Encrypts `input`, giving the ciphertext followed by a tag of `tag_length` bytes, or decrypts such a ciphertext.
fn aead(
    cipher: Cipher,
    encrypt: bool,
    key: &[u8],
    iv: &[u8],
    additional_data: &[u8],
    tag_length: usize,
    input: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    let (input, tag) = if encrypt {
        (input, None)
    } else {
        let split_index = input
            .len()
            .checked_sub(tag_length)
            .ok_or(CryptoError::Invalid("Ciphertext too short"))?;
        let (ciphertext, tag) = input.split_at(split_index);
        (ciphertext, Some(tag))
    };

    let mut context = CipherContext::create()?;
    context.init(encrypt, Some(cipher), None, None)?;
    context.set_iv_length(iv.len())?;
    // NB: In OCB mode the tag length can only be set before the IV is; otherwise a default length is used.
    let ocb_ciphers = [sys::EVP_aes_128_ocb, sys::EVP_aes_192_ocb, sys::EVP_aes_256_ocb].map(Cipher::from_static);
    if ocb_ciphers.contains(&cipher) {
        context.set_tag_length(tag_length)?;
    }
    context.init(encrypt, None, Some(key), Some(iv))?;
    if let Some(tag) = tag {
        context.set_tag(tag)?;
    }
    context.add_additional_data(additional_data)?;

    let mut output = Vec::with_capacity(input.len() + tag_length);
    // NB: OpenSSL writes a plaintext out before it checks its tag, and a failed finalization does not count the
    //     bytes it wrote.
    context
        .update(input, &mut output)
        .and_then(|()| context.finalize(&mut output))
        .inspect_err(|_| key::clear_allocation(&mut output))?;
    if encrypt {
        output.extend_from_slice(&context.tag(tag_length)?);
    }
    Ok(output)
}

/// AES-GCM encryption, giving the ciphertext followed by a tag of `tag_length` bytes, or decryption of such a
/// ciphertext.
pub fn aes_gcm_cipher(
    encrypt: bool,
    key: &[u8],
    input: &[u8],
    iv: &[u8],
    additional_data: &[u8],
    tag_length: usize,
) -> Result<Vec<u8>, CryptoError> {
    let cipher = aes_cipher(AesMode::Gcm, key)?;
    aead(cipher, encrypt, key, iv, additional_data, tag_length, input)
}

pub fn aes_ocb_cipher(
    encrypt: bool,
    key: &[u8],
    input: &[u8],
    iv: &[u8],
    additional_data: &[u8],
    tag_length: usize,
) -> Result<Vec<u8>, CryptoError> {
    let tag_length_bytes = tag_length / 8;
    if !(1..=16).contains(&tag_length_bytes) {
        return Err(CryptoError::Invalid("Invalid AES-OCB tag length"));
    }
    let cipher = aes_cipher(AesMode::Ocb, key).map_err(|_| CryptoError::Invalid("Invalid AES-OCB key length"))?;
    aead(cipher, encrypt, key, iv, additional_data, tag_length_bytes, input)
}

pub fn chacha20_poly1305_cipher(
    encrypt: bool,
    key: &[u8],
    iv: &[u8],
    input: &[u8],
    additional_data: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    const KEY_SIZE: usize = 32;
    const NONCE_SIZE: usize = 12;
    const TAG_SIZE: usize = 16;

    if key.len() != KEY_SIZE {
        return Err(CryptoError::Invalid("ChaCha20-Poly1305 key must be 32 bytes"));
    }
    if iv.len() != NONCE_SIZE {
        return Err(CryptoError::Invalid("ChaCha20-Poly1305 nonce must be 12 bytes"));
    }
    aead(
        Cipher::from_static(sys::EVP_chacha20_poly1305),
        encrypt,
        key,
        iv,
        additional_data,
        TAG_SIZE,
        input,
    )
}

impl OkpCurve {
    /// The name of the curve's key type, as OpenSSL takes it.
    fn name(self) -> &'static CStr {
        match self {
            Self::Ed25519 => c"ED25519",
            Self::Ed448 => c"ED448",
            Self::X25519 => c"X25519",
            Self::X448 => c"X448",
        }
    }
}

pub(crate) struct OkpKey {
    curve: OkpCurve,
    key: Pkey,
}

impl OkpKey {
    /// The stored form of the key: its raw public or private key.
    pub(crate) fn stored(&self, tag: HandleTag) -> StoredKey {
        let bytes = if tag == HandleTag::OkpPrivate {
            self.key.raw_private_key()
        } else {
            self.key.raw_public_key().map(SecretBytes::new)
        };
        StoredKey::new(tag, vec![bytes.expect("OpenSSL gives an OKP key's raw key")])
    }

    fn public_key(&self) -> Result<Self, CryptoError> {
        Ok(Self {
            curve: self.curve,
            key: Pkey::from_raw_public_key(self.curve.name(), &self.key.raw_public_key()?)?,
        })
    }
}

fn okp_key(key: &KeyHandle) -> &OkpKey {
    match &key.0 {
        Key::OkpPublic(key) | Key::OkpPrivate(key) => key,
        _ => panic!("The key is not an OKP key"),
    }
}

pub fn okp_generate_key_pair(curve: OkpCurve) -> Result<KeyPair, CryptoError> {
    let private_key = OkpKey {
        curve,
        key: Pkey::generate(curve.name(), &[end_parameter()])?,
    };
    Ok(KeyPair {
        public_key: KeyHandle(Key::OkpPublic(private_key.public_key()?)),
        private_key: KeyHandle(Key::OkpPrivate(private_key)),
    })
}

pub fn okp_key_from_raw(curve: OkpCurve, is_private: bool, bytes: &[u8]) -> Result<KeyHandle, CryptoError> {
    Ok(KeyHandle(if is_private {
        Key::OkpPrivate(OkpKey {
            curve,
            key: Pkey::from_raw_private_key(curve.name(), bytes)?,
        })
    } else {
        Key::OkpPublic(OkpKey {
            curve,
            key: Pkey::from_raw_public_key(curve.name(), bytes)?,
        })
    }))
}

/// The raw public key of an OKP public key, or of the public key of an OKP private key.
pub fn okp_raw_public_key(key: &KeyHandle) -> Result<Vec<u8>, CryptoError> {
    okp_key(key).key.raw_public_key()
}

pub fn okp_raw_private_key(key: &KeyHandle) -> Result<SecretBytes, CryptoError> {
    okp_key(key).key.raw_private_key()
}

pub fn okp_compute_coordinate(private_key: &KeyHandle, public_key: &KeyHandle) -> Result<SecretBytes, CryptoError> {
    let private_key = okp_key(private_key);
    if !matches!(private_key.curve, OkpCurve::X25519 | OkpCurve::X448) {
        return Err(CryptoError::Backend);
    }
    openssl::derive(&private_key.key, &okp_key(public_key).key, false)
}

pub fn okp_sign(private_key: &KeyHandle, message: &[u8], context: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let OkpKey { curve, key } = okp_key(private_key);
    match curve {
        OkpCurve::Ed25519 => openssl::digest_sign(key, None, &[end_parameter()], message),
        OkpCurve::Ed448 => openssl::digest_sign(key, None, &openssl::context_string_parameters(context), message),
        OkpCurve::X25519 | OkpCurve::X448 => Err(CryptoError::Backend),
    }
}

// NB: The y-coordinates of the points of order 1, 2, 4 and 8, which Web Crypto's verify operation returns false for
//     when the public key or the signature's R is one. The sign bit of x is ignored.
const SMALL_ORDER_ED25519_Y_COORDINATES: [[u8; 32]; 7] = [
    // y = 0, order 4
    [
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ],
    // y = 1, the identity
    [
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ],
    // Order 8
    [
        0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0, 0xd5, 0xdf,
        0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05,
    ],
    // Order 8
    [
        0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f, 0x2a, 0x20,
        0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a,
    ],
    // y = p - 1, order 2
    [
        0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
    ],
    // y = p, a non-canonical 0
    [
        0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
    ],
    // y = p + 1, a non-canonical 1
    [
        0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
    ],
];

fn is_small_order_ed25519_point(point: &[u8]) -> bool {
    let Some((&last, rest)) = point.split_last() else {
        return false;
    };
    SMALL_ORDER_ED25519_Y_COORDINATES
        .iter()
        .any(|y| rest == &y[..31] && last & 0x7f == y[31])
}

pub fn okp_verify(
    public_key: &KeyHandle,
    signature: &[u8],
    message: &[u8],
    context: &[u8],
) -> Result<bool, CryptoError> {
    let OkpKey { curve, key } = okp_key(public_key);
    if *curve == OkpCurve::Ed25519
        && (is_small_order_ed25519_point(&key.raw_public_key()?)
            || signature.get(..32).is_some_and(is_small_order_ed25519_point))
    {
        return Ok(false);
    }
    match curve {
        OkpCurve::Ed25519 => openssl::digest_verify(key, None, &[end_parameter()], message, signature),
        OkpCurve::Ed448 => openssl::digest_verify(
            key,
            None,
            &openssl::context_string_parameters(context),
            message,
            signature,
        ),
        OkpCurve::X25519 | OkpCurve::X448 => Err(CryptoError::Invalid("The algorithm does not sign")),
    }
}

pub fn okp_export_der(key: &KeyHandle) -> Result<Vec<u8>, CryptoError> {
    match &key.0 {
        Key::OkpPublic(public_key) => public_key.key.public_key_info(),
        Key::OkpPrivate(OkpKey { curve, key }) => {
            // https://www.rfc-editor.org/rfc/rfc8410#section-7
            // CurvePrivateKey ::= OCTET STRING
            let private_key = SecretBytes::new(der::element(der::OCTET_STRING, &[&key.raw_private_key()?]));
            Ok(der::private_key_info(
                KeyAlgorithm::Okp(*curve).oid(),
                None,
                &private_key,
            ))
        }
        _ => panic!("The key is not an OKP key"),
    }
}

pub fn public_key_for_private_key(key: &KeyHandle) -> Option<KeyHandle> {
    if let Key::OkpPrivate(private_key) = &key.0 {
        return Some(KeyHandle(Key::OkpPublic(private_key.public_key().ok()?)));
    }
    ml::public_key_for_private_key(key)
        .or_else(|| ec::public_key_for_private_key(key))
        .or_else(|| rsa::public_key_for_private_key(key))
}
