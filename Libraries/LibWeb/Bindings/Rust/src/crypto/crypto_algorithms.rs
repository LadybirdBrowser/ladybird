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
use std::collections::HashSet;
use std::rc::Rc;

use ak::{Span, SpanMut, Utf16String, Utf16StringUnits};
use libjs_rust::api::{
    ErrorKind, ErrorType, Gc, Object, PropertyKey, RawNativeFunctionResult, Realm, Trace, Value, Vm,
};

use crate::bindings::conversions::{
    buffer_source_type_of, to_octet_enforcing_range, to_unsigned_long_enforcing_range,
    to_unsigned_short_enforcing_range,
};
use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi, realm_arguments};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::crypto::crypto::web_crypto_fill_with_random;
use crate::crypto::crypto_bindings::{CryptoKeyPair, JsonWebKey, RawJsonWebKey};
use crate::crypto::crypto_key::{CryptoKey, InternalAlgorithmData, KEY_USAGES, KeyType, KeyUsage};
use crate::crypto::subtle_crypto::{
    AlgorithmIdentifier, CryptoKeyOrCryptoKeyPair, KeyFormat, dom_error, get_a_copy_of_the_bytes,
    normalize_an_algorithm,
};

// https://w3c.github.io/webcrypto/#algorithm-overview
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AlgorithmParams {
    pub(crate) name: Utf16String,
}

#[derive(Clone)]
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
    fn web_crypto_decode_base64url(
        raw: usize,
        out: *mut c_void,
        state: *mut c_void,
        receive: Option<ReceiveBytes>,
        error_message: &mut usize,
    ) -> Base64DecodeResult;
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
    state: *mut c_void,
    receive: Option<ReceiveBytes>,
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
    let result =
        unsafe { web_crypto_decode_base64url(base64_url_string.raw_identity(), out, state, receive, error_message) };
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
    unsafe { base64_url_bytes_decode(&base64_url_string, out, null_mut(), None, error_message) }
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
    X25519,
    X448,
    ED25519,
    ED448,
    MlDsa,
    MlKem,
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
    Ed448Params,
    ContextParams,
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

    fn encrypt(&self, _cx: &CallContext, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("encrypt is not supported"),
        ))
    }

    fn decrypt(&self, _cx: &CallContext, _key: Ref<CryptoKey>, _data: &Bytes) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("decrypt is not supported"),
        ))
    }

    fn sign(
        &self,
        _realm: Gc<Realm>,
        _algorithm: &AlgorithmParams,
        _key: Ref<CryptoKey>,
        _data: &Bytes,
    ) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("sign is not supported"),
        ))
    }

    fn verify(
        &self,
        _realm: Gc<Realm>,
        _algorithm: &AlgorithmParams,
        _key: Ref<CryptoKey>,
        _signature: &Bytes,
        _data: &Bytes,
    ) -> WebIdlResult<bool> {
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

    fn derive_bits(
        &self,
        _realm: Gc<Realm>,
        _algorithm: &AlgorithmParams,
        _key: Ref<CryptoKey>,
        _length: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("deriveBits is not supported"),
        ))
    }

    fn import_key(
        &self,
        _cx: &CallContext,
        _algorithm: &AlgorithmParams,
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
        _cx: &CallContext,
        _algorithm: &AlgorithmParams,
        _extractable: bool,
        _usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("generateKey is not supported"),
        ))
    }

    fn export_key(&self, _cx: &CallContext, _format: KeyFormat, _key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("exportKey is not supported"),
        ))
    }

    fn get_key_length(&self, _realm: Gc<Realm>, _algorithm: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
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

    fn encrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let realm = cx.realm();
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

    fn decrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let realm = cx.realm();
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

    fn sign(
        &self,
        realm: Gc<Realm>,
        _algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        data: &Bytes,
    ) -> WebIdlResult<Bytes> {
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

    fn verify(
        &self,
        realm: Gc<Realm>,
        _algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        data: &Bytes,
    ) -> WebIdlResult<bool> {
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

    fn derive_bits(
        &self,
        realm: Gc<Realm>,
        _algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length: Option<u32>,
    ) -> WebIdlResult<Bytes> {
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
        cx: &CallContext,
        _algorithm: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        let realm = cx.realm();
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
        cx: &CallContext,
        _algorithm: &AlgorithmParams,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        let realm = cx.realm();
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

    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        let realm = cx.realm();
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

    fn get_key_length(&self, realm: Gc<Realm>, _algorithm: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
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

fn key_usage_to_string(usage: KeyUsage) -> &'static str {
    std::str::from_utf8(KEY_USAGES[usage as usize]).expect("a key usage is ASCII")
}

fn parse_jwk_symmetric_key(vm: &Vm, jwk: &JsonWebKey) -> WebIdlResult<Bytes> {
    let Some(base64_url_string) = &jwk.k else {
        return Err(dom_error("DataError", b"JWK has no 'k' field"));
    };

    let mut result: Option<Bytes> = None;
    let mut error_message = 0;
    // SAFETY: The receiver borrows the decoded bytes for the call.
    let status = unsafe {
        base64_url_bytes_decode(
            base64_url_string,
            null_mut(),
            core::ptr::from_mut(&mut result).cast(),
            Some(receive_bytes),
            &mut error_message,
        )
    };
    match status {
        Base64DecodeResult::Success => Ok(result.expect("LibWeb returns the decoded bytes")),
        Base64DecodeResult::OutOfMemory => vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into),
        Base64DecodeResult::NonAscii => Err(dom_error("DataError", b"base64 decode: invalid base64url")),
        Base64DecodeResult::InvalidBase64 => {
            // SAFETY: The decoder hands over an owned diagnostic string.
            let message = unsafe { Utf16String::from_raw_owned(error_message) };
            Err(Exception::Dom("DataError", message))
        }
    }
}

// https://www.rfc-editor.org/rfc/rfc7517#section-4.3
fn validate_jwk_key_ops(jwk: &JsonWebKey, usages: &[KeyUsage]) -> WebIdlResult<()> {
    // Use of the "key_ops" member is OPTIONAL, unless the application requires its presence.
    let Some(key_operations) = &jwk.key_ops else {
        return Ok(());
    };

    // Duplicate key operation values MUST NOT be present in the array
    let mut seen_operations = HashSet::new();
    for key_operation in key_operations {
        if !seen_operations.insert(key_operation) {
            return Err(Exception::Dom(
                "DataError",
                libjs_rust::api::utf16_formatted("Duplicate key operation: {}", &[key_operation]),
            ));
        }
    }

    // Multiple unrelated key operations SHOULD NOT be specified for a key because of the potential
    // vulnerabilities associated with using the same key with multiple algorithms.  Thus, the
    // combinations "sign" with "verify", "encrypt" with "decrypt", and "wrapKey" with "unwrapKey"
    // are permitted, but other combinations SHOULD NOT be used.
    let is_used_for_signing = seen_operations.contains(&Utf16String::from_ascii(b"sign"))
        || seen_operations.contains(&Utf16String::from_ascii(b"verify"));
    let is_used_for_encryption = seen_operations.contains(&Utf16String::from_ascii(b"encrypt"))
        || seen_operations.contains(&Utf16String::from_ascii(b"decrypt"));
    let is_used_for_wrapping = seen_operations.contains(&Utf16String::from_ascii(b"wrapKey"))
        || seen_operations.contains(&Utf16String::from_ascii(b"unwrapKey"));
    let number_of_operation_types =
        u8::from(is_used_for_signing) + u8::from(is_used_for_encryption) + u8::from(is_used_for_wrapping);
    if number_of_operation_types > 1 {
        return Err(dom_error(
            "DataError",
            b"Multiple unrelated key operations are specified",
        ));
    }

    // The "use" and "key_ops" JWK members SHOULD NOT be used together; however, if both are used,
    // the information they convey MUST be consistent. Applications should specify which of these
    // members they use, if either is to be used by the application.
    if let Some(key_use) = &jwk.r#use {
        for key_operation in key_operations {
            if *key_operation == Utf16String::from_ascii(b"deriveKey")
                || *key_operation == Utf16String::from_ascii(b"deriveBits")
            {
                continue;
            }
            if *key_use == Utf16String::from_ascii(b"sig")
                && *key_operation != Utf16String::from_ascii(b"sign")
                && *key_operation != Utf16String::from_ascii(b"verify")
            {
                return Err(dom_error(
                    "DataError",
                    b"use=sig but key_ops does not contain 'sign' or 'verify'",
                ));
            }
            if *key_use == Utf16String::from_ascii(b"enc")
                && (*key_operation == Utf16String::from_ascii(b"sign")
                    || *key_operation == Utf16String::from_ascii(b"verify"))
            {
                return Err(dom_error(
                    "DataError",
                    b"use=enc but key_ops contains 'sign' or 'verify'",
                ));
            }
        }
    }

    // NOTE: This validation happens in multiple places in the spec, so it is here for convenience.
    for &usage in usages {
        let usage = key_usage_to_string(usage);
        if !seen_operations.contains(&Utf16String::from_utf8(usage)) {
            return Err(Exception::Dom(
                "DataError",
                libjs_rust::api::utf16_formatted("Missing key_ops usage: {}", &[&usage]),
            ));
        }
    }

    Ok(())
}

// NB: Keeps only the bytes that hold the first length_in_bits bits of data, and zeroes the unused low-order bits of
//     the last byte when the length is not a whole number of bytes.
fn keep_first_bits(data: &mut Vec<u8>, length_in_bits: usize) {
    let length_in_bytes = length_in_bits.div_ceil(8);
    assert!(length_in_bytes <= data.len());
    data.truncate(length_in_bytes);

    let remainder_bits = length_in_bits % 8;
    if remainder_bits != 0 {
        data[length_in_bytes - 1] &= !(0xff >> remainder_bits);
    }
}

fn generate_random_key(vm: &Vm, size_in_bits: u32) -> WebIdlResult<Vec<u8>> {
    let size_in_bits = size_in_bits as usize;
    let length_in_bytes = size_in_bits.div_ceil(8);
    let mut key_buffer = Vec::new();
    if key_buffer.try_reserve_exact(length_in_bytes).is_err() {
        return vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into);
    }
    key_buffer.resize(length_in_bytes, 0);
    // SAFETY: The service writes the bytes during this call.
    unsafe { web_crypto_fill_with_random(SpanMut::new(&mut key_buffer)) };
    keep_first_bits(&mut key_buffer, size_in_bits);
    Ok(key_buffer)
}

// NB: Our KMAC implementation is byte-oriented, so it cannot use keys with a partial last byte. OpenSSL also only
//     accepts KMAC keys of up to 512 bytes.
fn validate_kmac_key_length(length_in_bits: u32) -> WebIdlResult<()> {
    if !length_in_bits.is_multiple_of(8) {
        return Err(dom_error(
            "NotSupportedError",
            b"KMAC key length must be a multiple of 8",
        ));
    }
    if length_in_bits > 512 * 8 {
        return Err(dom_error(
            "NotSupportedError",
            b"KMAC key length must not exceed 4096 bits",
        ));
    }
    Ok(())
}

#[repr(i32)]
#[derive(Clone, Copy)]
enum KmacKind {
    Kmac128,
    Kmac256,
}

unsafe extern "C" {
    fn web_crypto_hmac_sign(
        kind: ShaHashKind,
        key: Span<'_, u8>,
        message: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    );
    fn web_crypto_timing_safe_compare(left: Span<'_, u8>, right: Span<'_, u8>) -> bool;
    fn web_crypto_kmac_default_key_length(kind: KmacKind) -> u32;
    fn web_crypto_kmac_validate_parameters(output_length: u32, customization: OptionalBytes<'_>) -> bool;
    fn web_crypto_kmac_sign(
        kind: KmacKind,
        key: Span<'_, u8>,
        message: Span<'_, u8>,
        output_length: u32,
        customization: OptionalBytes<'_>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
}

fn hmac_calculate_message_digest(hash_name: &Utf16String, key: &[u8], message: &[u8]) -> WebIdlResult<Bytes> {
    let hash_kind = if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
        ShaHashKind::Sha1
    } else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
        ShaHashKind::Sha256
    } else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
        ShaHashKind::Sha384
    } else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
        ShaHashKind::Sha512
    } else {
        return Err(Exception::Dom(
            "NotSupportedError",
            libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash_name]),
        ));
    };

    let mut mac: Option<Bytes> = None;
    // SAFETY: The input and output are lent for the duration of the call.
    unsafe {
        web_crypto_hmac_sign(
            hash_kind,
            Span::new(key),
            Span::new(message),
            core::ptr::from_mut(&mut mac).cast(),
            receive_bytes,
        );
    }
    Ok(mac.expect("LibCrypto returns a MAC"))
}

fn hmac_hash_block_size(hash: &HashAlgorithmIdentifier) -> WebIdlResult<u32> {
    let hash_name = &hash.name;
    if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
        return Ok(64);
    }
    if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
        return Ok(64);
    }
    if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
        return Ok(128);
    }
    if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
        return Ok(128);
    }
    Err(Exception::Dom(
        "NotSupportedError",
        libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash_name]),
    ))
}

fn kmac_kind_from_algorithm_name(algorithm_name: &Utf16String) -> Option<KmacKind> {
    if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"KMAC128")) {
        return Some(KmacKind::Kmac128);
    }
    if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"KMAC256")) {
        return Some(KmacKind::Kmac256);
    }
    None
}

