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
use core::ptr::{NonNull, null, null_mut};
use std::rc::Rc;

use ak::{Span, Utf16String, Utf16StringUnits};
use libjs_rust::api::{
    ErrorKind, ErrorType, Gc, Object, PropertyKey, RawNativeFunctionResult, Realm, Trace, Value, Vm,
};

use crate::bindings::conversions::{buffer_source_type_of, to_unsigned_long_enforcing_range};
use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi, realm_arguments};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::crypto::crypto_bindings::{CryptoKeyPair, JsonWebKey, RawJsonWebKey};
use crate::crypto::crypto_key::{CryptoKey, KeyUsage};
use crate::crypto::subtle_crypto::{
    AlgorithmIdentifier, CryptoKeyOrCryptoKeyPair, KeyFormat, get_a_copy_of_the_bytes, normalize_an_algorithm,
};

// https://w3c.github.io/webcrypto/#algorithm-overview
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AlgorithmParams {
    pub(crate) name: Utf16String,
}

struct HashAlgorithmIdentifier {
    name: Utf16String,
}

fn hash_algorithm_identifier_from_value(
    vm: &Vm,
    realm: Gc<Realm>,
    hash_value: Value,
    hash_string: Option<Utf16String>,
) -> WebIdlResult<HashAlgorithmIdentifier> {
    let algorithm = if let Some(hash_string) = hash_string {
        AlgorithmIdentifier::DOMString(hash_string)
    } else if hash_value.is_string() {
        AlgorithmIdentifier::DOMString(hash_value.to_utf16_string(vm)?)
    } else if hash_value.is_object() {
        AlgorithmIdentifier::Object(hash_value.to_object(vm)?)
    } else {
        unreachable!("the hash is a string or an object")
    };

    let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "digest")?;
    Ok(HashAlgorithmIdentifier {
        name: normalized_algorithm.parameter.name,
    })
}

/// # Safety
///
/// The VM and realm are live, the value or string is live, and the output is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_hash_algorithm_identifier_from_value(
    vm: *mut c_void,
    realm: *mut c_void,
    value: u64,
    raw_string: usize,
    out_name: &mut usize,
) -> RawNativeFunctionResult {
    // SAFETY: The caller supplies the live VM, realm and value.
    let ((vm, realm), value) = unsafe { (realm_arguments(vm, realm), Value::from_raw(value)) };
    let hash_string = (raw_string != 0).then(|| {
        // SAFETY: The caller lends a live string and we take our own reference.
        unsafe {
            ak::reference_utf16_string(raw_string);
            Utf16String::from_raw_owned(raw_string)
        }
    });
    let result = hash_algorithm_identifier_from_value(vm, realm, value, hash_string)
        .map(|identifier| {
            *out_name = identifier.name.into_raw();
            Value::UNDEFINED
        })
        .map_err(|error| error.throw(&CallContext::new(vm, realm)));
    RawNativeFunctionResult::from_completion(result)
}

// https://w3c.github.io/webcrypto/#concept-usage-intersection
fn usage_intersection(a: &[KeyUsage], b: &[KeyUsage]) -> Vec<KeyUsage> {
    let mut result = Vec::new();
    for &usage in a {
        if b.contains(&usage) && !result.contains(&usage) {
            result.push(usage);
        }
    }
    result.sort_unstable_by_key(|usage| *usage as u8);
    result
}

// https://w3c.github.io/webcrypto/#concept-normalized-usages
pub(crate) fn normalized_key_usages(usages: &[KeyUsage]) -> Vec<KeyUsage> {
    usage_intersection(
        usages,
        &[
            KeyUsage::Encrypt,
            KeyUsage::Decrypt,
            KeyUsage::Sign,
            KeyUsage::Verify,
            KeyUsage::DeriveKey,
            KeyUsage::DeriveBits,
            KeyUsage::WrapKey,
            KeyUsage::UnwrapKey,
            KeyUsage::EncapsulateKey,
            KeyUsage::EncapsulateBits,
            KeyUsage::DecapsulateKey,
            KeyUsage::DecapsulateBits,
        ],
    )
}

/// # Safety
///
/// The spans and the receiver are valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_usage_intersection(
    a: Span<'_, KeyUsage>,
    b: Span<'_, KeyUsage>,
    state: *mut c_void,
    receive: ReceiveUsages,
) {
    let result = usage_intersection(a.as_slice(), b.as_slice());
    // SAFETY: The caller supplies a receiver that takes the span only for the call.
    unsafe { receive(state, Span::new(&result)) };
}