fn kmac_calculate_mac(
    algorithm_name: &Utf16String,
    key: &[u8],
    message: &[u8],
    output_length_bits: u32,
    customization: Option<&Bytes>,
) -> WebIdlResult<Bytes> {
    let Some(kind) = kmac_kind_from_algorithm_name(algorithm_name) else {
        return Err(dom_error("OperationError", b"Unsupported KMAC algorithm"));
    };

    let mut mac: Option<Bytes> = None;
    // SAFETY: The input and output are lent for the duration of the call.
    let success = unsafe {
        web_crypto_kmac_sign(
            kind,
            Span::new(key),
            Span::new(message),
            output_length_bits,
            customization.into(),
            core::ptr::from_mut(&mut mac).cast(),
            receive_bytes,
        )
    };
    if !success {
        return Err(dom_error("OperationError", b"KMAC operation failed"));
    }
    Ok(mac.expect("LibCrypto returns a MAC"))
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct HmacImportParams {
    #[gc(untraced)]
    hash: HashAlgorithmIdentifier,
    length: Option<u32>,
}

impl HmacImportParams {
    pub(crate) fn from_value(vm: &Vm, realm: Gc<Realm>, object: Gc<Object>) -> WebIdlResult<Self> {
        let hash_value = object.get(vm, &PropertyKey::from_utf8("hash"))?;
        let hash = hash_algorithm_identifier_from_value(vm, realm, hash_value, None)?;

        let mut length = None;
        let length_property = PropertyKey::from_utf8("length");
        if object.has_property(vm, &length_property)? {
            let length_value = object.get(vm, &length_property)?;
            length = Some(to_unsigned_long_enforcing_range(vm, length_value)?);
        }

        Ok(Self { hash, length })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct HmacKeyGenParams {
    #[gc(untraced)]
    hash: HashAlgorithmIdentifier,
    length: Option<u32>,
}

impl HmacKeyGenParams {
    pub(crate) fn from_value(vm: &Vm, realm: Gc<Realm>, object: Gc<Object>) -> WebIdlResult<Self> {
        let hash_value = object.get(vm, &PropertyKey::from_utf8("hash"))?;
        let hash = hash_algorithm_identifier_from_value(vm, realm, hash_value, None)?;

        let mut length = None;
        let length_property = PropertyKey::from_utf8("length");
        if object.has_property(vm, &length_property)? {
            let length_value = object.get(vm, &length_property)?;
            length = Some(to_unsigned_long_enforcing_range(vm, length_value)?);
        }

        Ok(Self { hash, length })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Hmac;

impl AlgorithmMethods for Hmac {
    // https://w3c.github.io/webcrypto/#hmac-operations-sign
    fn sign(&self, _: Gc<Realm>, _: &AlgorithmParams, key: Ref<CryptoKey>, message: &Bytes) -> WebIdlResult<Bytes> {
        // 1. Let mac be the result of performing the MAC Generation operation described in Section 4 of
        //    [FIPS-198-1] using the key represented by [[handle]] internal slot of key, the hash
        //    function identified by the hash attribute of the [[algorithm]] internal slot of key and
        //    message as the input data text.
        let key_data = key.byte_handle_bytes();
        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::Hmac { hash, .. } = &*algorithm else {
            unreachable!("the key has an HMAC algorithm")
        };
        let mac = hmac_calculate_message_digest(hash, key_data, message.as_slice())?;

        // 2. Return the result of creating an ArrayBuffer containing mac.
        Ok(mac)
    }

    // https://w3c.github.io/webcrypto/#hmac-operations-verify
    fn verify(
        &self,
        _: Gc<Realm>,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        // 1. Let mac be the result of performing the MAC Generation operation described in Section 4 of
        //    [FIPS-198-1] using the key represented by [[handle]] internal slot of key, the hash
        //    function identified by the hash attribute of the [[algorithm]] internal slot of key and
        //    message as the input data text.
        let key_data = key.byte_handle_bytes();
        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::Hmac { hash, .. } = &*algorithm else {
            unreachable!("the key has an HMAC algorithm")
        };
        let mac = hmac_calculate_message_digest(hash, key_data, message.as_slice())?;

        // 2. Return true if mac is equal to signature and false otherwise. This comparison must be performed in
        //    constant-time.
        // SAFETY: The input bytes live for the duration of the comparison.
        Ok(mac.as_slice().len() == signature.as_slice().len()
            && unsafe { web_crypto_timing_safe_compare(Span::new(mac.as_slice()), Span::new(signature.as_slice())) })
    }

    // https://w3c.github.io/webcrypto/#hmac-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, _: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot
        //    of key cannot be accessed, then throw an OperationError.
        // NOTE: In our impl this is always accessible

        // 2. Let bits be the raw bits of the key represented by [[handle]] internal slot of key.
        // 3. Let data be an octet string containing bits.
        let data = key.byte_handle_bytes();

        // 4. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing symmetric algorithms in WebCrypto, "raw-secret" acts as an alias of "raw".
        let result = if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        // If format is "jwk":
        else if format == KeyFormat::Jwk {
            // Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // Set the k attribute of jwk to be a string containing data, encoded according to Section
            // 6.4 of JSON Web Algorithms [JWA].
            jwk.k = Some(base64_url_bytes_encode(data).expect("base64url encoding succeeds"));

            // Let algorithm be the [[algorithm]] internal slot of key.
            let algorithm = key.internal_algorithm();

            // Let hash be the hash attribute of algorithm.
            // If the name attribute of hash is "SHA-1":
            let InternalAlgorithmData::Hmac { hash: hash_name, .. } = &*algorithm else {
                unreachable!("the key has an HMAC algorithm")
            };
            if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
                // Set the alg attribute of jwk to the string "HS1".
                jwk.alg = Some(Utf16String::from_ascii(b"HS1"));
            }
            // If the name attribute of hash is "SHA-256":
            else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
                // Set the alg attribute of jwk to the string "HS256".
                jwk.alg = Some(Utf16String::from_ascii(b"HS256"));
            }
            // If the name attribute of hash is "SHA-384":
            else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
                // Set the alg attribute of jwk to the string "HS384".
                jwk.alg = Some(Utf16String::from_ascii(b"HS384"));
            }
            // If the name attribute of hash is "SHA-512":
            else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
                // Set the alg attribute of jwk to the string "HS512".
                jwk.alg = Some(Utf16String::from_ascii(b"HS512"));
            }
            // Otherwise, the name attribute of hash is defined in another applicable
            // specification:
            else {
                // Perform any key export steps defined by other applicable specifications,
                // passing format and key and obtaining alg.
                // Set the alg attribute of jwk to alg.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // Set the key_ops attribute of jwk to equal the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        };

        // 5. Return result.
        Ok(result)
    }
}

impl AlgorithmMethods for HmacKeyGenParams {
    // https://w3c.github.io/webcrypto/#hmac-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let normalized_algorithm = self;

        // NB: Step 2 throws an error when the length member of normalizedAlgorithm is not present and we do not know the
        //     block size of the hash function, or when the length member is zero.
        if normalized_algorithm.length.is_none() {
            hmac_hash_block_size(&normalized_algorithm.hash)?;
        } else if normalized_algorithm.length == Some(0) {
            return Err(dom_error("OperationError", b"Invalid length"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#hmac-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "sign" or "verify", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2. If the length member of normalizedAlgorithm is not present:
        let normalized_algorithm = self;
        let length = if normalized_algorithm.length.is_none() {
            // Let length be the block size in bits of the hash function identified by the hash member
            // of normalizedAlgorithm.
            hmac_hash_block_size(&normalized_algorithm.hash)? * 8
        }
        // Otherwise, if the length member of normalizedAlgorithm is non-zero:
        else if let Some(length @ 1..) = normalized_algorithm.length {
            // Let length be equal to the length member of normalizedAlgorithm.
            length
        }
        // Otherwise:
        else {
            // throw an OperationError.
            return Err(dom_error("OperationError", b"Invalid length"));
        };

        // 3. Generate a key of length length bits.
        // 4. If the key generation step fails, then throw an OperationError.
        let key_data =
            generate_random_key(cx.vm(), length).map_err(|_| dom_error("OperationError", b"Failed to generate key"))?;

        // 5. Let key be a new CryptoKey object representing the generated key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_data);

        // 6. Let algorithm be a new HmacKeyAlgorithm.
        // 7. Set the name attribute of algorithm to "HMAC".
        // 8. Let hash be a new KeyAlgorithm.
        // 9. Set the name attribute of hash to equal the name member of the hash member of normalizedAlgorithm.
        let hash_name = normalized_algorithm.hash.name.clone();

        // 12. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 10. Set the hash attribute of algorithm to hash.
        // 11. Set the length attribute of algorithm to length.
        // 13. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Hmac {
            name: Utf16String::from_ascii(b"HMAC"),
            hash: hash_name,
            length,
        });

        // 14. Set the [[extractable]] internal slot of key to be extractable.
        key.set_extractable(extractable);

        // 15. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(usages));

        // 16. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }
}

impl AlgorithmMethods for HmacImportParams {
    // https://w3c.github.io/webcrypto/#hmac-operations-import-key
    fn validate_import_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let normalized_algorithm = self;

        // 1. If the length member of normalizedAlgorithm is present and is zero, then throw a DataError.
        if normalized_algorithm.length == Some(0) {
            return Err(dom_error("DataError", b"Invalid key length"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#hmac-operations-import-key
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the key algorithm's unsigned long length truncates to 32 bits"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        key_format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        let normalized_algorithm = self;

        // NB: Step 1 is performed by validate_import_key_parameters().
        self.validate_import_key_parameters(cx.realm())?;

        // 2. Let keyData be the key data to be imported.
        // 3. If usages contains an entry which is not "sign" or "verify", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 4. Let hash be a new KeyAlgorithm.
        let hash_name;

        // 5. If format is "raw":
        let data;
        if key_format == KeyFormat::Raw || key_format == KeyFormat::RawSecret {
            // 5.1. Let data be the octet string contained in keyData.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;

            // 5.2. Set hash to equal the hash member of normalizedAlgorithm.
            hash_name = normalized_algorithm.hash.name.clone();
        }
        // If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary:
            //    Let jwk equal keyData.
            //    Otherwise:
            //    Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"Data is not a JsonWebKey dictionary"));
            };

            // 2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA],
            //    then throw a DataError.
            // 4. Let data be the octet string obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            // 5. Set the hash to equal the hash member of normalizedAlgorithm.
            hash_name = normalized_algorithm.hash.name.clone();

            // 6. If the name attribute of hash is "SHA-1":
            if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
                // If the alg field of jwk is present and is not "HS1", then throw a DataError.
                if jwk
                    .alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"HS1"))
                {
                    return Err(dom_error("DataError", b"Invalid algorithm"));
                }
            }
            // If the name attribute of hash is "SHA-256":
            else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
                // If the alg field of jwk is present and is not "HS256", then throw a DataError.
                if jwk
                    .alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"HS256"))
                {
                    return Err(dom_error("DataError", b"Invalid algorithm"));
                }
            }
            // If the name attribute of hash is "SHA-384":
            else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
                // If the alg field of jwk is present and is not "HS384", then throw a DataError.
                if jwk
                    .alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"HS384"))
                {
                    return Err(dom_error("DataError", b"Invalid algorithm"));
                }
            }
            // If the name attribute of hash is "SHA-512":
            else if matches!(hash_name.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
                // If the alg field of jwk is present and is not "HS512", then throw a DataError.
                if jwk
                    .alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"HS512"))
                {
                    return Err(dom_error("DataError", b"Invalid algorithm"));
                }
            }
            // Otherwise, if the name attribute of hash is defined in another applicable specification:
            else {
                // Perform any key import steps defined by other applicable specifications, passing format,
                // jwk and hash and obtaining hash.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 7. If usages is non-empty and the use field of jwk is present and is not "sig", then
            //    throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid use in JsonWebKey"));
            }

            // 8. If the key_ops field of jwk is present, and is invalid according to the requirements
            //    of JSON Web Key [JWK] or does not contain all of the specified usages values, then
            //    throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 9. If the ext field of jwk is present and has the value false and extractable is true,
            //    then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 6. Let length be equivalent to the length, in octets, of data, multiplied by 8.
        let mut length = data.as_slice().len() * 8;

        // 7. If length is zero then throw a DataError.
        if length == 0 {
            return Err(dom_error("DataError", b"No data provided"));
        }

        // 8. If the length member of normalizedAlgorithm is present:
        if let Some(normalized_algorithm_length) = normalized_algorithm.length {
            // If the length member of normalizedAlgorithm is greater than length:
            let normalized_algorithm_length = normalized_algorithm_length as usize;
            if normalized_algorithm_length > length {
                // throw a DataError.
                return Err(dom_error("DataError", b"Invalid data size"));
            }

            // If the length member of normalizedAlgorithm, is less than or equal to length minus eight:
            if normalized_algorithm_length <= length - 8 {
                // throw a DataError.
                return Err(dom_error("DataError", b"Invalid data size"));
            }

            // Otherwise:
            // Set length equal to the length member of normalizedAlgorithm.
            length = normalized_algorithm_length;
        }

        // 9. Let key be a new CryptoKey object representing an HMAC key with the first length bits of data.
        let mut data = Rc::unwrap_or_clone(data.0);
        keep_first_bits(&mut data, length);
        let key = CryptoKey::from_byte_handle(cx.vm(), &data);

        // 10. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 11. Let algorithm be a new HmacKeyAlgorithm.
        // 12. Set the name attribute of algorithm to "HMAC".
        // 13. Set the length attribute of algorithm to length.
        // 14. Set the hash attribute of algorithm to hash.
        // 15. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Hmac {
            name: Utf16String::from_ascii(b"HMAC"),
            hash: hash_name,
            length: length as u32,
        });

        // 16. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#hmac-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        let normalized_derived_key_algorithm = self;

        // 1. If the length member of normalizedDerivedKeyAlgorithm is not present:
        let length = if normalized_derived_key_algorithm.length.is_none() {
            // Let length be the block size in bits of the hash function identified by the hash member of
            // normalizedDerivedKeyAlgorithm.
            hmac_hash_block_size(&normalized_derived_key_algorithm.hash)? * 8
        }
        // Otherwise, if the length member of normalizedDerivedKeyAlgorithm is non-zero:
        else if let Some(length @ 1..) = normalized_derived_key_algorithm.length {
            // Let length be equal to the length member of normalizedDerivedKeyAlgorithm.
            length
        }
        // Otherwise:
        else {
            // throw a TypeError.
            return Err(Exception::type_error("Invalid key length"));
        };

        // 2. Return length.
        Ok(Some(length))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct KmacParams {
    output_length: u32,
    customization: Option<Bytes>,
}

impl KmacParams {
    // https://wicg.github.io/webcrypto-modern-algos/#kmac-params
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        // 1. If the outputLength member is missing, throw a TypeError.
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

        // 2. Let customization be the optional customization member or the empty octet string.
        let customization = get_optional_buffer_source(vm, object, &PropertyKey::from_utf8("customization"))?;

        Ok(Self {
            output_length: length,
            customization,
        })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct KmacKeyGenParams {
    length: Option<u32>,
}

impl KmacKeyGenParams {
    // https://wicg.github.io/webcrypto-modern-algos/#kmac-keygen-params
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        // 1. If the length member is present, record its unsigned long value; otherwise leave it unset.
        let mut length = None;
        let length_property = PropertyKey::from_utf8("length");
        if object.has_property(vm, &length_property)? {
            let length_value = object.get(vm, &length_property)?;
            length = Some(to_unsigned_long_enforcing_range(vm, length_value)?);
        }

        Ok(Self { length })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct KmacImportParams {
    length: Option<u32>,
}

impl KmacImportParams {
    // https://wicg.github.io/webcrypto-modern-algos/#kmac-import-params
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        // 1. If the length member is present, record its unsigned long value; otherwise leave it unset.
        let mut length = None;
        let length_property = PropertyKey::from_utf8("length");
        if object.has_property(vm, &length_property)? {
            let length_value = object.get(vm, &length_property)?;
            length = Some(to_unsigned_long_enforcing_range(vm, length_value)?);
        }

        Ok(Self { length })
    }
}

impl AlgorithmMethods for KmacParams {
    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-sign
    fn validate_sign_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Step 2 fails for the output lengths and customizations that our KMAC does not support.
        let normalized_algorithm = self;
        // SAFETY: The parameters are lent for the call.
        let result = unsafe {
            web_crypto_kmac_validate_parameters(
                normalized_algorithm.output_length,
                normalized_algorithm.customization.as_ref().into(),
            )
        };
        if !result {
            return Err(dom_error("OperationError", b"KMAC operation failed"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-sign
    fn sign(
        &self,
        _: Gc<Realm>,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        message: &Bytes,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;

        // 1. Let customization be the customization member of normalizedAlgorithm if present or the empty octet string otherwise.
        let customization = normalized_algorithm.customization.as_ref();

        let key_data = key.byte_handle_bytes();
        let output_length = normalized_algorithm.output_length;

        // 2. If the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC128":
        //    Let mac be the result of performing the KMAC128 function defined in Section 4 of [NIST-SP800-185]
        //    using the key represented by [[handle]] internal slot of key as the K input parameter,
        //    message as the X input parameter, the outputLength member of normalizedAlgorithm as the L input parameter,
        //    and customization as the S input parameter.
        // 2. If the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC256":
        //    Let mac be the result of performing the KMAC256 function defined in Section 4 of [NIST-SP800-185]
        //    using the key represented by [[handle]] internal slot of key as the K input parameter,
        //    message as the X input parameter, the outputLength member of normalizedAlgorithm as the L input parameter,
        //    and customization as the S input parameter.
        let mac = kmac_calculate_mac(
            &algorithm.name,
            key_data,
            message.as_slice(),
            output_length,
            customization,
        )?;

        // 3. Return a byte sequence containing mac.
        Ok(mac)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-verify
    fn validate_verify_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Step 2 fails for the output lengths and customizations that our KMAC does not support.
        let normalized_algorithm = self;
        // SAFETY: The parameters are lent for the call.
        let result = unsafe {
            web_crypto_kmac_validate_parameters(
                normalized_algorithm.output_length,
                normalized_algorithm.customization.as_ref().into(),
            )
        };
        if !result {
            return Err(dom_error("OperationError", b"KMAC operation failed"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-verify
    fn verify(
        &self,
        _: Gc<Realm>,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        let normalized_algorithm = self;

        // 1. Let customization be the customization member of normalizedAlgorithm if present or the empty octet string otherwise.
        let customization = normalized_algorithm.customization.as_ref();

        let key_data = key.byte_handle_bytes();
        let output_length = normalized_algorithm.output_length;

        // 2. If the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC128":
        //    Let mac be the result of performing the KMAC128 function defined in Section 4 of [NIST-SP800-185]
        //    using the key represented by [[handle]] internal slot of key as the K input parameter, message as the X input parameter,
        //    the outputLength member of normalizedAlgorithm as the L input parameter, and customization as the S input parameter.
        // 2. If the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC256":
        //    Let mac be the result of performing the KMAC256 function defined in Section 4 of [NIST-SP800-185]
        //    using the key represented by [[handle]] internal slot of key as the K input parameter, message as the X input parameter,
        //    the outputLength member of normalizedAlgorithm as the L input parameter, and customization as the S input parameter.
        let mac = kmac_calculate_mac(
            &algorithm.name,
            key_data,
            message.as_slice(),
            output_length,
            customization,
        )?;

        // 3. Let computedMac be a byte sequence containing mac.
        // 4. Return true if computedMac is equal to signature and false otherwise. This comparison must be performed in
        //    constant-time.
        // SAFETY: The input bytes live for the duration of the comparison.
        Ok(mac.as_slice().len() == signature.as_slice().len()
            && unsafe { web_crypto_timing_safe_compare(Span::new(mac.as_slice()), Span::new(signature.as_slice())) })
    }
}

impl AlgorithmMethods for KmacKeyGenParams {
    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Generating a key of a length that our KMAC does not support fails.
        let normalized_algorithm = self;
        if let Some(length) = normalized_algorithm.length {
            validate_kmac_key_length(length)?;
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "sign" or "verify", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let normalized_algorithm = self;

        // 2. If the length member of normalizedAlgorithm is present:
        //    Let length be equal to the length member of normalizedAlgorithm.
        // 2. Otherwise, if the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC128":
        //    Let length be 128.
        // 2. Otherwise, if the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC256":
        //    Let length be 256.
        let length = if let Some(length) = normalized_algorithm.length {
            length
        } else {
            let Some(kind) = kmac_kind_from_algorithm_name(&algorithm.name) else {
                return Err(dom_error("OperationError", b"Unsupported KMAC algorithm"));
            };
            // SAFETY: The enum variant names a LibCrypto KMAC algorithm.
            unsafe { web_crypto_kmac_default_key_length(kind) }
        };

        validate_kmac_key_length(length)?;

        // 3. Generate a key of length bits.
        // 4. If key generation fails, throw an OperationError.
        let key_data =
            generate_random_key(cx.vm(), length).map_err(|_| dom_error("OperationError", b"Failed to generate key"))?;

        // 5. Let key be a new CryptoKey object representing the generated key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_data);

        // 6. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 7. Let algorithm be a new KmacKeyAlgorithm.
        // 8. Set the name attribute of algorithm to the name member of normalizedAlgorithm.
        // 9. Set the length attribute of algorithm to length.
        // 10. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Kmac {
            name: algorithm.name.clone(),
            length,
        });

        // 11. Set the [[extractable]] internal slot of key to extractable.
        key.set_extractable(extractable);

        // 12. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(usages));

        // 13. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }
}

impl AlgorithmMethods for KmacImportParams {
    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-import-key
    fn validate_import_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Importing a key of a length that our KMAC does not support fails.
        let normalized_algorithm = self;
        if let Some(length) = normalized_algorithm.length {
            validate_kmac_key_length(length)?;
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the spec checks the JWK algorithm after matching the KMAC variant"
    )]
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the key algorithm's unsigned long length truncates to 32 bits"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        let normalized_algorithm = self;

        // 1. Let keyData be the key data to be imported.

        // 2. If usages contains an entry which is not "sign" or "verify", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let data;

        // 3. If format is "raw-secret":
        if format == KeyFormat::RawSecret {
            // 1. Let data be keyData.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;
        }
        // 3. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary:
            //        Let jwk equal keyData.
            //    Otherwise:
            //        Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"Data is not a JsonWebKey dictionary"));
            };

            // 2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
            // NOTE: parse_jwk_symmetric_key() performs the validation required for symmetric JWKs.

            // 4. Let data be the byte sequence obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            // 5. -> If the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC128":
            //           If the alg field of jwk is present and is not "K128", then throw a DataError.
            //    -> If the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC256":
            //           If the alg field of jwk is present and is not "K256", then throw a DataError.
            let algorithm_name = &algorithm.name;
            if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"KMAC128")) {
                if jwk
                    .alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"K128"))
                {
                    return Err(dom_error("DataError", b"Invalid alg field for KMAC128"));
                }
            } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"KMAC256")) {
                if jwk
                    .alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"K256"))
                {
                    return Err(dom_error("DataError", b"Invalid alg field for KMAC256"));
                }
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid use in JsonWebKey"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // 3. Otherwise:
        else {
            // 1. Throw a NotSupportedError.
            return Err(dom_error(
                "NotSupportedError",
                b"Only raw-secret and jwk formats are supported",
            ));
        }

        // 4. Let length be the length in bits of data.
        let mut length = (data.as_slice().len() * 8) as u32;

        // 5. If the length member of normalizedAlgorithm is present:
        if let Some(requested_length) = normalized_algorithm.length {
            // 1. If the length member of normalizedAlgorithm is greater than length, then throw a DataError.
            if requested_length > length {
                return Err(dom_error("DataError", b"Invalid data size"));
            }

            // 2. If the length member of normalizedAlgorithm is less than or equal to length minus eight, then throw a DataError.
            if requested_length.wrapping_add(8) <= length {
                return Err(dom_error("DataError", b"Invalid data size"));
            }

            // 3. Otherwise:
            //        Set length equal to the length member of normalizedAlgorithm.
            length = requested_length;
        }

        validate_kmac_key_length(length)?;

        // 6. Let key be a new CryptoKey object representing a KMAC key with the first length bits of the Keccak decoding
        //    of data.
        // NB: The length is a multiple of 8 and less than 8 bits shorter than data, so the key is all of data.
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 7. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 8. Let algorithm be a new KmacKeyAlgorithm.
        // 9. Set the name attribute of algorithm to the name member of normalizedAlgorithm.
        // 10. Set the length attribute of algorithm to length.
        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Kmac {
            name: algorithm.name.clone(),
            length,
        });

        // 12. Return key.
        Ok(key)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, algorithm: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        let normalized_algorithm = self;

        // 1. If the length member of normalizedAlgorithm is present:
        //        Let length be equal to the length member of normalizedAlgorithm.
        let length = if let Some(length) = normalized_algorithm.length {
            length
        }
        // 1. Otherwise, if the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC128":
        //        Let length be 128.
        // 1. Otherwise, if the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC256":
        //        Let length be 256.
        else {
            let Some(kind) = kmac_kind_from_algorithm_name(&algorithm.name) else {
                return Err(dom_error("OperationError", b"Unsupported KMAC algorithm"));
            };
            // SAFETY: The enum variant names a LibCrypto KMAC algorithm.
            unsafe { web_crypto_kmac_default_key_length(kind) }
        };

        // 2. Return length.
        Ok(Some(length))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Kmac;

impl AlgorithmMethods for Kmac {
    // https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 2. If format is "raw-secret":
        let result = if format == KeyFormat::RawSecret {
            // 1. Let result be the raw bits of the key represented by [[handle]] internal slot of key.
            let data = key.byte_handle_bytes();
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // 3. Set the k attribute of jwk to be a string containing the raw bits of the key represented by [[handle]] internal slot of key,
            //    encoded according to JSON Web Algorithms [JWA].
            let key_bytes = key.byte_handle_bytes();
            let Ok(encoded) = base64_url_bytes_encode(key_bytes) else {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            };
            jwk.k = Some(encoded);

            // 4. Let keyAlgorithm be the [[algorithm]] internal slot of key.
            // 5. -> If the name member of keyAlgorithm is "KMAC128":
            //        Set the alg attribute of jwk to the string "K128".
            //    -> If the name member of keyAlgorithm is "KMAC256":
            //        Set the alg attribute of jwk to the string "K256".
            let algorithm_name = key.algorithm_name();
            if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"KMAC128")) {
                jwk.alg = Some(Utf16String::from_ascii(b"K128"));
            } else if matches!(algorithm_name.as_units(), Utf16StringUnits::Ascii(b"KMAC256")) {
                jwk.alg = Some(Utf16String::from_ascii(b"K256"));
            }

            // 6. Set the key_ops attribute of jwk to equal the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 7. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 8. Let result be jwk.
            return Ok(ExportKeyResult::JsonWebKey(jwk));
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Cannot export to unsupported format"));
        };

        // 3. Return result.
        Ok(result)
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct HkdfParams {
    #[gc(untraced)]
    hash: HashAlgorithmIdentifier,
    salt: Bytes,
    info: Bytes,
}

impl HkdfParams {
    pub(crate) fn from_value(vm: &Vm, realm: Gc<Realm>, object: Gc<Object>) -> WebIdlResult<Self> {
        let hash_value = object.get(vm, &PropertyKey::from_utf8("hash"))?;
        let hash = hash_algorithm_identifier_from_value(vm, realm, hash_value, None)?;

        let salt_value = object.get(vm, &PropertyKey::from_utf8("salt"))?;
        let Some(salt_source) = buffer_source_type_of(salt_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };
        let salt = get_a_copy_of_the_bytes(vm, salt_source)?;

        let info_value = object.get(vm, &PropertyKey::from_utf8("info"))?;
        let Some(info_source) = buffer_source_type_of(info_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };
        let info = get_a_copy_of_the_bytes(vm, info_source)?;

        Ok(Self { hash, salt, info })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Pbkdf2Params {
    salt: Bytes,
    iterations: u32,
    #[gc(untraced)]
    hash: HashAlgorithmIdentifier,
}

impl Pbkdf2Params {
    pub(crate) fn from_value(vm: &Vm, realm: Gc<Realm>, object: Gc<Object>) -> WebIdlResult<Self> {
        let salt_value = object.get(vm, &PropertyKey::from_utf8("salt"))?;

        let Some(salt_source) = buffer_source_type_of(salt_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };

        let salt = get_a_copy_of_the_bytes(vm, salt_source)?;

        let iterations_value = object.get(vm, &PropertyKey::from_utf8("iterations"))?;
        let iterations = to_unsigned_long_enforcing_range(vm, iterations_value)?;

        let hash_value = object.get(vm, &PropertyKey::from_utf8("hash"))?;
        let hash = hash_algorithm_identifier_from_value(vm, realm, hash_value, None)?;

        Ok(Self { salt, iterations, hash })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Argon2Params {
    nonce: Bytes,
    parallelism: u32,
    memory: u32,
    passes: u32,
    version: Option<u8>,
    secret_value: Option<Bytes>,
    associated_data: Option<Bytes>,
}

impl Argon2Params {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let nonce_property = PropertyKey::from_utf8("nonce");
        if !object.has_property(vm, &nonce_property)? {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"nonce"])
                .map_err(Into::into);
        }
        let nonce_value = object.get(vm, &nonce_property)?;
        let Some(nonce_source) = buffer_source_type_of(nonce_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };
        let nonce = get_a_copy_of_the_bytes(vm, nonce_source)?;

        let extract_u32_value = |name: &str| -> WebIdlResult<u32> {
            let property = PropertyKey::from_utf8(name);
            if !object.has_property(vm, &property)? {
                return vm
                    .throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&name])
                    .map_err(Into::into);
            }
            let key_value = object.get(vm, &property)?;
            Ok(to_unsigned_long_enforcing_range(vm, key_value)?)
        };

        let parallelism = extract_u32_value("parallelism")?;
        let memory = extract_u32_value("memory")?;
        let passes = extract_u32_value("passes")?;

        let mut version = None;
        let version_property = PropertyKey::from_utf8("version");
        if object.has_property(vm, &version_property)? {
            let version_value = object.get(vm, &version_property)?;
            version = Some(to_octet_enforcing_range(vm, version_value)?);
        }

        let secret_value = get_optional_buffer_source(vm, object, &PropertyKey::from_utf8("secretValue"))?;
        let associated_data = get_optional_buffer_source(vm, object, &PropertyKey::from_utf8("associatedData"))?;

        Ok(Self {
            nonce,
            parallelism,
            memory,
            passes,
            version,
            secret_value,
            associated_data,
        })
    }
}

#[repr(i32)]
enum Argon2Type {
    Argon2d,
    Argon2i,
    Argon2id,
}

#[repr(C)]
struct Argon2Inputs<'a> {
    message: Span<'a, u8>,
    nonce: Span<'a, u8>,
    parallelism: u32,
    memory: u32,
    passes: u32,
    version: u32,
    secret_value: OptionalBytes<'a>,
    associated_data: OptionalBytes<'a>,
    tag_length: u32,
}