/// # Safety
///
/// The span and the receiver are valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_normalized_key_usages(
    usages: Span<'_, KeyUsage>,
    state: *mut c_void,
    receive: ReceiveUsages,
) {
    let result = normalized_key_usages(usages.as_slice());
    // SAFETY: The caller supplies a receiver that takes the span only for the call.
    unsafe { receive(state, Span::new(&result)) };
}

#[derive(PartialEq, Eq)]
#[repr(u8)]
pub enum Base64DecodeResult {
    #[allow(dead_code, reason = "the native base64 decoder constructs this result")]
    Success,
    #[allow(dead_code, reason = "the native base64 decoder constructs this result")]
    OutOfMemory,
    InvalidBase64,
    NonAscii,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum Asn1Structure {
    SubjectPublicKeyInfo,
    PrivateKeyInfo,
    #[allow(dead_code, reason = "the native ASN.1 callers supply this structure")]
    OctetString,
}

unsafe extern "C" {
    fn web_crypto_encode_base64url(bytes: Span<'_, u8>, receive: ReceiveBytes, state: *mut c_void) -> i32;
    fn web_crypto_decode_base64url(raw: usize, out: *mut c_void, error_message: &mut usize) -> Base64DecodeResult;
    fn web_crypto_decode_asn1_structure(
        data: Span<'_, u8>,
        structure: Asn1Structure,
        out: *mut c_void,
        all_consumed: &mut bool,
    ) -> usize;
}

unsafe extern "C" fn receive_base64url_string(state: *mut c_void, bytes: Span<'_, u8>) {
    // SAFETY: The state is the caller's slot, and the service lends the bytes.
    unsafe { *state.cast::<Option<Utf16String>>() = Some(Utf16String::from_ascii(bytes.as_slice())) };
}

// https://www.rfc-editor.org/rfc/rfc7518#section-2
fn base64_url_bytes_encode(bytes: &[u8]) -> Result<Utf16String, i32> {
    let mut encoded = None;
    // SAFETY: The input and the output slot live for the call.
    let error = unsafe {
        web_crypto_encode_base64url(
            Span::new(bytes),
            receive_base64url_string,
            core::ptr::from_mut(&mut encoded).cast(),
        )
    };
    if error != 0 {
        return Err(error);
    }
    Ok(encoded.expect("LibWeb returns the base64url string"))
}

/// # Safety
///
/// The span is live and the output is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_base64_url_bytes_encode(bytes: Span<'_, u8>, out: &mut usize) -> i32 {
    match base64_url_bytes_encode(bytes.as_slice()) {
        Ok(encoded) => {
            *out = encoded.into_raw();
            0
        }
        Err(error) => error,
    }
}

unsafe fn base64_url_bytes_decode(
    base64_url_string: &Utf16String,
    out: *mut c_void,
    error_message: &mut usize,
) -> Base64DecodeResult {
    let is_ascii = match base64_url_string.as_units() {
        Utf16StringUnits::Ascii(_) => true,
        Utf16StringUnits::Latin1(units) => units.is_ascii(),
        Utf16StringUnits::Utf16(units) => units.iter().all(|&unit| unit <= 0x7f),
    };
    if !is_ascii {
        return Base64DecodeResult::NonAscii;
    }

    // SAFETY: The caller passes a live native ByteBuffer as the output.
    let result = unsafe { web_crypto_decode_base64url(base64_url_string.raw_identity(), out, error_message) };
    if result == Base64DecodeResult::InvalidBase64 {
        // SAFETY: The decoder hands over an owned diagnostic string.
        let detail = unsafe { Utf16String::from_raw_owned(*error_message) };
        *error_message = libjs_rust::api::utf16_formatted("base64 decode: {}", &[&detail]).into_raw();
    }
    result
}

/// # Safety
///
/// The string is live and the output points to a live ByteBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_base64_url_bytes_decode(
    raw: usize,
    out: *mut c_void,
    error_message: &mut usize,
) -> Base64DecodeResult {
    // SAFETY: The caller lends a live string and we take our own reference.
    let base64_url_string = unsafe {
        ak::reference_utf16_string(raw);
        Utf16String::from_raw_owned(raw)
    };
    // SAFETY: The caller supplies a live ByteBuffer.
    unsafe { base64_url_bytes_decode(&base64_url_string, out, error_message) }
}