unsafe extern "C" {
    fn web_crypto_hash_digest_size(kind: ShaHashKind) -> usize;
    fn web_crypto_hkdf_derive_key(
        kind: ShaHashKind,
        salt: Span<'_, u8>,
        key: Span<'_, u8>,
        info: Span<'_, u8>,
        length: u32,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_pbkdf2_derive_key(
        kind: ShaHashKind,
        password: Span<'_, u8>,
        salt: Span<'_, u8>,
        iterations: u32,
        length: u32,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_argon2_validate_parameters(nonce: Span<'_, u8>) -> usize;
    fn web_crypto_argon2_derive_key(
        kind: Argon2Type,
        inputs: &Argon2Inputs<'_>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
}

fn sha_hash_kind(hash_algorithm: &Utf16String) -> Result<ShaHashKind, Utf16String> {
    if matches!(hash_algorithm.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
        return Ok(ShaHashKind::Sha1);
    }
    if matches!(hash_algorithm.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
        return Ok(ShaHashKind::Sha256);
    }
    if matches!(hash_algorithm.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
        return Ok(ShaHashKind::Sha384);
    }
    if matches!(hash_algorithm.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
        return Ok(ShaHashKind::Sha512);
    }
    Err(libjs_rust::api::utf16_formatted(
        "Invalid hash function '{}'",
        &[hash_algorithm],
    ))
}

/// # Safety
///
/// The string is live and the output is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_sha_hash_kind(raw: usize, out: &mut i32) -> usize {
    // SAFETY: The caller lends a live string and we take our own reference.
    let hash_algorithm = unsafe {
        ak::reference_utf16_string(raw);
        Utf16String::from_raw_owned(raw)
    };
    match sha_hash_kind(&hash_algorithm) {
        Ok(kind) => {
            *out = kind as i32;
            0
        }
        Err(message) => message.into_raw(),
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Hkdf;

impl AlgorithmMethods for Hkdf {
    // https://w3c.github.io/webcrypto/#hkdf-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. Let keyData be the key data to be imported.

        // 2. If format is "raw":
        //        (… see below …)
        //    Otherwise:
        //        throw a NotSupportedError.
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing symmetric algorithms in WebCrypto, "raw-secret" acts as an alias of "raw".
        if format != KeyFormat::Raw && format != KeyFormat::RawSecret {
            return Err(dom_error("NotSupportedError", b"Only raw format is supported"));
        }

        //        1. If usages contains a value that is not "deriveKey" or "deriveBits", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        //        2. If extractable is not false, then throw a SyntaxError.
        if extractable {
            return Err(dom_error("SyntaxError", b"extractable must be false"));
        }

        //        3. Let key be a new CryptoKey representing the key data provided in keyData.
        let ImportKeyData::Bytes(data) = key_data else {
            unreachable!("raw key data contains bytes")
        };
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        //        4. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        //        5. Let algorithm be a new KeyAlgorithm object.
        //        6. Set the name attribute of algorithm to "HKDF".
        //        7. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"HKDF"),
        });

        //        8. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#hkdf-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. Return null.
        Ok(None)
    }
}

impl AlgorithmMethods for HkdfParams {
    // https://w3c.github.io/webcrypto/#hkdf-operations-derive-bits
    fn validate_derive_bits_parameters(&self, _: Gc<Realm>, length_optional: Option<u32>) -> WebIdlResult<()> {
        let normalized_algorithm = self;

        // 1. If length is null or is not a multiple of 8, then throw an OperationError.
        let Some(length) = length_optional.filter(|length| length.is_multiple_of(8)) else {
            return Err(dom_error(
                "OperationError",
                b"Length must be specified and divisible by 8",
            ));
        };

        // 2. Let hashLength be the length in bits of the output of the hash function identified by the hash member of
        //    normalizedAlgorithm.
        let hash_kind = sha_hash_kind(&normalized_algorithm.hash.name)
            .map_err(|message| Exception::Dom("NotSupportedError", message))?;
        // SAFETY: The hash kind is one of the native SHA algorithms.
        let hash_length = unsafe { web_crypto_hash_digest_size(hash_kind) } * 8;

        // 3. If length is greater than 255 * hashLength, then throw an OperationError.
        if length as usize > 255 * hash_length {
            return Err(dom_error(
                "OperationError",
                b"Length is too large for the hash function",
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#hkdf-operations-derive-bits
    fn derive_bits(
        &self,
        realm: Gc<Realm>,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;

        // NB: Steps 1 to 3 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(realm, length_optional)?;
        let hash_kind = sha_hash_kind(&normalized_algorithm.hash.name)
            .map_err(|message| Exception::Dom("NotSupportedError", message))?;

        // 4. Let keyDerivationKey be the secret represented by [[handle]] internal slot of key as the message.
        let key_derivation_key = key.byte_handle_bytes();

        // 5. Let result be the result of performing the HKDF extract and then the HKDF expand step described in Section 2 of [RFC5869] using:
        //    * the hash member of normalizedAlgorithm as Hash,
        //    * keyDerivationKey as the input keying material, IKM,
        //    * the contents of the salt member of normalizedAlgorithm as salt,
        //    * the contents of the info member of normalizedAlgorithm as info,
        //    * length divided by 8 as the value of L,
        // Note: Although HKDF technically supports absent salt (treating it as hashLen many NUL bytes),
        // all major browsers instead raise a TypeError, for example:
        //     "Failed to execute 'deriveBits' on 'SubtleCrypto': HkdfParams: salt: Not a BufferSource"
        // Because we are forced by neither peer pressure nor the spec, we don't support it either.

        // Note: Check for zero length early because our implementation doesn't support it.
        let length = length_optional.expect("validation requires a length");
        if length == 0 {
            return Ok(Bytes::new(Vec::new()));
        }

        let mut result: Option<Bytes> = None;
        // SAFETY: The inputs and output are lent for this call.
        let success = unsafe {
            web_crypto_hkdf_derive_key(
                hash_kind,
                Span::new(normalized_algorithm.salt.as_slice()),
                Span::new(key_derivation_key),
                Span::new(normalized_algorithm.info.as_slice()),
                length / 8,
                core::ptr::from_mut(&mut result).cast(),
                receive_bytes,
            )
        };

        // 6. If the key derivation operation fails, then throw an OperationError.
        if !success {
            return Err(dom_error("OperationError", b"Failed to derive key"));
        }

        // 7. Return result
        Ok(result.expect("LibCrypto returns the derived bytes"))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Pbkdf2;

impl AlgorithmMethods for Pbkdf2Params {
    // https://w3c.github.io/webcrypto/#pbkdf2-operations-derive-bits
    fn validate_derive_bits_parameters(&self, _: Gc<Realm>, length_optional: Option<u32>) -> WebIdlResult<()> {
        let normalized_algorithm = self;

        // 1. If length is null or is not a multiple of 8, then throw an OperationError.
        if length_optional.is_none_or(|length| !length.is_multiple_of(8)) {
            return Err(dom_error(
                "OperationError",
                b"Length must be greater than 0 and divisible by 8",
            ));
        }

        // 2. If the iterations member of normalizedAlgorithm is zero, then throw an OperationError.
        if normalized_algorithm.iterations == 0 {
            return Err(dom_error("OperationError", b"Iterations must be greater than 0"));
        }

        // NB: Step 4 fails for the hash functions that our PBKDF2 does not support.
        sha_hash_kind(&normalized_algorithm.hash.name)
            .map_err(|message| Exception::Dom("NotSupportedError", message))?;

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#pbkdf2-operations-derive-bits
    fn derive_bits(
        &self,
        realm: Gc<Realm>,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;

        // NB: Steps 1 and 2 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(realm, length_optional)?;

        // 3. Let prf be the MAC Generation function described in Section 4 of [FIPS-198-1] using the hash function described by the hash member of normalizedAlgorithm.
        let hash_algorithm = &normalized_algorithm.hash.name;

        // 4. Let result be the result of performing the PBKDF2 operation defined in Section 5.2 of [RFC8018]
        // using prf as the pseudo-random function, PRF,
        // the password represented by [[handle]] internal slot of key as the password, P,
        // the contents of the salt attribute of normalizedAlgorithm as the salt, S,
        // the value of the iterations attribute of normalizedAlgorithm as the iteration count, c,
        // and length divided by 8 as the intended key length, dkLen.
        let password = key.byte_handle_bytes();
        let salt = &normalized_algorithm.salt;
        let iterations = normalized_algorithm.iterations;
        let derived_key_length_bytes = length_optional.expect("validation requires a length") / 8;

        let hash_kind =
            sha_hash_kind(hash_algorithm).map_err(|message| Exception::Dom("NotSupportedError", message))?;

        let mut result: Option<Bytes> = None;
        // SAFETY: The inputs and output are lent for this call.
        let success = unsafe {
            web_crypto_pbkdf2_derive_key(
                hash_kind,
                Span::new(password),
                Span::new(salt.as_slice()),
                iterations,
                derived_key_length_bytes,
                core::ptr::from_mut(&mut result).cast(),
                receive_bytes,
            )
        };

        // 5. If the key derivation operation fails, then throw an OperationError.
        if !success {
            return Err(dom_error("OperationError", b"Failed to derive key"));
        }

        // 6. Return result
        Ok(result.expect("LibCrypto returns the derived bytes"))
    }
}

impl AlgorithmMethods for Pbkdf2 {
    // https://w3c.github.io/webcrypto/#pbkdf2-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. Return null.
        Ok(None)
    }

    // https://w3c.github.io/webcrypto/#pbkdf2-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. If format is not "raw", throw a NotSupportedError
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing symmetric algorithms in WebCrypto, "raw-secret" acts as an alias of "raw".
        if format != KeyFormat::Raw && format != KeyFormat::RawSecret {
            return Err(dom_error("NotSupportedError", b"Only raw format is supported"));
        }

        // 2. If usages contains a value that is not "deriveKey" or "deriveBits", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 3. If extractable is not false, then throw a SyntaxError.
        if extractable {
            return Err(dom_error("SyntaxError", b"extractable must be false"));
        }

        // 4. Let key be a new CryptoKey representing keyData.
        let ImportKeyData::Bytes(data) = key_data else {
            unreachable!("raw key data contains bytes")
        };
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 5. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new KeyAlgorithm object.
        // 7. Set the name attribute of algorithm to "PBKDF2".
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"PBKDF2"),
        });

        // 9. Return key.
        Ok(key)
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Argon2;

impl AlgorithmMethods for Argon2 {
    // https://wicg.github.io/webcrypto-modern-algos/#argon2-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. Let keyData be the key data to be imported.
        // 2. If format is not "raw-secret", throw a NotSupportedError
        if format != KeyFormat::RawSecret {
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 3. If usages contains a value that is not "deriveKey" or "deriveBits", then throw a SyntaxError.
        for &usage in usages {
            if !(usage == KeyUsage::DeriveKey || usage == KeyUsage::DeriveBits) {
                return Err(dom_error("SyntaxError", b"Invalid key usage"));
            }
        }

        // 4. If extractable is not false, then throw a SyntaxError.
        if extractable {
            return Err(dom_error("SyntaxError", b"Key cannot be extractable"));
        }

        // 5. Let key be a new CryptoKey representing keyData.
        let ImportKeyData::Bytes(data) = key_data else {
            unreachable!("raw key data contains bytes")
        };
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 6. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 7. Let algorithm be a new KeyAlgorithm object.
        // 8. Set the name attribute of algorithm to the name member of normalizedAlgorithm.
        // 9. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Key {
            name: algorithm.name.clone(),
        });

        // 10. Return key.
        Ok(key)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#argon2-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. Return null.
        Ok(None)
    }
}

impl AlgorithmMethods for Argon2Params {
    // https://wicg.github.io/webcrypto-modern-algos/#argon2-operations-derive-bits
    fn validate_derive_bits_parameters(&self, _: Gc<Realm>, length: Option<u32>) -> WebIdlResult<()> {
        let normalized_algorithm = self;

        // 1. If length is null, or is less than 32 (4*8), or is not a multiple of 8, then throw an OperationError.
        if length.is_none_or(|length| length < 32 || !length.is_multiple_of(8)) {
            return Err(dom_error("OperationError", b"Length must be equal or greater than 32"));
        }

        // 2. If the version member of normalizedAlgorithm is present and is not 19 (0x13), then throw an OperationError.
        if normalized_algorithm.version.is_some_and(|version| version != 19) {
            return Err(dom_error("OperationError", b"Invalid algorithm version"));
        }

        // 3. If the parallelism member of normalizedAlgorithm is zero, or greater than 16777215 (2^24-1), then throw an OperationError.
        if normalized_algorithm.parallelism == 0 || normalized_algorithm.parallelism > (1 << 24) - 1 {
            return Err(dom_error("OperationError", b"Invalid parallelism"));
        }

        // 4. If the memory member of normalizedAlgorithm is less than 8 times the parallelism member of normalizedAlgorithm, then throw an OperationError.
        if normalized_algorithm.memory < 8 * normalized_algorithm.parallelism {
            return Err(dom_error(
                "OperationError",
                b"Memory is too low for the parallelism level",
            ));
        }

        // 5. If the passes member of normalizedAlgorithm is zero, then throw an OperationError.
        if normalized_algorithm.passes == 0 {
            return Err(dom_error("OperationError", b"Invalid passes"));
        }

        // NB: Our Argon2 rejects short nonces.
        // SAFETY: The nonce is lent to the validation service.
        let result = unsafe { web_crypto_argon2_validate_parameters(Span::new(normalized_algorithm.nonce.as_slice())) };
        if result != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(result) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Hashing function failed: {}", &[&detail]),
            ));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#argon2-operations-derive-bits
    fn derive_bits(
        &self,
        realm: Gc<Realm>,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;
        // NB: Steps 1 to 5 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(realm, length)?;

        // 6 => If the name member of normalizedAlgorithm is a case-sensitive string match for "Argon2d":
        //      Let type be 0.
        let kind = if matches!(algorithm.name.as_units(), Utf16StringUnits::Ascii(b"Argon2d")) {
            Argon2Type::Argon2d
        //   => If the name member of normalizedAlgorithm is a case-sensitive string match for "Argon2i":
        //      Let type be 1.
        } else if matches!(algorithm.name.as_units(), Utf16StringUnits::Ascii(b"Argon2i")) {
            Argon2Type::Argon2i
        //   => If the name member of normalizedAlgorithm is a case-sensitive string match for "Argon2id":
        //      Let type be 2.
        } else if matches!(algorithm.name.as_units(), Utf16StringUnits::Ascii(b"Argon2id")) {
            Argon2Type::Argon2id
        } else {
            unreachable!("the name identifies an Argon2 variant");
        };

        // 7. Let secretValue be the secretValue member of normalizedAlgorithm, if present.
        let secret_value = normalized_algorithm.secret_value.as_ref();

        // 8. Let associatedData be the associatedData member of normalizedAlgorithm, if present.
        let associated_data = normalized_algorithm.associated_data.as_ref();

        // 9. Let result be the result of performing the Argon2 function defined in Section 3 of [RFC9106] using the
        //    password represented by [[handle]] internal slot of key as the message, P, the nonce attribute of
        //    normalizedAlgorithm as the nonce, S, the value of the parallelism attribute of normalizedAlgorithm as the
        //    degree of parallelism, p, the value of the memory attribute of normalizedAlgorithm as the memory size, m, the
        //    value of the passes attribute of normalizedAlgorithm as the number of passes, t, 0x13 as the version number,
        //    v, secretValue (if present) as the secret value, K, associatedData (if present) as the associated data, X, type
        //    as the type, y, and length divided by 8 as the tag length, T.
        let inputs = Argon2Inputs {
            message: Span::new(key.byte_handle_bytes()),
            nonce: Span::new(normalized_algorithm.nonce.as_slice()),
            parallelism: normalized_algorithm.parallelism,
            memory: normalized_algorithm.memory,
            passes: normalized_algorithm.passes,
            version: 0x13,
            secret_value: secret_value.into(),
            associated_data: associated_data.into(),
            tag_length: length.expect("validation requires a length") / 8,
        };
        let mut result: Option<Bytes> = None;
        // SAFETY: The inputs and output are lent for this call.
        let maybe_result = unsafe {
            web_crypto_argon2_derive_key(kind, &inputs, core::ptr::from_mut(&mut result).cast(), receive_bytes)
        };

        // 10. If the key derivation operation fails, then throw an OperationError.
        if maybe_result != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(maybe_result) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Hashing function failed: {}", &[&detail]),
            ));
        }

        Ok(result.expect("LibCrypto returns the derived bytes"))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct AesCbcParams {
    iv: Bytes,
}

impl AesCbcParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let iv_value = object.get(vm, &PropertyKey::from_utf8("iv"))?;
        let Some(iv_source) = buffer_source_type_of(iv_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };
        let iv = get_a_copy_of_the_bytes(vm, iv_source)?;

        Ok(Self { iv })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct AesCtrParams {
    counter: Bytes,
    length: u8,
}

impl AesCtrParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let iv_value = object.get(vm, &PropertyKey::from_utf8("counter"))?;
        let Some(iv_source) = buffer_source_type_of(iv_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };
        let iv = get_a_copy_of_the_bytes(vm, iv_source)?;

        let length_value = object.get(vm, &PropertyKey::from_utf8("length"))?;
        let length = to_octet_enforcing_range(vm, length_value)?;

        Ok(Self { counter: iv, length })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct AesGcmParams {
    iv: Bytes,
    additional_data: Option<Bytes>,
    tag_length: Option<u8>,
}

impl AesGcmParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let iv_value = object.get(vm, &PropertyKey::from_utf8("iv"))?;
        let Some(iv_source) = buffer_source_type_of(iv_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };
        let iv = get_a_copy_of_the_bytes(vm, iv_source)?;

        let mut additional_data = None;
        let additional_data_property = PropertyKey::from_utf8("additionalData");
        if object.has_property(vm, &additional_data_property)? {
            let additional_data_value = object.get(vm, &additional_data_property)?;
            let Some(additional_data_source) = buffer_source_type_of(additional_data_value) else {
                return vm
                    .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                    .map_err(Into::into);
            };
            additional_data = Some(get_a_copy_of_the_bytes(vm, additional_data_source)?);
        }

        let mut tag_length = None;
        let tag_length_property = PropertyKey::from_utf8("tagLength");
        if object.has_property(vm, &tag_length_property)? {
            let tag_length_value = object.get(vm, &tag_length_property)?;
            tag_length = Some(to_octet_enforcing_range(vm, tag_length_value)?);
        }

        Ok(Self {
            iv,
            additional_data,
            tag_length,
        })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct AesKeyGenParams {
    length: u16,
}

impl AesKeyGenParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let length_value = object.get(vm, &PropertyKey::from_utf8("length"))?;
        let length = to_unsigned_short_enforcing_range(vm, length_value)?;

        Ok(Self { length })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct AesDerivedKeyParams {
    length: u16,
}

impl AesDerivedKeyParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let length_value = object.get(vm, &PropertyKey::from_utf8("length"))?;
        let length = to_unsigned_short_enforcing_range(vm, length_value)?;

        Ok(Self { length })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct AeadParams {
    iv: Bytes,
    additional_data: Option<Bytes>,
    tag_length: Option<u8>,
}

impl AeadParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let iv_value = object.get(vm, &PropertyKey::from_utf8("iv"))?;
        let Some(iv_source) = buffer_source_type_of(iv_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                .map_err(Into::into);
        };
        let iv = get_a_copy_of_the_bytes(vm, iv_source)?;

        let mut additional_data = None;
        let additional_data_property = PropertyKey::from_utf8("additionalData");
        if object.has_property(vm, &additional_data_property)? {
            let additional_data_value = object.get(vm, &additional_data_property)?;
            let Some(additional_data_source) = buffer_source_type_of(additional_data_value) else {
                return vm
                    .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                    .map_err(Into::into);
            };
            additional_data = Some(get_a_copy_of_the_bytes(vm, additional_data_source)?);
        }

        let mut tag_length = None;
        let tag_length_property = PropertyKey::from_utf8("tagLength");
        if object.has_property(vm, &tag_length_property)? {
            let tag_length_value = object.get(vm, &tag_length_property)?;
            tag_length = Some(to_octet_enforcing_range(vm, tag_length_value)?);
        }

        Ok(Self {
            iv,
            additional_data,
            tag_length,
        })
    }
}

#[derive(Clone, Copy)]
pub(crate) enum AesParameterType {
    Base,
    Cbc,
    Ctr,
    Gcm,
    KeyGen,
    DerivedKey,
    Aead,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
enum AesParameters {
    Base,
    Cbc(AesCbcParams),
    Ctr(AesCtrParams),
    Gcm(AesGcmParams),
    KeyGen(AesKeyGenParams),
    DerivedKey(AesDerivedKeyParams),
    Aead(AeadParams),
}

impl AesParameters {
    fn from_value(vm: &Vm, object: Gc<Object>, parameter_type: AesParameterType) -> WebIdlResult<Self> {
        Ok(match parameter_type {
            AesParameterType::Base => Self::Base,
            AesParameterType::Cbc => Self::Cbc(AesCbcParams::from_value(vm, object)?),
            AesParameterType::Ctr => Self::Ctr(AesCtrParams::from_value(vm, object)?),
            AesParameterType::Gcm => Self::Gcm(AesGcmParams::from_value(vm, object)?),
            AesParameterType::KeyGen => Self::KeyGen(AesKeyGenParams::from_value(vm, object)?),
            AesParameterType::DerivedKey => Self::DerivedKey(AesDerivedKeyParams::from_value(vm, object)?),
            AesParameterType::Aead => Self::Aead(AeadParams::from_value(vm, object)?),
        })
    }
}

unsafe extern "C" {
    fn web_crypto_aes_cbc_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        iv: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_aes_ctr_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        counter: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_aes_kw_cipher(
        wrap: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_aes_gcm_encrypt(
        key: Span<'_, u8>,
        plaintext: Span<'_, u8>,
        iv: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        tag_length: usize,
        ciphertext: *mut c_void,
        tag: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_aes_gcm_decrypt(
        key: Span<'_, u8>,
        ciphertext: Span<'_, u8>,
        iv: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        tag: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_aes_ocb_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        iv: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        tag_length: usize,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
}
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AesCbc {
    parameters: AesParameters,
}

impl AesCbc {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, parameter_type: AesParameterType) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: AesParameters::from_value(vm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for AesCbc {
    // https://w3c.github.io/webcrypto/#aes-cbc-operations-encrypt
    fn validate_encrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Cbc(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Cbc parameters")
        };

        // 1. If the iv member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
        if normalized_algorithm.iv.as_slice().len() != 16 {
            return Err(dom_error("OperationError", b"IV to AES-CBC must be exactly 16 bytes"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-encrypt
    fn encrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, plaintext: &Bytes) -> WebIdlResult<Bytes> {
        let AesParameters::Cbc(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Cbc parameters")
        };

        // NB: Step 1 is performed by validate_encrypt_parameters().
        self.validate_encrypt_parameters(cx.realm())?;

        // 2. Let paddedPlaintext be the result of adding padding octets to the contents of plaintext according to the procedure defined in Section 10.3 of [RFC2315], step 2, with a value of k of 16.
        // 3. Let ciphertext be the result of performing the CBC Encryption operation described in Section 6.2 of [NIST-SP800-38A] using AES as the block cipher, the contents of the iv member of normalizedAlgorithm as the IV input parameter and paddedPlaintext as the input plaintext.
        let mut ciphertext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_ciphertext = unsafe {
            web_crypto_aes_cbc_cipher(
                true,
                Span::new(key.byte_handle_bytes()),
                Span::new(plaintext.as_slice()),
                Span::new(normalized_algorithm.iv.as_slice()),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        };
        if !maybe_ciphertext {
            return Err(dom_error("OperationError", b"Failed to encrypt"));
        }

        // 4. Return the result of creating an ArrayBuffer containing ciphertext.
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-decrypt
    fn validate_decrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Cbc(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Cbc parameters")
        };

        // 1. If the iv member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
        if normalized_algorithm.iv.as_slice().len() != 16 {
            return Err(dom_error("OperationError", b"IV to AES-CBC must be exactly 16 bytes"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-decrypt
    fn decrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, ciphertext: &Bytes) -> WebIdlResult<Bytes> {
        let AesParameters::Cbc(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Cbc parameters")
        };

        // NB: Step 1 is performed by validate_decrypt_parameters().
        self.validate_decrypt_parameters(cx.realm())?;

        // 2. If the length of ciphertext is zero or is not a multiple of 16 bytes, then throw an OperationError.
        if ciphertext.as_slice().is_empty() || !ciphertext.as_slice().len().is_multiple_of(16) {
            return Err(dom_error(
                "OperationError",
                b"Ciphertext length must be a multiple of 16 bytes",
            ));
        }

        // 3. Let paddedPlaintext be the result of performing the CBC Decryption operation described in Section 6.2 of [NIST-SP800-38A] using AES as the block cipher, the iv member of normalizedAlgorithm as the IV input parameter and ciphertext as the input ciphertext.
        // 4. Let p be the value of the last octet of paddedPlaintext.
        // 5. If p is zero or greater than 16, or if any of the last p octets of paddedPlaintext have a value which is not p, then throw an OperationError.
        // 6. Let plaintext be the result of removing p octets from the end of paddedPlaintext.
        let mut plaintext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_plaintext = unsafe {
            web_crypto_aes_cbc_cipher(
                false,
                Span::new(key.byte_handle_bytes()),
                Span::new(ciphertext.as_slice()),
                Span::new(normalized_algorithm.iv.as_slice()),
                core::ptr::from_mut(&mut plaintext).cast(),
                receive_bytes,
            )
        };
        if !maybe_plaintext {
            return Err(dom_error("OperationError", b"Failed to decrypt"));
        }

        // 7. Return plaintext.
        Ok(plaintext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the spec checks the JWK algorithm after matching the key length"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2.
        let data;
        if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // -> If format is "raw":
            //    1. Let data be the octet string contained in keyData.
            //    2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;
            let length_in_bits = data.as_slice().len() * 8;
            if length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256 {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted(
                        "Invalid key length '{}' bits (must be either 128, 192, or 256 bits)",
                        &[&length_in_bits],
                    ),
                ));
            }
        } else if format == KeyFormat::Jwk {
            // -> If format is "jwk":
            //    1. ->   If keyData is a JsonWebKey dictionary:
            //                Let jwk equal keyData.
            //       ->   Otherwise:
            //                Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            //    2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            //    3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
            // Specifically, those requirements are:
            // - ".k" is a valid bas64url encoded octet stream, which we do by just parsing it, in step 4.
            // - ".alg" is checked only in step 5.

            //    4. Let data be the octet string obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            //    5. -> If data has length 128 bits:
            //              If the alg field of jwk is present, and is not "A128CBC", then throw a DataError.
            //       -> If data has length 192 bits:
            //              If the alg field of jwk is present, and is not "A192CBC", then throw a DataError.
            //       -> If data has length 256 bits:
            //              If the alg field of jwk is present, and is not "A256CBC", then throw a DataError.
            //       -> Otherwise:
            //              throw a DataError.
            let data_bits = data.as_slice().len() * 8;
            let alg = &jwk.alg;
            if data_bits == 128 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A128CBC"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm",
                    ));
                }
            } else if data_bits == 192 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A192CBC"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm",
                    ));
                }
            } else if data_bits == 256 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A256CBC"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm",
                    ));
                }
            } else {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted("Invalid key size: {} bits", &[&data_bits]),
                ));
            }

            //    6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
            if !key_usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            //    7. If the key_ops field of jwk is present, and is invalid according to the
            //       requirements of JSON Web Key [JWK] or does not contain all of the specified usages
            //       values, then throw a DataError.
            validate_jwk_key_ops(&jwk, key_usages)?;

            //    8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        } else {
            //    Otherwise:
            //        throw a NotSupportedError
            return Err(dom_error(
                "NotSupportedError",
                b"Only raw and jwk formats are supported",
            ));
        }

        // 3. Let key be a new CryptoKey object representing an AES key with value data.
        let data_bits = data.as_slice().len() * 8;
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 4. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 5. Let algorithm be a new AesKeyAlgorithm.
        // 6. Set the name attribute of algorithm to "AES-CBC".
        // 7. Set the length attribute of algorithm to the length, in bits, of data.
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-CBC"),
            length: u16::try_from(data_bits).expect("AES key length was validated"),
        });

        // 9. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
        let AesParameters::KeyGen(length) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = length.length;
        if bits != 128 && bits != 192 && bits != 256 {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Cannot create AES-CBC key with unusual amount of {} bits", &[&bits]),
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let AesParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };

        // NB: Step 2 is performed by validate_generate_key_parameters().
        self.validate_generate_key_parameters(cx.realm())?;
        let bits = normalized_algorithm.length;

        // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
        let key_buffer = generate_random_key(cx.vm(), u32::from(bits))?;

        // 4. If the key generation step fails, then throw an OperationError.
        // Note: Cannot happen in our implementation; and if we OOM, then allocating the Exception is probably going to crash anyway.

        // 5. Let key be a new CryptoKey object representing the generated AES key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_buffer);

        // 9. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new AesKeyAlgorithm.
        // 7. Set the name attribute of algorithm to "AES-CBC".
        // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
        // 10. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-CBC"),
            length: bits,
        });

        // 11. Set the [[extractable]] internal slot of key to be extractable.
        key.set_extractable(extractable);

        // 12. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(key_usages));

        // 13. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 2. -> If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing symmetric algorithms in WebCrypto, "raw-secret" acts as an alias of "raw".
        let result = if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
            let data = key.byte_handle_bytes();

            // 2. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        //    -> If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key, encoded according to Section 6.4 of JSON Web Algorithms [JWA].
            let key_bytes = key.byte_handle_bytes();
            let Ok(encoded) = base64_url_bytes_encode(key_bytes) else {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            };
            jwk.k = Some(encoded);

            // 4. -> If the length attribute of key is 128:
            //        Set the alg attribute of jwk to the string "A128CBC".
            //    -> If the length attribute of key is 192:
            //        Set the alg attribute of jwk to the string "A192CBC".
            //    -> If the length attribute of key is 256:
            //        Set the alg attribute of jwk to the string "A256CBC".
            let key_bits = key_bytes.len() * 8;
            if key_bits == 128 {
                jwk.alg = Some(Utf16String::from_ascii(b"A128CBC"));
            } else if key_bits == 192 {
                jwk.alg = Some(Utf16String::from_ascii(b"A192CBC"));
            } else if key_bits == 256 {
                jwk.alg = Some(Utf16String::from_ascii(b"A256CBC"));
            } else {
                return Err(dom_error("OperationError", b"unclear key size"));
            }

            // 5. Set the key_ops attribute of jwk to equal the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        //    -> Otherwise:
        else {
            //        throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Cannot export to unsupported format"));
        };

        // 3. Return result.
        Ok(result)
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw an OperationError.
        let AesParameters::DerivedKey(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses DerivedKey parameters")
        };
        let length = normalized_algorithm.length;
        if length != 128 && length != 192 && length != 256 {
            return Err(dom_error("OperationError", b"Invalid key length"));
        }

        // 2. Return the length member of normalizedDerivedKeyAlgorithm.
        Ok(Some(u32::from(length)))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AesCtr {
    parameters: AesParameters,
}