// https://w3c.github.io/webcrypto/#concept-parse-an-asn1-structure
unsafe fn parse_an_asn1_structure(
    data: Span<'_, u8>,
    structure: Asn1Structure,
    exact_data: bool,
    out: *mut c_void,
) -> Result<(), Utf16String> {
    // 1. Let data be a sequence of bytes to be parsed.
    // 2. Let structure be the ASN.1 structure to be parsed.
    // 3. Let exactData be an optional boolean value. If it is not supplied, let it be initialized to true.

    // 4. Parse data according to the Distinguished Encoding Rules of [X690], using structure as the ASN.1 structure to be decoded.
    let mut all_consumed = false;
    // SAFETY: The caller passes a live native value of the specified structure.
    let error = unsafe { web_crypto_decode_asn1_structure(data, structure, out, &mut all_consumed) };
    if error != 0 {
        // SAFETY: The decoder hands over an owned diagnostic string.
        let detail = unsafe { Utf16String::from_raw_owned(error) };
        let message = match structure {
            Asn1Structure::SubjectPublicKeyInfo => "Error parsing subjectPublicKeyInfo: {}",
            Asn1Structure::PrivateKeyInfo => "Error parsing privateKeyInfo: {}",
            Asn1Structure::OctetString => "Read of kind OctetString failed: {}",
        };
        return Err(libjs_rust::api::utf16_formatted(message, &[&detail]));
    }

    // 5. If exactData was specified, and all of the bytes of data were not consumed during the parsing phase, then throw a DataError.
    if exact_data && !all_consumed {
        return Err(Utf16String::from_utf8(
            "Not all bytes were consumed during the parsing phase",
        ));
    }

    // 6. Return the parsed ASN.1 structure.
    Ok(())
}

// https://w3c.github.io/webcrypto/#concept-parse-a-spki
unsafe fn parse_a_subject_public_key_info(data: Span<'_, u8>, out: *mut c_void) -> Result<(), Utf16String> {
    // When this specification says to parse a subjectPublicKeyInfo, the user agent must parse an ASN.1 structure,
    // with data set to the sequence of bytes to be parsed, structure as the ASN.1 structure of subjectPublicKeyInfo,
    // as specified in [RFC5280], and exactData set to true.
    // SAFETY: The caller passes a live native SubjectPublicKey.
    unsafe { parse_an_asn1_structure(data, Asn1Structure::SubjectPublicKeyInfo, true, out) }
}

// https://w3c.github.io/webcrypto/#concept-parse-a-privateKeyInfo
unsafe fn parse_a_private_key_info(data: Span<'_, u8>, out: *mut c_void) -> Result<(), Utf16String> {
    // When this specification says to parse a PrivateKeyInfo, the user agent must parse an ASN.1 structure
    // with data set to the sequence of bytes to be parsed, structure as the ASN.1 structure of PrivateKeyInfo,
    // as specified in [RFC5208], and exactData set to true.
    // SAFETY: The caller passes a live native PrivateKey.
    unsafe { parse_an_asn1_structure(data, Asn1Structure::PrivateKeyInfo, true, out) }
}