impl AesCtr {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, parameter_type: AesParameterType) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: AesParameters::from_value(vm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for AesCtr {
    // https://w3c.github.io/webcrypto/#aes-ctr-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the spec checks the JWK algorithm after matching the key length"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let data;

        // 2. If format is "raw":
        if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // 1. Let data be the octet string contained in keyData.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;

            // 2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
            let length_in_bits = data.as_slice().len() * 8;
            if length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256 {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted(
                        "Invalid key length '{}' bits (must be either 128, 192, or 256 bits)",
                        &[&length_in_bits],
                    ),
                ));
            }
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. -> If keyData is a JsonWebKey dictionary:
            //         Let jwk equal keyData.
            //    -> Otherwise:
            //         Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
            // Specifically, those requirements are:
            // * the member "k" is used to represent a symmetric key (or another key whose value is a single octet sequence).
            // * An "alg" member SHOULD also be present to identify the algorithm intended to be used with the key,
            //   unless the application uses another means or convention to determine the algorithm used.
            if jwk.k.is_none() {
                return Err(dom_error("DataError", b"Missing 'k' field"));
            }

            if jwk.alg.is_none() {
                return Err(dom_error("DataError", b"Missing 'alg' field"));
            }

            // 4. Let data be the octet string obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            //    5. -> If data has length 128 bits:
            //              If the alg field of jwk is present, and is not "A128CTR", then throw a DataError.
            //       -> If data has length 192 bits:
            //              If the alg field of jwk is present, and is not "A192CTR", then throw a DataError.
            //       -> If data has length 256 bits:
            //              If the alg field of jwk is present, and is not "A256CTR", then throw a DataError.
            //       -> Otherwise:
            //              throw a DataError.
            let data_bits = data.as_slice().len() * 8;
            let alg = &jwk.alg;
            if data_bits == 128 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A128CTR"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm",
                    ));
                }
            } else if data_bits == 192 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A192CTR"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm",
                    ));
                }
            } else if data_bits == 256 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A256CTR"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm",
                    ));
                }
            } else {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted("Invalid key size: {} bits", &[&data_bits]),
                ));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
            if !key_usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, key_usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error(
                "NotSupportedError",
                b"Only raw and jwk formats are supported",
            ));
        }

        let data_bits = data.as_slice().len() * 8;

        // 3. Let key be a new CryptoKey object representing an AES key with value data.
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 4. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 5. Let algorithm be a new AesKeyAlgorithm.
        // 6. Set the name attribute of algorithm to "AES-CTR".
        // 7. Set the length attribute of algorithm to the length, in bits, of data.
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-CTR"),
            length: u16::try_from(data_bits).expect("AES key length was validated"),
        });

        // 9. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 2. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing symmetric algorithms in WebCrypto, "raw-secret" acts as an alias of "raw".
        let result = if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
            let data = key.byte_handle_bytes();

            // 2. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
            //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
            let key_bytes = key.byte_handle_bytes();
            let Ok(encoded) = base64_url_bytes_encode(key_bytes) else {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            };
            jwk.k = Some(encoded);

            // 4. -> If the length attribute of key is 128:
            //        Set the alg attribute of jwk to the string "A128CTR".
            //    -> If the length attribute of key is 192:
            //        Set the alg attribute of jwk to the string "A192CTR".
            //    -> If the length attribute of key is 256:
            //        Set the alg attribute of jwk to the string "A256CTR".
            let key_bits = key_bytes.len() * 8;
            if key_bits == 128 {
                jwk.alg = Some(Utf16String::from_ascii(b"A128CTR"));
            } else if key_bits == 192 {
                jwk.alg = Some(Utf16String::from_ascii(b"A192CTR"));
            } else if key_bits == 256 {
                jwk.alg = Some(Utf16String::from_ascii(b"A256CTR"));
            }

            // 5. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Cannot export to unsupported format"));
        };

        // 3. Return result.
        Ok(result)
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw a OperationError.
        let AesParameters::DerivedKey(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses DerivedKey parameters")
        };
        let length = normalized_algorithm.length;
        if length != 128 && length != 192 && length != 256 {
            return Err(dom_error("OperationError", b"Invalid key length"));
        }

        // 2. Return the length member of normalizedDerivedKeyAlgorithm.
        Ok(Some(u32::from(length)))
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
        let AesParameters::KeyGen(length) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = length.length;
        if bits != 128 && bits != 192 && bits != 256 {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Cannot create AES-CTR key with unusual amount of {} bits", &[&bits]),
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // NB: Step 2 is performed by validate_generate_key_parameters().
        self.validate_generate_key_parameters(cx.realm())?;
        let AesParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = normalized_algorithm.length;

        // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
        // 4. If the key generation step fails, then throw an OperationError.
        let key_buffer = generate_random_key(cx.vm(), u32::from(bits))?;

        // 5. Let key be a new CryptoKey object representing the generated AES key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_buffer);

        // 9. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new AesKeyAlgorithm.
        // 7. Set the name attribute of algorithm to "AES-CTR".
        // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
        // 10. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-CTR"),
            length: bits,
        });

        // 11. Set the [[extractable]] internal slot of key to be extractable.
        key.set_extractable(extractable);

        // 12. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(key_usages));

        // 13. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-encrypt
    fn validate_encrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Ctr(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Ctr parameters")
        };

        // 1. If the counter member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
        if normalized_algorithm.counter.as_slice().len() != 16 {
            return Err(dom_error("OperationError", b"Invalid counter length"));
        }

        // 2. If the length member of normalizedAlgorithm is zero or is greater than 128, then throw an OperationError.
        if normalized_algorithm.length == 0 || normalized_algorithm.length > 128 {
            return Err(dom_error("OperationError", b"Invalid length"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-encrypt
    fn encrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, plaintext: &Bytes) -> WebIdlResult<Bytes> {
        // NB: Steps 1 and 2 are performed by validate_encrypt_parameters().
        self.validate_encrypt_parameters(cx.realm())?;
        let AesParameters::Ctr(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Ctr parameters")
        };
        let counter = &normalized_algorithm.counter;

        // 3. Let ciphertext be the result of performing the CTR Encryption operation described in Section 6.5 of [NIST-SP800-38A] using
        //    AES as the block cipher,
        //    the contents of the counter member of normalizedAlgorithm as the initial value of the counter block,
        //    the length member of normalizedAlgorithm as the input parameter m to the standard counter block incrementing function defined in Appendix B.1 of [NIST-SP800-38A]
        //    and the contents of plaintext as the input plaintext.
        let mut ciphertext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_ciphertext = unsafe {
            web_crypto_aes_ctr_cipher(
                true,
                Span::new(key.byte_handle_bytes()),
                Span::new(plaintext.as_slice()),
                Span::new(counter.as_slice()),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        };
        if !maybe_ciphertext {
            return Err(dom_error("OperationError", b"Encryption failed"));
        }

        // 4. Return the result of creating an ArrayBuffer containing plaintext.
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-decrypt
    fn validate_decrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Ctr(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Ctr parameters")
        };

        // 1. If the counter member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
        if normalized_algorithm.counter.as_slice().len() != 16 {
            return Err(dom_error("OperationError", b"Invalid counter length"));
        }

        // 2. If the length member of normalizedAlgorithm is zero or is greater than 128, then throw an OperationError.
        if normalized_algorithm.length == 0 || normalized_algorithm.length > 128 {
            return Err(dom_error("OperationError", b"Invalid length"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-decrypt
    fn decrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, ciphertext: &Bytes) -> WebIdlResult<Bytes> {
        // NB: Steps 1 and 2 are performed by validate_decrypt_parameters().
        self.validate_decrypt_parameters(cx.realm())?;
        let AesParameters::Ctr(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Ctr parameters")
        };
        let counter = &normalized_algorithm.counter;

        // 3. Let plaintext be the result of performing the CTR Decryption operation described in Section 6.5 of [NIST-SP800-38A] using
        //    AES as the block cipher,
        //    the contents of the counter member of normalizedAlgorithm as the initial value of the counter block,
        //    the length member of normalizedAlgorithm as the input parameter m to the standard counter block incrementing function defined in Appendix B.1 of [NIST-SP800-38A]
        //    and the contents of ciphertext as the input ciphertext.
        let mut plaintext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_plaintext = unsafe {
            web_crypto_aes_ctr_cipher(
                false,
                Span::new(key.byte_handle_bytes()),
                Span::new(ciphertext.as_slice()),
                Span::new(counter.as_slice()),
                core::ptr::from_mut(&mut plaintext).cast(),
                receive_bytes,
            )
        };
        if !maybe_plaintext {
            return Err(dom_error("OperationError", b"Decryption failed"));
        }

        // 4. Return the result of creating an ArrayBuffer containing plaintext.
        Ok(plaintext.expect("LibCrypto returns the result"))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AesGcm {
    parameters: AesParameters,
}

impl AesGcm {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, parameter_type: AesParameterType) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: AesParameters::from_value(vm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for AesGcm {
    // https://w3c.github.io/webcrypto/#aes-gcm-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw a OperationError.
        let AesParameters::DerivedKey(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses DerivedKey parameters")
        };
        let length = normalized_algorithm.length;
        if length != 128 && length != 192 && length != 256 {
            return Err(dom_error("OperationError", b"Invalid key length"));
        }

        // 2. Return the length member of normalizedDerivedKeyAlgorithm.
        Ok(Some(u32::from(length)))
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the spec checks the JWK algorithm after matching the key length"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let data;

        // 2. If format is "raw":
        if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // 1. Let data be the octet string contained in keyData.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;

            // 2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
            let length_in_bits = data.as_slice().len() * 8;
            if length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256 {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted(
                        "Invalid key length '{}' bits (must be either 128, 192, or 256 bits)",
                        &[&length_in_bits],
                    ),
                ));
            }
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. -> If keyData is a JsonWebKey dictionary:
            //         Let jwk equal keyData.
            //    -> Otherwise:
            //         Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
            // Specifically, those requirements are:
            // * the member "k" is used to represent a symmetric key (or another key whose value is a single octet sequence).
            // * An "alg" member SHOULD also be present to identify the algorithm intended to be used with the key,
            //   unless the application uses another means or convention to determine the algorithm used.
            if jwk.k.is_none() {
                return Err(dom_error("DataError", b"Missing 'k' field"));
            }

            if jwk.alg.is_none() {
                return Err(dom_error("DataError", b"Missing 'alg' field"));
            }

            // 4. Let data be the octet string obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            //    5. -> If data has length 128 bits:
            //              If the alg field of jwk is present, and is not "A128GCM", then throw a DataError.
            //       -> If data has length 192 bits:
            //              If the alg field of jwk is present, and is not "A192GCM", then throw a DataError.
            //       -> If data has length 256 bits:
            //              If the alg field of jwk is present, and is not "A256GCM", then throw a DataError.
            //       -> Otherwise:
            //              throw a DataError.
            let data_bits = data.as_slice().len() * 8;
            let alg = &jwk.alg;
            if data_bits == 128 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A128GCM"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm",
                    ));
                }
            } else if data_bits == 192 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A192GCM"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm",
                    ));
                }
            } else if data_bits == 256 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A256GCM"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm",
                    ));
                }
            } else {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted("Invalid key size: {} bits", &[&data_bits]),
                ));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
            if !key_usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, key_usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error(
                "NotSupportedError",
                b"Only raw and jwk formats are supported",
            ));
        }

        let data_bits = data.as_slice().len() * 8;

        // 3. Let key be a new CryptoKey object representing an AES key with value data.
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 4. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 5. Let algorithm be a new AesKeyAlgorithm.
        // 6. Set the name attribute of algorithm to "AES-GCM".
        // 7. Set the length attribute of algorithm to the length, in bits, of data.
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-GCM"),
            length: u16::try_from(data_bits).expect("AES key length was validated"),
        });

        // 9. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 2. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing symmetric algorithms in WebCrypto, "raw-secret" acts as an alias of "raw".
        let result = if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
            let data = key.byte_handle_bytes();

            // 2. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
            //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
            let key_bytes = key.byte_handle_bytes();
            let Ok(encoded) = base64_url_bytes_encode(key_bytes) else {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            };
            jwk.k = Some(encoded);

            // 4. -> If the length attribute of key is 128:
            //        Set the alg attribute of jwk to the string "A128GCM".
            //    -> If the length attribute of key is 192:
            //        Set the alg attribute of jwk to the string "A192GCM".
            //    -> If the length attribute of key is 256:
            //        Set the alg attribute of jwk to the string "A256GCM".
            let key_bits = key_bytes.len() * 8;
            if key_bits == 128 {
                jwk.alg = Some(Utf16String::from_ascii(b"A128GCM"));
            } else if key_bits == 192 {
                jwk.alg = Some(Utf16String::from_ascii(b"A192GCM"));
            } else if key_bits == 256 {
                jwk.alg = Some(Utf16String::from_ascii(b"A256GCM"));
            }

            // 5. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Cannot export to unsupported format"));
        };

        // 3. Return result.
        Ok(result)
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-encrypt
    fn validate_encrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Gcm(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Gcm parameters")
        };

        // NB: Our AES-GCM rejects an empty IV.
        if normalized_algorithm.iv.as_slice().is_empty() {
            return Err(dom_error("OperationError", b"IV must not be empty"));
        }

        // 1. If the iv member of normalizedAlgorithm has a length greater than 2^64 - 1 bytes, then throw an OperationError.
        // NOTE: This is not possible

        // 2. If the additionalData member of normalizedAlgorithm is present and has a length greater than 2^64 - 1 bytes, then throw an OperationError.
        // NOTE: This is not possible

        // 3. If the tagLength member of normalizedAlgorithm is not present: Let tagLength be 128.
        //    If the tagLength member of normalizedAlgorithm is one of 32, 64, 96, 104, 112, 120 or 128: Let tagLength be equal to the tagLength member of normalizedAlgorithm
        //    Otherwise: throw an OperationError.
        let valid_tag_lengths = [32, 64, 96, 104, 112, 120, 128];
        if normalized_algorithm
            .tag_length
            .is_some_and(|length| !valid_tag_lengths.contains(&length))
        {
            return Err(dom_error("OperationError", b"Invalid tag length"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-encrypt
    fn encrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, plaintext: &Bytes) -> WebIdlResult<Bytes> {
        let AesParameters::Gcm(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Gcm parameters")
        };

        // NB: Steps 1 to 3 are performed by validate_encrypt_parameters(), which leaves tagLength as the tagLength member
        //     of normalizedAlgorithm if present, or 128 otherwise.
        self.validate_encrypt_parameters(cx.realm())?;
        let tag_length = usize::from(normalized_algorithm.tag_length.unwrap_or(128));

        // 4. If plaintext has a length greater than 2^39 - 256 bytes, then throw an OperationError.
        if (plaintext.as_slice().len() as u64) > (1u64 << 39) - 256 {
            return Err(dom_error("OperationError", b"Invalid plaintext length"));
        }

        // 5. Let additionalData be the contents of the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
        let additional_data = normalized_algorithm
            .additional_data
            .as_ref()
            .map_or(&[][..], Bytes::as_slice);

        // 6. Let C and T be the outputs that result from performing the Authenticated Encryption Function described in Section 7.1 of [NIST-SP800-38D] using
        //    AES as the block cipher,
        //    the contents of the iv member of normalizedAlgorithm as the IV input parameter,
        //    the contents of additionalData as the A input parameter,
        //    tagLength as the t pre-requisite
        //    and the contents of plaintext as the input plaintext.
        let mut ciphertext: Option<Bytes> = None;
        let mut tag: Option<Bytes> = None;
        // SAFETY: The inputs and output slots live for the call.
        let maybe_encrypted = unsafe {
            web_crypto_aes_gcm_encrypt(
                Span::new(key.byte_handle_bytes()),
                Span::new(plaintext.as_slice()),
                Span::new(normalized_algorithm.iv.as_slice()),
                Span::new(additional_data),
                tag_length / 8,
                core::ptr::from_mut(&mut ciphertext).cast(),
                core::ptr::from_mut(&mut tag).cast(),
                receive_bytes,
            )
        };
        if !maybe_encrypted {
            return Err(dom_error("OperationError", b"Encryption failed"));
        }

        let mut ciphertext = Rc::unwrap_or_clone(ciphertext.expect("LibCrypto returns ciphertext").0);
        let tag = tag.expect("LibCrypto returns an authentication tag");

        // 7. Let ciphertext be equal to C | T, where '|' denotes concatenation.
        if ciphertext.try_reserve_exact(tag.as_slice().len()).is_err() {
            return cx
                .vm()
                .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .map_err(Into::into);
        }
        ciphertext.extend_from_slice(tag.as_slice());

        // 8. Return the result of creating an ArrayBuffer containing ciphertext.
        Ok(Bytes::new(ciphertext))
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-decrypt
    fn validate_decrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Gcm(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Gcm parameters")
        };

        // NB: Our AES-GCM rejects an empty IV.
        if normalized_algorithm.iv.as_slice().is_empty() {
            return Err(dom_error("OperationError", b"IV must not be empty"));
        }

        // 1. If the tagLength member of normalizedAlgorithm is not present: Let tagLength be 128.
        //    If the tagLength member of normalizedAlgorithm is one of 32, 64, 96, 104, 112, 120 or 128: Let tagLength be equal to the tagLength member of normalizedAlgorithm
        //    Otherwise: throw an OperationError.
        let valid_tag_lengths = [32, 64, 96, 104, 112, 120, 128];
        if normalized_algorithm
            .tag_length
            .is_some_and(|length| !valid_tag_lengths.contains(&length))
        {
            return Err(dom_error("OperationError", b"Invalid tag length"));
        }

        // 2. If the iv member of normalizedAlgorithm has a length greater than 2^64 - 1 bytes, then throw an OperationError.
        // NOTE: This is not possible

        // 3. If the additionalData member of normalizedAlgorithm is present and has a length greater than 2^64 - 1 bytes, then throw an OperationError.
        // NOTE: This is not possible

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-decrypt
    fn decrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, ciphertext: &Bytes) -> WebIdlResult<Bytes> {
        let AesParameters::Gcm(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Gcm parameters")
        };

        // NB: Steps 1 to 3 are performed by validate_decrypt_parameters(), which leaves tagLength as the tagLength member
        //     of normalizedAlgorithm if present, or 128 otherwise.
        self.validate_decrypt_parameters(cx.realm())?;
        let tag_length = usize::from(normalized_algorithm.tag_length.unwrap_or(128));

        // 4. If ciphertext has a length in bits less than tagLength, then throw an OperationError.
        if ciphertext.as_slice().len() < tag_length / 8 {
            return Err(dom_error("OperationError", b"Invalid ciphertext length"));
        }

        // 5. Let tag be the last tagLength bits of ciphertext.
        let tag_bytes = tag_length / 8;
        let split = ciphertext.as_slice().len() - tag_bytes;
        let copy_bytes = |bytes: &[u8]| -> WebIdlResult<Vec<u8>> {
            let mut copy = Vec::new();
            if copy.try_reserve_exact(bytes.len()).is_err() {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            }
            copy.extend_from_slice(bytes);
            Ok(copy)
        };
        let tag = copy_bytes(&ciphertext.as_slice()[split..])?;
        // 6. Let actualCiphertext be the result of removing the last tagLength bits from ciphertext.
        let actual_ciphertext = copy_bytes(&ciphertext.as_slice()[..split])?;

        // 7. Let additionalData be the contents of the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
        let additional_data = normalized_algorithm
            .additional_data
            .as_ref()
            .map_or(&[][..], Bytes::as_slice);

        // 8. Perform the Authenticated Decryption Function described in Section 7.2 of [NIST-SP800-38D] using
        //    AES as the block cipher,
        //    the contents of the iv member of normalizedAlgorithm as the IV input parameter,
        //    the contents of additionalData as the A input parameter,
        //    tagLength as the t pre-requisite,
        //    the contents of actualCiphertext as the input ciphertext, C
        //    and the contents of tag as the authentication tag, T.
        // If the result of the algorithm is the indication of inauthenticity, "FAIL": throw an OperationError
        let mut plaintext: Option<Bytes> = None;
        // SAFETY: The inputs and output slot live for the call.
        let maybe_plaintext = unsafe {
            web_crypto_aes_gcm_decrypt(
                Span::new(key.byte_handle_bytes()),
                Span::new(&actual_ciphertext),
                Span::new(normalized_algorithm.iv.as_slice()),
                Span::new(additional_data),
                Span::new(&tag),
                core::ptr::from_mut(&mut plaintext).cast(),
                receive_bytes,
            )
        };

        if !maybe_plaintext {
            return Err(dom_error("OperationError", b"Decryption failed"));
        }

        // Otherwise: Let plaintext be the output P of the Authenticated Decryption Function.
        // 9. Return the result of creating an ArrayBuffer containing plaintext.
        Ok(plaintext.expect("LibCrypto returns plaintext"))
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
        let AesParameters::KeyGen(length) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = length.length;
        if bits != 128 && bits != 192 && bits != 256 {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Cannot create AES-GCM key with unusual amount of {} bits", &[&bits]),
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-gcm-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // NB: Step 2 is performed by validate_generate_key_parameters().
        self.validate_generate_key_parameters(cx.realm())?;
        let AesParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = normalized_algorithm.length;

        // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
        // 4. If the key generation step fails, then throw an OperationError.
        let key_buffer = generate_random_key(cx.vm(), u32::from(bits))?;

        // 5. Let key be a new CryptoKey object representing the generated AES key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_buffer);

        // 9. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new AesKeyAlgorithm.
        // 7. Set the name attribute of algorithm to "AES-GCM".
        // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
        // 10. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-GCM"),
            length: bits,
        });

        // 11. Set the [[extractable]] internal slot of key to be extractable.
        key.set_extractable(extractable);

        // 12. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(key_usages));

        // 13. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AesKw {
    parameters: AesParameters,
}

impl AesKw {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, parameter_type: AesParameterType) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: AesParameters::from_value(vm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for AesKw {
    // https://w3c.github.io/webcrypto/#aes-kw-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the spec checks the JWK algorithm after matching the key length"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. If usages contains an entry which is not one of "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::WrapKey && usage != KeyUsage::UnwrapKey {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let data;

        // 2. If format is "raw":
        if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // 1. Let data be the octet string contained in keyData.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;

            // 2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
            let length_in_bits = data.as_slice().len() * 8;
            if length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256 {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted(
                        "Invalid key length '{}' bits (must be either 128, 192, or 256 bits)",
                        &[&length_in_bits],
                    ),
                ));
            }
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. -> If keyData is a JsonWebKey dictionary:
            //         Let jwk equal keyData.
            //    -> Otherwise:
            //         Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
            // Specifically, those requirements are:
            // * the member "k" is used to represent a symmetric key (or another key whose value is a single octet sequence).
            // * An "alg" member SHOULD also be present to identify the algorithm intended to be used with the key,
            //   unless the application uses another means or convention to determine the algorithm used.
            // NOTE: "k" is already checked in step 4.
            if jwk.alg.is_none() {
                return Err(dom_error("DataError", b"Missing 'alg' field"));
            }

            // 4. Let data be the octet string obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            // 5. -> If data has length 128 bits:
            //           If the alg field of jwk is present, and is not "A128KW", then throw a DataError.
            //    -> If data has length 192 bits:
            //           If the alg field of jwk is present, and is not "A192KW", then throw a DataError.
            //    -> If data has length 256 bits:
            //           If the alg field of jwk is present, and is not "A256KW", then throw a DataError.
            //    -> Otherwise:
            //           throw a DataError.
            let data_bits = data.as_slice().len() * 8;
            let alg = &jwk.alg;
            if data_bits == 128 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A128KW"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm",
                    ));
                }
            } else if data_bits == 192 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A192KW"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm",
                    ));
                }
            } else if data_bits == 256 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A256KW"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm",
                    ));
                }
            } else {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted("Invalid key size: {} bits", &[&data_bits]),
                ));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
            if !key_usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, key_usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error(
                "NotSupportedError",
                b"Only raw and jwk formats are supported",
            ));
        }

        let data_bits = data.as_slice().len() * 8;

        // 3. Let key be a new CryptoKey object representing an AES key with value data.
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 4. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 5. Let algorithm be a new AesKeyAlgorithm.
        // 6. Set the name attribute of algorithm to "AES-KW".
        // 7. Set the length attribute of algorithm to the length, in bits, of data.
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-KW"),
            length: u16::try_from(data_bits).expect("AES key length was validated"),
        });

        // 9. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#aes-kw-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 2. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing symmetric algorithms in WebCrypto, "raw-secret" acts as an alias of "raw".
        let result = if format == KeyFormat::Raw || format == KeyFormat::RawSecret {
            // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
            let data = key.byte_handle_bytes();

            // 2. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
            //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
            let key_bytes = key.byte_handle_bytes();
            let Ok(encoded) = base64_url_bytes_encode(key_bytes) else {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            };
            jwk.k = Some(encoded);

            // 4. -> If the length attribute of key is 128:
            //        Set the alg attribute of jwk to the string "A128KW".
            //    -> If the length attribute of key is 192:
            //        Set the alg attribute of jwk to the string "A192KW".
            //    -> If the length attribute of key is 256:
            //        Set the alg attribute of jwk to the string "A256KW".
            let key_bits = key_bytes.len() * 8;
            if key_bits == 128 {
                jwk.alg = Some(Utf16String::from_ascii(b"A128KW"));
            } else if key_bits == 192 {
                jwk.alg = Some(Utf16String::from_ascii(b"A192KW"));
            } else if key_bits == 256 {
                jwk.alg = Some(Utf16String::from_ascii(b"A256KW"));
            }

            // 5. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Cannot export to unsupported format"));
        };

        // 3. Return result.
        Ok(result)
    }

    // https://w3c.github.io/webcrypto/#aes-kw-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw an OperationError.
        let AesParameters::DerivedKey(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses DerivedKey parameters")
        };
        let length = normalized_algorithm.length;
        if length != 128 && length != 192 && length != 256 {
            return Err(dom_error("OperationError", b"Invalid key length"));
        }

        // 2. Return the length member of normalizedDerivedKeyAlgorithm.
        Ok(Some(u32::from(length)))
    }

    // https://w3c.github.io/webcrypto/#aes-kw-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. If the length property of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
        let AesParameters::KeyGen(length) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = length.length;
        if bits != 128 && bits != 192 && bits != 256 {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Cannot create AES-KW key with unusual amount of {} bits", &[&bits]),
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#aes-kw-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::WrapKey && usage != KeyUsage::UnwrapKey {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // NB: Step 2 is performed by validate_generate_key_parameters().
        self.validate_generate_key_parameters(cx.realm())?;
        let AesParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = normalized_algorithm.length;

        // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
        // 4. If the key generation step fails, then throw an OperationError.
        let key_buffer = generate_random_key(cx.vm(), u32::from(bits))?;

        // 5. Let key be a new CryptoKey object representing the generated AES key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_buffer);

        // 9. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new AesKeyAlgorithm.
        // 7. Set the name attribute of algorithm to "AES-KW".
        // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
        // 10. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-KW"),
            length: bits,
        });

        // 11. Set the [[extractable]] internal slot of key to be extractable.
        key.set_extractable(extractable);

        // 12. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(key_usages));

        // 13. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }

    // https://w3c.github.io/webcrypto/#aes-kw-operations-wrap-key
    fn wrap_key(&self, _: Gc<Realm>, key: Ref<CryptoKey>, plaintext: &Bytes) -> WebIdlResult<Bytes> {
        // 1. If plaintext is not a multiple of 64 bits in length, then throw an OperationError.
        if !plaintext.as_slice().len().is_multiple_of(8) {
            return Err(dom_error("OperationError", b"Invalid plaintext length"));
        }

        // 2. Let ciphertext be the result of performing the Key Wrap operation described in Section 2.2.1 of [RFC3394]
        //    with plaintext as the plaintext to be wrapped and using the default Initial Value defined in Section 2.2.3.1 of the same document.
        let mut ciphertext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_ciphertext = unsafe {
            web_crypto_aes_kw_cipher(
                true,
                Span::new(key.byte_handle_bytes()),
                Span::new(plaintext.as_slice()),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        };
        if !maybe_ciphertext {
            return Err(dom_error("OperationError", b"Key wrap failed"));
        }

        // 3. Return ciphertext.
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#aes-kw-operations-unwrap-key
    fn unwrap_key(&self, _: Gc<Realm>, key: Ref<CryptoKey>, ciphertext: &Bytes) -> WebIdlResult<Bytes> {
        // NOTE: The spec does not mention this, but we need to check
        if ciphertext.as_slice().len() < 8 {
            return Err(dom_error("OperationError", b"Invalid ciphertext length"));
        }

        // 1. Let plaintext be the result of performing the Key Unwrap operation described in Section 2.2.2 of [RFC3394]
        //     with ciphertext as the input ciphertext and using the default Initial Value defined in Section 2.2.3.1 of the same document
        // 2. If the Key Unwrap operation returns an error, then throw an OperationError.
        let mut plaintext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_plaintext = unsafe {
            web_crypto_aes_kw_cipher(
                false,
                Span::new(key.byte_handle_bytes()),
                Span::new(ciphertext.as_slice()),
                core::ptr::from_mut(&mut plaintext).cast(),
                receive_bytes,
            )
        };
        if !maybe_plaintext {
            return Err(dom_error("OperationError", b"Key unwrap failed"));
        }

        // 3. Return plaintext.
        Ok(plaintext.expect("LibCrypto returns the result"))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AesOcb {
    parameters: AesParameters,
}

impl AesOcb {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, parameter_type: AesParameterType) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: AesParameters::from_value(vm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for AesOcb {
    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-encrypt
    fn validate_encrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Aead(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Aead parameters")
        };

        // 1. If the iv member of normalizedAlgorithm has a length greater than 15 bytes, then throw an OperationError.
        if normalized_algorithm.iv.as_slice().len() > 15 {
            return Err(dom_error("OperationError", b"IV must have a length of 15 bytes"));
        }

        // NB: Our AES-OCB rejects an empty IV.
        if normalized_algorithm.iv.as_slice().is_empty() {
            return Err(dom_error("OperationError", b"IV must not be empty"));
        }

        // 2. If the tagLength member of normalizedAlgorithm is not present: Let tagLength be 128.
        //    If the tagLength member of normalizedAlgorithm is one of 64, 96 or 128: Let tagLength be equal to the tagLength member of normalizedAlgorithm
        //    Otherwise: throw an OperationError.
        let valid_tag_lengths = [64, 96, 128];
        if normalized_algorithm
            .tag_length
            .is_some_and(|length| !valid_tag_lengths.contains(&length))
        {
            return Err(dom_error("OperationError", b"Invalid tag length"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-encrypt
    fn encrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, plaintext: &Bytes) -> WebIdlResult<Bytes> {
        let AesParameters::Aead(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Aead parameters")
        };

        // NB: Steps 1 and 2 are performed by validate_encrypt_parameters(), which leaves tagLength as the tagLength member
        //     of normalizedAlgorithm if present, or 128 otherwise.
        self.validate_encrypt_parameters(cx.realm())?;
        let tag_length = usize::from(normalized_algorithm.tag_length.unwrap_or(128));

        // 3. Let additionalData be the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
        let additional_data = normalized_algorithm
            .additional_data
            .as_ref()
            .map_or(&[][..], Bytes::as_slice);

        // 4. Let C be the output that results from performing the OCB-ENCRYPT function described in Section 4.2
        //    of [RFC7253] using AES as the block cipher,
        //    using the key represented by [[handle]] internal slot of key as the K input parameter,
        //    the iv member of normalizedAlgorithm as the N input parameter,
        //    additionalData as the A input parameter,
        //    plaintext as the P input parameter,
        //    and tagLength as the TAGLEN global parameter.
        let mut ciphertext: Option<Bytes> = None;
        // SAFETY: The inputs and output slot live for the call.
        let maybe_ciphertext = unsafe {
            web_crypto_aes_ocb_cipher(
                true,
                Span::new(key.byte_handle_bytes()),
                Span::new(plaintext.as_slice()),
                Span::new(normalized_algorithm.iv.as_slice()),
                Span::new(additional_data),
                tag_length,
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        };

        if maybe_ciphertext != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(maybe_ciphertext) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Encryption failed: {}", &[&detail]),
            ));
        }

        // 5. Return C
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-decrypt
    fn validate_decrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Aead(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Aead parameters")
        };

        // 1. If the iv member of normalizedAlgorithm has a length greater than 15 bytes, then throw an OperationError.
        if normalized_algorithm.iv.as_slice().len() > 15 {
            return Err(dom_error("OperationError", b"IV must have a length of 15 bytes"));
        }

        // NB: Our AES-OCB rejects an empty IV.
        if normalized_algorithm.iv.as_slice().is_empty() {
            return Err(dom_error("OperationError", b"IV must not be empty"));
        }

        // 2. If the tagLength member of normalizedAlgorithm is not present: Let tagLength be 128.
        //    If the tagLength member of normalizedAlgorithm is one of 64, 96 or 128: Let tagLength be equal to the tagLength member of normalizedAlgorithm
        //    Otherwise: throw an OperationError.
        let valid_tag_lengths = [64, 96, 128];
        if normalized_algorithm
            .tag_length
            .is_some_and(|length| !valid_tag_lengths.contains(&length))
        {
            return Err(dom_error("OperationError", b"Invalid tag length"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-decrypt
    fn decrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, ciphertext: &Bytes) -> WebIdlResult<Bytes> {
        let AesParameters::Aead(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Aead parameters")
        };

        // NB: Steps 1 and 2 are performed by validate_decrypt_parameters(), which leaves tagLength as the tagLength member
        //     of normalizedAlgorithm if present, or 128 otherwise.
        self.validate_decrypt_parameters(cx.realm())?;
        let tag_length = usize::from(normalized_algorithm.tag_length.unwrap_or(128));

        // 3. If ciphertext has a length less than tagLength bits, then throw an OperationError.
        if ciphertext.as_slice().len() < tag_length / 8 {
            return Err(dom_error("OperationError", b"Ciphertext is less than tagLength bits"));
        }

        // 4. Let additionalData be the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
        let additional_data = normalized_algorithm
            .additional_data
            .as_ref()
            .map_or(&[][..], Bytes::as_slice);

        // 5. Perform the OCB-DECRYPT function described in Section 4.3 of [RFC7253]
        //    using AES as the block cipher,
        //    using the key represented by [[handle]] internal slot of key as the K input parameter,
        //    the iv member of normalizedAlgorithm as the N input parameter,
        //    additionalData as the A input parameter,
        //    ciphertext as the C input parameter,
        //    and tagLength as the TAGLEN global parameter.
        let mut plaintext: Option<Bytes> = None;
        // SAFETY: The inputs and output slot live for the call.
        let maybe_plaintext = unsafe {
            web_crypto_aes_ocb_cipher(
                false,
                Span::new(key.byte_handle_bytes()),
                Span::new(ciphertext.as_slice()),
                Span::new(normalized_algorithm.iv.as_slice()),
                Span::new(additional_data),
                tag_length,
                core::ptr::from_mut(&mut plaintext).cast(),
                receive_bytes,
            )
        };

        // 5. If the result of the algorithm is the indication of authentication failure:
        //       throw an OperationError
        //    Otherwise:
        //       Let plaintext be the resulting plaintext.
        if maybe_plaintext != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(maybe_plaintext) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Decryption failed: {}", &[&detail]),
            ));
        }

        // 6. Return plaintext.
        Ok(plaintext.expect("LibCrypto returns the result"))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
        let AesParameters::KeyGen(length) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = length.length;
        if bits != 128 && bits != 192 && bits != 256 {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Cannot create AES-OCB key with unusual amount of {} bits", &[&bits]),
            ));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // NB: Step 2 is performed by validate_generate_key_parameters().
        self.validate_generate_key_parameters(cx.realm())?;
        let AesParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses KeyGen parameters")
        };
        let bits = normalized_algorithm.length;

        // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
        // 4. If the key generation step fails, then throw an OperationError.
        let key_buffer = generate_random_key(cx.vm(), u32::from(bits))?;

        // 5. Let key be a new CryptoKey object representing the generated AES key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_buffer);

        // 6. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 7. Let algorithm be a new AesKeyAlgorithm.
        // 8. Set the name attribute of algorithm to "AES-OCB".
        // 9. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
        // 10. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-OCB"),
            length: bits,
        });

        // 11. Set the [[extractable]] internal slot of key to be extractable.
        key.set_extractable(extractable);

        // 12. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(key_usages));

        // 13. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the spec checks the JWK algorithm after matching the key length"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. Let keyData be the key data to be imported.

        // 2. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let data;

        // 3. If format is "raw-secret":
        if format == KeyFormat::RawSecret {
            // 1. Let data be keyData.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;

            // 2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
            let length_in_bits = data.as_slice().len() * 8;
            if length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256 {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted(
                        "Invalid key length '{}' bits (must be either 128, 192, or 256 bits)",
                        &[&length_in_bits],
                    ),
                ));
            }
        }
        // 3. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary:
            //        Let jwk equal keyData.
            //    Otherwise:
            //        Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"Invalid JWK key data"));
            };

            // 2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
            // NOTE: "k" is already checked in step 4.
            if jwk.alg.is_none() {
                return Err(dom_error("DataError", b"Missing 'alg' field"));
            }

            // 4. Let data be the byte sequence obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            // 5. -> If data has length 128 bits:
            //           If the alg field of jwk is present, and is not "A128OCB", then throw a DataError.
            //    -> If data has length 192 bits:
            //           If the alg field of jwk is present, and is not "A192OCB", then throw a DataError.
            //    -> If data has length 256 bits:
            //           If the alg field of jwk is present, and is not "A256OCB", then throw a DataError.
            //    -> Otherwise:
            //           throw a DataError.
            let data_bits = data.as_slice().len() * 8;
            let alg = &jwk.alg;
            if data_bits == 128 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A128OCB"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm",
                    ));
                }
            } else if data_bits == 192 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A192OCB"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm",
                    ));
                }
            } else if data_bits == 256 {
                if alg
                    .as_ref()
                    .is_some_and(|alg| *alg != Utf16String::from_ascii(b"A256OCB"))
                {
                    return Err(dom_error(
                        "DataError",
                        b"Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm",
                    ));
                }
            } else {
                return Err(Exception::Dom(
                    "DataError",
                    libjs_rust::api::utf16_formatted("Invalid key size: {} bits", &[&data_bits]),
                ));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error(
                "NotSupportedError",
                b"Only raw-secret and jwk formats are supported",
            ));
        }

        let data_bits = data.as_slice().len() * 8;

        // 4. Let key be a new CryptoKey object representing an AES key with value data.
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 5. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new AesKeyAlgorithm.
        // 7. Set the name attribute of algorithm to "AES-OCB".
        // 8. Set the length attribute of algorithm to the length, in bits, of data.
        // 9. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Aes {
            name: Utf16String::from_ascii(b"AES-OCB"),
            length: u16::try_from(data_bits).expect("AES key length was validated"),
        });

        // 10. Return key.
        Ok(key)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 2. If format is "raw-secret":
        let result = if format == KeyFormat::RawSecret {
            // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
            let data = key.byte_handle_bytes();

            // 2. Let result be data.
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
            //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
            let key_bytes = key.byte_handle_bytes();
            let Ok(encoded) = base64_url_bytes_encode(key_bytes) else {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            };
            jwk.k = Some(encoded);

            // 4. -> If the length attribute of key is 128:
            //        Set the alg attribute of jwk to the string "A128OCB".
            //    -> If the length attribute of key is 192:
            //        Set the alg attribute of jwk to the string "A192OCB".
            //    -> If the length attribute of key is 256:
            //        Set the alg attribute of jwk to the string "A256OCB".
            let key_bits = key_bytes.len() * 8;
            if key_bits == 128 {
                jwk.alg = Some(Utf16String::from_ascii(b"A128OCB"));
            } else if key_bits == 192 {
                jwk.alg = Some(Utf16String::from_ascii(b"A192OCB"));
            } else if key_bits == 256 {
                jwk.alg = Some(Utf16String::from_ascii(b"A256OCB"));
            }

            // 5. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 7. Let result be jwk.
            return Ok(ExportKeyResult::JsonWebKey(jwk));
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Cannot export to unsupported format"));
        };

        // 3. Return result.
        Ok(result)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        let AesParameters::DerivedKey(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses DerivedKey parameters")
        };

        // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw an OperationError.
        let length = normalized_algorithm.length;
        if length != 128 && length != 192 && length != 256 {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Invalid key length: {}", &[&length]),
            ));
        }

        // 2. Return the length member of normalizedDerivedKeyAlgorithm.
        Ok(Some(u32::from(length)))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ChaCha20Poly1305 {
    parameters: Option<AeadParams>,
}