/// # Safety
///
/// The span is live and the output points to a live native value of the specified structure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_parse_an_asn1_structure(
    data: Span<'_, u8>,
    structure: Asn1Structure,
    exact_data: bool,
    out: *mut c_void,
) -> usize {
    // SAFETY: The caller supplies a live value of the specified structure.
    let result = unsafe {
        match structure {
            Asn1Structure::SubjectPublicKeyInfo => parse_a_subject_public_key_info(data, out),
            Asn1Structure::PrivateKeyInfo => parse_a_private_key_info(data, out),
            Asn1Structure::OctetString => parse_an_asn1_structure(data, structure, exact_data, out),
        }
    };
    result.err().map_or(0, Utf16String::into_raw)
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub(crate) enum AlgorithmId {
    RsassaPkcs1,
    RsaPss,
    RsaOaep,
    Ecdsa,
    Ecdh,
    AesCtr,
    AesCbc,
    AesGcm,
    AesKw,
    Hmac,
    Hkdf,
    Pbkdf2,
    X25519,
    X448,
    ED25519,
    ED448,
    MlDsa,
    MlKem,
    Argon2,
    ChaCha20Poly1305,
    AesOcb,
    Kmac,
}

#[derive(Clone, Copy)]
#[repr(u8)]
#[allow(
    clippy::enum_variant_names,
    reason = "each variant names an algorithm parameter dictionary"
)]
pub(crate) enum ParameterId {
    AlgorithmParams,
    RsaHashedKeyGenParams,
    RsaHashedImportParams,
    RsaPssParams,
    RsaOaepParams,
    EcdsaParams,
    EcKeyGenParams,
    EcKeyImportParams,
    EcdhKeyDeriveParams,
    AesCtrParams,
    AesKeyGenParams,
    AesDerivedKeyParams,
    AesCbcParams,
    AesGcmParams,
    HmacKeyGenParams,
    HmacImportParams,
    HkdfParams,
    Pbkdf2Params,
    Ed448Params,
    ContextParams,
    Argon2Params,
    AeadParams,
    KmacParams,
    KmacKeyGenParams,
    KmacImportParams,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Bytes(#[gc(untraced)] Rc<Vec<u8>>);

impl Bytes {
    pub(crate) fn new(bytes: Vec<u8>) -> Self {
        Self(Rc::new(bytes))
    }

    pub(crate) fn as_slice(&self) -> &[u8] {
        self.0.as_slice()
    }
}

pub(crate) enum ExportKeyResult {
    Bytes(Bytes),
    JsonWebKey(JsonWebKey),
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ImportKeyData {
    Bytes(Bytes),
    JsonWebKey(JsonWebKey),
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct EncapsulatedBitsData {
    pub(crate) shared_key: Option<Bytes>,
    pub(crate) ciphertext: Option<Bytes>,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct EncapsulatedKeyData {
    pub(crate) shared_key: Ref<CryptoKey>,
    pub(crate) ciphertext: Option<Bytes>,
}

#[repr(C)]
#[derive(Default)]
struct OptionalLength {
    has_value: bool,
    value: u32,
}

impl From<Option<u32>> for OptionalLength {
    fn from(value: Option<u32>) -> Self {
        Self {
            has_value: value.is_some(),
            value: value.unwrap_or_default(),
        }
    }
}

type ReceiveBytes = unsafe extern "C" fn(*mut c_void, Span<'_, u8>);
type ReceiveUsages = unsafe extern "C" fn(*mut c_void, Span<'_, KeyUsage>);
type ReceiveJsonWebKey = unsafe extern "C" fn(*mut c_void, &RawJsonWebKey<'_>);

unsafe extern "C" {
    fn web_crypto_algorithm_from_value(
        realm: *mut c_void,
        method_id: AlgorithmId,
        parameter_id: ParameterId,
        value: u64,
        name: usize,
        out: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_destroy(algorithm: *mut c_void);
    fn web_crypto_algorithm_public_key_usages(algorithm: *mut c_void, state: *mut c_void, receive: ReceiveUsages);
    fn web_crypto_public_key_for_private_key(key: *mut c_void) -> *mut c_void;
    fn web_crypto_algorithm_encrypt(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_decrypt(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_sign(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_verify(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        signature: Span<'_, u8>,
        data: Span<'_, u8>,
        out: &mut bool,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_derive_bits(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        length: OptionalLength,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_import_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        format: KeyFormat,
        key_data: Span<'_, u8>,
        jwk: *const c_void,
        extractable: bool,
        usages: Span<'_, KeyUsage>,
        out: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_generate_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        extractable: bool,
        usages: Span<'_, KeyUsage>,
        out_key: &mut *mut c_void,
        out_public_key: &mut *mut c_void,
        out_private_key: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_export_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        format: KeyFormat,
        key: *mut c_void,
        state: *mut c_void,
        receive_bytes: ReceiveBytes,
        receive_jwk: ReceiveJsonWebKey,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_get_key_length(
        realm: *mut c_void,
        algorithm: *mut c_void,
        out: &mut OptionalLength,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_get_shared_key_length(
        realm: *mut c_void,
        algorithm: *mut c_void,
        out: &mut u32,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_wrap_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_unwrap_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_encapsulate(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        shared_key: *mut c_void,
        ciphertext: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_decapsulate(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_encrypt_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_decrypt_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_sign_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_verify_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_generate_key_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_import_key_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_derive_bits_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
        length: OptionalLength,
    ) -> RawNativeFunctionResult;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppAlgorithm(#[gc(untraced)] NonNull<c_void>);

impl Drop for CppAlgorithm {
    fn drop(&mut self) {
        // SAFETY: This object owns the C++ algorithm.
        unsafe { web_crypto_algorithm_destroy(self.0.as_ptr()) };
    }
}

impl CppAlgorithm {
    pub(crate) fn from_value(
        realm: Gc<Realm>,
        method_id: AlgorithmId,
        parameter_id: ParameterId,
        value: Value,
        name: &Utf16String,
    ) -> WebIdlResult<Self> {
        let mut out = null_mut();
        // SAFETY: The realm and value are live, and the call borrows the name.
        unsafe {
            web_crypto_algorithm_from_value(
                realm.as_ptr().cast(),
                method_id,
                parameter_id,
                value.to_raw(),
                name.raw_identity(),
                &mut out,
            )
        }
        .into_completion()?;
        Ok(Self(NonNull::new(out).expect("LibWeb returns an algorithm")))
    }
}

pub(crate) trait AlgorithmMethods: Trace {
    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        None
    }

    fn encrypt(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("encrypt is not supported"),
        ))
    }

    fn decrypt(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("decrypt is not supported"),
        ))
    }

    fn sign(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("sign is not supported"),
        ))
    }

    fn verify(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _signature: &Bytes, _data: &Bytes) -> WebIdlResult<bool> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("verify is not supported"),
        ))
    }

    fn digest(&self, _realm: Gc<Realm>, _algorithm: &AlgorithmParams, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("digest is not supported"),
        ))
    }

    fn derive_bits(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _length: Option<u32>) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("deriveBits is not supported"),
        ))
    }

    fn import_key(
        &self,
        _realm: Gc<Realm>,
        _format: KeyFormat,
        _key_data: ImportKeyData,
        _extractable: bool,
        _usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("importKey is not supported"),
        ))
    }

    fn generate_key(
        &self,
        _realm: Gc<Realm>,
        _extractable: bool,
        _usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("generateKey is not supported"),
        ))
    }