impl ChaCha20Poly1305 {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, has_aead_parameters: bool) -> WebIdlResult<Self> {
        let parameters = if has_aead_parameters {
            Some(AeadParams::from_value(vm, object)?)
        } else {
            None
        };
        Ok(Self { parameters })
    }
}

unsafe extern "C" {
    fn web_crypto_chacha20_poly1305_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        iv: Span<'_, u8>,
        input: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
}

impl AlgorithmMethods for ChaCha20Poly1305 {
    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-encrypt
    fn validate_encrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses AEAD parameters");

        // 1. If the iv member of normalizedAlgorithm does not have a length of 12 bytes, then throw an OperationError.
        if normalized_algorithm.iv.as_slice().len() != 12 {
            return Err(dom_error("OperationError", b"IV must have a length of 12 bytes"));
        }

        // 2. If the tagLength member of normalizedAlgorithm is present and is not 128, then throw an OperationError.
        if normalized_algorithm.tag_length.is_some_and(|length| length != 128) {
            return Err(dom_error("OperationError", b"tagLength must be 128"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-encrypt
    fn encrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, plaintext: &Bytes) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses AEAD parameters");

        // NB: Steps 1 and 2 are performed by validate_encrypt_parameters().
        self.validate_encrypt_parameters(cx.realm())?;

        // 3. Let additionalData be the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
        let additional_data = normalized_algorithm
            .additional_data
            .as_ref()
            .map_or(&[][..], Bytes::as_slice);

        // 4. Let ciphertext be the output that results from performing the AEAD_CHACHA20_POLY1305 encryption algorithm
        //    described in Section 2.8 of [RFC8439], using the key represented by [[handle]] internal slot of key as the
        //    key input parameter, the iv member of normalizedAlgorithm as the nonce input parameter, plaintext as the
        //    plaintext input parameter, and additionalData as the additional authenticated data (AAD) input parameter.
        let mut ciphertext: Option<Bytes> = None;
        // SAFETY: The inputs and output slot live for the call.
        let maybe_ciphertext = unsafe {
            web_crypto_chacha20_poly1305_cipher(
                true,
                Span::new(key.byte_handle_bytes()),
                Span::new(normalized_algorithm.iv.as_slice()),
                Span::new(plaintext.as_slice()),
                Span::new(additional_data),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        };

        if maybe_ciphertext != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(maybe_ciphertext) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Encryption failed: {}", &[&detail]),
            ));
        }

        // 5. Return ciphertext.
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-decrypt
    fn validate_decrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses AEAD parameters");

        // 1. If the iv member of normalizedAlgorithm does not have a length of 12 bytes, then throw an OperationError.
        if normalized_algorithm.iv.as_slice().len() != 12 {
            return Err(dom_error("OperationError", b"IV must have a length of 12 bytes"));
        }

        // 2. If the tagLength member of normalizedAlgorithm is present and is not 128, then throw an OperationError.
        if normalized_algorithm.tag_length.is_some_and(|length| length != 128) {
            return Err(dom_error("OperationError", b"tagLength must be 128"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-decrypt
    fn decrypt(&self, cx: &CallContext, key: Ref<CryptoKey>, ciphertext: &Bytes) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses AEAD parameters");

        // NB: Steps 1 and 2 are performed by validate_decrypt_parameters().
        self.validate_decrypt_parameters(cx.realm())?;

        // 3. If ciphertext has a length less than 128 bits, then throw an OperationError.
        if ciphertext.as_slice().len() < 16 {
            return Err(dom_error("OperationError", b"Ciphertext must be at least 128 bits"));
        }

        // 4. Let additionalData be the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
        let additional_data = normalized_algorithm
            .additional_data
            .as_ref()
            .map_or(&[][..], Bytes::as_slice);

        // 5. Perform the AEAD_CHACHA20_POLY1305 decryption algorithm described in Section 2.8 of [RFC8439], using the key
        //    represented by [[handle]] internal slot of key as the key input parameter, the iv member of normalizedAlgorithm
        //    as the nonce input parameter, ciphertext as the ciphertext input parameter, and additionalData as the additional
        //    authenticated data (AAD) input parameter.
        let mut plaintext: Option<Bytes> = None;
        // SAFETY: The inputs and output slot live for the call.
        let maybe_plaintext = unsafe {
            web_crypto_chacha20_poly1305_cipher(
                false,
                Span::new(key.byte_handle_bytes()),
                Span::new(normalized_algorithm.iv.as_slice()),
                Span::new(ciphertext.as_slice()),
                Span::new(additional_data),
                core::ptr::from_mut(&mut plaintext).cast(),
                receive_bytes,
            )
        };

        // If the result of the algorithm is the indication of authentication failure:
        //       throw an OperationError
        //    Otherwise:
        //       Let plaintext be the resulting plaintext.
        if maybe_plaintext != 0 {
            // SAFETY: LibCrypto transfers an ownership reference.
            let detail = unsafe { Utf16String::from_raw_owned(maybe_plaintext) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Decryption failed: {}", &[&detail]),
            ));
        }

        // 6. Return plaintext.
        Ok(plaintext.expect("LibCrypto returns the result"))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2. Generate a 256-bit key.
        let key_buffer = generate_random_key(cx.vm(), 256)?;

        // 3. If the key generation step fails, then throw an OperationError.
        // Note: Cannot happen in our implementation; and if we OOM, then allocating the Exception is probably going to crash anyway.

        // 4. Let key be a new CryptoKey object representing the generated key.
        let key = CryptoKey::from_byte_handle(cx.vm(), &key_buffer);

        // 5. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new KeyAlgorithm.
        // 7. Set the name attribute of algorithm to "ChaCha20-Poly1305".
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"ChaCha20-Poly1305"),
        });

        // 9. Set the [[extractable]] internal slot of key to be extractable.
        key.set_extractable(extractable);

        // 10. Set the [[usages]] internal slot of key to be the normalized value of usages.
        key.set_usages(normalized_key_usages(key_usages));

        // 11. Return key.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(key))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the spec checks the JWK algorithm after matching the key length"
    )]
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. Let keyData be the key data to be imported.

        // 2. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::Encrypt
                && usage != KeyUsage::Decrypt
                && usage != KeyUsage::WrapKey
                && usage != KeyUsage::UnwrapKey
            {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let data;

        // 3. If format is "raw-secret":
        if format == KeyFormat::RawSecret {
            // 1. Let data be keyData.
            let ImportKeyData::Bytes(bytes) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            data = bytes;

            // 2. If the length in bits of data is not 256 then throw a DataError.
            if data.as_slice().len() != 32 {
                return Err(dom_error("DataError", b"Key data must be 256 bits"));
            }
        }
        // 3. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary:
            //        Let jwk equal keyData.
            //    Otherwise:
            //        Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"Invalid JWK key data"));
            };

            // 2. If the kty field of jwk is not "oct", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"oct")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
            // NB: Specifically, those requirements are:
            // - ".k" is a valid bas64url encoded octet stream, which we do by just parsing it, in step 4.
            // - ".alg" is checked only in step 5.

            // 4. Let data be the octet string obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            // 5. If the alg field of jwk is present, and is not "C20P", then throw a DataError.
            if jwk
                .alg
                .as_ref()
                .is_some_and(|alg| *alg != Utf16String::from_ascii(b"C20P"))
            {
                return Err(dom_error("DataError", b"Invalid alg field"));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error(
                "NotSupportedError",
                b"Only raw-secret and jwk formats are supported",
            ));
        }

        // 4. Let key be a new CryptoKey object representing a key with value data.
        let key = CryptoKey::from_byte_handle(cx.vm(), data.as_slice());

        // 5. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 6. Let algorithm be a new KeyAlgorithm.
        // 7. Set the name attribute of algorithm to "ChaCha20-Poly1305".
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"ChaCha20-Poly1305"),
        });

        // 9. Return key.
        Ok(key)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 2. If format is "raw-secret":
        let result = if format == KeyFormat::RawSecret {
            // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
            let data = key.byte_handle_bytes();

            // 2. Let result be data.
            ExportKeyResult::Bytes(Bytes::new(data.to_vec()))
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "oct".
            jwk.kty = Some(Utf16String::from_ascii(b"oct"));

            // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
            //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
            let key_bytes = key.byte_handle_bytes();
            let Ok(encoded) = base64_url_bytes_encode(key_bytes) else {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            };
            jwk.k = Some(encoded);

            // 4. Set the alg attribute of jwk to the string "C20P".
            jwk.alg = Some(Utf16String::from_ascii(b"C20P"));

            // 5. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 7. Let result be jwk.
            return Ok(ExportKeyResult::JsonWebKey(jwk));
        }
        // 2. Otherwise:
        else {
            // 1. throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Cannot export to unsupported format"));
        };

        // 3. Return result.
        Ok(result)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-operations-get-key-length
    fn get_key_length(&self, _: Gc<Realm>, _: &AlgorithmParams) -> WebIdlResult<Option<u32>> {
        // 1. Return 256.
        Ok(Some(256))
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