    fn export_key(&self, _realm: Gc<Realm>, _format: KeyFormat, _key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("exportKey is not supported"),
        ))
    }

    fn get_key_length(&self, _realm: Gc<Realm>) -> WebIdlResult<Option<u32>> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("getKeyLength is not supported"),
        ))
    }

    fn get_shared_key_length(&self, _realm: Gc<Realm>) -> WebIdlResult<u32> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("getSharedKeyLength is not supported"),
        ))
    }

    fn wrap_key(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("wrapKey is not supported"),
        ))
    }

    fn unwrap_key(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("unwwrapKey is not supported"),
        ))
    }

    fn encapsulate(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>) -> WebIdlResult<EncapsulatedBitsData> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("encapsulate is not supported"),
        ))
    }

    fn decapsulate(&self, _realm: Gc<Realm>, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("decalpsulate is not supported"),
        ))
    }

    fn validate_encrypt_parameters(&self, _realm: Gc<Realm>) -> WebIdlResult<()> {
        Ok(())
    }

    fn validate_decrypt_parameters(&self, _realm: Gc<Realm>) -> WebIdlResult<()> {
        Ok(())
    }

    fn validate_sign_parameters(&self, _realm: Gc<Realm>) -> WebIdlResult<()> {
        Ok(())
    }

    fn validate_verify_parameters(&self, _realm: Gc<Realm>) -> WebIdlResult<()> {
        Ok(())
    }

    fn validate_digest_parameters(&self, _realm: Gc<Realm>) -> WebIdlResult<()> {
        Ok(())
    }

    fn validate_generate_key_parameters(&self, _realm: Gc<Realm>) -> WebIdlResult<()> {
        Ok(())
    }

    fn validate_import_key_parameters(&self, _realm: Gc<Realm>) -> WebIdlResult<()> {
        Ok(())
    }

    fn validate_derive_bits_parameters(&self, _realm: Gc<Realm>, _length: Option<u32>) -> WebIdlResult<()> {
        Ok(())
    }
}

impl AlgorithmMethods for CppAlgorithm {
    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        let mut usages = None;
        // SAFETY: This object owns the algorithm and the state is writable for the call.
        unsafe {
            web_crypto_algorithm_public_key_usages(
                self.0.as_ptr(),
                core::ptr::from_mut(&mut usages).cast(),
                receive_usages,
            );
        };
        usages
    }

    fn encrypt(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_encrypt(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    fn decrypt(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_decrypt(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    fn sign(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_sign(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    fn verify(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, signature: &Bytes, data: &Bytes) -> WebIdlResult<bool> {
        let mut out = false;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_verify(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(signature.as_slice()),
                Span::new(data.as_slice()),
                &mut out,
            )
        }
        .into_completion()?;
        Ok(out)
    }

    fn derive_bits(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, length: Option<u32>) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_derive_bits(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                length.into(),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    fn import_key(
        &self,
        realm: Gc<Realm>,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        let mut out = null_mut();
        let mut import = |data, jwk| {
            // SAFETY: The realm and algorithm are live, and the input and output are lent for the call.
            unsafe {
                web_crypto_algorithm_import_key(
                    realm.as_ptr().cast(),
                    self.0.as_ptr(),
                    format,
                    data,
                    jwk,
                    extractable,
                    Span::new(usages),
                    &mut out,
                )
            }
            .into_completion()
        };
        match key_data {
            ImportKeyData::Bytes(bytes) => import(Span::new(bytes.as_slice()), null())?,
            ImportKeyData::JsonWebKey(key) => {
                key.with_raw_fields(|fields| import(Span::default(), core::ptr::from_ref(fields).cast()))?
            }
        };
        // SAFETY: LibWeb returns a live CryptoKey cell.
        Ok(unsafe { cell_from_abi(out) })
    }

    fn generate_key(
        &self,
        realm: Gc<Realm>,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        let (mut key, mut public_key, mut private_key) = (null_mut(), null_mut(), null_mut());
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_generate_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                extractable,
                Span::new(usages),
                &mut key,
                &mut public_key,
                &mut private_key,
            )
        }
        .into_completion()?;
        // SAFETY: LibWeb returns a live CryptoKey or two live CryptoKeys.
        unsafe {
            if !key.is_null() {
                Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(cell_from_abi(key)))
            } else {
                Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
                    public_key: Some(cell_from_abi(public_key)),
                    private_key: Some(cell_from_abi(private_key)),
                }))
            }
        }
    }

    fn export_key(&self, realm: Gc<Realm>, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        let mut out: Option<ExportKeyResult> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_export_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                format,
                cell_into_abi(key),
                core::ptr::from_mut(&mut out).cast(),
                receive_export_bytes,
                receive_export_jwk,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns an export result"))
    }

    fn get_key_length(&self, realm: Gc<Realm>) -> WebIdlResult<Option<u32>> {
        let mut out = OptionalLength::default();
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_get_key_length(realm.as_ptr().cast(), self.0.as_ptr(), &mut out) }
            .into_completion()?;
        Ok(out.has_value.then_some(out.value))
    }

    fn get_shared_key_length(&self, realm: Gc<Realm>) -> WebIdlResult<u32> {
        let mut out = 0;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_get_shared_key_length(realm.as_ptr().cast(), self.0.as_ptr(), &mut out) }
            .into_completion()?;
        Ok(out)
    }

    fn wrap_key(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_wrap_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    fn unwrap_key(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_unwrap_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    fn encapsulate(&self, realm: Gc<Realm>, key: Ref<CryptoKey>) -> WebIdlResult<EncapsulatedBitsData> {
        let (mut shared_key, mut ciphertext): (Option<Bytes>, Option<Bytes>) = (None, None);
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_encapsulate(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                core::ptr::from_mut(&mut shared_key).cast(),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(EncapsulatedBitsData { shared_key, ciphertext })
    }

    fn decapsulate(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_decapsulate(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    fn validate_encrypt_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_encrypt_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    fn validate_decrypt_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_decrypt_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    fn validate_sign_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_sign_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    fn validate_verify_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_verify_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    fn validate_generate_key_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_generate_key_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    fn validate_import_key_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_import_key_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    fn validate_derive_bits_parameters(&self, realm: Gc<Realm>, length: Option<u32>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_validate_derive_bits_parameters(realm.as_ptr().cast(), self.0.as_ptr(), length.into())
        }
        .into_completion()?;
        Ok(())
    }
}

#[repr(i32)]
enum ShaHashKind {
    Sha1 = 4,
    Sha256,
    Sha384,
    Sha512,
    Sha3_256,
    Sha3_384,
    Sha3_512,
}

#[repr(i32)]
enum CShakeKind {
    CShake128,
    CShake256,
}

#[repr(C)]
struct OptionalBytes<'a> {
    has_value: bool,
    bytes: Span<'a, u8>,
}

impl<'a> From<Option<&'a Bytes>> for OptionalBytes<'a> {
    fn from(value: Option<&'a Bytes>) -> Self {
        Self {
            has_value: value.is_some(),
            bytes: Span::new(value.map_or(&[], Bytes::as_slice)),
        }
    }
}

unsafe extern "C" {
    fn web_crypto_sha_digest(kind: ShaHashKind, data: Span<'_, u8>, state: *mut c_void, receive: ReceiveBytes) -> bool;
    fn web_crypto_cshake_validate_parameters(
        output_length: u32,
        customization: OptionalBytes<'_>,
        function_name: OptionalBytes<'_>,
    ) -> usize;
    fn web_crypto_cshake_digest(
        kind: CShakeKind,
        data: Span<'_, u8>,
        output_length: u32,
        customization: OptionalBytes<'_>,
        function_name: OptionalBytes<'_>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
}

fn get_optional_buffer_source(vm: &Vm, object: Gc<Object>, name: &PropertyKey) -> WebIdlResult<Option<Bytes>> {
    if !object.has_property(vm, name)? {
        return Ok(None);
    }

    let value = object.get(vm, name)?;
    let Some(buffer_source) = buffer_source_type_of(value) else {
        return vm
            .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
            .map_err(Into::into);
    };

    Ok(Some(get_a_copy_of_the_bytes(vm, buffer_source)?))
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CShakeParams {
    output_length: u32,
    function_name: Option<Bytes>,
    customization: Option<Bytes>,
}

impl CShakeParams {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let output_length_property = PropertyKey::from_utf8("outputLength");
        if !object.has_property(vm, &output_length_property)? {
            return vm
                .throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::MissingRequiredProperty,
                    &[&"outputLength"],
                )
                .map_err(Into::into);
        }

        let output_length = object.get(vm, &output_length_property)?;

        let length = to_unsigned_long_enforcing_range(vm, output_length)?;

        let function_name = get_optional_buffer_source(vm, object, &PropertyKey::from_utf8("functionName"))?;

        let customization = get_optional_buffer_source(vm, object, &PropertyKey::from_utf8("customization"))?;

        Ok(Self {
            output_length: length,
            function_name,
            customization,
        })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Sha;

impl AlgorithmMethods for Sha {
    // https://w3c.github.io/webcrypto/#sha-operations-digest
    fn digest(&self, _: Gc<Realm>, algorithm: &AlgorithmParams, data: &Bytes) -> WebIdlResult<Bytes> {
        let algorithm_name = &algorithm.name;

        let hash_kind = if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
            ShaHashKind::Sha1
        } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
            ShaHashKind::Sha256
        } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
            ShaHashKind::Sha384
        } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
            ShaHashKind::Sha512
        } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"SHA3-256")) {
            ShaHashKind::Sha3_256
        } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"SHA3-384")) {
            ShaHashKind::Sha3_384
        } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"SHA3-512")) {
            ShaHashKind::Sha3_512
        } else {
            return Err(Exception::Dom(
                "NotSupportedError",
                libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[algorithm_name]),
            ));
        };

        let mut result_buffer: Option<Bytes> = None;
        // SAFETY: The input and the output slot live for the call.
        let success = unsafe {
            web_crypto_sha_digest(
                hash_kind,
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut result_buffer).cast(),
                receive_bytes,
            )
        };
        if !success {
            return Err(Exception::Dom(
                "OperationError",
                Utf16String::from_utf8("Failed to create result buffer"),
            ));
        }

        Ok(result_buffer.expect("LibCrypto returns the digest"))
    }
}

impl AlgorithmMethods for CShakeParams {
    // https://wicg.github.io/webcrypto-modern-algos/#cshake-operations-digest
    fn validate_digest_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Step 5 throws an OperationError for the parameters that our cSHAKE does not support.
        let normalized_algorithm = self;
        // SAFETY: The parameters live for the call.
        let result = unsafe {
            web_crypto_cshake_validate_parameters(
                normalized_algorithm.output_length,
                normalized_algorithm.customization.as_ref().into(),
                normalized_algorithm.function_name.as_ref().into(),
            )
        };
        if result != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(result) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Hash function failed: {}", &[&detail]),
            ));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#cshake-operations-digest
    fn digest(&self, _: Gc<Realm>, algorithm: &AlgorithmParams, data: &Bytes) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;

        // 1. Let outputLength be the outputLength member of normalizedAlgorithm.
        let output_length = normalized_algorithm.output_length;

        // 2. Let functionName be the functionName member of normalizedAlgorithm if present or the empty octet string otherwise.
        let function_name = normalized_algorithm.function_name.as_ref();

        // 3. Let customization be the customization member of normalizedAlgorithm if present or the empty octet string otherwise.
        let customization = normalized_algorithm.customization.as_ref();

        // 4. If the name member of normalizedAlgorithm is a case-sensitive string match for "cSHAKE128":
        let kind = if matches!(algorithm.name.as_units(), Utf16StringUnits::Ascii(b"cSHAKE128")) {
            CShakeKind::CShake128
        // 4. If the name member of normalizedAlgorithm is a case-sensitive string match for "cSHAKE256":
        } else if matches!(algorithm.name.as_units(), Utf16StringUnits::Ascii(b"cSHAKE256")) {
            CShakeKind::CShake256
        } else {
            unreachable!("the algorithm is cSHAKE128 or cSHAKE256");
        };

        // 4. Let result be the result of performing the cSHAKE128/cSHAKE256 function defined in Section 3 of [NIST-SP800-185]
        // using message as the X input parameter,
        // outputLength as the L input parameter,
        // functionName as the N input parameter,
        // and customization as the S input parameter.
        let mut result: Option<Bytes> = None;
        // SAFETY: The input and the output slot live for the call.
        let maybe_result = unsafe {
            web_crypto_cshake_digest(
                kind,
                Span::new(data.as_slice()),
                output_length,
                customization.into(),
                function_name.into(),
                core::ptr::from_mut(&mut result).cast(),
                receive_bytes,
            )
        };

        // 5. If performing the operation results in an error, then throw an OperationError.
        if maybe_result != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(maybe_result) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Hash function failed: {}", &[&detail]),
            ));
        }

        // 6. Return result.
        Ok(result.expect("LibCrypto returns the digest"))
    }
}

unsafe extern "C" fn receive_bytes(state: *mut c_void, bytes: Span<'_, u8>) {
    // SAFETY: The state is the caller's slot, and the service lends the bytes.
    unsafe { *state.cast::<Option<Bytes>>() = Some(Bytes::new(bytes.as_slice().to_vec())) };
}

unsafe extern "C" fn receive_usages(state: *mut c_void, usages: Span<'_, KeyUsage>) {
    // SAFETY: The state is the caller's slot, and the service lends the usages.
    unsafe { *state.cast::<Option<Vec<KeyUsage>>>() = Some(usages.as_slice().to_vec()) };
}

unsafe extern "C" fn receive_export_bytes(state: *mut c_void, bytes: Span<'_, u8>) {
    // SAFETY: The state is the caller's slot, and the service lends the bytes.
    unsafe {
        *state.cast::<Option<ExportKeyResult>>() = Some(ExportKeyResult::Bytes(Bytes::new(bytes.as_slice().to_vec())));
    };
}

unsafe extern "C" fn receive_export_jwk(state: *mut c_void, key: &RawJsonWebKey<'_>) {
    // SAFETY: The state is the caller's slot, and the service lends the fields.
    unsafe {
        *state.cast::<Option<ExportKeyResult>>() = Some(ExportKeyResult::JsonWebKey(JsonWebKey::from_raw_fields(key)));
    };
}

pub(crate) fn public_key_for_private_key(key: Ref<CryptoKey>) -> Option<Ref<CryptoKey>> {
    // SAFETY: The key is live.
    let public_key = unsafe { web_crypto_public_key_for_private_key(cell_into_abi(key)) };
    // SAFETY: A non-null pointer returned by LibWeb is a live CryptoKey cell.
    NonNull::new(public_key).map(|key| unsafe { cell_from_abi(key.as_ptr()) })
}
