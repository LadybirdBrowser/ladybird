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
use core::ptr::{NonNull, null_mut};
use std::collections::HashSet;
use std::rc::Rc;

use ak::{Span, SpanMut, Utf16String, Utf16StringUnits};
use libjs_rust::api::{
    ErrorKind, ErrorType, Gc, Object, PropertyKey, Realm, Trace, TypedArrayBase, TypedArrayKind, Value, Vm,
};

use crate::bindings::conversions::{
    BufferSource, buffer_source_type_of, to_octet_enforcing_range, to_unsigned_long_enforcing_range,
    to_unsigned_short_enforcing_range,
};
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::crypto::crypto::web_crypto_fill_with_random;
use crate::crypto::crypto_bindings::{CryptoKeyPair, JsonWebKey, crypto_key_from_value};
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

#[derive(PartialEq, Eq)]
#[repr(u8)]
enum Base64DecodeResult {
    #[allow(dead_code, reason = "the native base64 decoder constructs this result")]
    Success,
    #[allow(dead_code, reason = "the native base64 decoder constructs this result")]
    OutOfMemory,
    InvalidBase64,
    NonAscii,
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum Asn1Structure {
    SubjectPublicKeyInfo,
    PrivateKeyInfo,
    OctetString,
}

unsafe extern "C" {
    fn web_crypto_encode_base64url(bytes: Span<'_, u8>, receive: ReceiveBytes, state: *mut c_void) -> i32;
    fn web_crypto_decode_base64url(
        raw: usize,
        state: *mut c_void,
        receive: ReceiveBytes,
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

unsafe fn decode_base64url(
    base64_url_string: &Utf16String,
    state: *mut c_void,
    receive: ReceiveBytes,
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

    // SAFETY: The caller supplies a receiver and a live output slot.
    let result =
        unsafe { web_crypto_decode_base64url(base64_url_string.raw_identity(), state, receive, error_message) };
    if result == Base64DecodeResult::InvalidBase64 {
        // SAFETY: The decoder hands over an owned diagnostic string.
        let detail = unsafe { Utf16String::from_raw_owned(*error_message) };
        *error_message = libjs_rust::api::utf16_formatted("base64 decode: {}", &[&detail]).into_raw();
    }
    result
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

type ReceiveBytes = unsafe extern "C" fn(*mut c_void, Span<'_, u8>);

unsafe extern "C" {
    fn web_crypto_public_key_for_private_key(key: *const c_void, name: usize, named_curve: usize) -> *mut c_void;
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
        _: &CallContext,
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
        _: &CallContext,
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
        _: &CallContext,
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

    fn encapsulate(
        &self,
        _realm: Gc<Realm>,
        _params: &AlgorithmParams,
        _key: Ref<CryptoKey>,
    ) -> WebIdlResult<EncapsulatedBitsData> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("encapsulate is not supported"),
        ))
    }

    fn decapsulate(
        &self,
        _realm: Gc<Realm>,
        _params: &AlgorithmParams,
        _key: Ref<CryptoKey>,
        _data: &Bytes,
    ) -> WebIdlResult<Bytes> {
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

    fn validate_derive_bits_parameters(
        &self,
        _realm: Gc<Realm>,
        _: &AlgorithmParams,
        _length: Option<u32>,
    ) -> WebIdlResult<()> {
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

        // 2. Let functionName be the Keccak decoding of the functionName member of normalizedAlgorithm if present, or
        //    the empty bit string otherwise.
        let function_name = normalized_algorithm.function_name.as_ref();

        // 3. Let customization be the Keccak decoding of the customization member of normalizedAlgorithm if present, or
        //    the empty bit string otherwise.
        let customization = normalized_algorithm.customization.as_ref();

        // 4. If the name member of normalizedAlgorithm is a case-sensitive string match for "cSHAKE128":
        //    Let result be the result of performing the cSHAKE128 function defined in Section 3 of [NIST-SP800-185]
        //    using the Keccak decoding of message as the X input parameter, outputLength as the L input parameter,
        //    functionName as the N input parameter, and customization as the S input parameter.
        let kind = if matches!(algorithm.name.as_units(), Utf16StringUnits::Ascii(b"cSHAKE128")) {
            CShakeKind::CShake128
        // 4. If the name member of normalizedAlgorithm is a case-sensitive string match for "cSHAKE256":
        //    Let result be the result of performing the cSHAKE256 function defined in Section 3 of [NIST-SP800-185]
        //    using the Keccak decoding of message as the X input parameter, outputLength as the L input parameter,
        //    functionName as the N input parameter, and customization as the S input parameter.
        } else if matches!(algorithm.name.as_units(), Utf16StringUnits::Ascii(b"cSHAKE256")) {
            CShakeKind::CShake256
        } else {
            unreachable!("the algorithm is cSHAKE128 or cSHAKE256");
        };

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

        // 6. Return the Keccak encoding of result.
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

    base64_url_bytes_decode(vm, base64_url_string)
}

fn base64_url_bytes_decode(vm: &Vm, base64_url_string: &Utf16String) -> WebIdlResult<Bytes> {
    let mut result: Option<Bytes> = None;
    let mut error_message = 0;
    // SAFETY: The receiver borrows the decoded bytes for the call.
    let status = unsafe {
        decode_base64url(
            base64_url_string,
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
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
    fn sign(&self, _: &CallContext, _: &AlgorithmParams, key: Ref<CryptoKey>, message: &Bytes) -> WebIdlResult<Bytes> {
        // 1. Let mac be the result of performing the HMAC function described in Section 2 of [NIST-SP800-224] using the
        //    key represented by the [[handle]] internal slot of key, the hash function identified by the hash attribute
        //    of the [[algorithm]] internal slot of key and message as the message M.
        let key_data = key.byte_handle_bytes();
        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::Hmac { hash, .. } = &*algorithm else {
            unreachable!("the key has an HMAC algorithm")
        };
        let mac = hmac_calculate_message_digest(hash, key_data, message.as_slice())?;

        // 2. Return mac.
        Ok(mac)
    }

    // https://w3c.github.io/webcrypto/#hmac-operations-verify
    fn verify(
        &self,
        _: &CallContext,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        // 1. Let mac be the result of performing the HMAC function described in Section 2 of [NIST-SP800-224] using the
        //    key represented by the [[handle]] internal slot of key, the hash function identified by the hash attribute
        //    of the [[algorithm]] internal slot of key and message as the message M.
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

        // 2. Let bits be the raw bits of the key represented by the [[handle]] internal slot of key.
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
        // 10. Set the name attribute of hash to equal the name member of the hash member of normalizedAlgorithm.
        let hash_name = normalized_algorithm.hash.name.clone();

        // 12. Set the [[type]] internal slot of key to "secret".
        key.set_type(KeyType::Secret);

        // 11. Set the hash attribute of algorithm to hash.
        // 8. Set the length attribute of algorithm to length.
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
            // 4. Let data be the byte sequence obtained by decoding the k field of jwk.
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
        _: &CallContext,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        message: &Bytes,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;

        // 1. Let customization be the Keccak decoding of the customization member of normalizedAlgorithm if present, or
        //    the empty bit string otherwise.
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
        _: &CallContext,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        let normalized_algorithm = self;

        // 1. Let customization be the Keccak decoding of the customization member of normalizedAlgorithm if present, or
        //    the empty bit string otherwise.
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
        // 5. Return true if computedMac is equal to signature and false otherwise. This comparison must be performed in
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
        // 2. Otherwise, if the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC256":
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

        // 3. Generate a key of length length bits.
        // 4. If the key generation step fails, then throw an OperationError.
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

        // 11. Set the [[extractable]] internal slot of key to be extractable.
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
        // 1. Otherwise, if the name member of normalizedAlgorithm is a case-sensitive string match for "KMAC256":
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
    fn validate_derive_bits_parameters(
        &self,
        _: Gc<Realm>,
        _: &AlgorithmParams,
        length_optional: Option<u32>,
    ) -> WebIdlResult<()> {
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
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;

        // NB: Steps 1 to 3 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(cx.realm(), algorithm, length_optional)?;
        let hash_kind = sha_hash_kind(&normalized_algorithm.hash.name)
            .map_err(|message| Exception::Dom("NotSupportedError", message))?;

        // 4. Let keyDerivationKey be the secret represented by the [[handle]] internal slot of key.
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

        // 7. Return result.
        Ok(result.expect("LibCrypto returns the derived bytes"))
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Pbkdf2;

impl AlgorithmMethods for Pbkdf2Params {
    // https://w3c.github.io/webcrypto/#pbkdf2-operations-derive-bits
    fn validate_derive_bits_parameters(
        &self,
        _: Gc<Realm>,
        _: &AlgorithmParams,
        length_optional: Option<u32>,
    ) -> WebIdlResult<()> {
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
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;

        // NB: Steps 1 and 2 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(cx.realm(), algorithm, length_optional)?;

        // 4. Let prf be the HMAC function described in Section 2 of [NIST-SP800-224] using the hash function described
        //    by the hash member of normalizedAlgorithm.
        let hash_algorithm = &normalized_algorithm.hash.name;

        // 5. Let result be the result of performing the PBKDF2 operation defined in Section 5.2 of [RFC8018] using prf
        //    as the pseudo-random function, PRF, the password represented by the [[handle]] internal slot of key as the
        //    password, P, the salt attribute of normalizedAlgorithm as the salt, S, the value of the iterations
        //    attribute of normalizedAlgorithm as the iteration count, c, and length divided by 8 as the intended key
        //    length, dkLen.
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

        // 6. If the key derivation operation fails, then throw an OperationError.
        if !success {
            return Err(dom_error("OperationError", b"Failed to derive key"));
        }

        // 7. Return result.
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
    fn validate_derive_bits_parameters(
        &self,
        _: Gc<Realm>,
        _: &AlgorithmParams,
        length: Option<u32>,
    ) -> WebIdlResult<()> {
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
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self;
        // NB: Steps 1 to 5 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(cx.realm(), algorithm, length)?;

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

        // 1. If the iv member of normalizedAlgorithm does not have a length of 16 bytes, then throw an OperationError.
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

        // 2. Let paddedPlaintext be the result of adding padding octets to plaintext according to the procedure defined
        //    in Section 10.3 of [RFC2315], step 2, with a value of k of 16.
        // 3. Let ciphertext be the result of performing the CBC Encryption operation described in Section 6.2 of
        //    [NIST-SP800-38A] using AES as the block cipher, the iv member of normalizedAlgorithm as the IV input
        //    parameter and paddedPlaintext as the input plaintext.
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

        // 4. Return ciphertext.
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#aes-cbc-operations-decrypt
    fn validate_decrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Cbc(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Cbc parameters")
        };

        // 1. If the iv member of normalizedAlgorithm does not have a length of 16 bytes, then throw an OperationError.
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

            // 4. Let data be the byte sequence obtained by decoding the k field of jwk.
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

        // 1. If the counter member of normalizedAlgorithm does not have a length of 16 bytes, then throw an
        //    OperationError.
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

        // 3. Let ciphertext be the result of performing the CTR Encryption operation described in Section 6.5 of
        //    [NIST-SP800-38A] using AES as the block cipher, the counter member of normalizedAlgorithm as the initial
        //    value of the counter block, the length member of normalizedAlgorithm as the input parameter m to the
        //    standard counter block incrementing function defined in Appendix B.1 of [NIST-SP800-38A] and plaintext as
        //    the input plaintext.
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

        // 4. Return ciphertext.
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#aes-ctr-operations-decrypt
    fn validate_decrypt_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let AesParameters::Ctr(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses Ctr parameters")
        };

        // 1. If the counter member of normalizedAlgorithm does not have a length of 16 bytes, then throw an
        //    OperationError.
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

        // 3. Let plaintext be the result of performing the CTR Decryption operation described in Section 6.5 of
        //    [NIST-SP800-38A] using AES as the block cipher, the counter member of normalizedAlgorithm as the initial
        //    value of the counter block, the length member of normalizedAlgorithm as the input parameter m to the
        //    standard counter block incrementing function defined in Appendix B.1 of [NIST-SP800-38A] and ciphertext as
        //    the input ciphertext.
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

        // 4. Return plaintext.
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
        // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw an
        //    OperationError.
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

            // 4. Let data be the byte sequence obtained by decoding the k field of jwk.
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

        // 6. Let C and T be the outputs that result from performing the Authenticated Encryption Function described in
        //    Section 7.1 of [NIST-SP800-38D] using AES as the block cipher, the iv member of normalizedAlgorithm as the
        //    IV input parameter, additionalData as the A input parameter, tagLength as the t pre-requisite and
        //    plaintext as the input plaintext.
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

        // 8. Return ciphertext.
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

        // 8. Perform the Authenticated Decryption Function described in Section 7.2 of [NIST-SP800-38D] using AES as
        //    the block cipher, the iv member of normalizedAlgorithm as the IV input parameter, additionalData as the A
        //    input parameter, tagLength as the t pre-requisite, actualCiphertext as the input ciphertext, C and tag as
        //    the authentication tag, T.
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
        // 9. Return plaintext.
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

            // 4. Let data be the byte sequence obtained by decoding the k field of jwk.
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

        // 3. Let key be a new CryptoKey representing an AES key with value data.
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
        //    with ciphertext as the input ciphertext and using the default Initial Value defined in Section 2.2.3.1 of
        //    the same document.
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

            // 5. Set the key_ops attribute of jwk to equal the usages attribute of key.
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
            // - ".alg" is checked only in step 6.

            // 4. Let data be the byte sequence obtained by decoding the k field of jwk.
            data = parse_jwk_symmetric_key(cx.vm(), &jwk)?;

            // FIXME: 5. If the length in bits of data is not 256, then throw a DataError.

            // 6. If the alg field of jwk is present, and is not "C20P", then throw a DataError.
            if jwk
                .alg
                .as_ref()
                .is_some_and(|alg| *alg != Utf16String::from_ascii(b"C20P"))
            {
                return Err(dom_error("DataError", b"Invalid alg field"));
            }

            // 7. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 8. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key
            //    [JWK] or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 9. If the ext field of jwk is present and has the value false and extractable is true, then throw a
            //    DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }
        }
        // 3. Otherwise:
        else {
            // throw a NotSupportedError.
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

            // 5. Set the key_ops attribute of jwk to equal the usages attribute of key.
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

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct RsaKeyGenParams {
    modulus_length: u32,
    public_exponent: Bytes,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct RsaHashedKeyGenParams {
    key_gen: RsaKeyGenParams,
    #[gc(untraced)]
    hash: HashAlgorithmIdentifier,
}

impl RsaHashedKeyGenParams {
    fn from_value(vm: &Vm, realm: Gc<Realm>, object: Gc<Object>) -> WebIdlResult<Self> {
        let modulus_length_value = object.get(vm, &PropertyKey::from_utf8("modulusLength"))?;
        let modulus_length = to_unsigned_long_enforcing_range(vm, modulus_length_value)?;

        let public_exponent_value = object.get(vm, &PropertyKey::from_utf8("publicExponent"))?;
        let Some(BufferSource::TypedArray(public_exponent)) = buffer_source_type_of(public_exponent_value) else {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Uint8Array"])
                .map_err(Into::into);
        };
        if public_exponent.kind() != TypedArrayKind::Uint8Array {
            return vm
                .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Uint8Array"])
                .map_err(Into::into);
        }

        let hash_value = object.get(vm, &PropertyKey::from_utf8("hash"))?;
        let hash = hash_algorithm_identifier_from_value(vm, realm, hash_value, None)?;
        let public_exponent = big_integer_from_api_big_integer(vm, public_exponent)?;

        Ok(Self {
            key_gen: RsaKeyGenParams {
                modulus_length,
                public_exponent,
            },
            hash,
        })
    }
}

// https://w3c.github.io/webcrypto/#big-integer
fn big_integer_from_api_big_integer(vm: &Vm, big_integer: Gc<TypedArrayBase>) -> WebIdlResult<Bytes> {
    // The BigInteger typedef is a Uint8Array that holds an arbitrary magnitude unsigned integer
    // **in big-endian order**. Values read from the API SHALL have minimal typed array length
    // (that is, at most 7 leading zero bits, except the value 0 which shall have length 8 bits).
    // The API SHALL accept values with any number of leading zero bits, including the empty array, which represents zero.
    get_a_copy_of_the_bytes(vm, BufferSource::TypedArray(big_integer))
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct RsaHashedImportParams {
    #[gc(untraced)]
    hash: HashAlgorithmIdentifier,
}

impl RsaHashedImportParams {
    fn from_value(vm: &Vm, realm: Gc<Realm>, object: Gc<Object>) -> WebIdlResult<Self> {
        let hash_value = object.get(vm, &PropertyKey::from_utf8("hash"))?;
        let hash = hash_algorithm_identifier_from_value(vm, realm, hash_value, None)?;
        Ok(Self { hash })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct RsaOaepParams {
    label: Bytes,
}

impl RsaOaepParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let label_value = object.get(vm, &PropertyKey::from_utf8("label"))?;

        let mut label = Bytes::new(Vec::new());
        if !label_value.is_nullish() {
            let Some(label_source) = buffer_source_type_of(label_value) else {
                return vm
                    .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                    .map_err(Into::into);
            };

            label = get_a_copy_of_the_bytes(vm, label_source)?;
        }
        Ok(Self { label })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct RsaPssParams {
    salt_length: u32,
}

impl RsaPssParams {
    // https://w3c.github.io/webcrypto/#RsaPssParams-dictionary
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let salt_length_value = object.get(vm, &PropertyKey::from_utf8("saltLength"))?;
        let salt_length = to_unsigned_long_enforcing_range(vm, salt_length_value)?;
        Ok(Self { salt_length })
    }
}

#[derive(Clone, Copy)]
pub(crate) enum RsaParameterType {
    Base,
    KeyGen,
    Import,
    Oaep,
    Pss,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
enum RsaParameters {
    Base,
    KeyGen(RsaHashedKeyGenParams),
    Import(RsaHashedImportParams),
    Oaep(RsaOaepParams),
    Pss(RsaPssParams),
}

impl RsaParameters {
    fn from_value(
        vm: &Vm,
        realm: Gc<Realm>,
        object: Gc<Object>,
        parameter_type: RsaParameterType,
    ) -> WebIdlResult<Self> {
        Ok(match parameter_type {
            RsaParameterType::Base => Self::Base,
            RsaParameterType::KeyGen => Self::KeyGen(RsaHashedKeyGenParams::from_value(vm, realm, object)?),
            RsaParameterType::Import => Self::Import(RsaHashedImportParams::from_value(vm, realm, object)?),
            RsaParameterType::Oaep => Self::Oaep(RsaOaepParams::from_value(vm, object)?),
            RsaParameterType::Pss => Self::Pss(RsaPssParams::from_value(vm, object)?),
        })
    }
}

#[repr(u8)]
enum RsaJwkInteger {
    Modulus,
    PublicExponent,
    PrivateExponent,
    Prime1,
    Prime2,
    Exponent1,
    Exponent2,
    Coefficient,
}

unsafe extern "C" {
    fn web_crypto_asn1_allocate_key_info(structure: Asn1Structure) -> *mut c_void;
    fn web_crypto_asn1_delete_key_info(structure: Asn1Structure, key_info: *mut c_void);
    fn web_crypto_rsa_key_info_has_encryption_oid(structure: Asn1Structure, key_info: *const c_void) -> bool;
    fn web_crypto_rsa_key_info_read_key(structure: Asn1Structure, key_info: *const c_void) -> *mut c_void;
    fn web_crypto_delete_uninstalled_key(key: *mut c_void);
    fn web_crypto_rsa_key_is_valid(key: *const c_void, valid: &mut bool) -> bool;
    fn web_crypto_rsa_validate_public_exponent(exponent: Span<'_, u8>, modulus_length: u32) -> bool;
    fn web_crypto_rsa_validate_modulus_length(modulus_length: u32) -> bool;
    fn web_crypto_rsa_generate_key_pair(
        modulus_length: u32,
        exponent: Span<'_, u8>,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> bool;
    fn web_crypto_rsa_export_public_exponent(exponent: Span<'_, u8>, state: *mut c_void, receive: ReceiveBytes)
    -> bool;
    fn web_crypto_rsa_read_key_metadata(
        key: *const c_void,
        modulus_length: &mut u32,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_rsa_key_from_jwk_components(is_private: bool, components: Span<'_, Span<'_, u8>>) -> *mut c_void;
    fn web_crypto_rsa_export_der(
        key: *const c_void,
        is_public: bool,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_rsa_export_jwk_integer(
        key: *const c_void,
        component: RsaJwkInteger,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_rsa_oaep_cipher(
        encrypt: bool,
        hash_kind: ShaHashKind,
        key: *const c_void,
        label: Span<'_, u8>,
        input: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_rsa_pss_sign(
        hash_kind: ShaHashKind,
        key: *const c_void,
        salt_length: u32,
        message: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_rsa_pss_verify(
        hash_kind: ShaHashKind,
        key: *const c_void,
        salt_length: u32,
        message: Span<'_, u8>,
        signature: Span<'_, u8>,
        verified: &mut bool,
    ) -> bool;
    fn web_crypto_rsa_pkcs1_sign(
        hash_kind: ShaHashKind,
        key: *const c_void,
        message: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_rsa_pkcs1_verify(
        hash_kind: ShaHashKind,
        key: *const c_void,
        message: Span<'_, u8>,
        signature: Span<'_, u8>,
        verified: &mut bool,
    ) -> bool;
}

struct Asn1KeyInfo {
    structure: Asn1Structure,
    key_info: NonNull<c_void>,
}

impl Asn1KeyInfo {
    fn parse(data: &Bytes, structure: Asn1Structure) -> WebIdlResult<Self> {
        // SAFETY: The structure is either SubjectPublicKeyInfo or PrivateKeyInfo.
        let key_info = unsafe { web_crypto_asn1_allocate_key_info(structure) };
        let key_info = Self {
            structure,
            key_info: NonNull::new(key_info).expect("LibCrypto returns the key info"),
        };
        // SAFETY: The data and the native structure live for the call.
        unsafe {
            match structure {
                Asn1Structure::SubjectPublicKeyInfo => {
                    parse_a_subject_public_key_info(Span::new(data.as_slice()), key_info.key_info.as_ptr())
                }
                Asn1Structure::PrivateKeyInfo => {
                    parse_a_private_key_info(Span::new(data.as_slice()), key_info.key_info.as_ptr())
                }
                Asn1Structure::OctetString => unreachable!("octet strings are not key info"),
            }
        }
        .map_err(|message| Exception::Dom("DataError", message))?;
        Ok(key_info)
    }

    fn has_rsa_encryption_oid(&self) -> bool {
        // SAFETY: This object owns the live structure.
        unsafe { web_crypto_rsa_key_info_has_encryption_oid(self.structure, self.key_info.as_ptr()) }
    }

    fn rsa_key(&self) -> UninstalledKey {
        // SAFETY: This object owns the live structure.
        let key = unsafe { web_crypto_rsa_key_info_read_key(self.structure, self.key_info.as_ptr()) };
        UninstalledKey(NonNull::new(key).expect("LibCrypto returns an RSA key"))
    }
}

impl Drop for Asn1KeyInfo {
    fn drop(&mut self) {
        // SAFETY: This object owns the native structure.
        unsafe { web_crypto_asn1_delete_key_info(self.structure, self.key_info.as_ptr()) };
    }
}

struct UninstalledKey(NonNull<c_void>);

impl Drop for UninstalledKey {
    fn drop(&mut self) {
        // SAFETY: This object owns the native RSA key.
        unsafe { web_crypto_delete_uninstalled_key(self.0.as_ptr()) };
    }
}

impl UninstalledKey {
    fn is_valid(&self) -> Result<bool, ()> {
        let mut valid = false;
        // SAFETY: This object owns the native key and the output is writable.
        let success = unsafe { web_crypto_rsa_key_is_valid(self.0.as_ptr(), &mut valid) };
        if !success {
            return Err(());
        }
        Ok(valid)
    }

    fn install(self, vm: &Vm) -> Ref<CryptoKey> {
        let key = core::mem::ManuallyDrop::new(self).0;
        // SAFETY: Ownership of the live key is transferred.
        unsafe { CryptoKey::create_with_handle(vm, key) }
    }
}

struct NativeKeyPair {
    public_key: UninstalledKey,
    private_key: UninstalledKey,
}

fn rsa_generate_key_pair(modulus_length: u32, exponent: &Bytes) -> Result<NativeKeyPair, ()> {
    let mut public_key = null_mut();
    let mut private_key = null_mut();
    // SAFETY: The exponent and the output slots are lent for the call.
    let success = unsafe {
        web_crypto_rsa_generate_key_pair(
            modulus_length,
            Span::new(exponent.as_slice()),
            &mut public_key,
            &mut private_key,
        )
    };
    if !success {
        return Err(());
    }
    Ok(NativeKeyPair {
        public_key: UninstalledKey(NonNull::new(public_key).expect("LibCrypto returns a public key")),
        private_key: UninstalledKey(NonNull::new(private_key).expect("LibCrypto returns a private key")),
    })
}

// https://w3c.github.io/webcrypto/#dfn-validate-rsa-key-generation-parameters
fn validate_rsa_key_generation_parameters(normalized_algorithm: &RsaKeyGenParams) -> WebIdlResult<()> {
    // 1. Let modulusLength be the modulusLength member of normalizedAlgorithm.
    let modulus_length = normalized_algorithm.modulus_length;

    // 2. Let publicExponent be the result of converting the publicExponent member of normalizedAlgorithm to a
    //    non-negative integer.
    let public_exponent = &normalized_algorithm.public_exponent;

    // 3. If modulusLength is less than 4, or if publicExponent is less than 3, is even, or is greater than or equal to
    //    2^modulusLength - 1, then throw an OperationError.
    // NOTE: publicExponent + 1 has more than modulusLength bits exactly when publicExponent >= 2^modulusLength - 1.
    // SAFETY: The exponent bytes live for the call.
    if modulus_length < 4
        || !unsafe { web_crypto_rsa_validate_public_exponent(Span::new(public_exponent.as_slice()), modulus_length) }
    {
        return Err(dom_error("OperationError", b"Invalid RSA key generation parameters"));
    }

    // NB: Generating the key pair fails for the moduli that our RSA does not support.
    // SAFETY: This call takes no pointers.
    if !unsafe { web_crypto_rsa_validate_modulus_length(modulus_length) } {
        return Err(dom_error(
            "OperationError",
            b"RSA modulus length must be at least 512 bits",
        ));
    }
    Ok(())
}

fn export_public_exponent(vm: &Vm, exponent: &Bytes) -> WebIdlResult<Vec<u8>> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The exponent and the output slot are lent for the call.
    let success = unsafe {
        web_crypto_rsa_export_public_exponent(
            Span::new(exponent.as_slice()),
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    };
    if !success {
        return vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into);
    }
    Ok(Rc::unwrap_or_clone(result.expect("LibCrypto returns the exponent").0))
}

fn rsa_key_metadata(vm: &Vm, key: Ref<CryptoKey>) -> WebIdlResult<(u32, Vec<u8>)> {
    let mut modulus_length = 0;
    let mut exponent: Option<Bytes> = None;
    // SAFETY: The key handle and the output slots are lent for the call.
    let success = unsafe {
        web_crypto_rsa_read_key_metadata(
            key.handle().as_ptr(),
            &mut modulus_length,
            core::ptr::from_mut(&mut exponent).cast(),
            receive_bytes,
        )
    };
    if !success {
        return vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into);
    }
    Ok((
        modulus_length,
        Rc::unwrap_or_clone(exponent.expect("LibCrypto returns the exponent").0),
    ))
}

fn parse_jwk_rsa_key(vm: &Vm, jwk: &JsonWebKey, is_private: bool) -> WebIdlResult<UninstalledKey> {
    let decode = |component: &Option<Utf16String>| {
        base64_url_bytes_decode(vm, component.as_ref().expect("the RSA JWK contains the component"))
    };
    let components = if is_private {
        let n = decode(&jwk.n)?;
        let d = decode(&jwk.d)?;
        let e = decode(&jwk.e)?;
        let mut components = vec![n, e, d];
        // We know that if any of the extra parameters are provided, all of them must be
        if jwk.p.is_some() {
            for component in [&jwk.p, &jwk.q, &jwk.dp, &jwk.dq, &jwk.qi] {
                components.push(decode(component)?);
            }
        }
        components
    } else {
        let e = decode(&jwk.e)?;
        let n = decode(&jwk.n)?;
        vec![n, e]
    };
    let components: Vec<_> = components.iter().map(|bytes| Span::new(bytes.as_slice())).collect();
    // SAFETY: The decoded bytes are lent for the call.
    let key = unsafe { web_crypto_rsa_key_from_jwk_components(is_private, Span::new(&components)) };
    Ok(UninstalledKey(NonNull::new(key).expect("LibCrypto returns an RSA key")))
}

fn rsa_export_der(vm: &Vm, key: Ref<CryptoKey>, is_public: bool) -> WebIdlResult<Bytes> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The key handle and the output slot are lent for the call.
    let success = unsafe {
        web_crypto_rsa_export_der(
            key.handle().as_ptr(),
            is_public,
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    };
    if !success {
        return vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into);
    }
    Ok(result.expect("LibCrypto returns the DER bytes"))
}

fn rsa_export_jwk_integer(vm: &Vm, key: Ref<CryptoKey>, component: RsaJwkInteger) -> WebIdlResult<Utf16String> {
    let mut bytes: Option<Bytes> = None;
    // SAFETY: The key handle and the output slot are lent for the call.
    let success = unsafe {
        web_crypto_rsa_export_jwk_integer(
            key.handle().as_ptr(),
            component,
            core::ptr::from_mut(&mut bytes).cast(),
            receive_bytes,
        )
    };
    if !success {
        return Err(out_of_memory(vm));
    }
    let bytes = bytes.expect("LibCrypto returns the exported integer");
    encode_jwk_bytes(vm, bytes.as_slice())
}

fn key_format_to_string(format: KeyFormat) -> &'static str {
    match format {
        KeyFormat::RawPublic => "raw-public",
        KeyFormat::RawPrivate => "raw-private",
        KeyFormat::RawSeed => "raw-seed",
        KeyFormat::RawSecret => "raw-secret",
        KeyFormat::Raw => "raw",
        KeyFormat::Spki => "spki",
        KeyFormat::Pkcs8 => "pkcs8",
        KeyFormat::Jwk => "jwk",
    }
}
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RsaOaep {
    parameters: RsaParameters,
}

impl RsaOaep {
    pub(crate) fn from_value(
        vm: &Vm,
        realm: Gc<Realm>,
        object: Gc<Object>,
        parameter_type: RsaParameterType,
    ) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: RsaParameters::from_value(vm, realm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for RsaOaep {
    // https://w3c.github.io/webcrypto/#rsa-oaep-operations-encrypt
    fn encrypt(&self, _: &CallContext, key: Ref<CryptoKey>, plaintext: &Bytes) -> WebIdlResult<Bytes> {
        let RsaParameters::Oaep(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA-OAEP parameters")
        };

        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
        }

        // 2. Let label be the label member of normalizedAlgorithm or the empty byte sequence if the label member of
        //    normalizedAlgorithm is not present.
        let label = &normalized_algorithm.label;

        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
            unreachable!("the key has an RSA algorithm")
        };

        // 3. Perform the encryption operation defined in Section 7.1 of [RFC3447] with the key represented by key as
        //    the recipient's RSA public key, plaintext as the message to be encrypted, M and label as the label, L, and
        //    with the hash function specified by the hash attribute of the [[algorithm]] internal slot of key as the
        //    Hash option and MGF1 (defined in Section B.2.1 of [RFC3447]) as the MGF option.
        let mut hash_kind = None;
        if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
            hash_kind = Some(ShaHashKind::Sha1);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
            hash_kind = Some(ShaHashKind::Sha256);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
            hash_kind = Some(ShaHashKind::Sha384);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
            hash_kind = Some(ShaHashKind::Sha512);
        }

        // 4. If performing the operation results in an error, then throw an OperationError.
        let Some(hash_kind) = hash_kind else {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash]),
            ));
        };

        // 5. Let ciphertext be the value C that results from performing the operation.
        let mut ciphertext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_ciphertext = unsafe {
            web_crypto_rsa_oaep_cipher(
                true,
                hash_kind,
                key.handle().as_ptr(),
                Span::new(label.as_slice()),
                Span::new(plaintext.as_slice()),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        };
        if !maybe_ciphertext {
            return Err(dom_error("OperationError", b"Failed to encrypt"));
        }

        // 6. Return ciphertext.
        Ok(ciphertext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#rsa-oaep-operations-decrypt
    fn decrypt(&self, _: &CallContext, key: Ref<CryptoKey>, ciphertext: &Bytes) -> WebIdlResult<Bytes> {
        let RsaParameters::Oaep(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA-OAEP parameters")
        };

        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 2. Let label be the label member of normalizedAlgorithm or the empty byte sequence if the label member of
        //    normalizedAlgorithm is not present.
        let label = &normalized_algorithm.label;

        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
            unreachable!("the key has an RSA algorithm")
        };

        // 3. Perform the decryption operation defined in Section 7.1 of [RFC3447] with the key represented by key as
        //    the recipient's RSA private key, ciphertext as the ciphertext to be decrypted, C, and label as the label,
        //    L, and with the hash function specified by the hash attribute of the [[algorithm]] internal slot of key as
        //    the Hash option and MGF1 (defined in Section B.2.1 of [RFC3447]) as the MGF option.
        let mut hash_kind = None;
        if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
            hash_kind = Some(ShaHashKind::Sha1);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
            hash_kind = Some(ShaHashKind::Sha256);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
            hash_kind = Some(ShaHashKind::Sha384);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
            hash_kind = Some(ShaHashKind::Sha512);
        }

        // 4. If performing the operation results in an error, then throw an OperationError.
        let Some(hash_kind) = hash_kind else {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash]),
            ));
        };

        // 5. Let plaintext the value M that results from performing the operation.
        let mut plaintext: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_plaintext = unsafe {
            web_crypto_rsa_oaep_cipher(
                false,
                hash_kind,
                key.handle().as_ptr(),
                Span::new(label.as_slice()),
                Span::new(ciphertext.as_slice()),
                core::ptr::from_mut(&mut plaintext).cast(),
                receive_bytes,
            )
        };
        if !maybe_plaintext {
            return Err(dom_error("OperationError", b"Failed to encrypt"));
        }

        // 6. Return plaintext.
        Ok(plaintext.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#rsa-oaep-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. Perform the validate RSA key generation parameters algorithm with normalizedAlgorithm.
        let RsaParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA key generation parameters")
        };
        validate_rsa_key_generation_parameters(&normalized_algorithm.key_gen)
    }

    // https://w3c.github.io/webcrypto/#rsa-oaep-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
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

        let RsaParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA key generation parameters")
        };

        // 2. Perform the validate RSA key generation parameters algorithm with normalizedAlgorithm.
        validate_rsa_key_generation_parameters(&normalized_algorithm.key_gen)?;

        // 3. Generate an RSA key pair, as defined in [RFC3447], with RSA modulus length equal to the modulusLength member of normalizedAlgorithm
        //    and RSA public exponent equal to the publicExponent member of normalizedAlgorithm.
        // 4. If performing the operation results in an error, then throw an OperationError.
        let maybe_key_pair = rsa_generate_key_pair(
            normalized_algorithm.key_gen.modulus_length,
            &normalized_algorithm.key_gen.public_exponent,
        );
        let Ok(key_pair) = maybe_key_pair else {
            return Err(dom_error("OperationError", b"Failed generating RSA key pair"));
        };

        let public_exponent = export_public_exponent(cx.vm(), &normalized_algorithm.key_gen.public_exponent)?;
        let hash_name = normalized_algorithm.hash.name.clone();

        // 10. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
        let public_key = key_pair.public_key.install(cx.vm());

        // 11. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 5. Let algorithm be a new RsaHashedKeyAlgorithm object.
        // 6. Set the name attribute of algorithm to "RSA-OAEP".
        // 7. Set the modulusLength attribute of algorithm to equal the modulusLength member of normalizedAlgorithm.
        // 8. Set the publicExponent attribute of algorithm to equal the publicExponent member of normalizedAlgorithm.
        // 9. Set the hash attribute of algorithm to equal the hash member of normalizedAlgorithm.
        // 12. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSA-OAEP"),
            modulus_length: normalized_algorithm.key_gen.modulus_length,
            public_exponent: {
                let mut exponent_copy = Vec::new();
                if exponent_copy.try_reserve_exact(public_exponent.len()).is_err() {
                    return cx
                        .vm()
                        .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                        .map_err(Into::into);
                }
                exponent_copy.extend_from_slice(&public_exponent);
                exponent_copy
            },
            hash: hash_name.clone(),
        });

        // 13. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 14. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "encrypt", "wrapKey" ].
        public_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Encrypt, KeyUsage::WrapKey]));

        // 15. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
        let private_key = key_pair.private_key.install(cx.vm());

        // 16. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 17. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSA-OAEP"),
            modulus_length: normalized_algorithm.key_gen.modulus_length,
            public_exponent,
            hash: hash_name,
        });

        // 18. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 19. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "decrypt", "unwrapKey" ].
        private_key.set_usages(usage_intersection(
            key_usages,
            &[KeyUsage::Decrypt, KeyUsage::UnwrapKey],
        ));

        // 20. Let result be a new CryptoKeyPair dictionary.
        // 21. Set the publicKey attribute of result to be publicKey.
        // 22. Set the privateKey attribute of result to be privateKey.
        // 23. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://w3c.github.io/webcrypto/#rsa-oaep-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        key_format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. Let keyData be the key data to be imported.

        let key;
        let RsaParameters::Import(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA import parameters")
        };

        // 2. -> If format is "spki":
        if key_format == KeyFormat::Spki {
            // 1. If usages contains an entry which is not "encrypt" or "wrapKey", then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Encrypt && usage != KeyUsage::WrapKey {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
            if !spki.has_rsa_encryption_oid() {
                return Err(dom_error(
                    "DataError",
                    b"Algorithm object identifier is not the rsaEncryption object identifier",
                ));
            }

            // 5. Let publicKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the subjectPublicKeyInfo field of spki, structure as the RSAPublicKey structure
            //    specified in Section A.1.1 of [RFC3447], and exactData set to true.
            // NOTE: We already did this in parse_a_subject_public_key_info
            let public_key = spki.rsa_key();

            // 6. If an error occurred while parsing, or it can be determined that publicKey is not
            //    a valid public key according to [RFC3447], then throw a DataError.
            let maybe_valid = public_key.is_valid();
            let Ok(maybe_valid) = maybe_valid else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 7. Let key be a new CryptoKey that represents the RSA public key identified by publicKey.
            key = public_key.install(cx.vm());

            // 8. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);
        }
        // -> If format is "pkcs8":
        else if key_format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "decrypt" or "unwrapKey", then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Decrypt && usage != KeyUsage::UnwrapKey {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
            //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
            if !private_key_info.has_rsa_encryption_oid() {
                return Err(dom_error(
                    "DataError",
                    b"Algorithm object identifier is not the rsaEncryption object identifier",
                ));
            }

            // 5. Let rsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the RSAPrivateKey structure
            //    specified in Section A.1.2 of [RFC3447], and exactData set to true.
            // NOTE: We already did this in parse_a_private_key_info
            let rsa_private_key = private_key_info.rsa_key();

            // 6. If an error occurred while parsing, or if rsaPrivateKey is not
            //    a valid RSA private key according to [RFC3447], then throw a DataError.
            let maybe_valid = rsa_private_key.is_valid();
            let Ok(maybe_valid) = maybe_valid else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 7. Let key be a new CryptoKey that represents the RSA private key identified by rsaPrivateKey.
            key = rsa_private_key.install(cx.vm());

            // 8. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);
        }
        // -> If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1. -> If keyData is a JsonWebKey dictionary:
            //         Let jwk equal keyData.
            //    -> Otherwise:
            //         Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field of jwk is present and usages contains an entry which is not "decrypt" or "unwrapKey", then throw a SyntaxError.
            if jwk.d.is_some() {
                for &usage in usages {
                    if usage != KeyUsage::Decrypt && usage != KeyUsage::UnwrapKey {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the d field of jwk is not present and usages contains an entry which is not "encrypt" or "wrapKey", then throw a SyntaxError.
            if jwk.d.is_none() {
                for &usage in usages {
                    if usage != KeyUsage::Encrypt && usage != KeyUsage::WrapKey {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 4. If the kty field of jwk is not a case-sensitive string match to "RSA", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"RSA")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 5. If usages is non-empty and the use field of jwk is present and is not a case-sensitive string match to "enc", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }

            let mut hash = None;
            // 8. -> If the alg field of jwk is not present:
            if jwk.alg.is_none() {
                //     Let hash be undefined.
            }
            //    ->  If the alg field of jwk is equal to "RSA-OAEP":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RSA-OAEP")) {
                //     Let hash be the string "SHA-1".
                hash = Some(Utf16String::from_ascii(b"SHA-1"));
            }
            //    -> If the alg field of jwk is equal to "RSA-OAEP-256":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RSA-OAEP-256")) {
                //     Let hash be the string "SHA-256".
                hash = Some(Utf16String::from_ascii(b"SHA-256"));
            }
            //    -> If the alg field of jwk is equal to "RSA-OAEP-384":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RSA-OAEP-384")) {
                //     Let hash be the string "SHA-384".
                hash = Some(Utf16String::from_ascii(b"SHA-384"));
            }
            //    -> If the alg field of jwk is equal to "RSA-OAEP-512":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RSA-OAEP-512")) {
                //     Let hash be the string "SHA-512".
                hash = Some(Utf16String::from_ascii(b"SHA-512"));
            }
            //    -> Otherwise:
            else {
                // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining hash.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 9.  If hash is not undefined:
            if let Some(hash) = hash {
                // 1. Let normalizedHash be the result of normalize an algorithm with alg set to hash and op set to digest.
                let normalized_hash =
                    hash_algorithm_identifier_from_value(cx.vm(), cx.realm(), Value::UNDEFINED, Some(hash))?;

                // 2. If normalizedHash is not equal to the hash member of normalizedAlgorithm, throw a DataError.
                if normalized_hash.name != normalized_algorithm.hash.name {
                    return Err(dom_error("DataError", b"Invalid hash"));
                }
            }

            // 10. -> If the d field of jwk is present:
            if jwk.d.is_some() {
                // 1. If jwk does not meet the requirements of Section 6.3.2 of JSON Web Algorithms [JWA], then throw a DataError.
                let mut meets_requirements = jwk.e.is_some() && jwk.n.is_some() && jwk.d.is_some();
                if jwk.p.is_some() || jwk.q.is_some() || jwk.dp.is_some() || jwk.dq.is_some() || jwk.qi.is_some() {
                    meets_requirements &=
                        jwk.p.is_some() && jwk.q.is_some() && jwk.dp.is_some() && jwk.dq.is_some() && jwk.qi.is_some();
                }

                if jwk.oth.is_some() {
                    // FIXME: We don't support > 2 primes in RSA keys
                    meets_requirements = false;
                }

                if !meets_requirements {
                    return Err(dom_error("DataError", b"Invalid JWK private key"));
                }

                // 2. Let privateKey represent the RSA private key identified by interpreting jwk according to Section 6.3.2 of JSON Web Algorithms [JWA].
                let private_key = parse_jwk_rsa_key(cx.vm(), &jwk, true)?;

                // 3. If privateKey can be determined to not be a valid RSA private key according to [RFC3447], then throw a DataError.
                let maybe_valid = private_key.is_valid();
                let Ok(maybe_valid) = maybe_valid else {
                    return Err(dom_error("DataError", b"Failed to verify key"));
                };
                if !maybe_valid {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                // 4. Let key be a new CryptoKey representing privateKey.
                key = private_key.install(cx.vm());

                // 5. Set the [[type]] internal slot of key to "private"
                key.set_type(KeyType::Private);
            }
            //     -> Otherwise:
            else {
                // 1. If jwk does not meet the requirements of Section 6.3.1 of JSON Web Algorithms [JWA], then throw a DataError.
                if jwk.e.is_none() || jwk.n.is_none() {
                    return Err(dom_error("DataError", b"Invalid JWK public key"));
                }

                // 2. Let publicKey represent the RSA public key identified by interpreting jwk according to Section 6.3.1 of JSON Web Algorithms [JWA].
                let public_key = parse_jwk_rsa_key(cx.vm(), &jwk, false)?;

                // 3. If publicKey can be determined to not be a valid RSA public key according to [RFC3447], then throw a DataError.
                let maybe_valid = public_key.is_valid();
                let Ok(maybe_valid) = maybe_valid else {
                    return Err(dom_error("DataError", b"Failed to verify key"));
                };
                if !maybe_valid {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                // 4. Let key be a new CryptoKey representing publicKey.
                key = public_key.install(cx.vm());

                // 5. Set the [[type]] internal slot of key to "public"
                key.set_type(KeyType::Public);
            }
        }
        // -> Otherwise: throw a NotSupportedError.
        else {
            return Err(dom_error("NotSupportedError", b"Unsupported key format"));
        }

        // 5. Set the modulusLength attribute of algorithm to the length, in bits, of the RSA public modulus.
        // 6. Set the publicExponent attribute of algorithm to the BigInteger representation of the RSA public exponent.
        let (modulus_length, public_exponent) = rsa_key_metadata(cx.vm(), key)?;

        // 7. Set the hash attribute of algorithm to the hash member of normalizedAlgorithm.
        let hash_name = normalized_algorithm.hash.name.clone();

        // 3. Let algorithm be a new RsaHashedKeyAlgorithm.
        // 4. Set the name attribute of algorithm to "RSA-OAEP"
        // 8. Set the [[algorithm]] internal slot of key to algorithm
        key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSA-OAEP"),
            modulus_length,
            public_exponent,
            hash: hash_name,
        });

        // 9. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#rsa-oaep-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        let vm = cx.vm();

        // 1. Let key be the key to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki"
        let result = if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not public"));
            }

            // 2. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the
            //    following properties:
            // - Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //   - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
            //   - Set the params field to the ASN.1 type NULL.
            // - Set the subjectPublicKey field to the result of DER-encoding an RSAPublicKey ASN.1 type, as defined in [RFC3447], Appendix A.1.1,
            //   that represents the RSA public key represented by the [[handle]] internal slot of key
            let data = rsa_export_der(vm, key, true)?;

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(data)
        }
        // If format is "pkcs8"
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not private"));
            }

            // 2. Let data be the result of encoding a privateKeyInfo structure with the following properties:
            // - Set the version field to 0.
            // - Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            // - - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
            // - - Set the params field to the ASN.1 type NULL.
            // - Set the privateKey field to the result of DER-encoding an RSAPrivateKey ASN.1 type, as defined in [RFC3447], Appendix A.1.2,
            // that represents the RSA private key represented by the [[handle]] internal slot of key
            let data = rsa_export_der(vm, key, false)?;

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(data)
        }
        // If format is "jwk"
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "RSA".
            jwk.kty = Some(Utf16String::from_ascii(b"RSA"));

            // 3. Let hash be the name attribute of the hash attribute of the [[algorithm]] internal slot of key.
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
                unreachable!("the key has an RSA algorithm")
            };

            // 4. If hash is "SHA-1":
            //      - Set the alg attribute of jwk to the string "RSA-OAEP".
            if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RSA-OAEP"));
            }
            //    If hash is "SHA-256":
            //      - Set the alg attribute of jwk to the string "RSA-OAEP-256".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RSA-OAEP-256"));
            }
            //    If hash is "SHA-384":
            //      - Set the alg attribute of jwk to the string "RSA-OAEP-384".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RSA-OAEP-384"));
            }
            //    If hash is "SHA-512":
            //      - Set the alg attribute of jwk to the string "RSA-OAEP-512".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RSA-OAEP-512"));
            } else {
                // - Perform any key export steps defined by other applicable specifications,
                //   passing format and the hash attribute of the [[algorithm]] internal slot of key and obtaining alg.
                // - Set the alg attribute of jwk to alg.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. Set the attributes n and e of jwk according to the corresponding definitions in JSON Web Algorithms
            //    [JWA], Section 6.3.1.
            jwk.n = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Modulus)?);
            jwk.e = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::PublicExponent)?);

            // 6. If the [[type]] internal slot of key is "private":
            //    1. Set the attributes named d, p, q, dp, dq, and qi of jwk according to the corresponding definitions in JSON Web Algorithms [JWA], Section 6.3.2.
            if key.key_type() == KeyType::Private {
                jwk.d = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::PrivateExponent)?);
                jwk.p = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Prime1)?);
                jwk.q = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Prime2)?);
                jwk.dp = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Exponent1)?);
                jwk.dq = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Exponent2)?);
                jwk.qi = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Coefficient)?);

                // 2. If the underlying RSA private key represented by the [[handle]] internal slot of key is
                //    represented by more than two primes, set the attribute named oth of jwk according to the
                //    corresponding definition in JSON Web Algorithms [JWA], Section 6.3.2.7
                // FIXME: We don't support more than 2 primes on RSA keys
            }

            // 7. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 15. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // Otherwise throw a NotSupportedError.
        else {
            return Err(Exception::Dom(
                "NotSupportedError",
                libjs_rust::api::utf16_formatted(
                    "Exporting to format {} is not supported",
                    &[&key_format_to_string(format)],
                ),
            ));
        };

        // 8. Return result
        Ok(result)
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::Encrypt, KeyUsage::WrapKey])
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RsaPss {
    parameters: RsaParameters,
}

impl RsaPss {
    pub(crate) fn from_value(
        vm: &Vm,
        realm: Gc<Realm>,
        object: Gc<Object>,
        parameter_type: RsaParameterType,
    ) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: RsaParameters::from_value(vm, realm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for RsaPss {
    // https://w3c.github.io/webcrypto/#rsa-pss-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. Perform the validate RSA key generation parameters algorithm with normalizedAlgorithm.
        let RsaParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA key generation parameters")
        };
        validate_rsa_key_generation_parameters(&normalized_algorithm.key_gen)
    }

    // https://w3c.github.io/webcrypto/#rsa-pss-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "sign" or "verify", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let RsaParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA key generation parameters")
        };

        // 2. Perform the validate RSA key generation parameters algorithm with normalizedAlgorithm.
        validate_rsa_key_generation_parameters(&normalized_algorithm.key_gen)?;

        // 3. Generate an RSA key pair, as defined in [RFC3447], with RSA modulus length equal to the modulusLength member of normalizedAlgorithm
        //    and RSA public exponent equal to the publicExponent member of normalizedAlgorithm.
        // 4. If performing the operation results in an error, then throw an OperationError.
        let maybe_key_pair = rsa_generate_key_pair(
            normalized_algorithm.key_gen.modulus_length,
            &normalized_algorithm.key_gen.public_exponent,
        );
        let Ok(key_pair) = maybe_key_pair else {
            return Err(dom_error("OperationError", b"Failed to generate RSA key pair"));
        };

        let public_exponent = export_public_exponent(cx.vm(), &normalized_algorithm.key_gen.public_exponent)?;
        let hash_name = normalized_algorithm.hash.name.clone();

        // 10. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
        let public_key = key_pair.public_key.install(cx.vm());

        // 11. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 5. Let algorithm be a new RsaHashedKeyAlgorithm dictionary.
        // 6. Set the name attribute of algorithm to "RSA-PSS".
        // 7. Set the modulusLength attribute of algorithm to equal the modulusLength member of normalizedAlgorithm.
        // 8. Set the publicExponent attribute of algorithm to equal the publicExponent member of normalizedAlgorithm.
        // 9. Set the hash attribute of algorithm to equal the hash member of normalizedAlgorithm.
        // 12. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSA-PSS"),
            modulus_length: normalized_algorithm.key_gen.modulus_length,
            public_exponent: {
                let mut exponent_copy = Vec::new();
                if exponent_copy.try_reserve_exact(public_exponent.len()).is_err() {
                    return cx
                        .vm()
                        .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                        .map_err(Into::into);
                }
                exponent_copy.extend_from_slice(&public_exponent);
                exponent_copy
            },
            hash: hash_name.clone(),
        });

        // 13. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 14. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
        public_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Verify]));

        // 15. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
        let private_key = key_pair.private_key.install(cx.vm());

        // 16. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 17. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSA-PSS"),
            modulus_length: normalized_algorithm.key_gen.modulus_length,
            public_exponent,
            hash: hash_name,
        });

        // 18. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 19. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
        private_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Sign]));

        // 20. Let result be a new CryptoKeyPair dictionary.
        // 21. Set the publicKey attribute of result to publicKey.
        // 22. Set the privateKey attribute of result to privateKey.
        // 23. Return result.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://w3c.github.io/webcrypto/#rsa-pss-operations-sign
    fn sign(&self, _: &CallContext, _: &AlgorithmParams, key: Ref<CryptoKey>, message: &Bytes) -> WebIdlResult<Bytes> {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        let RsaParameters::Pss(pss_params) = &self.parameters else {
            unreachable!("the operation uses RSA-PSS parameters")
        };
        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
            unreachable!("the key has an RSA algorithm")
        };

        // 2. Perform the signature generation operation defined in Section 8.1 of [RFC3447] with the key represented by
        //    the [[handle]] internal slot of key as the signer's private key, K, and message as the message to be
        //    signed, M, and using the hash function specified by the hash attribute of the [[algorithm]] internal slot
        //    of key as the Hash option, MGF1 (defined in Section B.2.1 of [RFC3447]) as the MGF option and the
        //    saltLength member of normalizedAlgorithm as the salt length option for the EMSA-PSS-ENCODE operation.
        let mut hash_kind = None;
        if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
            hash_kind = Some(ShaHashKind::Sha1);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
            hash_kind = Some(ShaHashKind::Sha256);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
            hash_kind = Some(ShaHashKind::Sha384);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
            hash_kind = Some(ShaHashKind::Sha512);
        }

        // 3. If performing the operation results in an error, then throw an OperationError.
        let Some(hash_kind) = hash_kind else {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash]),
            ));
        };

        // 4. Let signature be the signature, S, that results from performing the operation.
        let mut signature: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_signature = unsafe {
            web_crypto_rsa_pss_sign(
                hash_kind,
                key.handle().as_ptr(),
                pss_params.salt_length,
                Span::new(message.as_slice()),
                core::ptr::from_mut(&mut signature).cast(),
                receive_bytes,
            )
        };
        if !maybe_signature {
            return Err(dom_error("OperationError", b"Failed to sign message"));
        }

        // 6. Return signature.
        Ok(signature.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#rsa-pss-operations-verify
    fn verify(
        &self,
        _: &CallContext,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
        }

        let RsaParameters::Pss(pss_params) = &self.parameters else {
            unreachable!("the operation uses RSA-PSS parameters")
        };
        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
            unreachable!("the key has an RSA algorithm")
        };

        // 2. Perform the signature verification operation defined in Section 8.1 of [RFC3447] with the key represented
        //    by the [[handle]] internal slot of key as the signer's RSA public key and message as M and signature as S
        //    and using the hash function specified by the hash attribute of the [[algorithm]] internal slot of key as
        //    the Hash option, MGF1 (defined in Section B.2.1 of [RFC3447]) as the MGF option and the saltLength member
        //    of normalizedAlgorithm as the salt length option for the EMSA-PSS-VERIFY operation.
        let mut hash_kind = None;
        if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
            hash_kind = Some(ShaHashKind::Sha1);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
            hash_kind = Some(ShaHashKind::Sha256);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
            hash_kind = Some(ShaHashKind::Sha384);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
            hash_kind = Some(ShaHashKind::Sha512);
        }

        let Some(hash_kind) = hash_kind else {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash]),
            ));
        };

        // 3. Let result be a boolean with the value true if the result of the operation was "valid signature" and the value false otherwise.
        let mut verified = false;
        // SAFETY: The input and output slots live for the call.
        let maybe_verification = unsafe {
            web_crypto_rsa_pss_verify(
                hash_kind,
                key.handle().as_ptr(),
                pss_params.salt_length,
                Span::new(message.as_slice()),
                Span::new(signature.as_slice()),
                &mut verified,
            )
        };
        if !maybe_verification {
            return Err(dom_error("OperationError", b"Failed to verify message"));
        }

        Ok(verified)
    }

    // https://w3c.github.io/webcrypto/#rsa-pss-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        key_format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. Let keyData be the key data to be imported.

        let key;
        let RsaParameters::Import(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA import parameters")
        };

        // 2. -> If format is "spki":
        if key_format == KeyFormat::Spki {
            // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
            if !spki.has_rsa_encryption_oid() {
                return Err(dom_error(
                    "DataError",
                    b"Algorithm object identifier is not the rsaEncryption object identifier",
                ));
            }

            // 5. Let publicKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the subjectPublicKeyInfo field of spki, structure as the RSAPublicKey structure
            //    specified in Section A.1.1 of [RFC3447], and exactData set to true.
            // NOTE: We already did this in parse_a_subject_public_key_info
            let public_key = spki.rsa_key();

            // 6. If an error occurred while parsing, or it can be determined that publicKey is not
            //    a valid public key according to [RFC3447], then throw a DataError.
            let maybe_valid = public_key.is_valid();
            let Ok(maybe_valid) = maybe_valid else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 7. Let key be a new CryptoKey that represents the RSA public key identified by publicKey.
            key = public_key.install(cx.vm());

            // 8. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);
        }
        // -> If format is "pkcs8":
        else if key_format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Sign {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
            //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
            if !private_key_info.has_rsa_encryption_oid() {
                return Err(dom_error(
                    "DataError",
                    b"Algorithm object identifier is not the rsaEncryption object identifier",
                ));
            }

            // 5. Let rsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the RSAPrivateKey structure
            //    specified in Section A.1.2 of [RFC3447], and exactData set to true.
            // NOTE: We already did this in parse_a_private_key_info
            let rsa_private_key = private_key_info.rsa_key();

            // 6. If an error occurred while parsing, or if rsaPrivateKey is not
            //    a valid RSA private key according to [RFC3447], then throw a DataError.
            let maybe_valid = rsa_private_key.is_valid();
            let Ok(maybe_valid) = maybe_valid else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 7. Let key be a new CryptoKey that represents the RSA private key identified by rsaPrivateKey.
            key = rsa_private_key.install(cx.vm());

            // 8. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);
        }
        // -> If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1. -> If keyData is a JsonWebKey dictionary:
            //         Let jwk equal keyData.
            //    -> Otherwise:
            //         Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field of jwk is present and usages contains an entry which is not "sign", or,
            //    if the d field of jwk is not present and usages contains an entry which is not "verify"
            //    then throw a SyntaxError.
            if jwk.d.is_some() {
                for &usage in usages {
                    if usage != KeyUsage::Sign {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            } else {
                for &usage in usages {
                    if usage != KeyUsage::Verify {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the kty field of jwk is not a case-sensitive string match to "RSA", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"RSA")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 4. If usages is non-empty and the use field of jwk is present and is not a case-sensitive string match to "sig", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 5. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 6. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }

            let mut hash = None;
            // 7. -> If the alg field of jwk is not present:
            if jwk.alg.is_none() {
                //     Let hash be undefined.
            }
            //    ->  If the alg field of jwk is equal to "PS1":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"PS1")) {
                //     Let hash be the string "SHA-1".
                hash = Some(Utf16String::from_ascii(b"SHA-1"));
            }
            //    -> If the alg field of jwk is equal to "PS256":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"PS256")) {
                //     Let hash be the string "SHA-256".
                hash = Some(Utf16String::from_ascii(b"SHA-256"));
            }
            //    -> If the alg field of jwk is equal to "PS384":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"PS384")) {
                //     Let hash be the string "SHA-384".
                hash = Some(Utf16String::from_ascii(b"SHA-384"));
            }
            //    -> If the alg field of jwk is equal to "PS512":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"PS512")) {
                //     Let hash be the string "SHA-512".
                hash = Some(Utf16String::from_ascii(b"SHA-512"));
            }
            //    -> Otherwise:
            else {
                // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining hash.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 8. If hash is not undefined:
            if let Some(hash) = hash {
                // 1. Let normalizedHash be the result of normalize an algorithm with alg set to hash and op set to digest.
                let normalized_hash =
                    hash_algorithm_identifier_from_value(cx.vm(), cx.realm(), Value::UNDEFINED, Some(hash))?;

                // 2. If normalizedHash is not equal to the hash member of normalizedAlgorithm, throw a DataError.
                if normalized_hash.name != normalized_algorithm.hash.name {
                    return Err(dom_error("DataError", b"Invalid hash"));
                }
            }

            // 9. -> If the d field of jwk is present:
            if jwk.d.is_some() {
                // 1. If jwk does not meet the requirements of Section 6.3.2 of JSON Web Algorithms [JWA], then throw a DataError.
                let mut meets_requirements = jwk.e.is_some() && jwk.n.is_some() && jwk.d.is_some();
                if jwk.p.is_some() || jwk.q.is_some() || jwk.dp.is_some() || jwk.dq.is_some() || jwk.qi.is_some() {
                    meets_requirements &=
                        jwk.p.is_some() && jwk.q.is_some() && jwk.dp.is_some() && jwk.dq.is_some() && jwk.qi.is_some();
                }

                if jwk.oth.is_some() {
                    // FIXME: We don't support > 2 primes in RSA keys
                    meets_requirements = false;
                }

                if !meets_requirements {
                    return Err(dom_error("DataError", b"Invalid JWK private key"));
                }

                // 2. Let privateKey represent the RSA private key identified by interpreting jwk according to Section 6.3.2 of JSON Web Algorithms [JWA].
                let private_key = parse_jwk_rsa_key(cx.vm(), &jwk, true)?;

                // 3. If privateKey can be determined to not be a valid RSA private key according to [RFC3447], then throw a DataError.
                let maybe_valid = private_key.is_valid();
                let Ok(maybe_valid) = maybe_valid else {
                    return Err(dom_error("DataError", b"Failed to verify key"));
                };
                if !maybe_valid {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                // 4. Let key be a new CryptoKey representing privateKey.
                key = private_key.install(cx.vm());

                // 5. Set the [[type]] internal slot of key to "private"
                key.set_type(KeyType::Private);
            }
            //     -> Otherwise:
            else {
                // 1. If jwk does not meet the requirements of Section 6.3.1 of JSON Web Algorithms [JWA], then throw a DataError.
                if jwk.e.is_none() || jwk.n.is_none() {
                    return Err(dom_error("DataError", b"Invalid JWK public key"));
                }

                // 2. Let publicKey represent the RSA public key identified by interpreting jwk according to Section 6.3.1 of JSON Web Algorithms [JWA].
                let public_key = parse_jwk_rsa_key(cx.vm(), &jwk, false)?;

                // 3. If publicKey can be determined to not be a valid RSA public key according to [RFC3447], then throw a DataError.
                let maybe_valid = public_key.is_valid();
                let Ok(maybe_valid) = maybe_valid else {
                    return Err(dom_error("DataError", b"Failed to verify key"));
                };
                if !maybe_valid {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                // 4. Let key be a new CryptoKey representing publicKey.
                key = public_key.install(cx.vm());

                // 5. Set the [[type]] internal slot of key to "public"
                key.set_type(KeyType::Public);
            }
        }
        // -> Otherwise: throw a NotSupportedError.
        else {
            return Err(dom_error("NotSupportedError", b"Unsupported key format"));
        }

        // 5. Set the modulusLength attribute of algorithm to the length, in bits, of the RSA public modulus.
        // 6. Set the publicExponent attribute of algorithm to the BigInteger representation of the RSA public exponent.
        let (modulus_length, public_exponent) = rsa_key_metadata(cx.vm(), key)?;

        // 7. Set the hash attribute of algorithm to the hash member of normalizedAlgorithm.
        let hash_name = normalized_algorithm.hash.name.clone();

        // 3. Let algorithm be a new RsaHashedKeyAlgorithm dictionary.
        // 4. Set the name attribute of algorithm to "RSA-PSS"
        // 8. Set the [[algorithm]] internal slot of key to algorithm
        key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSA-PSS"),
            modulus_length,
            public_exponent,
            hash: hash_name,
        });

        // 9. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#rsa-pss-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        let vm = cx.vm();

        // 1. Let key be the key to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki"
        let result = if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not public"));
            }

            // 2. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the
            //    following properties:
            // - Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //   - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
            //   - Set the params field to the ASN.1 type NULL.
            // - Set the subjectPublicKey field to the result of DER-encoding an RSAPublicKey ASN.1 type, as defined in [RFC3447], Appendix A.1.1,
            //   that represents the RSA public key represented by the [[handle]] internal slot of key
            let data = rsa_export_der(vm, key, true)?;

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(data)
        }
        // If format is "pkcs8"
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not private"));
            }

            // 2. Let data be the result of encoding a privateKeyInfo structure with the following properties:
            // - Set the version field to 0.
            // - Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            // - - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
            // - - Set the params field to the ASN.1 type NULL.
            // - Set the privateKey field to the result of DER-encoding an RSAPrivateKey ASN.1 type, as defined in [RFC3447], Appendix A.1.2,
            // that represents the RSA private key represented by the [[handle]] internal slot of key
            let data = rsa_export_der(vm, key, false)?;

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(data)
        }
        // If format is "jwk"
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "RSA".
            jwk.kty = Some(Utf16String::from_ascii(b"RSA"));

            // 3. Let hash be the name attribute of the hash attribute of the [[algorithm]] internal slot of key.
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
                unreachable!("the key has an RSA algorithm")
            };

            // 4. If hash is "SHA-1":
            //      - Set the alg attribute of jwk to the string "PS1".
            if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
                jwk.alg = Some(Utf16String::from_ascii(b"PS1"));
            }
            //    If hash is "SHA-256":
            //      - Set the alg attribute of jwk to the string "PS256".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
                jwk.alg = Some(Utf16String::from_ascii(b"PS256"));
            }
            //    If hash is "SHA-384":
            //      - Set the alg attribute of jwk to the string "PS384".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
                jwk.alg = Some(Utf16String::from_ascii(b"PS384"));
            }
            //    If hash is "SHA-512":
            //      - Set the alg attribute of jwk to the string "PS512".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
                jwk.alg = Some(Utf16String::from_ascii(b"PS512"));
            } else {
                // - Perform any key export steps defined by other applicable specifications,
                //   passing format and the hash attribute of the [[algorithm]] internal slot of key and obtaining alg.
                // - Set the alg attribute of jwk to alg.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. Set the attributes n and e of jwk according to the corresponding definitions in JSON Web Algorithms [JWA], Section 6.3.1.
            jwk.n = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Modulus)?);
            jwk.e = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::PublicExponent)?);

            // 6. If the [[type]] internal slot of key is "private":
            if key.key_type() == KeyType::Private {
                // 1. Set the attributes named d, p, q, dp, dq, and qi of jwk according to the corresponding definitions
                //    in JSON Web Algorithms [JWA], Section 6.3.2.
                jwk.d = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::PrivateExponent)?);
                jwk.p = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Prime1)?);
                jwk.q = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Prime2)?);
                jwk.dp = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Exponent1)?);
                jwk.dq = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Exponent2)?);
                jwk.qi = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Coefficient)?);

                // 2. If the underlying RSA private key represented by the [[handle]] internal slot of key is represented by more than two primes,
                //    set the attribute named oth of jwk according to the corresponding definition in JSON Web Algorithms [JWA], Section 6.3.2.7
                // FIXME: We don't support more than 2 primes on RSA keys
            }

            // 7. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 9. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // Otherwise throw a NotSupportedError.
        else {
            return Err(Exception::Dom(
                "NotSupportedError",
                libjs_rust::api::utf16_formatted(
                    "Exporting to format {} is not supported",
                    &[&key_format_to_string(format)],
                ),
            ));
        };

        // 8. Return result
        Ok(result)
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::Verify])
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RsassaPkcs1 {
    parameters: RsaParameters,
}

impl RsassaPkcs1 {
    pub(crate) fn from_value(
        vm: &Vm,
        realm: Gc<Realm>,
        object: Gc<Object>,
        parameter_type: RsaParameterType,
    ) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: RsaParameters::from_value(vm, realm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for RsassaPkcs1 {
    // https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // 2. Perform the validate RSA key generation parameters algorithm with normalizedAlgorithm.
        let RsaParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA key generation parameters")
        };
        validate_rsa_key_generation_parameters(&normalized_algorithm.key_gen)
    }

    // https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "sign" or "verify", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let RsaParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA key generation parameters")
        };

        // 2. Perform the validate RSA key generation parameters algorithm with normalizedAlgorithm.
        validate_rsa_key_generation_parameters(&normalized_algorithm.key_gen)?;

        // 3. Generate an RSA key pair, as defined in [RFC3447], with RSA modulus length equal to the modulusLength member of normalizedAlgorithm
        //    and RSA public exponent equal to the publicExponent member of normalizedAlgorithm.
        // 4. If performing the operation results in an error, then throw an OperationError.
        let maybe_key_pair = rsa_generate_key_pair(
            normalized_algorithm.key_gen.modulus_length,
            &normalized_algorithm.key_gen.public_exponent,
        );
        let Ok(key_pair) = maybe_key_pair else {
            return Err(dom_error("OperationError", b"Failed to generate RSA key pair"));
        };

        let public_exponent = export_public_exponent(cx.vm(), &normalized_algorithm.key_gen.public_exponent)?;
        let hash_name = normalized_algorithm.hash.name.clone();

        // 10. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
        let public_key = key_pair.public_key.install(cx.vm());

        // 11. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 5. Let algorithm be a new RsaHashedKeyAlgorithm dictionary.
        // 6. Set the name attribute of algorithm to "RSASSA-PKCS1-v1_5".
        // 7. Set the modulusLength attribute of algorithm to equal the modulusLength member of normalizedAlgorithm.
        // 8. Set the publicExponent attribute of algorithm to equal the publicExponent member of normalizedAlgorithm.
        // 9. Set the hash attribute of algorithm to equal the hash member of normalizedAlgorithm.
        // 12. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSASSA-PKCS1-v1_5"),
            modulus_length: normalized_algorithm.key_gen.modulus_length,
            public_exponent: {
                let mut exponent_copy = Vec::new();
                if exponent_copy.try_reserve_exact(public_exponent.len()).is_err() {
                    return cx
                        .vm()
                        .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                        .map_err(Into::into);
                }
                exponent_copy.extend_from_slice(&public_exponent);
                exponent_copy
            },
            hash: hash_name.clone(),
        });

        // 13. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 14. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
        public_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Verify]));

        // 15. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
        let private_key = key_pair.private_key.install(cx.vm());

        // 16. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 17. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSASSA-PKCS1-v1_5"),
            modulus_length: normalized_algorithm.key_gen.modulus_length,
            public_exponent,
            hash: hash_name,
        });

        // 18. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 19. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
        private_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Sign]));

        // 20. Let result be a new CryptoKeyPair dictionary.
        // 21. Set the publicKey attribute of result to be publicKey.
        // 22. Set the privateKey attribute of result to be privateKey.
        // 23. Return result.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations-sign
    fn sign(&self, _: &CallContext, _: &AlgorithmParams, key: Ref<CryptoKey>, message: &Bytes) -> WebIdlResult<Bytes> {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
            unreachable!("the key has an RSA algorithm")
        };

        // 2. Perform the signature generation operation defined in Section 8.2 of [RFC3447] with the key represented by
        //    the [[handle]] internal slot of key as the signer's private key and message as M and using the hash
        //    function specified in the hash attribute of the [[algorithm]] internal slot of key as the Hash option for
        //    the EMSA-PKCS1-v1_5 encoding method.
        let mut hash_kind = None;
        if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
            hash_kind = Some(ShaHashKind::Sha1);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
            hash_kind = Some(ShaHashKind::Sha256);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
            hash_kind = Some(ShaHashKind::Sha384);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
            hash_kind = Some(ShaHashKind::Sha512);
        }

        // 3. If performing the operation results in an error, then throw an OperationError.
        let Some(hash_kind) = hash_kind else {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash]),
            ));
        };

        // 5. Let signature be the signature, S, that results from performing the operation.
        let mut signature: Option<Bytes> = None;
        // SAFETY: The input and output slots live for the call.
        let maybe_signature = unsafe {
            web_crypto_rsa_pkcs1_sign(
                hash_kind,
                key.handle().as_ptr(),
                Span::new(message.as_slice()),
                core::ptr::from_mut(&mut signature).cast(),
                receive_bytes,
            )
        };
        if !maybe_signature {
            return Err(dom_error("OperationError", b"Failed to sign message"));
        }

        // 6. Return signature.
        Ok(signature.expect("LibCrypto returns the result"))
    }

    // https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations-verify
    fn verify(
        &self,
        _: &CallContext,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
        }

        let algorithm = key.internal_algorithm();
        let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
            unreachable!("the key has an RSA algorithm")
        };

        // 2. Perform the signature verification operation defined in Section 8.2 of [RFC3447] with the key represented
        //    by the [[handle]] internal slot of key as the signer's RSA public key and message as M and signature as S
        //    and using the hash function specified in the hash attribute of the [[algorithm]] internal slot of key as
        //    the Hash option for the EMSA-PKCS1-v1_5 encoding method.
        let mut hash_kind = None;
        if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
            hash_kind = Some(ShaHashKind::Sha1);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
            hash_kind = Some(ShaHashKind::Sha256);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
            hash_kind = Some(ShaHashKind::Sha384);
        } else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
            hash_kind = Some(ShaHashKind::Sha512);
        }

        let Some(hash_kind) = hash_kind else {
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Invalid hash function '{}'", &[hash]),
            ));
        };

        // 3. Let result be a boolean with value true if the result of the operation was "valid signature" and the value
        //    false otherwise.
        let mut verified = false;
        // SAFETY: The input and output slots live for the call.
        let maybe_verification = unsafe {
            web_crypto_rsa_pkcs1_verify(
                hash_kind,
                key.handle().as_ptr(),
                Span::new(message.as_slice()),
                Span::new(signature.as_slice()),
                &mut verified,
            )
        };
        if !maybe_verification {
            return Err(dom_error("OperationError", b"Failed to verify message"));
        }

        Ok(verified)
    }

    // https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        key_format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        // 1. Let keyData be the key data to be imported.

        let key;
        let RsaParameters::Import(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses RSA import parameters")
        };

        // 2. -> If format is "spki":
        if key_format == KeyFormat::Spki {
            // 1. If usages contains an entry which is not "verify", then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
            if !spki.has_rsa_encryption_oid() {
                return Err(dom_error(
                    "DataError",
                    b"Algorithm object identifier is not the rsaEncryption object identifier",
                ));
            }

            // 5. Let publicKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the subjectPublicKeyInfo field of spki, structure as the RSAPublicKey structure
            //    specified in Section A.1.1 of [RFC3447], and exactData set to true.
            // NOTE: We already did this in parse_a_subject_public_key_info
            let public_key = spki.rsa_key();

            // 6. If an error occurred while parsing, or it can be determined that publicKey is not
            //    a valid public key according to [RFC3447], then throw a DataError.
            let maybe_valid = public_key.is_valid();
            let Ok(maybe_valid) = maybe_valid else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 7. Let key be a new CryptoKey that represents the RSA public key identified by publicKey.
            key = public_key.install(cx.vm());

            // 8. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);
        }
        // -> If format is "pkcs8":
        else if key_format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Sign {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
            //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
            if !private_key_info.has_rsa_encryption_oid() {
                return Err(dom_error(
                    "DataError",
                    b"Algorithm object identifier is not the rsaEncryption object identifier",
                ));
            }

            // 5. Let rsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the RSAPrivateKey structure
            //    specified in Section A.1.2 of [RFC3447], and exactData set to true.
            // NOTE: We already did this in parse_a_private_key_info
            let rsa_private_key = private_key_info.rsa_key();

            // 6. If an error occurred while parsing, or if rsaPrivateKey is not
            //    a valid RSA private key according to [RFC3447], then throw a DataError.
            let maybe_valid = rsa_private_key.is_valid();
            let Ok(maybe_valid) = maybe_valid else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 7. Let key be a new CryptoKey that represents the RSA private key identified by rsaPrivateKey.
            key = rsa_private_key.install(cx.vm());

            // 8. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);
        }
        // -> If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1. -> If keyData is a JsonWebKey dictionary:
            //         Let jwk equal keyData.
            //    -> Otherwise:
            //         Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field of jwk is present and usages contains an entry which is not "sign", or,
            //    if the d field of jwk is not present and usages contains an entry which is not "verify"
            //    then throw a SyntaxError.
            if jwk.d.is_some() {
                for &usage in usages {
                    if usage != KeyUsage::Sign {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            } else {
                for &usage in usages {
                    if usage != KeyUsage::Verify {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the kty field of jwk is not a case-sensitive string match to "RSA", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"RSA")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 4. If usages is non-empty and the use field of jwk is present and is not a case-sensitive string match to "sig", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid use field"));
            }

            // 5. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
            //    or does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 6. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }

            let mut hash = None;
            // 8. -> If the alg field of jwk is not present:
            if jwk.alg.is_none() {
                //     Let hash be undefined.
            }
            //    ->  If the alg field of jwk is equal to "RS1":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RS1")) {
                //     Let hash be the string "SHA-1".
                hash = Some(Utf16String::from_ascii(b"SHA-1"));
            }
            //    -> If the alg field of jwk is equal to "RS256":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RS256")) {
                //     Let hash be the string "SHA-256".
                hash = Some(Utf16String::from_ascii(b"SHA-256"));
            }
            //    -> If the alg field of jwk is equal to "RS384":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RS384")) {
                //     Let hash be the string "SHA-384".
                hash = Some(Utf16String::from_ascii(b"SHA-384"));
            }
            //    -> If the alg field of jwk is equal to "RS512":
            else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"RS512")) {
                //     Let hash be the string "SHA-512".
                hash = Some(Utf16String::from_ascii(b"SHA-512"));
            }
            //    -> Otherwise:
            else {
                // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining hash.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 8. If hash is not undefined:
            if let Some(hash) = hash {
                // 1. Let normalizedHash be the result of normalize an algorithm with alg set to hash and op set to digest.
                let normalized_hash =
                    hash_algorithm_identifier_from_value(cx.vm(), cx.realm(), Value::UNDEFINED, Some(hash))?;

                // 2. If normalizedHash is not equal to the hash member of normalizedAlgorithm, throw a DataError.
                if normalized_hash.name != normalized_algorithm.hash.name {
                    return Err(dom_error("DataError", b"Invalid hash"));
                }
            }

            // 9. -> If the d field of jwk is present:
            if jwk.d.is_some() {
                // 1. If jwk does not meet the requirements of Section 6.3.2 of JSON Web Algorithms [JWA], then throw a DataError.
                let mut meets_requirements = jwk.e.is_some() && jwk.n.is_some() && jwk.d.is_some();
                if jwk.p.is_some() || jwk.q.is_some() || jwk.dp.is_some() || jwk.dq.is_some() || jwk.qi.is_some() {
                    meets_requirements &=
                        jwk.p.is_some() && jwk.q.is_some() && jwk.dp.is_some() && jwk.dq.is_some() && jwk.qi.is_some();
                }

                if jwk.oth.is_some() {
                    // FIXME: We don't support > 2 primes in RSA keys
                    meets_requirements = false;
                }

                if !meets_requirements {
                    return Err(dom_error("DataError", b"Invalid JWK private key"));
                }

                // 2. Let privateKey represents the RSA private key identified by interpreting jwk according to Section
                //    6.3.2 of JSON Web Algorithms [JWA].
                let private_key = parse_jwk_rsa_key(cx.vm(), &jwk, true)?;

                // 3. If publicKey can be determined to not be a valid RSA public key according to [RFC3447], then throw
                //    a DataError.
                let maybe_valid = private_key.is_valid();
                let Ok(maybe_valid) = maybe_valid else {
                    return Err(dom_error("DataError", b"Failed to verify key"));
                };
                if !maybe_valid {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                // 4. Let key be a new CryptoKey representing publicKey.
                key = private_key.install(cx.vm());

                // 5. Set the [[type]] internal slot of key to "private"
                key.set_type(KeyType::Private);
            }
            //     -> Otherwise:
            else {
                // 1. If jwk does not meet the requirements of Section 6.3.1 of JSON Web Algorithms [JWA], then throw a DataError.
                if jwk.e.is_none() || jwk.n.is_none() {
                    return Err(dom_error("DataError", b"Invalid JWK public key"));
                }

                // 2. Let publicKey represent the RSA public key identified by interpreting jwk according to Section 6.3.1 of JSON Web Algorithms [JWA].
                let public_key = parse_jwk_rsa_key(cx.vm(), &jwk, false)?;

                // 3. If publicKey can be determined to not be a valid RSA public key according to [RFC3447], then throw a DataError.
                let maybe_valid = public_key.is_valid();
                let Ok(maybe_valid) = maybe_valid else {
                    return Err(dom_error("DataError", b"Failed to verify key"));
                };
                if !maybe_valid {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                // 4. Let key be a new CryptoKey representing publicKey.
                key = public_key.install(cx.vm());

                // 5. Set the [[type]] internal slot of key to "public"
                key.set_type(KeyType::Public);
            }
        }
        // -> Otherwise: throw a NotSupportedError.
        else {
            return Err(dom_error("NotSupportedError", b"Unsupported key format"));
        }

        // 5. Set the modulusLength attribute of algorithm to the length, in bits, of the RSA public modulus.
        // 6. Set the publicExponent attribute of algorithm to the BigInteger representation of the RSA public exponent.
        let (modulus_length, public_exponent) = rsa_key_metadata(cx.vm(), key)?;

        // 7. Set the hash attribute of algorithm to the hash member of normalizedAlgorithm.
        let hash_name = normalized_algorithm.hash.name.clone();

        // 3. Let algorithm be a new RsaHashedKeyAlgorithm dictionary.
        // 4. Set the name attribute of algorithm to "RSASSA-PKCS1-v1_5"
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key.set_algorithm(InternalAlgorithmData::RsaHashed {
            name: Utf16String::from_ascii(b"RSASSA-PKCS1-v1_5"),
            modulus_length,
            public_exponent,
            hash: hash_name,
        });

        // 9. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        let vm = cx.vm();

        // 1. Let key be the key to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki"
        let result = if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not public"));
            }

            // 2. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the
            //    following properties:
            // - Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //   - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
            //   - Set the params field to the ASN.1 type NULL.
            // - Set the subjectPublicKey field to the result of DER-encoding an RSAPublicKey ASN.1 type, as defined in [RFC3447], Appendix A.1.1,
            //   that represents the RSA public key represented by the [[handle]] internal slot of key
            let data = rsa_export_der(vm, key, true)?;

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(data)
        }
        // If format is "pkcs8"
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not private"));
            }

            // 2. Let data be the result of encoding a privateKeyInfo structure with the following properties:
            // - Set the version field to 0.
            // - Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            // - - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
            // - - Set the params field to the ASN.1 type NULL.
            // - Set the privateKey field to the result of DER-encoding an RSAPrivateKey ASN.1 type, as defined in [RFC3447], Appendix A.1.2,
            // that represents the RSA private key represented by the [[handle]] internal slot of key
            let data = rsa_export_der(vm, key, false)?;

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(data)
        }
        // If format is "jwk"
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to the string "RSA".
            jwk.kty = Some(Utf16String::from_ascii(b"RSA"));

            // 3. Let hash be the name attribute of the hash attribute of the [[algorithm]] internal slot of key.
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::RsaHashed { hash, .. } = &*algorithm else {
                unreachable!("the key has an RSA algorithm")
            };

            // 4. If hash is "SHA-1":
            //      - Set the alg attribute of jwk to the string "RS1".
            if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-1")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RS1"));
            }
            //    If hash is "SHA-256":
            //      - Set the alg attribute of jwk to the string "RS256".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-256")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RS256"));
            }
            //    If hash is "SHA-384":
            //      - Set the alg attribute of jwk to the string "RS384".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-384")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RS384"));
            }
            //    If hash is "SHA-512":
            //      - Set the alg attribute of jwk to the string "RS512".
            else if matches!(hash.as_units(), Utf16StringUnits::Ascii(b"SHA-512")) {
                jwk.alg = Some(Utf16String::from_ascii(b"RS512"));
            } else {
                // - Perform any key export steps defined by other applicable specifications,
                //   passing format and the hash attribute of the [[algorithm]] internal slot of key and obtaining alg.
                // - Set the alg attribute of jwk to alg.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. Set the attributes n and e of jwk according to the corresponding definitions in JSON Web Algorithms [JWA], Section 6.3.1.
            jwk.n = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Modulus)?);
            jwk.e = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::PublicExponent)?);

            // 6. If the [[type]] internal slot of key is "private":
            if key.key_type() == KeyType::Private {
                // 1. Set the attributes named d, p, q, dp, dq, and qi of jwk according to the corresponding definitions
                //    in JSON Web Algorithms [JWA], Section 6.3.2.
                jwk.d = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::PrivateExponent)?);
                jwk.p = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Prime1)?);
                jwk.q = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Prime2)?);
                jwk.dp = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Exponent1)?);
                jwk.dq = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Exponent2)?);
                jwk.qi = Some(rsa_export_jwk_integer(vm, key, RsaJwkInteger::Coefficient)?);

                // 2. If the underlying RSA private key represented by the [[handle]] internal slot of key is represented by more than two primes,
                //    set the attribute named oth of jwk according to the corresponding definition in JSON Web Algorithms [JWA], Section 6.3.2.7
                // FIXME: We don't support more than 2 primes on RSA keys
            }

            // 7. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 9. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // Otherwise throw a NotSupportedError.
        else {
            return Err(Exception::Dom(
                "NotSupportedError",
                libjs_rust::api::utf16_formatted(
                    "Exporting to format {} is not supported",
                    &[&key_format_to_string(format)],
                ),
            ));
        };

        // 8. Return result
        Ok(result)
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::Verify])
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct EcdsaParams {
    #[gc(untraced)]
    hash: HashAlgorithmIdentifier,
}

impl EcdsaParams {
    fn from_value(vm: &Vm, realm: Gc<Realm>, object: Gc<Object>) -> WebIdlResult<Self> {
        let hash_value = object.get(vm, &PropertyKey::from_utf8("hash"))?;
        let hash = hash_algorithm_identifier_from_value(vm, realm, hash_value, None)?;
        Ok(Self { hash })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct EcKeyGenParams {
    named_curve: Utf16String,
}

impl EcKeyGenParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let curve_value = object.get(vm, &PropertyKey::from_utf8("namedCurve"))?;
        let named_curve = curve_value.to_utf16_string(vm)?;
        Ok(Self { named_curve })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct EcdhKeyDeriveParams {
    public_key: Ref<CryptoKey>,
}

impl EcdhKeyDeriveParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let public_key = crypto_key_from_value(vm, object.get(vm, &PropertyKey::from_utf8("public"))?)?;
        Ok(Self { public_key })
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct EcKeyImportParams {
    named_curve: Utf16String,
}

impl EcKeyImportParams {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let named_curve_value = object.get(vm, &PropertyKey::from_utf8("namedCurve"))?;
        let named_curve = named_curve_value.to_utf16_string(vm)?;
        Ok(Self { named_curve })
    }
}

#[derive(Clone, Copy)]
pub(crate) enum EcParameterType {
    Base,
    SignOrVerify,
    KeyGen,
    Derive,
    Import,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
enum EcParameters {
    Base,
    SignOrVerify(EcdsaParams),
    KeyGen(EcKeyGenParams),
    Derive(EcdhKeyDeriveParams),
    Import(EcKeyImportParams),
}

impl EcParameters {
    fn from_value(
        vm: &Vm,
        realm: Gc<Realm>,
        object: Gc<Object>,
        parameter_type: EcParameterType,
    ) -> WebIdlResult<Self> {
        Ok(match parameter_type {
            EcParameterType::Base => Self::Base,
            EcParameterType::SignOrVerify => Self::SignOrVerify(EcdsaParams::from_value(vm, realm, object)?),
            EcParameterType::KeyGen => Self::KeyGen(EcKeyGenParams::from_value(vm, object)?),
            EcParameterType::Derive => Self::Derive(EcdhKeyDeriveParams::from_value(vm, object)?),
            EcParameterType::Import => Self::Import(EcKeyImportParams::from_value(vm, object)?),
        })
    }
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum EcCurve {
    P256,
    P384,
    P521,
}

impl EcCurve {
    fn from_name(named_curve: &Utf16String) -> Option<Self> {
        if *named_curve == Utf16String::from_ascii(b"P-256") {
            Some(Self::P256)
        } else if *named_curve == Utf16String::from_ascii(b"P-384") {
            Some(Self::P384)
        } else if *named_curve == Utf16String::from_ascii(b"P-521") {
            Some(Self::P521)
        } else {
            None
        }
    }
}

const SECP256R1_OID: [u32; 7] = [1, 2, 840, 10045, 3, 1, 7];
const SECP384R1_OID: [u32; 5] = [1, 3, 132, 0, 34];
const SECP521R1_OID: [u32; 5] = [1, 3, 132, 0, 35];

unsafe extern "C" {
    fn web_crypto_ec_generate_key_pair(
        curve: EcCurve,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> bool;
    fn web_crypto_ec_sign(
        curve: EcCurve,
        key: *const c_void,
        digest: Span<'_, u8>,
        error: &mut usize,
        r: *mut c_void,
        s: *mut c_void,
        receive: ReceiveBytes,
    ) -> u8;
    fn web_crypto_ec_verify(
        curve: EcCurve,
        key: *const c_void,
        digest: Span<'_, u8>,
        r: Span<'_, u8>,
        s: Span<'_, u8>,
        verified: &mut bool,
    ) -> usize;
    fn web_crypto_ec_public_key_scalar_size(key: *const c_void) -> usize;
    fn web_crypto_ec_compute_coordinate(
        curve: EcCurve,
        key: *const c_void,
        public_key: *const c_void,
        error: &mut usize,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> u8;
    fn web_crypto_ec_key_info_has_public_key_oid(structure: Asn1Structure, key_info: *const c_void) -> bool;
    fn web_crypto_ec_key_info_parameters(
        structure: Asn1Structure,
        key_info: *const c_void,
        parameters: &mut Span<'_, u32>,
    ) -> bool;
    fn web_crypto_ec_private_key_info_parameters_match(key_info: *const c_void) -> bool;
    fn web_crypto_ec_key_info_read_key(structure: Asn1Structure, key_info: *const c_void) -> *mut c_void;
    fn web_crypto_ec_key_from_coordinates(
        x: Span<'_, u8>,
        y: Span<'_, u8>,
        coord_size: usize,
        d: OptionalBytes<'_>,
    ) -> *mut c_void;
    fn web_crypto_ec_parse_raw_public_key(bytes: Span<'_, u8>) -> *mut c_void;
    fn web_crypto_ec_key_is_valid(curve: EcCurve, key: *const c_void, valid: &mut bool) -> bool;
    fn web_crypto_ec_export_der(
        curve: EcCurve,
        key: *const c_void,
        is_public: bool,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_ec_export_uncompressed(key: *const c_void, state: *mut c_void, receive: ReceiveBytes) -> bool;
    fn web_crypto_ec_export_public_coordinates(
        curve: EcCurve,
        key: *const c_void,
        x: *mut c_void,
        y: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_ec_export_private_scalar(key: *const c_void, state: *mut c_void, receive: ReceiveBytes) -> bool;
}

impl Asn1KeyInfo {
    fn has_ec_public_key_oid(&self) -> bool {
        // SAFETY: This object owns the live structure.
        unsafe { web_crypto_ec_key_info_has_public_key_oid(self.structure, self.key_info.as_ptr()) }
    }

    fn ec_parameters(&self) -> Option<&[u32]> {
        let mut parameters = Span::new(&[]);
        // SAFETY: This object owns the live structure and the parameters borrow from it.
        if unsafe { web_crypto_ec_key_info_parameters(self.structure, self.key_info.as_ptr(), &mut parameters) } {
            Some(parameters.as_slice())
        } else {
            None
        }
    }

    fn ec_private_key_parameters_match(&self) -> bool {
        assert!(matches!(self.structure, Asn1Structure::PrivateKeyInfo));
        // SAFETY: This object owns the live private key info.
        unsafe { web_crypto_ec_private_key_info_parameters_match(self.key_info.as_ptr()) }
    }

    fn ec_key(&self) -> UninstalledKey {
        // SAFETY: This object owns the live structure.
        let key = unsafe { web_crypto_ec_key_info_read_key(self.structure, self.key_info.as_ptr()) };
        UninstalledKey(NonNull::new(key).expect("LibCrypto returns an EC key"))
    }
}

fn ec_generate_key_pair(curve: EcCurve) -> Result<NativeKeyPair, ()> {
    let mut public_key = null_mut();
    let mut private_key = null_mut();
    // SAFETY: The output slots are lent for the call.
    if !unsafe { web_crypto_ec_generate_key_pair(curve, &mut public_key, &mut private_key) } {
        return Err(());
    }
    Ok(NativeKeyPair {
        public_key: UninstalledKey(NonNull::new(public_key).expect("LibCrypto returns a public key")),
        private_key: UninstalledKey(NonNull::new(private_key).expect("LibCrypto returns a private key")),
    })
}

fn ec_key_from_coordinates(vm: &Vm, x: &Bytes, y: &Bytes, coord_size: usize, d: Option<&Bytes>) -> Ref<CryptoKey> {
    // SAFETY: The input bytes are lent for the call.
    let key = unsafe {
        web_crypto_ec_key_from_coordinates(Span::new(x.as_slice()), Span::new(y.as_slice()), coord_size, d.into())
    };
    // SAFETY: The service transfers ownership of the live key.
    unsafe { CryptoKey::create_with_handle(vm, NonNull::new(key).expect("LibCrypto returns an EC key")) }
}

fn ec_parse_raw_public_key(vm: &Vm, key_bytes: &Bytes) -> WebIdlResult<Ref<CryptoKey>> {
    // SAFETY: The input bytes are lent for the call.
    let key = unsafe { web_crypto_ec_parse_raw_public_key(Span::new(key_bytes.as_slice())) };
    let Some(key) = NonNull::new(key) else {
        return Err(dom_error("DataError", b"Failed to parse key"));
    };
    // SAFETY: The service transfers ownership of the live key.
    Ok(unsafe { CryptoKey::create_with_handle(vm, key) })
}

fn ec_key_is_valid(curve: EcCurve, key: Ref<CryptoKey>) -> Result<bool, ()> {
    let mut valid = false;
    // SAFETY: The key handle is live and the output is writable.
    if !unsafe { web_crypto_ec_key_is_valid(curve, key.handle().as_ptr(), &mut valid) } {
        return Err(());
    }
    Ok(valid)
}

fn ec_export_der(curve: EcCurve, key: Ref<CryptoKey>, is_public: bool) -> Result<Bytes, ()> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The key handle and the output slot are lent for the call.
    if !unsafe {
        web_crypto_ec_export_der(
            curve,
            key.handle().as_ptr(),
            is_public,
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    } {
        return Err(());
    }
    Ok(result.expect("LibCrypto returns the DER bytes"))
}

fn ec_export_uncompressed(key: Ref<CryptoKey>) -> Result<Bytes, ()> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The key handle and the output slot are lent for the call.
    if !unsafe {
        web_crypto_ec_export_uncompressed(
            key.handle().as_ptr(),
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    } {
        return Err(());
    }
    Ok(result.expect("LibCrypto returns the encoded bytes"))
}

fn ec_export_public_coordinates(curve: EcCurve, key: Ref<CryptoKey>) -> Result<(Bytes, Bytes), ()> {
    let mut x: Option<Bytes> = None;
    let mut y: Option<Bytes> = None;
    // SAFETY: The key handle and the output slots are lent for the call.
    if !unsafe {
        web_crypto_ec_export_public_coordinates(
            curve,
            key.handle().as_ptr(),
            core::ptr::from_mut(&mut x).cast(),
            core::ptr::from_mut(&mut y).cast(),
            receive_bytes,
        )
    } {
        return Err(());
    }
    Ok((
        x.expect("LibCrypto returns an x coordinate"),
        y.expect("LibCrypto returns a y coordinate"),
    ))
}

fn ec_export_private_scalar(key: Ref<CryptoKey>) -> Result<Bytes, ()> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The key handle and the output slot are lent for the call.
    if !unsafe {
        web_crypto_ec_export_private_scalar(
            key.handle().as_ptr(),
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    } {
        return Err(());
    }
    Ok(result.expect("LibCrypto returns the private scalar"))
}

fn copy_bytes(vm: &Vm, bytes: &[u8]) -> WebIdlResult<Vec<u8>> {
    let mut copy = Vec::new();
    if copy.try_reserve_exact(bytes.len()).is_err() {
        return vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into);
    }
    copy.extend_from_slice(bytes);
    Ok(copy)
}
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Ecdsa {
    parameters: EcParameters,
}

impl Ecdsa {
    pub(crate) fn from_value(
        vm: &Vm,
        realm: Gc<Realm>,
        object: Gc<Object>,
        parameter_type: EcParameterType,
    ) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: EcParameters::from_value(vm, realm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for Ecdsa {
    // https://w3c.github.io/webcrypto/#ecdsa-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Step 2 throws a NotSupportedError when the namedCurve member of normalizedAlgorithm is not "P-256", "P-384" or
        //     "P-521".
        let EcParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC KeyGen parameters")
        };
        if EcCurve::from_name(&normalized_algorithm.named_curve).is_none() {
            return Err(dom_error(
                "NotSupportedError",
                b"Only 'P-256', 'P-384' and 'P-521' is supported",
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "sign" or "verify", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let EcParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC KeyGen parameters")
        };

        // 2. If the namedCurve member of normalizedAlgorithm is "P-256", "P-384" or "P-521":
        // Generate an Elliptic Curve key pair, as defined in [RFC6090]
        // with domain parameters for the curve identified by the namedCurve member of normalizedAlgorithm.
        // If the namedCurve member of normalizedAlgorithm is a value specified in an applicable specification:
        // Perform the ECDSA generation steps specified in that specification,
        // passing in normalizedAlgorithm and resulting in an elliptic curve key pair.
        // Otherwise: throw a NotSupportedError
        // NOTE: Spec jumps to 6 here for some reason
        // 3. If performing the key generation operation results in an error, then throw an OperationError.
        let Some(curve) = EcCurve::from_name(&normalized_algorithm.named_curve) else {
            return Err(dom_error(
                "NotSupportedError",
                b"Only 'P-256', 'P-384' and 'P-521' is supported",
            ));
        };
        let Ok(key_pair) = ec_generate_key_pair(curve) else {
            return Err(dom_error("OperationError", b"Failed to create valid crypto instance"));
        };

        // 7. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
        let public_key = key_pair.public_key.install(cx.vm());

        // 8. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 4. Let algorithm be a new EcKeyAlgorithm object.
        // 5. Set the name attribute of algorithm to "ECDSA".
        // 6. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
        // 9. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Ec {
            name: Utf16String::from_ascii(b"ECDSA"),
            named_curve: normalized_algorithm.named_curve.clone(),
        });

        // 10. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 11. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
        public_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Verify]));

        // 12. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
        let private_key = key_pair.private_key.install(cx.vm());

        // 13. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 14. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Ec {
            name: Utf16String::from_ascii(b"ECDSA"),
            named_curve: normalized_algorithm.named_curve.clone(),
        });

        // 15. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 16. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
        private_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Sign]));

        // 17. Let result be a new CryptoKeyPair dictionary.
        // 18. Set the publicKey attribute of result to be publicKey.
        // 19. Set the privateKey attribute of result to be privateKey.
        // 23. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-sign
    fn validate_sign_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Step 3 fails for the hash functions that our ECDSA does not support.
        let EcParameters::SignOrVerify(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC signature parameters")
        };
        sha_hash_kind(&normalized_algorithm.hash.name)
            .map_err(|message| Exception::Dom("NotSupportedError", message))?;

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-sign
    fn sign(&self, cx: &CallContext, _: &AlgorithmParams, key: Ref<CryptoKey>, message: &Bytes) -> WebIdlResult<Bytes> {
        let EcParameters::SignOrVerify(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC SignOrVerify parameters")
        };

        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 2. Let hashAlgorithm be the hash member of normalizedAlgorithm.
        let hash_algorithm = &normalized_algorithm.hash.name;

        // 3. Let M be the result of performing the digest operation specified by hashAlgorithm using message.
        let hash_kind =
            sha_hash_kind(hash_algorithm).map_err(|message| Exception::Dom("NotSupportedError", message))?;
        let mut digest: Option<Bytes> = None;
        // SAFETY: The input and output slot live for the call.
        let success = unsafe {
            web_crypto_sha_digest(
                hash_kind,
                Span::new(message.as_slice()),
                core::ptr::from_mut(&mut digest).cast(),
                receive_bytes,
            )
        };
        if !success {
            return cx
                .vm()
                .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .map_err(Into::into);
        }
        let m = digest.expect("LibCrypto returns the digest");

        // 4. Let d be the ECDSA private key associated with key.
        let d = key.handle();

        // FIXME: 5. Let params be the EC domain parameters associated with key.

        let internal_algorithm = key.internal_algorithm();
        let InternalAlgorithmData::Ec { named_curve, .. } = &*internal_algorithm else {
            unreachable!("the key has an EC algorithm")
        };

        let result;

        // 6. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        if let Some(curve) = EcCurve::from_name(named_curve) {
            let coord_size = match curve {
                EcCurve::P256 => 32,
                EcCurve::P384 => 48,
                EcCurve::P521 => 66,
            };

            // 1. Perform the ECDSA signing process, as specified in [RFC6090], Section 5.4.2, with M as the message,
            //    using params as the EC domain parameters, and with d as the private key.
            // 2. Let r and s be the pair of integers resulting from performing the ECDSA signing process.
            let mut error = 0;
            let mut r_bytes: Option<Bytes> = None;
            let mut s_bytes: Option<Bytes> = None;
            // SAFETY: The key handle, digest and output slots live for the call.
            let maybe_signature = unsafe {
                web_crypto_ec_sign(
                    curve,
                    d.as_ptr(),
                    Span::new(m.as_slice()),
                    &mut error,
                    core::ptr::from_mut(&mut r_bytes).cast(),
                    core::ptr::from_mut(&mut s_bytes).cast(),
                    receive_bytes,
                )
            };
            if maybe_signature == 1 {
                // SAFETY: LibCrypto transfers ownership of the error message.
                let message = unsafe { Utf16String::from_raw_owned(error) };
                return Err(Exception::Dom("OperationError", message));
            }
            if maybe_signature == 2 {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            }

            // 3. Let result be an empty byte sequence.
            let mut result_bytes = Vec::new();
            if result_bytes.try_reserve_exact(coord_size * 2).is_err() {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            }
            result_bytes.resize(coord_size * 2, 0);

            // 4. Let n be the smallest integer such that n * 8 is greater than the logarithm to base 2 of the order of the base point of the elliptic curve identified by params.
            // 5. Convert r to an octet string of length n and append this sequence of bytes to result.
            let r_bytes = r_bytes.expect("LibCrypto returns the signature component");
            assert!(r_bytes.as_slice().len() <= coord_size);
            result_bytes[..r_bytes.as_slice().len()].copy_from_slice(r_bytes.as_slice());

            // 6. Convert s to an octet string of length n and append this sequence of bytes to result.
            let s_bytes = s_bytes.expect("LibCrypto returns the signature component");
            assert!(s_bytes.as_slice().len() <= coord_size);
            result_bytes[coord_size..coord_size + s_bytes.as_slice().len()].copy_from_slice(s_bytes.as_slice());
            result = Bytes::new(result_bytes);
        } else {
            // Otherwise, the namedCurve attribute of the [[algorithm]] internal slot of key is a value specified in an applicable specification:
            // Perform the ECDSA signature steps specified in that specification, passing in M, params and d and resulting in result.
            return Err(dom_error("DataError", b"Invalid algorithm"));
        }

        // NOTE: The spec jumps to 9 here for some reason
        // 9. Return the result of creating an ArrayBuffer containing result.
        Ok(result)
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-verify
    fn validate_verify_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Step 3 fails for the hash functions that our ECDSA does not support.
        let EcParameters::SignOrVerify(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC signature parameters")
        };
        sha_hash_kind(&normalized_algorithm.hash.name)
            .map_err(|message| Exception::Dom("NotSupportedError", message))?;

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-verify
    fn verify(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        let EcParameters::SignOrVerify(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC SignOrVerify parameters")
        };

        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
        }

        // 2. Let hashAlgorithm be the hash member of normalizedAlgorithm.
        let hash_algorithm = &normalized_algorithm.hash.name;

        // 3. Let M be the result of performing the digest operation specified by hashAlgorithm using message.
        let hash_kind =
            sha_hash_kind(hash_algorithm).map_err(|message| Exception::Dom("NotSupportedError", message))?;
        let mut digest: Option<Bytes> = None;
        // SAFETY: The input and output slot live for the call.
        let success = unsafe {
            web_crypto_sha_digest(
                hash_kind,
                Span::new(message.as_slice()),
                core::ptr::from_mut(&mut digest).cast(),
                receive_bytes,
            )
        };
        if !success {
            return cx
                .vm()
                .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .map_err(Into::into);
        }
        let m = digest.expect("LibCrypto returns the digest");

        // 4. Let Q be the ECDSA public key associated with key.
        let q = key.handle();

        // FIXME: 5. Let params be the EC domain parameters associated with key.

        // 6. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        let internal_algorithm = key.internal_algorithm();
        let InternalAlgorithmData::Ec { named_curve, .. } = &*internal_algorithm else {
            unreachable!("the key has an EC algorithm")
        };

        let mut result = false;

        if let Some(curve) = EcCurve::from_name(named_curve) {
            // 1. Let n be the smallest integer such that n * 8 is greater than the logarithm to base 2 of the order of the
            //    base point of the elliptic curve identified by params.
            // SAFETY: Q is a live EC public key handle.
            let n = unsafe { web_crypto_ec_public_key_scalar_size(q.as_ptr()) };

            // 2. If signature does not have a length of n * 2 bytes, then return false.
            if signature.as_slice().len() != n * 2 {
                return Ok(false);
            }

            // 3. Let r be the result of converting the first n bytes of signature to an integer.
            let r = &signature.as_slice()[..n];

            // 4. Let s be the result of converting the last n bytes of signature to an integer.
            let s = &signature.as_slice()[n..];

            // 5. Perform the ECDSA verifying process, as specified in [RFC6090], Section 5.4.3, with M as the received
            //    message, (r, s) as the signature and using params as the EC domain parameters, and Q as the public key.
            // SAFETY: The inputs and output slot live for the call.
            let maybe_result = unsafe {
                web_crypto_ec_verify(
                    curve,
                    q.as_ptr(),
                    Span::new(m.as_slice()),
                    Span::new(r),
                    Span::new(s),
                    &mut result,
                )
            };
            if maybe_result != 0 {
                // SAFETY: LibCrypto transfers ownership of the error message.
                let message = unsafe { Utf16String::from_raw_owned(maybe_result) };
                return Err(Exception::Dom("OperationError", message));
            }
        } else {
            // Otherwise, the namedCurve attribute of the [[algorithm]] internal slot of key is a value specified in an applicable specification:
            // Perform the ECDSA verification steps specified in that specification passing in M, signature, params and Q and resulting in an indication of whether or not the purported signature is valid.
            return Err(dom_error("NotSupportedError", b"Invalid algorithm"));
        }

        // 7. Let result be a boolean with the value true if the signature is valid and the value false otherwise.
        // 8. Return result.
        Ok(result)
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-import-key
    fn validate_import_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let EcParameters::Import(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC Import parameters")
        };

        // 1. If the namedCurve member of normalizedAlgorithm is not one of "P-256", "P-384" or "P-521", and is not a value
        //    specified in an applicable specification that specifies the use of that value with ECDSA, then throw a
        //    NotSupportedError.
        if EcCurve::from_name(&normalized_algorithm.named_curve).is_none() {
            return Err(Exception::Dom(
                "NotSupportedError",
                libjs_rust::api::utf16_formatted("Unsupported named curve '{}'", &[&normalized_algorithm.named_curve]),
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the specification checks the JWK usage in its own step"
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
        let EcParameters::Import(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC Import parameters")
        };

        // NB: Step 1 is performed by validate_import_key_parameters().
        self.validate_import_key_parameters(cx.realm())?;

        // NOTE: This is a parameter to the function
        // 2. Let keyData be the key data to be imported.

        let key;

        // 3. If format is "spki":
        if key_format == KeyFormat::Spki {
            // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the id-ecPublicKey object identifier defined in [RFC5480], then throw a DataError.
            if !spki.has_ec_public_key_oid() {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is absent, then throw a DataError.
            if spki.ec_parameters().is_none() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let params be the parameters field of the algorithm AlgorithmIdentifier field of spki.
            let ec_params = spki.ec_parameters().expect("the algorithm parameters are present");

            // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
            // 8. Let namedCurve be a string whose initial value is undefined.
            let mut named_curve = Utf16String::default();

            // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
            if ec_params == SECP256R1_OID {
                // Set namedCurve to "P-256".
                named_curve = Utf16String::from_ascii(b"P-256");
            }
            // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP384R1_OID {
                // Set namedCurve to "P-384".
                named_curve = Utf16String::from_ascii(b"P-384");
            }
            // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP521R1_OID {
                // Set namedCurve to "P-521".
                named_curve = Utf16String::from_ascii(b"P-521");
            }

            // 10. If namedCurve is not undefined:
            if !named_curve.is_empty() {
                // 1. Let publicKey be the Elliptic Curve public key identified by performing
                //    the conversion steps defined in Section 2.3.4 of [SEC1] to the subjectPublicKey field of spki.
                // The uncompressed point format MUST be supported.
                let public_key = spki.ec_key();

                // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
                // 3. If a decode error occurs or an identity point is found, throw a DataError.
                // 4. Let key be a new CryptoKey that represents publicKey.
                // NOTE: We already did this in parse_a_subject_public_key_info
                key = public_key.install(cx.vm());
            } else {
                // Otherwise:
                // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
            if !named_curve.is_empty() && named_curve != normalized_algorithm.named_curve {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 12. If the public key value is not a valid point on the Elliptic Curve identified
            //     by the namedCurve member of normalizedAlgorithm throw a DataError.
            let curve = EcCurve::from_name(&named_curve).expect("the named curve is supported");
            let Ok(maybe_valid) = ec_key_is_valid(curve, key) else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 13. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 14. Let algorithm be a new EcKeyAlgorithm.
            // 15. Set the name attribute of algorithm to "ECDSA".
            // 16. Set the namedCurve attribute of algorithm to namedCurve.
            // 17. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDSA"),
                named_curve,
            });
        }
        // 3. If format is "pkcs8":
        else if key_format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Sign {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
            //    is not equal to the id-ecPublicKey object identifier defined in [RFC5480], then throw a DataError.
            if !private_key_info.has_ec_public_key_oid() {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field
            //    of privateKeyInfo is not present, then throw a DataError.
            if private_key_info.ec_parameters().is_none() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let params be the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo.
            let ec_params = private_key_info
                .ec_parameters()
                .expect("the algorithm parameters are present");

            // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
            // 8. Let namedCurve be a string whose initial value is undefined.
            let mut named_curve = Utf16String::default();

            // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
            if ec_params == SECP256R1_OID {
                // Set namedCurve to "P-256".
                named_curve = Utf16String::from_ascii(b"P-256");
            }
            // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP384R1_OID {
                // Set namedCurve to "P-384".
                named_curve = Utf16String::from_ascii(b"P-384");
            }
            // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP521R1_OID {
                // Set namedCurve to "P-521".
                named_curve = Utf16String::from_ascii(b"P-521");
            }

            // 10. If namedCurve is not undefined:
            if !named_curve.is_empty() {
                // 1. Let ecPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
                //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 ECPrivateKey
                //    structure specified in Section 3 of [RFC5915], and exactData set to true.
                // NOTE: We already did this in parse_a_private_key_info
                // 2. If an error occurred while parsing, then throw a DataError.
                let ec_private_key = private_key_info.ec_key();

                // 3. If the parameters field of ecPrivateKey is present, and is not an instance of the namedCurve ASN.1
                //    type defined in [RFC5480], or does not contain the same object identifier as the parameters field
                //    of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo, then throw a
                //    DataError.
                if !private_key_info.ec_private_key_parameters_match() {
                    return Err(dom_error("DataError", b"Invalid algorithm parameters"));
                }

                // 4. Let key be a new CryptoKey that represents the Elliptic Curve private key identified
                //    by performing the conversion steps defined in Section 3 of [RFC5915] using ecPrivateKey.
                key = ec_private_key.install(cx.vm());
            } else {
                // Otherwise:
                // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
            if !named_curve.is_empty() && named_curve != normalized_algorithm.named_curve {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 12. If the public key value is not a valid point on the Elliptic Curve identified by the namedCurve
            //    member of normalizedAlgorithm throw a DataError.
            let curve = EcCurve::from_name(&named_curve).expect("the named curve is supported");
            let Ok(maybe_valid) = ec_key_is_valid(curve, key) else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 13. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 14. Let algorithm be a new EcKeyAlgorithm.
            // 15. Set the name attribute of algorithm to "ECDSA".
            // 16. Set the namedCurve attribute of algorithm to namedCurve.
            // 17. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDSA"),
                named_curve,
            });
        }
        // 3. If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
            //    Otherwise: Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field is present and usages contains a value which is not "sign", or,
            //    if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
            if jwk.d.is_some() {
                for &usage in usages {
                    if usage != KeyUsage::Sign {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            } else {
                for &usage in usages {
                    if usage != KeyUsage::Verify {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the kty field of jwk is not "EC", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"EC")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 4. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid key use"));
            }

            // 5. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
            //    or it does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 6. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid extractable"));
            }

            // 7. Let namedCurve be a string whose value is equal to the crv field of jwk.
            // NOTE: The spec does not say what to do if crv is missing.
            if jwk.crv.is_none() {
                return Err(dom_error("DataError", b"Invalid key crv"));
            }
            let named_curve = jwk.crv.as_ref().expect("the named curve is present").clone();

            // 8. If namedCurve is not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
            if named_curve != normalized_algorithm.named_curve {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 3. If namedCurve is "P-256", "P-384" or "P-521":
            if EcCurve::from_name(&named_curve).is_some() {
                // 1. Let algNamedCurve be a string whose initial value is undefined.
                let mut alg_named_curve = Utf16String::default();

                // 2. If the alg field is not present:
                //    Let algNamedCurve be undefined.
                if jwk.alg.is_some() {
                    // If the alg field is equal to the string "ES256":
                    if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"ES256")) {
                        // Let algNamedCurve be the string "P-256".
                        alg_named_curve = Utf16String::from_ascii(b"P-256");
                    }
                    // If the alg field is equal to the string "ES384":
                    else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"ES384")) {
                        // Let algNamedCurve be the string "P-384".
                        alg_named_curve = Utf16String::from_ascii(b"P-384");
                    }
                    // If the alg field is equal to the string "ES512":
                    else if jwk.alg.as_ref() == Some(&Utf16String::from_ascii(b"ES512")) {
                        // Let algNamedCurve be the string "P-521".
                        alg_named_curve = Utf16String::from_ascii(b"P-521");
                    }
                    // otherwise:
                    else {
                        // throw a DataError.
                        return Err(dom_error("DataError", b"Invalid algorithm"));
                    }
                }

                // 3. If algNamedCurve is defined, and is not equal to namedCurve, throw a DataError.
                if !alg_named_curve.is_empty() && alg_named_curve != named_curve {
                    return Err(dom_error("DataError", b"Invalid algorithm"));
                }

                let coord_size = match EcCurve::from_name(&named_curve).expect("the named curve is supported") {
                    EcCurve::P256 => 32,
                    EcCurve::P384 => 48,
                    EcCurve::P521 => 66,
                };

                // NOTE: according to Section 6.2.1 and 6.2.2 of JSON Web Algorithms [JWA], x and y are always required
                if jwk.x.is_none() || jwk.y.is_none() {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                let maybe_x_bytes =
                    base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"));
                let Ok(x_bytes) = maybe_x_bytes else {
                    return Err(dom_error("DataError", b"Failed to decode base64"));
                };
                if x_bytes.as_slice().len() != coord_size {
                    return Err(dom_error("DataError", b"Invalid key size"));
                }

                let maybe_y_bytes =
                    base64_url_bytes_decode(cx.vm(), jwk.y.as_ref().expect("the coordinate is present"));
                let Ok(y_bytes) = maybe_y_bytes else {
                    return Err(dom_error("DataError", b"Failed to decode base64"));
                };
                if y_bytes.as_slice().len() != coord_size {
                    return Err(dom_error("DataError", b"Invalid key size"));
                }

                // If the d field is present:
                if let Some(d) = &jwk.d {
                    // 1. If jwk does not meet the requirements of Section 6.2.2 of JSON Web Algorithms [JWA], then throw a DataError.
                    let maybe_d_bytes = base64_url_bytes_decode(cx.vm(), d);
                    let Ok(d_bytes) = maybe_d_bytes else {
                        return Err(dom_error("DataError", b"Failed to decode base64"));
                    };
                    if d_bytes.as_slice().len() != coord_size {
                        return Err(dom_error("DataError", b"Invalid key size"));
                    }

                    // 2. Let key be a new CryptoKey object that represents the Elliptic Curve private key identified
                    //    by interpreting jwk according to Section 6.2.2 of JSON Web Algorithms [JWA].
                    key = ec_key_from_coordinates(cx.vm(), &x_bytes, &y_bytes, coord_size, Some(&d_bytes));

                    // 3. Set the [[type]] internal slot of Key to "private".
                    key.set_type(KeyType::Private);
                } else {
                    // Otherwise:
                    // 1. If jwk does not meet the requirements of Section 6.2.1 of JSON Web Algorithms [JWA], then throw a DataError.
                    // 2. Let key be a new CryptoKey object that represents the Elliptic Curve public key identified by interpreting
                    //    jwk according to Section 6.2.1 of JSON Web Algorithms [JWA].
                    key = ec_key_from_coordinates(cx.vm(), &x_bytes, &y_bytes, coord_size, None);

                    // 3. Set the [[type]] internal slot of Key to "public".
                    key.set_type(KeyType::Public);
                }
            } else {
                // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 10. If the key value is not a valid point on the Elliptic Curve identified
            //     by the namedCurve member of normalizedAlgorithm throw a DataError.
            let curve = EcCurve::from_name(&named_curve).expect("the named curve is supported");
            let Ok(maybe_valid) = ec_key_is_valid(curve, key) else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 11. Let algorithm be a new instance of an EcKeyAlgorithm object.
            // 12. Set the name attribute of algorithm to "ECDSA".
            // 13. Set the namedCurve attribute of algorithm to namedCurve.
            // 14. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDSA"),
                named_curve,
            });
        }
        // 3. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        else if key_format == KeyFormat::Raw || key_format == KeyFormat::RawPublic {
            // 1. If the namedCurve member of normalizedAlgorithm is not a named curve, then throw a DataError.
            if EcCurve::from_name(&normalized_algorithm.named_curve).is_none() {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 2. If usages contains an entry which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 3. If namedCurve is "P-256", "P-384" or "P-521":
            if EcCurve::from_name(&normalized_algorithm.named_curve).is_some() {
                // 1. Let Q be the Elliptic Curve public key on the curve identified by the namedCurve
                //          member of normalizedAlgorithm identified by performing the conversion steps
                //          defined in Section 2.3.4 of [SEC1] to keyData.
                // The uncompressed point format MUST be supported.
                // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
                // 3. If a decode error occurs or an identity point is found, throw a DataError.
                // 4. Let key be a new CryptoKey that represents Q.
                let ImportKeyData::Bytes(key_bytes) = key_data else {
                    unreachable!("raw key data contains bytes")
                };
                key = ec_parse_raw_public_key(cx.vm(), &key_bytes)?;
            } else {
                // Otherwise:
                // 1. Perform any key import steps defined by other applicable specifications, passing format, keyData and obtaining key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 4. Let algorithm be a new EcKeyAlgorithm object.
            // 5. Set the name attribute of algorithm to "ECDSA".
            // 6. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
            // 7. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 8. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDSA"),
                named_curve: normalized_algorithm.named_curve.clone(),
            });
        }
        // Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 4. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#ecdsa-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, _: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // NOTE: This is a parameter to the function
        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki":
        let result = if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            let data;
            // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
            //    Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
            //    Set the subjectPublicKey field to keyData
            //    If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            if let Some(curve) = EcCurve::from_name(named_curve) {
                //  Let keyData be the octet string that represents the Elliptic Curve public key represented by the [[handle]] internal slot
                //  of key according to the encoding rules specified in Section 2.3.3 of [SEC1] and using the uncompressed form.
                //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
                //      Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
                //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
                //      Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
                //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
                //      Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
                // NOTE: everything above happens in wrap_in_subject_public_key_info
                let maybe_data = ec_export_der(curve, key, true);

                if maybe_data.is_err() {
                    return Err(dom_error("DataError", b"Failed to encode public key"));
                }

                data = maybe_data.expect("encoding succeeded");
            } else {
                // Otherwise:
                // 1. Perform any key export steps defined by other applicable specifications, passing format and the
                //    namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and a new
                //    value of jwk.
                // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // NOTE: Spec does not say anything about this.
            ExportKeyResult::Bytes(data)
        }
        // 3. If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            let data;

            // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    Set the version field to 0.
            //    Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //      Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
            //      Set the privateKey field to keyData.
            //      Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
            //          If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            if let Some(curve) = EcCurve::from_name(named_curve) {
                // Let keyData be the result of DER-encoding an instance of the ECPrivateKey structure defined
                // in Section 3 of [RFC5915] for the Elliptic Curve private key represented by the [[handle]] internal slot
                // of key and that conforms to the following:
                //      The parameters field is present, and is equivalent to the parameters field of the privateKeyAlgorithm field
                //      of this PrivateKeyInfo ASN.1 structure.
                //      The publicKey field is present and represents the Elliptic Curve public key associated with the Elliptic Curve
                //      private key represented by the [[handle]] internal slot of key.
                //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
                //          Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
                //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
                //          Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
                //       If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
                //          Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
                // NOTE: everything above happens in wrap_in_private_key_info
                let maybe_data = ec_export_der(curve, key, false);

                if maybe_data.is_err() {
                    return Err(dom_error("DataError", b"Failed to encode private key"));
                }

                data = maybe_data.expect("encoding succeeded");
            } else {
                // Otherwise:
                // 1. Perform any key export steps defined by other applicable specifications, passing format and the
                //    namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and a new
                //    value of jwk.
                // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 3. Let result be the result of DER-encoding data.
            ExportKeyResult::Bytes(data)
        }
        // 3. If format is "jwt":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to "EC".
            jwk.kty = Some(Utf16String::from_ascii(b"EC"));

            // 3. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            if let Some(curve) = EcCurve::from_name(named_curve) {
                // 1. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
                if *named_curve == Utf16String::from_ascii(b"P-256") {
                    // Set the crv attribute of jwk to "P-256"
                    jwk.crv = Some(Utf16String::from_ascii(b"P-256"));
                }
                // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
                else if *named_curve == Utf16String::from_ascii(b"P-384") {
                    // Set the crv attribute of jwk to "P-384"
                    jwk.crv = Some(Utf16String::from_ascii(b"P-384"));
                }
                // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
                else if *named_curve == Utf16String::from_ascii(b"P-521") {
                    // Set the crv attribute of jwk to "P-521"
                    jwk.crv = Some(Utf16String::from_ascii(b"P-521"));
                }

                // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                let (x_bytes, y_bytes) = ec_export_public_coordinates(curve, key)
                    .map_err(|()| dom_error("DataError", b"Failed to encode public key"))?;
                jwk.x = Some(
                    base64_url_bytes_encode(x_bytes.as_slice())
                        .map_err(|_| dom_error("DataError", b"Failed to encode public key"))?,
                );
                jwk.y = Some(
                    base64_url_bytes_encode(y_bytes.as_slice())
                        .map_err(|_| dom_error("DataError", b"Failed to encode public key"))?,
                );

                // 4. If the [[type]] internal slot of key is "private"
                if key.key_type() == KeyType::Private {
                    // Set the d attribute of jwk according to the definition in Section 6.2.2.1 of JSON Web Algorithms [JWA].
                    let d_bytes = ec_export_private_scalar(key)
                        .map_err(|()| dom_error("DataError", b"Failed to encode private key"))?;
                    jwk.d = Some(
                        base64_url_bytes_encode(d_bytes.as_slice())
                            .map_err(|_| dom_error("DataError", b"Failed to encode private key"))?,
                    );
                }
            } else {
                // 1. Perform any key export steps defined by other applicable specifications,
                //    passing format and the namedCurve attribute of the [[algorithm]] internal slot
                //    of key and obtaining namedCurve and a new value of jwk.
                // 2. Set the crv attribute of jwk to namedCurve.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 4. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 5. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 6. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // 3. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        else if format == KeyFormat::Raw || format == KeyFormat::RawPublic {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            let data = if EcCurve::from_name(named_curve).is_some() {
                // Let data be an octet string representing the Elliptic Curve point Q represented by [[handle]] internal slot
                // of key according to [SEC1] 2.3.3 using the uncompressed format.
                let maybe_data = ec_export_uncompressed(key);

                if maybe_data.is_err() {
                    return Err(dom_error("DataError", b"Failed to encode public key"));
                }

                maybe_data.expect("encoding succeeded")
            } else {
                // Perform any key export steps defined by other applicable specifications, passing format and
                // the namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and data.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            };

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            ExportKeyResult::Bytes(data)
        }
        // Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        };

        // 4. Return result.
        Ok(result)
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::Verify])
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Ecdh {
    parameters: EcParameters,
}

impl Ecdh {
    pub(crate) fn from_value(
        vm: &Vm,
        realm: Gc<Realm>,
        object: Gc<Object>,
        parameter_type: EcParameterType,
    ) -> WebIdlResult<Self> {
        Ok(Self {
            parameters: EcParameters::from_value(vm, realm, object, parameter_type)?,
        })
    }
}

impl AlgorithmMethods for Ecdh {
    // https://w3c.github.io/webcrypto/#ecdh-operations-generate-key
    fn validate_generate_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        // NB: Step 2 throws a NotSupportedError when the namedCurve member of normalizedAlgorithm is not "P-256", "P-384" or
        //     "P-521".
        let EcParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC KeyGen parameters")
        };
        if EcCurve::from_name(&normalized_algorithm.named_curve).is_none() {
            return Err(dom_error(
                "NotSupportedError",
                b"Only 'P-256', 'P-384' and 'P-521' is supported",
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#ecdh-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        let EcParameters::KeyGen(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC KeyGen parameters")
        };

        // 2. If the namedCurve member of normalizedAlgorithm is "P-256", "P-384" or "P-521":
        // Generate an Elliptic Curve key pair, as defined in [RFC6090]
        // with domain parameters for the curve identified by the namedCurve member of normalizedAlgorithm.
        // If the namedCurve member of normalizedAlgorithm is a value specified in an applicable specification
        // that specifies the use of that value with ECDH:
        // Perform the ECDH generation steps specified in that specification,
        // passing in normalizedAlgorithm and resulting in an elliptic curve key pair.
        // Otherwise: throw a NotSupportedError
        // 3. If performing the operation results in an error, then throw a OperationError.
        let Some(curve) = EcCurve::from_name(&normalized_algorithm.named_curve) else {
            return Err(dom_error(
                "NotSupportedError",
                b"Only 'P-256', 'P-384' and 'P-521' is supported",
            ));
        };
        let Ok(key_pair) = ec_generate_key_pair(curve) else {
            return Err(dom_error("OperationError", b"Failed to create valid crypto instance"));
        };

        // 7. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
        let public_key = key_pair.public_key.install(cx.vm());

        // 8. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 4. Let algorithm be a new EcKeyAlgorithm object.
        // 5. Set the name attribute of algorithm to "ECDH".
        // 6. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
        // 9. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Ec {
            name: Utf16String::from_ascii(b"ECDH"),
            named_curve: normalized_algorithm.named_curve.clone(),
        });

        // 10. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 11. Set the [[usages]] internal slot of publicKey to be the empty list.
        public_key.set_usages(Vec::new());

        // 12. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
        let private_key = key_pair.private_key.install(cx.vm());

        // 13. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 14. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Ec {
            name: Utf16String::from_ascii(b"ECDH"),
            named_curve: normalized_algorithm.named_curve.clone(),
        });

        // 15. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 16. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
        private_key.set_usages(usage_intersection(
            key_usages,
            &[KeyUsage::DeriveKey, KeyUsage::DeriveBits],
        ));

        // 17. Let result be a new CryptoKeyPair dictionary.
        // 18. Set the publicKey attribute of result to be publicKey.
        // 19. Set the privateKey attribute of result to be privateKey.
        // 20. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://w3c.github.io/webcrypto/#ecdh-operations-derive-bits
    fn validate_derive_bits_parameters(
        &self,
        _: Gc<Realm>,
        algorithm: &AlgorithmParams,
        length_optional: Option<u32>,
    ) -> WebIdlResult<()> {
        let EcParameters::Derive(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC Derive parameters")
        };

        // 1. Let publicKey be the public member of normalizedAlgorithm.
        let public_key = normalized_algorithm.public_key;

        // 2. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
        if public_key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Public key is not a public key"));
        }

        // 3. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to the name member of
        //    normalizedAlgorithm, then throw an InvalidAccessError.
        if public_key.algorithm_name() != algorithm.name {
            return Err(dom_error("InvalidAccessError", b"Algorithm mismatch"));
        }

        // 4. Let maximumLength be the length in bits of the output of the field element to octet string conversion
        //    defined in Section 6.2 of [RFC6090] for the EC domain parameters associated with publicKey.
        // SAFETY: The public key has a live EC public key handle.
        let maximum_length = unsafe { web_crypto_ec_public_key_scalar_size(public_key.handle().as_ptr()) } * 8;

        // 5. If length is not null and is greater than maximumLength, then throw an OperationError.
        if length_optional.is_some_and(|length| length as usize > maximum_length) {
            return Err(dom_error("OperationError", b"Length is too large"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#ecdh-operations-derive-bits
    fn derive_bits(
        &self,
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let EcParameters::Derive(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC Derive parameters")
        };

        // NB: Steps 1 to 5 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(cx.realm(), algorithm, length_optional)?;
        let public_key = normalized_algorithm.public_key;

        // 6. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 7. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
        //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
        let internal_algorithm = key.internal_algorithm();
        let public_internal_algorithm = public_key.internal_algorithm();
        let InternalAlgorithmData::Ec { name, named_curve } = &*internal_algorithm else {
            unreachable!("the private key has an EC algorithm")
        };
        let InternalAlgorithmData::Ec {
            name: public_name,
            named_curve: public_named_curve,
        } = &*public_internal_algorithm
        else {
            unreachable!("the public key has an EC algorithm")
        };
        if name != public_name {
            return Err(dom_error("InvalidAccessError", b"Algorithm mismatch"));
        }

        // 8. If the namedCurve attribute of the [[algorithm]] internal slot of publicKey is not equal to
        //    the namedCurve property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
        if named_curve != public_named_curve {
            return Err(dom_error("InvalidAccessError", b"Curve mismatch"));
        }

        let secret;

        // 9. If the namedCurve property of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        // 10. If performing the operation results in an error, then throw a OperationError.
        if let Some(curve) = EcCurve::from_name(named_curve) {
            // 1. Perform the ECDH primitive specified in [RFC6090] Section 4
            //    with key as the EC private key d and the EC public key represented
            //    by the [[handle]] internal slot of publicKey as the EC public key.
            // 2. Let secret be a byte sequence containing the result of applying the field element to octet string
            //    conversion defined in Section 6.2 of [RFC6090] to the output of the ECDH primitive.
            // NOTE: Remove the prefix from the uncompressed point
            let mut error = 0;
            let mut secret_data: Option<Bytes> = None;
            // SAFETY: The key handles and output slots live for the call.
            let status = unsafe {
                web_crypto_ec_compute_coordinate(
                    curve,
                    key.handle().as_ptr(),
                    public_key.handle().as_ptr(),
                    &mut error,
                    core::ptr::from_mut(&mut secret_data).cast(),
                    receive_bytes,
                )
            };
            if status == 1 {
                // SAFETY: LibCrypto transfers ownership of the error message.
                let detail = unsafe { Utf16String::from_raw_owned(error) };
                return Err(Exception::Dom(
                    "OperationError",
                    libjs_rust::api::utf16_formatted("Failed to compute secret: {}", &[&detail]),
                ));
            }
            if status == 2 {
                return Err(dom_error("OperationError", b"Failed to convert secret"));
            }
            let secret_data = secret_data.expect("LibCrypto returns the uncompressed point");
            assert_eq!(secret_data.as_slice()[0], 0x04);

            // NOTE: Use the x-coordinate as shared secret. RFC6090 section 4.2:
            //   In the ECDH key exchange protocol, after the element g^(j*k) has been
            //   computed, the x-coordinate of that value can be used as the shared secret.
            secret = copy_bytes(cx.vm(), &secret_data.as_slice()[1..=secret_data.as_slice().len() / 2])?;
        } else {
            // If the namedCurve property of the [[algorithm]] internal slot of key is a value specified
            // in an applicable specification that specifies the use of that value with ECDH:
            // Perform the ECDH derivation steps specified in that specification,
            // passing in key and publicKey and resulting in secret.
            return Err(dom_error("DataError", b"Invalid algorithm"));
        }

        // 11. If length is null: Return secret.
        let Some(length) = length_optional else {
            return Ok(Bytes::new(secret));
        };

        // Otherwise: If the length of secret in bits is less than length: throw an OperationError.
        let length = length as usize;
        if secret.len() * 8 < length {
            return Err(dom_error("OperationError", b"Secret is too short"));
        }

        // Otherwise: Return an octet string containing the first length bits of secret.
        let mut slice = copy_bytes(cx.vm(), &secret[..length.div_ceil(8)])?;
        if !length.is_multiple_of(8) {
            // Zero out the last bits
            *slice.last_mut().expect("a partial byte is present") &= 0xFF << (8 - (length % 8));
        }

        Ok(Bytes::new(slice))
    }

    // https://w3c.github.io/webcrypto/#ecdh-operations-import-key
    fn validate_import_key_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let EcParameters::Import(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC Import parameters")
        };

        // 1. If the namedCurve member of normalizedAlgorithm is not one of "P-256", "P-384" or "P-521", and is not a value
        //    specified in an applicable specification that specifies the use of that value with ECDH, then throw a
        //    NotSupportedError.
        if EcCurve::from_name(&normalized_algorithm.named_curve).is_none() {
            return Err(Exception::Dom(
                "NotSupportedError",
                libjs_rust::api::utf16_formatted("Unsupported named curve '{}'", &[&normalized_algorithm.named_curve]),
            ));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#ecdh-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the specification checks the JWK usage in its own step"
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
        let EcParameters::Import(normalized_algorithm) = &self.parameters else {
            unreachable!("the operation uses EC Import parameters")
        };

        // NB: Step 1 is performed by validate_import_key_parameters().
        self.validate_import_key_parameters(cx.realm())?;

        // NOTE: This is a parameter to the function
        // 2. Let keyData be the key data to be imported.

        let key;

        // 3. If format is "spki":
        if key_format == KeyFormat::Spki {
            // 1. If usages is not empty then throw a SyntaxError.
            if !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the id-ecPublicKey object identifier defined in [RFC5480], then throw a DataError.
            if !spki.has_ec_public_key_oid() {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is absent, then throw a DataError.
            if spki.ec_parameters().is_none() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let params be the parameters field of the algorithm AlgorithmIdentifier field of spki.
            let ec_params = spki.ec_parameters().expect("the algorithm parameters are present");

            // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
            // 8. Let namedCurve be a string whose initial value is undefined.
            let mut named_curve = Utf16String::default();

            // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
            if ec_params == SECP256R1_OID {
                // Set namedCurve to "P-256".
                named_curve = Utf16String::from_ascii(b"P-256");
            }
            // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP384R1_OID {
                // Set namedCurve to "P-384".
                named_curve = Utf16String::from_ascii(b"P-384");
            }
            // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP521R1_OID {
                // Set namedCurve to "P-521".
                named_curve = Utf16String::from_ascii(b"P-521");
            }

            // 10. If namedCurve is not undefined:
            if !named_curve.is_empty() {
                // 1. Let publicKey be the Elliptic Curve public key identified by performing the conversion steps
                //    defined in Section 2.3.4 of [SEC1] to the subjectPublicKey field of spki. The uncompressed point
                //    format MUST be supported.
                // The uncompressed point format MUST be supported.
                let public_key = spki.ec_key();

                // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
                // 3. If a decode error occurs or an identity point is found, throw a DataError.
                // 4. Let key be a new CryptoKey that represents publicKey.
                // NOTE: We already did this in parse_a_subject_public_key_info
                key = public_key.install(cx.vm());
            } else {
                // Otherwise:
                // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
            if !named_curve.is_empty() && named_curve != normalized_algorithm.named_curve {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 12. If the key value is not a valid point on the Elliptic Curve identified
            //     by the namedCurve member of normalizedAlgorithm throw a DataError.
            let curve = EcCurve::from_name(&named_curve).expect("the named curve is supported");
            let Ok(maybe_valid) = ec_key_is_valid(curve, key) else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 13. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 14. Let algorithm be a new EcKeyAlgorithm.
            // 15. Set the name attribute of algorithm to "ECDH".
            // 16. Set the namedCurve attribute of algorithm to namedCurve.
            // 17. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDH"),
                named_curve,
            });
        }
        // 3. If format is "pkcs8":
        else if key_format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of
            //    privateKeyInfo is not equal to the id-ecPublicKey object identifier defined in [RFC5480], throw a
            //    DataError.
            if !private_key_info.has_ec_public_key_oid() {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of
            //    privateKeyInfo is not present, throw a DataError.
            if private_key_info.ec_parameters().is_none() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let params be the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo.
            let ec_params = private_key_info
                .ec_parameters()
                .expect("the algorithm parameters are present");

            // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
            // 8. Let namedCurve be a string whose initial value is undefined.
            let mut named_curve = Utf16String::default();

            // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
            if ec_params == SECP256R1_OID {
                // Set namedCurve to "P-256".
                named_curve = Utf16String::from_ascii(b"P-256");
            }
            // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP384R1_OID {
                // Set namedCurve to "P-384".
                named_curve = Utf16String::from_ascii(b"P-384");
            }
            // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
            else if ec_params == SECP521R1_OID {
                // Set namedCurve to "P-521".
                named_curve = Utf16String::from_ascii(b"P-521");
            }

            // 10. If namedCurve is not undefined:
            if !named_curve.is_empty() {
                // 1. Let ecPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
                //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 ECPrivateKey
                //    structure specified in Section 3 of [RFC5915], and exactData set to true.
                // NOTE: We already did this in parse_a_private_key_info
                // 2. If an error occurred while parsing, then throw a DataError.
                let ec_private_key = private_key_info.ec_key();

                // 3. If the parameters field of ecPrivateKey is present, and is not an instance
                //    of the namedCurve ASN.1 type defined in [RFC5480], or does not contain
                //    the same object identifier as the parameters field of the privateKeyAlgorithm
                //    PrivateKeyAlgorithmIdentifier field of privateKeyInfo, throw a DataError.
                if !private_key_info.ec_private_key_parameters_match() {
                    return Err(dom_error("DataError", b"Invalid algorithm parameters"));
                }

                // 4. Let key be a new CryptoKey that represents the Elliptic Curve private key identified
                //    by performing the conversion steps defined in Section 3 of [RFC5915] using ecPrivateKey.
                key = ec_private_key.install(cx.vm());
            } else {
                // Otherwise:
                // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
            if !named_curve.is_empty() && named_curve != normalized_algorithm.named_curve {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 12. If the key value is not a valid point on the Elliptic Curve identified
            //     by the namedCurve member of normalizedAlgorithm throw a DataError.
            let curve = EcCurve::from_name(&named_curve).expect("the named curve is supported");
            let Ok(maybe_valid) = ec_key_is_valid(curve, key) else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 13. Set the [[type]] internal slot of key to "private".
            key.set_type(KeyType::Private);

            // 14. Let algorithm be a new EcKeyAlgorithm.
            // 15. Set the name attribute of algorithm to "ECDH".
            // 16. Set the namedCurve attribute of algorithm to namedCurve.
            // 17. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDH"),
                named_curve,
            });
        }
        // 3. If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
            //    Otherwise: Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
            if jwk.d.is_some() && !usages.is_empty() {
                for &usage in usages {
                    if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
            if jwk.d.is_none() && !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 4. If the kty field of jwk is not "EC", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"EC")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 5. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid key use"));
            }

            // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
            //    or it does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid extractable"));
            }

            // 8. Let namedCurve be a string whose value is equal to the crv field of jwk.
            // NOTE: The spec does not say what to do if crv is missing.
            if jwk.crv.is_none() {
                return Err(dom_error("DataError", b"Invalid key crv"));
            }
            let named_curve = jwk.crv.as_ref().expect("the named curve is present").clone();

            // 9. If namedCurve is not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
            if named_curve != normalized_algorithm.named_curve {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 10. If namedCurve is "P-256", "P-384" or "P-521":
            if EcCurve::from_name(&named_curve).is_some() {
                let coord_size = match EcCurve::from_name(&named_curve).expect("the named curve is supported") {
                    EcCurve::P256 => 32,
                    EcCurve::P384 => 48,
                    EcCurve::P521 => 66,
                };

                // NOTE: according to Section 6.2.1 and 6.2.2 of JSON Web Algorithms [JWA], x and y are always required
                if jwk.x.is_none() || jwk.y.is_none() {
                    return Err(dom_error("DataError", b"Invalid key"));
                }

                let maybe_x_bytes =
                    base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"));
                let Ok(x_bytes) = maybe_x_bytes else {
                    return Err(dom_error("DataError", b"Failed to decode base64"));
                };
                if x_bytes.as_slice().len() != coord_size {
                    return Err(dom_error("DataError", b"Invalid key size"));
                }

                let maybe_y_bytes =
                    base64_url_bytes_decode(cx.vm(), jwk.y.as_ref().expect("the coordinate is present"));
                let Ok(y_bytes) = maybe_y_bytes else {
                    return Err(dom_error("DataError", b"Failed to decode base64"));
                };
                if y_bytes.as_slice().len() != coord_size {
                    return Err(dom_error("DataError", b"Invalid key size"));
                }

                // If the d field is present:
                if let Some(d) = &jwk.d {
                    // 1. If jwk does not meet the requirements of Section 6.2.2 of JSON Web Algorithms [JWA], then throw a DataError.
                    let maybe_d_bytes = base64_url_bytes_decode(cx.vm(), d);
                    let Ok(d_bytes) = maybe_d_bytes else {
                        return Err(dom_error("DataError", b"Failed to decode base64"));
                    };
                    if d_bytes.as_slice().len() != coord_size {
                        return Err(dom_error("DataError", b"Invalid key size"));
                    }

                    // 2. Let key be a new CryptoKey object that represents the Elliptic Curve private key identified
                    //    by interpreting jwk according to Section 6.2.2 of JSON Web Algorithms [JWA].
                    key = ec_key_from_coordinates(cx.vm(), &x_bytes, &y_bytes, coord_size, Some(&d_bytes));

                    // 13. Set the [[type]] internal slot of key to "private".
                    key.set_type(KeyType::Private);
                } else {
                    // Otherwise:
                    // 1. If jwk does not meet the requirements of Section 6.2.1 of JSON Web Algorithms [JWA], then throw a DataError.
                    // 2. Let key be a new CryptoKey object that represents the Elliptic Curve public key identified by interpreting
                    //    jwk according to Section 6.2.1 of JSON Web Algorithms [JWA].
                    key = ec_key_from_coordinates(cx.vm(), &x_bytes, &y_bytes, coord_size, None);

                    // 3. Set the [[type]] internal slot of Key to "public".
                    key.set_type(KeyType::Public);
                }
            } else {
                // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 11. If the key value is not a valid point on the Elliptic Curve identified
            //     by the namedCurve member of normalizedAlgorithm throw a DataError.
            let curve = EcCurve::from_name(&named_curve).expect("the named curve is supported");
            let Ok(maybe_valid) = ec_key_is_valid(curve, key) else {
                return Err(dom_error("DataError", b"Failed to verify key"));
            };
            if !maybe_valid {
                return Err(dom_error("DataError", b"Invalid key"));
            }

            // 12. Let algorithm be a new instance of an EcKeyAlgorithm object.
            // 13. Set the name attribute of algorithm to "ECDH".
            // 14. Set the namedCurve attribute of algorithm to namedCurve.
            // 15. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDH"),
                named_curve,
            });
        }
        // 3. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        else if key_format == KeyFormat::Raw || key_format == KeyFormat::RawPublic {
            // 1. If the namedCurve member of normalizedAlgorithm is not a named curve, then throw a DataError.
            if EcCurve::from_name(&normalized_algorithm.named_curve).is_none() {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 2. If usages is not the empty list, then throw a SyntaxError.
            if !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 3. If namedCurve is "P-256", "P-384" or "P-521":
            if EcCurve::from_name(&normalized_algorithm.named_curve).is_some() {
                // 1. Let Q be the Elliptic Curve public key on the curve identified by the namedCurve
                //          member of normalizedAlgorithm identified by performing the conversion steps
                //          defined in Section 2.3.4 of [SEC1] to keyData.
                // The uncompressed point format MUST be supported.
                // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
                // 3. If a decode error occurs or an identity point is found, throw a DataError.
                // 4. Let key be a new CryptoKey that represents Q.
                let ImportKeyData::Bytes(key_bytes) = key_data else {
                    unreachable!("raw key data contains bytes")
                };
                key = ec_parse_raw_public_key(cx.vm(), &key_bytes)?;
            } else {
                // Otherwise:
                // 1. Perform any key import steps defined by other applicable specifications, passing format, keyData and obtaining key.
                // 2. If an error occurred or there are no applicable specifications, throw a DataError.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 4. Let algorithm be a new EcKeyAlgorithm object.
            // 5. Set the name attribute of algorithm to "ECDH".
            // 6. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
            // 7. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 8. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Ec {
                name: Utf16String::from_ascii(b"ECDH"),
                named_curve: normalized_algorithm.named_curve.clone(),
            });
        }
        // AD-HOC: The specification leaves key unset for unsupported formats.
        else {
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 4. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#ecdh-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, _: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // NOTE: This is a parameter to the function
        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        let mut result = None;

        // 3. If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            let data;
            // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
            //    Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
            //    Set the subjectPublicKey field to keyData
            //    If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            if let Some(curve) = EcCurve::from_name(named_curve) {
                //  Let keyData be the octet string that represents the Elliptic Curve public key represented by the [[handle]] internal slot
                //  of key according to the encoding rules specified in Section 2.3.3 of [SEC1] and using the uncompressed form.
                //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
                //      Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
                //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
                //      Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
                //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
                //      Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
                // NOTE: everything above happens in wrap_in_subject_public_key_info
                let maybe_data = ec_export_der(curve, key, true);

                if maybe_data.is_err() {
                    return Err(dom_error("DataError", b"Failed to encode public key"));
                }

                data = maybe_data.expect("encoding succeeded");
            } else {
                // Otherwise:
                // 1. Perform any key export steps defined by other applicable specifications, passing format and the
                //    namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and a new
                //    value of jwk.
                // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // NOTE: Spec does not say anything about this.
            result = Some(ExportKeyResult::Bytes(data));
        }
        // 3. If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            let data;

            // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    Set the version field to 0.
            //    Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //      Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
            //      Set the privateKey field to keyData.
            //      Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
            //          If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            if let Some(curve) = EcCurve::from_name(named_curve) {
                // Let keyData be the result of DER-encoding an instance of the ECPrivateKey structure defined
                // in Section 3 of [RFC5915] for the Elliptic Curve private key represented by the [[handle]] internal slot
                // of key and that conforms to the following:
                //      The parameters field is present, and is equivalent to the parameters field of the privateKeyAlgorithm field
                //      of this PrivateKeyInfo ASN.1 structure.
                //      The publicKey field is present and represents the Elliptic Curve public key associated with the Elliptic Curve
                //      private key represented by the [[handle]] internal slot of key.
                //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
                //          Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
                //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
                //          Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
                //       If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
                //          Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
                // NOTE: everything above happens in wrap_in_private_key_info
                let maybe_data = ec_export_der(curve, key, false);

                if maybe_data.is_err() {
                    return Err(dom_error("DataError", b"Failed to encode private key"));
                }

                data = maybe_data.expect("encoding succeeded");
            } else {
                // Otherwise:
                // 1. Perform any key export steps defined by other applicable specifications, passing format and the
                //    namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and a new
                //    value of jwk.
                // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // NOTE: Spec does not say anything about this.
            result = Some(ExportKeyResult::Bytes(data));
        }
        // 3. If format is "jwt":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to "EC".
            jwk.kty = Some(Utf16String::from_ascii(b"EC"));

            // 3. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            if let Some(curve) = EcCurve::from_name(named_curve) {
                // 1. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
                if *named_curve == Utf16String::from_ascii(b"P-256") {
                    // Set the crv attribute of jwk to "P-256"
                    jwk.crv = Some(Utf16String::from_ascii(b"P-256"));
                }
                // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
                else if *named_curve == Utf16String::from_ascii(b"P-384") {
                    // Set the crv attribute of jwk to "P-384"
                    jwk.crv = Some(Utf16String::from_ascii(b"P-384"));
                }
                // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
                else if *named_curve == Utf16String::from_ascii(b"P-521") {
                    // Set the crv attribute of jwk to "P-521"
                    jwk.crv = Some(Utf16String::from_ascii(b"P-521"));
                }

                // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                let (x_bytes, y_bytes) = ec_export_public_coordinates(curve, key)
                    .map_err(|()| dom_error("DataError", b"Failed to encode public key"))?;
                jwk.x = Some(
                    base64_url_bytes_encode(x_bytes.as_slice())
                        .map_err(|_| dom_error("DataError", b"Failed to encode public key"))?,
                );
                jwk.y = Some(
                    base64_url_bytes_encode(y_bytes.as_slice())
                        .map_err(|_| dom_error("DataError", b"Failed to encode public key"))?,
                );

                // 4. If the [[type]] internal slot of key is "private"
                if key.key_type() == KeyType::Private {
                    // Set the d attribute of jwk according to the definition in Section 6.2.2.1 of JSON Web Algorithms [JWA].
                    let d_bytes = ec_export_private_scalar(key)
                        .map_err(|()| dom_error("DataError", b"Failed to encode private key"))?;
                    jwk.d = Some(
                        base64_url_bytes_encode(d_bytes.as_slice())
                            .map_err(|_| dom_error("DataError", b"Failed to encode private key"))?,
                    );
                }
            } else {
                // 1. Perform any key export steps defined by other applicable specifications,
                //    passing format and the namedCurve attribute of the [[algorithm]] internal slot
                //    of key and obtaining namedCurve and a new value of jwk.
                // 2. Set the crv attribute of jwk to namedCurve.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 4. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 5. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 6. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            result = Some(ExportKeyResult::JsonWebKey(jwk));
        }
        // 3. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        else if format == KeyFormat::Raw || format == KeyFormat::RawPublic {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
            let algorithm = key.internal_algorithm();
            let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm else {
                unreachable!("the key has an EC algorithm")
            };
            let data = if EcCurve::from_name(named_curve).is_some() {
                // Let data be the octet string that represents the Elliptic Curve public key represented by the [[handle]] internal slot
                // of key according to the encoding rules specified in Section 2.3.3 of [SEC1] and using the uncompressed form.
                let maybe_data = ec_export_uncompressed(key);

                if maybe_data.is_err() {
                    return Err(dom_error("DataError", b"Failed to encode public key"));
                }

                maybe_data.expect("encoding succeeded")
            } else {
                // Perform any key export steps defined by other applicable specifications, passing format and
                // the namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and data.
                return Err(dom_error("DataError", b"Invalid algorithm"));
            };

            // 3. Let result be the result of creating an ArrayBuffer containing data.
            result = Some(ExportKeyResult::Bytes(data));
        }

        // 4. Return result.
        Ok(result.expect("the operation has a result"))
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(Vec::new())
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct Ed448Params {
    context: Option<Bytes>,
}

impl Ed448Params {
    fn from_value(vm: &Vm, object: Gc<Object>) -> WebIdlResult<Self> {
        let mut context = None;
        let context_property = PropertyKey::from_utf8("context");
        if object.has_property(vm, &context_property)? {
            let context_value = object.get(vm, &context_property)?;
            let Some(context_source) = buffer_source_type_of(context_value) else {
                return vm
                    .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BufferSource"])
                    .map_err(Into::into);
            };
            context = Some(get_a_copy_of_the_bytes(vm, context_source)?);
        }
        Ok(Self { context })
    }
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum OkpCurve {
    Ed25519,
    Ed448,
    X25519,
    X448,
}

unsafe extern "C" {
    fn web_crypto_okp_generate_private_key(curve: OkpCurve, state: *mut c_void, receive: ReceiveBytes) -> bool;
    fn web_crypto_okp_generate_public_key(
        curve: OkpCurve,
        private_key: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_okp_compute_coordinate(
        curve: OkpCurve,
        private_key: Span<'_, u8>,
        public_key: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_okp_sign(
        curve: OkpCurve,
        private_key: Span<'_, u8>,
        message: Span<'_, u8>,
        context: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_okp_verify(
        curve: OkpCurve,
        public_key: Span<'_, u8>,
        signature: Span<'_, u8>,
        message: Span<'_, u8>,
        context: Span<'_, u8>,
        verified: &mut bool,
    ) -> usize;
    fn web_crypto_okp_key_info_has_oid(curve: OkpCurve, structure: Asn1Structure, key_info: *const c_void) -> bool;
    fn web_crypto_asn1_key_info_raw_key(structure: Asn1Structure, key_info: *const c_void, bytes: &mut Span<'_, u8>);
    fn web_crypto_asn1_octet_string_bytes(string: *const c_void, bytes: &mut Span<'_, u8>);
    fn web_crypto_okp_export_der(
        curve: OkpCurve,
        is_public: bool,
        key: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
}

impl Asn1KeyInfo {
    fn has_okp_oid(&self, curve: OkpCurve) -> bool {
        // SAFETY: This object owns the live structure.
        unsafe { web_crypto_okp_key_info_has_oid(curve, self.structure, self.key_info.as_ptr()) }
    }

    fn raw_key(&self) -> &[u8] {
        let mut bytes = Span::new(&[]);
        // SAFETY: This object owns the structure that the bytes borrow from.
        unsafe { web_crypto_asn1_key_info_raw_key(self.structure, self.key_info.as_ptr(), &mut bytes) };
        bytes.as_slice()
    }
}

fn parse_octet_string(vm: &Vm, bytes: &[u8]) -> WebIdlResult<Vec<u8>> {
    // SAFETY: The service allocates an empty native StringView.
    let string = unsafe { web_crypto_asn1_allocate_key_info(Asn1Structure::OctetString) };
    let string = Asn1KeyInfo {
        structure: Asn1Structure::OctetString,
        key_info: NonNull::new(string).expect("LibCrypto returns an ASN.1 view"),
    };
    // SAFETY: The bytes and the native StringView live for the call.
    unsafe {
        parse_an_asn1_structure(
            Span::new(bytes),
            Asn1Structure::OctetString,
            true,
            string.key_info.as_ptr(),
        )
    }
    .map_err(|message| Exception::Dom("DataError", message))?;
    let mut octets = Span::new(&[]);
    // SAFETY: The StringView borrows from the input bytes for the duration of the copy.
    unsafe { web_crypto_asn1_octet_string_bytes(string.key_info.as_ptr(), &mut octets) };
    copy_bytes(vm, octets.as_slice())
}

fn okp_generate_private_key(curve: OkpCurve) -> Result<Bytes, ()> {
    let mut key: Option<Bytes> = None;
    // SAFETY: The output slot is lent for the call.
    if !unsafe { web_crypto_okp_generate_private_key(curve, core::ptr::from_mut(&mut key).cast(), receive_bytes) } {
        return Err(());
    }
    Ok(key.expect("LibCrypto returns the private key"))
}

fn okp_generate_public_key(curve: OkpCurve, private_key: &[u8]) -> Result<Bytes, ()> {
    let mut key: Option<Bytes> = None;
    // SAFETY: The private key and output slot are lent for the call.
    if !unsafe {
        web_crypto_okp_generate_public_key(
            curve,
            Span::new(private_key),
            core::ptr::from_mut(&mut key).cast(),
            receive_bytes,
        )
    } {
        return Err(());
    }
    Ok(key.expect("LibCrypto returns the public key"))
}

fn okp_export_der(vm: &Vm, curve: OkpCurve, is_public: bool, key: &[u8]) -> WebIdlResult<Bytes> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The key and the output slot are lent for the call.
    if !unsafe {
        web_crypto_okp_export_der(
            curve,
            is_public,
            Span::new(key),
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    } {
        return vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into);
    }
    Ok(result.expect("LibCrypto returns the DER bytes"))
}

fn encode_jwk_bytes(vm: &Vm, bytes: &[u8]) -> WebIdlResult<Utf16String> {
    let Ok(encoded) = base64_url_bytes_encode(bytes) else {
        return vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into);
    };
    Ok(encoded)
}

fn out_of_memory(vm: &Vm) -> Exception {
    vm.throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
        .expect_err("creating an out-of-memory exception throws")
        .into()
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Ed25519;

impl AlgorithmMethods for Ed25519 {
    // https://w3c.github.io/webcrypto/#ed25519-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "sign" or "verify", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2. Generate an Ed25519 key pair, as defined in [RFC8032], section 5.1.5.
        let maybe_private_key = okp_generate_private_key(OkpCurve::Ed25519);
        let Ok(private_key_data) = maybe_private_key else {
            return Err(dom_error("OperationError", b"Failed to generate private key"));
        };

        let maybe_public_key = okp_generate_public_key(OkpCurve::Ed25519, private_key_data.as_slice());
        let Ok(public_key_data) = maybe_public_key else {
            return Err(dom_error("OperationError", b"Failed to generate public key"));
        };

        // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
        // and representing the public key of the generated key pair.
        let public_key = CryptoKey::from_okp_handle(cx.vm(), false, public_key_data.as_slice());

        // 6. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 3. Let algorithm be a new KeyAlgorithm object.
        // 4. Set the name attribute of algorithm to "Ed25519".
        // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"Ed25519"),
        });

        // 8. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 9. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
        public_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Verify]));

        // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
        // and representing the private key of the generated key pair.
        let private_key = CryptoKey::from_okp_handle(cx.vm(), true, private_key_data.as_slice());

        // 11. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"Ed25519"),
        });

        // 13. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
        private_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Sign]));

        // 15. Let result be a new CryptoKeyPair dictionary.
        // 16. Set the publicKey attribute of result to be publicKey.
        // 17. Set the privateKey attribute of result to be privateKey.
        // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://w3c.github.io/webcrypto/#ed25519-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the specification checks key usage in a separate step"
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
        let key;

        // 1. Let keyData be the key data to be imported.

        // 2. If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the id-Ed25519 object identifier defined in [RFC8410], then throw a DataError.
            if !spki.has_okp_oid(OkpCurve::Ed25519) {
                return Err(dom_error("DataError", b"Invalid algorithm identifier"));
            }

            // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
            if spki.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let publicKey be the Ed25519 public key identified by the subjectPublicKey field of spki.
            let public_key = spki.raw_key();

            // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
            //    and that represents publicKey.
            key = CryptoKey::from_okp_handle(cx.vm(), false, public_key);

            // 8. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 9. Let algorithm be a new KeyAlgorithm.
            // 10. Set the name attribute of algorithm to "Ed25519".
            // 11. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed25519"),
            });
        }
        // 2. If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Sign {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurs while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field
            //    of privateKeyInfo is not equal to the id-Ed25519 object identifier defined in [RFC8410], then throw a DataError.
            if !private_key_info.has_okp_oid(OkpCurve::Ed25519) {
                return Err(dom_error("DataError", b"Invalid algorithm identifier"));
            }

            // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present,
            //    then throw a DataError.
            if private_key_info.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey structure
            //    specified in Section 7 of [RFC8410], and exactData set to true.
            // 7. If an error occurred while parsing, then throw a DataError.
            let curve_private_key_bytes = parse_octet_string(cx.vm(), private_key_info.raw_key())?;

            // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
            //    and that represents the Ed25519 private key identified by curvePrivateKey.
            key = CryptoKey::from_okp_handle(cx.vm(), true, curve_private_key_bytes.as_slice());

            // 9. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 10. Let algorithm be a new KeyAlgorithm.
            // 11. Set the name attribute of algorithm to "Ed25519".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed25519"),
            });
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
            //    Otherwise: Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field is present and usages contains a value which is not "sign",
            //    or, if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
            if jwk.d.is_some() {
                for &usage in usages {
                    if usage != KeyUsage::Sign {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            } else {
                for &usage in usages {
                    if usage != KeyUsage::Verify {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the kty field of jwk is not "OKP", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 4. If the crv field of jwk is not "Ed25519", then throw a DataError.
            if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"Ed25519")) {
                return Err(dom_error("DataError", b"Invalid curve"));
            }

            // 5. If the alg field of jwk is present and is not "Ed25519" or "EdDSA", then throw a DataError.
            if jwk.alg.as_ref().is_some_and(|alg| {
                *alg != Utf16String::from_ascii(b"Ed25519") && *alg != Utf16String::from_ascii(b"EdDSA")
            }) {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid key usage"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
            //    or it does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid extractable"));
            }

            // 9. If the d field is present:
            if let Some(d) = &jwk.d {
                // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037],
                //    then throw a DataError.
                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"Ed25519")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 32 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_none() {
                    return Err(dom_error("DataError", b"Present d field"));
                }
                let private_key = base64_url_bytes_decode(cx.vm(), d)?;
                if private_key.as_slice().len() != 32 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                let derived_public_key = okp_generate_public_key(OkpCurve::Ed25519, private_key.as_slice())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                if derived_public_key.as_slice() != public_key.as_slice() {
                    return Err(dom_error("DataError", b"Invalid key pair"));
                }

                // 2. Let key be a new CryptoKey object that represents the Ed25519 private key identified by interpreting jwk according to Section 2 of [RFC8037].
                key = CryptoKey::from_okp_handle(cx.vm(), true, private_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "private".
                key.set_type(KeyType::Private);
            }
            // Otherwise:
            else {
                // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"Ed25519")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 32 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_some() {
                    return Err(dom_error("DataError", b"Present d field"));
                }

                // 2. Let key be a new CryptoKey object that represents the Ed25519 public key identified by interpreting jwk according to Section 2 of [RFC8037].
                key = CryptoKey::from_okp_handle(cx.vm(), false, public_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "public".
                key.set_type(KeyType::Public);
            }

            // 10. Let algorithm be a new instance of a KeyAlgorithm object.
            // 11. Set the name attribute of algorithm to "Ed25519".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed25519"),
            });
        }
        // 2. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        else if format == KeyFormat::Raw || format == KeyFormat::RawPublic {
            // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. If the length in bits of keyData is not 256 then throw a DataError.
            let ImportKeyData::Bytes(data) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            if data.as_slice().len() * 8 != 256 {
                return Err(dom_error("DataError", b"Invalid key length"));
            }

            // 5. Let key be a new CryptoKey representing the key data provided in keyData.
            key = CryptoKey::from_okp_handle(cx.vm(), false, data.as_slice());

            // 6. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 3. Let algorithm be a new KeyAlgorithm object.
            // 4. Set the name attribute of algorithm to "Ed25519".
            // 7. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed25519"),
            });
        }
        // 2. Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 3. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#ed25519-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the
            //    following properties:
            //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //      * Set the algorithm object identifier to the id-Ed25519 OID defined in [RFC8410].
            //    * Set the subjectPublicKey field to keyData.
            let data = okp_export_der(cx.vm(), OkpCurve::Ed25519, true, key.okp_handle_bytes())?;

            // 3. Let result be the result of DER-encoding data.
            return Ok(ExportKeyResult::Bytes(data));
        }

        // 3. If format is "pkcs8":
        if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let data be an instance of the PrivateKeyInfo ASN.1 structure defined in [RFC5208] with the following
            //    properties:
            //    * Set the version field to 0.
            //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //      * Set the algorithm object identifier to the id-Ed25519 OID defined in [RFC8410].
            //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type,
            //      as defined in Section 7 of [RFC8410], that represents the Ed25519 private key
            //      represented by the [[handle]] internal slot of key

            let data = okp_export_der(cx.vm(), OkpCurve::Ed25519, false, key.okp_handle_bytes())?;

            // 3. Let result be the result of DER-encoding data.
            return Ok(ExportKeyResult::Bytes(data));
        }

        // 2. If format is "jwk":
        if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to "OKP".
            jwk.kty = Some(Utf16String::from_ascii(b"OKP"));

            // 3. Set the alg attribute of jwk to "Ed25519".
            jwk.alg = Some(Utf16String::from_ascii(b"Ed25519"));

            // 4. Set the crv attribute of jwk to "Ed25519".
            jwk.crv = Some(Utf16String::from_ascii(b"Ed25519"));

            // 5. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
            if key.key_type() == KeyType::Public {
                jwk.x = Some(encode_jwk_bytes(cx.vm(), key.okp_handle_bytes())?);
            } else {
                // The "x" parameter of the "epk" field is set as follows:
                // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
                // and the standard base point (as u-coordinate input).
                // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
                let public_key = okp_generate_public_key(OkpCurve::Ed25519, key.okp_handle_bytes())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                jwk.x = Some(encode_jwk_bytes(cx.vm(), public_key.as_slice())?);
            }

            // 6. If the [[type]] internal slot of key is "private"
            if key.key_type() == KeyType::Private {
                // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
                jwk.d = Some(encode_jwk_bytes(cx.vm(), key.okp_handle_bytes())?);
            }

            // 7. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 9. Let result be jwk.
            return Ok(ExportKeyResult::JsonWebKey(jwk));
        }

        // 2. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        if format == KeyFormat::Raw || format == KeyFormat::RawPublic {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an octet string representing the Ed25519 public key represented by the [[handle]] internal slot of key.
            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            return Ok(ExportKeyResult::Bytes(Bytes::new(key.okp_handle_bytes().to_vec())));
        }

        // 2. Otherwise:
        //    throw a NotSupportedError.
        Err(dom_error("NotSupportedError", b"Invalid key format"))
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::Verify])
    }

    // https://w3c.github.io/webcrypto/#ed25519-operations-sign
    fn sign(&self, cx: &CallContext, _: &AlgorithmParams, key: Ref<CryptoKey>, message: &Bytes) -> WebIdlResult<Bytes> {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 2. Let result be the result of performing the Ed25519 signing process, as specified in [RFC8032], Section
        //    5.1.6, with message as M, using the Ed25519 private key associated with key.
        let private_key = key.okp_handle_bytes();

        let maybe_public_key = okp_generate_public_key(OkpCurve::Ed25519, private_key);
        if maybe_public_key.is_err() {
            return Err(dom_error("OperationError", b"Failed to generate public key"));
        }

        let mut signature: Option<Bytes> = None;
        // SAFETY: The input bytes and output slot are lent for the call.
        let maybe_signature = unsafe {
            web_crypto_okp_sign(
                OkpCurve::Ed25519,
                Span::new(key.okp_handle_bytes()),
                Span::new(message.as_slice()),
                Span::new(&[]),
                core::ptr::from_mut(&mut signature).cast(),
                receive_bytes,
            )
        };
        if !maybe_signature {
            return Err(dom_error("OperationError", b"Failed to sign message"));
        }

        // 3. Return result.
        let signature = signature.expect("LibCrypto returns the signature");
        let result = Bytes::new(copy_bytes(cx.vm(), signature.as_slice())?);
        Ok(result)
    }

    // https://w3c.github.io/webcrypto/#ed25519-operations-verify
    fn verify(
        &self,
        _: &CallContext,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
        }

        // NOTE: this is checked by ED25519::verify().
        // 2. If the key data of key represents an invalid point or a small-order element on the Elliptic Curve of Ed25519, return false.
        // 3. If the point R, encoded in the first half of signature, represents an invalid point or a small-order element on the Elliptic Curve of Ed25519, return false.

        // 4. Perform the Ed25519 verification steps, as specified in [RFC8032], Section 5.1.7,
        // using the cofactorless (unbatched) equation, [S]B = R + [k]A', on the signature,
        // with message as M, using the Ed25519 public key associated with key.

        // 5. Let result be a boolean with the value true if the signature is valid and the value false otherwise.
        let mut verified = false;
        // SAFETY: The input bytes and output slot are lent for the call.
        let maybe_verified = unsafe {
            web_crypto_okp_verify(
                OkpCurve::Ed25519,
                Span::new(key.okp_handle_bytes()),
                Span::new(signature.as_slice()),
                Span::new(message.as_slice()),
                Span::new(&[]),
                &mut verified,
            )
        };
        if maybe_verified != 0 {
            // SAFETY: LibCrypto transfers ownership of the error message.
            let error = unsafe { Utf16String::from_raw_owned(maybe_verified) };
            return Err(Exception::Dom("OperationError", error));
        }

        // 10. Return result.
        Ok(verified)
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Ed448 {
    parameters: Option<Ed448Params>,
}

impl Ed448 {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, has_parameters: bool) -> WebIdlResult<Self> {
        let parameters = if has_parameters {
            Some(Ed448Params::from_value(vm, object)?)
        } else {
            None
        };
        Ok(Self { parameters })
    }
}

impl AlgorithmMethods for Ed448 {
    // https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains a value which is not one of "sign" or "verify", then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2. Generate an Ed448 key pair, as defined in [RFC8032], section 5.1.5.
        let maybe_private_key = okp_generate_private_key(OkpCurve::Ed448);
        let Ok(private_key_data) = maybe_private_key else {
            return Err(dom_error("OperationError", b"Failed to generate private key"));
        };

        let maybe_public_key = okp_generate_public_key(OkpCurve::Ed448, private_key_data.as_slice());
        let Ok(public_key_data) = maybe_public_key else {
            return Err(dom_error("OperationError", b"Failed to generate public key"));
        };

        // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
        // and representing the public key of the generated key pair.
        let public_key = CryptoKey::from_okp_handle(cx.vm(), false, public_key_data.as_slice());

        // 6. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 3. Let algorithm be a new KeyAlgorithm object.
        // 4. Set the name attribute of algorithm to "Ed448".
        // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"Ed448"),
        });

        // 8. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 9. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
        public_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Verify]));

        // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
        // and representing the private key of the generated key pair.
        let private_key = CryptoKey::from_okp_handle(cx.vm(), true, private_key_data.as_slice());

        // 11. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"Ed448"),
        });

        // 13. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
        private_key.set_usages(usage_intersection(key_usages, &[KeyUsage::Sign]));

        // 15. Let result be a new CryptoKeyPair dictionary.
        // 16. Set the publicKey attribute of result to be publicKey.
        // 17. Set the privateKey attribute of result to be privateKey.
        // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
    #[allow(
        clippy::collapsible_if,
        reason = "the specification checks key usage in a separate step"
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
        let key;

        // 1. Let keyData be the key data to be imported.

        // 2. If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the id-Ed448 object identifier defined in [RFC8410], then throw a DataError.
            if !spki.has_okp_oid(OkpCurve::Ed448) {
                return Err(dom_error("DataError", b"Invalid algorithm identifier"));
            }

            // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
            if spki.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let publicKey be the Ed448 public key identified by the subjectPublicKey field of spki.
            let public_key = spki.raw_key();

            // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
            //    and that represents publicKey.
            key = CryptoKey::from_okp_handle(cx.vm(), false, public_key);

            // 8. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 9. Let algorithm be a new KeyAlgorithm.
            // 10. Set the name attribute of algorithm to "Ed448".
            // 11. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed448"),
            });
        }
        // 2. If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If usages contains a value which is not "sign" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Sign {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurs while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field
            //    of privateKeyInfo is not equal to the id-Ed448 object identifier defined in [RFC8410], then throw a DataError.
            if !private_key_info.has_okp_oid(OkpCurve::Ed448) {
                return Err(dom_error("DataError", b"Invalid algorithm identifier"));
            }

            // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present,
            //    then throw a DataError.
            if private_key_info.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey structure
            //    specified in Section 7 of [RFC8410], and exactData set to true.
            // 7. If an error occurred while parsing, then throw a DataError.
            let curve_private_key_bytes = parse_octet_string(cx.vm(), private_key_info.raw_key())?;

            // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
            //    and that represents the Ed448 private key identified by curvePrivateKey.
            key = CryptoKey::from_okp_handle(cx.vm(), true, curve_private_key_bytes.as_slice());

            // 9. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 10. Let algorithm be a new KeyAlgorithm.
            // 11. Set the name attribute of algorithm to "Ed448".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed448"),
            });
        }
        // 2. If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
            //    Otherwise: Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field is present and usages contains a value which is not "sign",
            //    or, if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
            if jwk.d.is_some() {
                for &usage in usages {
                    if usage != KeyUsage::Sign {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            } else {
                for &usage in usages {
                    if usage != KeyUsage::Verify {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the kty field of jwk is not "OKP", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 4. If the crv field of jwk is not "Ed448", then throw a DataError.
            if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"Ed448")) {
                return Err(dom_error("DataError", b"Invalid curve"));
            }

            // 5. If the alg field of jwk is present and is not "Ed448" or "EdDSA", then throw a DataError.
            if jwk.alg.as_ref().is_some_and(|alg| {
                *alg != Utf16String::from_ascii(b"Ed448") && *alg != Utf16String::from_ascii(b"EdDSA")
            }) {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid key usage"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
            //    or it does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid extractable"));
            }

            // 9. If the d field is present:
            if let Some(d) = &jwk.d {
                // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037],
                //    then throw a DataError.
                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"Ed448")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 57 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_none() {
                    return Err(dom_error("DataError", b"Present d field"));
                }
                let private_key = base64_url_bytes_decode(cx.vm(), d)?;
                if private_key.as_slice().len() != 57 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                let derived_public_key = okp_generate_public_key(OkpCurve::Ed448, private_key.as_slice())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                if derived_public_key.as_slice() != public_key.as_slice() {
                    return Err(dom_error("DataError", b"Invalid key pair"));
                }

                // 2. Let key be a new CryptoKey object that represents the Ed448 private key identified by interpreting jwk according to Section 2 of [RFC8037].
                key = CryptoKey::from_okp_handle(cx.vm(), true, private_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "private".
                key.set_type(KeyType::Private);
            }
            // Otherwise:
            else {
                // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"Ed448")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 57 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_some() {
                    return Err(dom_error("DataError", b"Present d field"));
                }

                // 2. Let key be a new CryptoKey object that represents the Ed448 public key identified by interpreting jwk according to Section 2 of [RFC8037].
                key = CryptoKey::from_okp_handle(cx.vm(), false, public_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "public".
                key.set_type(KeyType::Public);
            }

            // 10. Let algorithm be a new instance of a KeyAlgorithm object.
            // 11. Set the name attribute of algorithm to "Ed448".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed448"),
            });
        }
        // 2. If format is "raw":
        else if format == KeyFormat::Raw {
            // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let data be keyData.
            let ImportKeyData::Bytes(data) = key_data else {
                unreachable!("raw key data contains bytes")
            };

            // 3. If the length in bits of data is not 448 then throw a DataError.
            // AD-HOC: The spec has a typo with the size of the key length
            //         See spec comment: https://github.com/w3c/webcrypto/pull/425#discussion_r3070135408
            if data.as_slice().len() * 8 != 456 {
                return Err(dom_error("DataError", b"Invalid key length"));
            }

            // 6. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents data.
            key = CryptoKey::from_okp_handle(cx.vm(), false, data.as_slice());

            // 7. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 4. Let algorithm be a new KeyAlgorithm object.
            // 5. Set the name attribute of algorithm to "Ed448".
            // 8. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"Ed448"),
            });
        }
        // 2. Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        Ok(key)
    }

    // https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //      * Set the algorithm object identifier to the id-Ed448 OID defined in [RFC8410].
            //    * Set the subjectPublicKey field to keyData.
            let data = okp_export_der(cx.vm(), OkpCurve::Ed448, true, key.okp_handle_bytes())?;

            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            return Ok(ExportKeyResult::Bytes(data));
        }

        // 3. If format is "pkcs8":
        if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
            //    * Set the version field to 0.
            //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //      * Set the algorithm object identifier to the id-Ed448 OID defined in [RFC8410].
            //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type,
            //      as defined in Section 7 of [RFC8410], that represents the Ed448 private key
            //      represented by the [[handle]] internal slot of key

            let data = okp_export_der(cx.vm(), OkpCurve::Ed448, false, key.okp_handle_bytes())?;

            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            return Ok(ExportKeyResult::Bytes(data));
        }

        // 2. If format is "jwk":
        if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to "OKP".
            jwk.kty = Some(Utf16String::from_ascii(b"OKP"));

            // 3. Set the alg attribute of jwk to "Ed448".
            jwk.alg = Some(Utf16String::from_ascii(b"Ed448"));

            // 4. Set the crv attribute of jwk to "Ed448".
            jwk.crv = Some(Utf16String::from_ascii(b"Ed448"));

            // 5. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
            if key.key_type() == KeyType::Public {
                jwk.x = Some(encode_jwk_bytes(cx.vm(), key.okp_handle_bytes())?);
            } else {
                // The "x" parameter of the "epk" field is set as follows:
                // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
                // and the standard base point (as u-coordinate input).
                // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
                let public_key = okp_generate_public_key(OkpCurve::Ed448, key.okp_handle_bytes())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                jwk.x = Some(encode_jwk_bytes(cx.vm(), public_key.as_slice())?);
            }

            // 6. If the [[type]] internal slot of key is "private"
            if key.key_type() == KeyType::Private {
                // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
                jwk.d = Some(encode_jwk_bytes(cx.vm(), key.okp_handle_bytes())?);
            }

            // 7. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 9. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            return Ok(ExportKeyResult::JsonWebKey(jwk));
        }

        // 2. If format is "raw":
        if format == KeyFormat::Raw {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an octet string representing the Ed448 public key represented by the [[handle]] internal slot of key.
            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            return Ok(ExportKeyResult::Bytes(Bytes::new(key.okp_handle_bytes().to_vec())));
        }

        // 2. Otherwise:
        //    throw a NotSupportedError.
        Err(dom_error("NotSupportedError", b"Invalid key format"))
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::Verify])
    }

    // https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
    fn sign(&self, _: &CallContext, _: &AlgorithmParams, key: Ref<CryptoKey>, message: &Bytes) -> WebIdlResult<Bytes> {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 2. Let context be the contents of the context member of normalizedAlgorithm
        //    or the empty octet string if the context member of normalizedAlgorithm is not present.
        let algorithm = self.parameters.as_ref().expect("the operation uses Ed448 parameters");
        let context = algorithm.context.as_ref().map_or(&[][..], Bytes::as_slice);

        // 3. If context has a length greater than 255 bytes, then throw an OperationError.
        if context.len() > 255 {
            return Err(dom_error("OperationError", b"Context is too long"));
        }

        // 4. Perform the Ed448 signing process, as specified in [RFC8032], Section 5.2.6,
        //    with message as M and context as C, using the Ed448 private key associated with key.
        let mut signature: Option<Bytes> = None;
        // SAFETY: The input bytes and output slot are lent for the call.
        let maybe_signature = unsafe {
            web_crypto_okp_sign(
                OkpCurve::Ed448,
                Span::new(key.okp_handle_bytes()),
                Span::new(message.as_slice()),
                Span::new(context),
                core::ptr::from_mut(&mut signature).cast(),
                receive_bytes,
            )
        };
        if !maybe_signature {
            return Err(dom_error("OperationError", b"Failed to sign message"));
        }

        // 5. Return a new ArrayBuffer associated with the relevant global object of this [HTML],
        //    and containing the bytes of the signature resulting from performing the Ed448 signing process.
        Ok(signature.expect("LibCrypto returns the signature"))
    }

    // https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
    fn verify(
        &self,
        _: &CallContext,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
        }

        // 2. Let context be the contents of the context member of normalizedAlgorithm
        //    or the empty octet string if the context member of normalizedAlgorithm is not present.
        let algorithm = self.parameters.as_ref().expect("the operation uses Ed448 parameters");
        let context = algorithm.context.as_ref().map_or(&[][..], Bytes::as_slice);

        // 3. If context has a length greater than 255 bytes, then throw an OperationError.
        if context.len() > 255 {
            return Err(dom_error("OperationError", b"Context is too long"));
        }

        // 4. If the key data of key represents an invalid point or a small-order element
        //    on the Elliptic Curve of Ed448, return false.
        // NOTE: https://github.com/WICG/webcrypto-secure-curves/issues/27

        // 5. If the point R, encoded in the first half of signature, represents an invalid point
        //    or a small-order element on the Elliptic Curve of Ed448, return false.
        // NOTE: https://github.com/WICG/webcrypto-secure-curves/issues/27

        // 6. Perform the Ed448 verification steps, as specified in [RFC8032], Section 5.2.7, using
        //    the cofactorless (unbatched) equation, [S]B = R + [k]A', on the signature,
        //    with message as M and context as C, using the Ed448 public key associated with key.
        let mut verified = false;
        // SAFETY: The input bytes and output slot are lent for the call.
        let maybe_verified = unsafe {
            web_crypto_okp_verify(
                OkpCurve::Ed448,
                Span::new(key.okp_handle_bytes()),
                Span::new(signature.as_slice()),
                Span::new(message.as_slice()),
                Span::new(context),
                &mut verified,
            )
        };
        if maybe_verified != 0 {
            // SAFETY: LibCrypto transfers ownership of the error message.
            let error = unsafe { Utf16String::from_raw_owned(maybe_verified) };
            return Err(Exception::Dom("OperationError", error));
        }

        // 7. Let result be a boolean with the value true if the signature is valid
        //    and the value false otherwise.
        // 8. Return result.
        Ok(verified)
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct X25519 {
    parameters: Option<EcdhKeyDeriveParams>,
}

impl X25519 {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, has_parameters: bool) -> WebIdlResult<Self> {
        let parameters = if has_parameters {
            Some(EcdhKeyDeriveParams::from_value(vm, object)?)
        } else {
            None
        };
        Ok(Self { parameters })
    }
}

impl AlgorithmMethods for X25519 {
    // https://w3c.github.io/webcrypto/#x25519-operations-derive-bits
    fn validate_derive_bits_parameters(
        &self,
        _: Gc<Realm>,
        algorithm: &AlgorithmParams,
        length_optional: Option<u32>,
    ) -> WebIdlResult<()> {
        let normalized_algorithm = self
            .parameters
            .as_ref()
            .expect("the operation uses key agreement parameters");

        // 1. Let publicKey be the public member of normalizedAlgorithm.
        let public_key = normalized_algorithm.public_key;

        // 2. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
        if public_key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Public key is not a public key"));
        }

        // 3. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to the name member of
        //    normalizedAlgorithm, then throw an InvalidAccessError.
        if public_key.algorithm_name() != algorithm.name {
            return Err(dom_error("InvalidAccessError", b"Algorithm mismatch"));
        }

        // 4. If length is not null and is greater than 256, then throw an OperationError.
        if length_optional.is_some_and(|length| length > 256) {
            return Err(dom_error("OperationError", b"Length is too large"));
        }

        Ok(())
    }

    // https://w3c.github.io/webcrypto/#x25519-operations-derive-bits
    fn derive_bits(
        &self,
        cx: &CallContext,
        algorithm: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self
            .parameters
            .as_ref()
            .expect("the operation uses key agreement parameters");

        // NB: Steps 1 to 4 are performed by validate_derive_bits_parameters().
        self.validate_derive_bits_parameters(cx.realm(), algorithm, length_optional)?;
        let public_key = normalized_algorithm.public_key;

        // 5. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 6. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
        //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
        if key.algorithm_name() != public_key.algorithm_name() {
            return Err(dom_error("InvalidAccessError", b"Algorithm mismatch"));
        }

        // 7. Let secret be the result of performing the X25519 function specified in [RFC7748] Section 5 with
        //    key as the X25519 private key k and
        //    the X25519 public key represented by the [[handle]] internal slot of publicKey as the X25519 public key u.
        let private_key = key.okp_handle_bytes();
        let public_key_data = public_key.okp_handle_bytes();

        let mut secret: Option<Bytes> = None;
        // SAFETY: The input bytes and output slot are lent for the call.
        let maybe_secret = unsafe {
            web_crypto_okp_compute_coordinate(
                OkpCurve::X25519,
                Span::new(private_key),
                Span::new(public_key_data),
                core::ptr::from_mut(&mut secret).cast(),
                receive_bytes,
            )
        };
        if !maybe_secret {
            return Err(dom_error("OperationError", b"Failed to compute secret"));
        }

        let secret = secret.expect("LibCrypto returns the shared secret");

        // 8. If secret is the all-zero value, then throw a OperationError.
        //    This check must be performed in constant-time, as per [RFC7748] Section 6.1.
        // NOTE: The check may be performed by ORing all the bytes together and checking whether the result is zero,
        //       as this eliminates standard side-channels in software implementations.
        let mut or_bytes = 0;
        for &byte in secret.as_slice() {
            or_bytes |= byte;
        }

        if or_bytes == 0 {
            return Err(dom_error("OperationError", b"Secret is the all-zero value"));
        }

        // 9. If length is null: Return secret.
        let Some(length) = length_optional else {
            let result = Bytes::new(copy_bytes(cx.vm(), secret.as_slice())?);
            return Ok(result);
        };

        // Otherwise: If the length of secret in bits is less than length: throw an OperationError.
        let length = length as usize;
        if secret.as_slice().len() * 8 < length {
            return Err(dom_error("OperationError", b"Secret is too short"));
        }

        // Otherwise: Return an octet string containing the first length bits of secret.
        let mut slice = copy_bytes(cx.vm(), &secret.as_slice()[..length.div_ceil(8)])?;
        if !length.is_multiple_of(8) {
            // Zero out the last bits
            *slice.last_mut().expect("a partial byte is present") &= 0xFF << (8 - (length % 8));
        }

        Ok(Bytes::new(slice))
    }

    // https://w3c.github.io/webcrypto/#x25519-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        key_usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for &usage in key_usages {
            if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2. Generate an X25519 key pair, with the private key being 32 random bytes,
        //    and the public key being X25519(a, 9), as defined in [RFC7748], section 6.1.
        let maybe_private_key = okp_generate_private_key(OkpCurve::X25519);
        let Ok(private_key_data) = maybe_private_key else {
            return Err(dom_error("OperationError", b"Failed to generate private key"));
        };

        let maybe_public_key = okp_generate_public_key(OkpCurve::X25519, private_key_data.as_slice());
        let Ok(public_key_data) = maybe_public_key else {
            return Err(dom_error("OperationError", b"Failed to generate public key"));
        };

        // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and representing the public key of the generated key pair.
        let public_key = CryptoKey::from_okp_handle(cx.vm(), false, public_key_data.as_slice());

        // 6. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 3. Let algorithm be a new KeyAlgorithm object.
        // 4. Set the name attribute of algorithm to "X25519".
        // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"X25519"),
        });

        // 8. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 9. Set the [[usages]] internal slot of publicKey to be the empty list.
        public_key.set_usages(Vec::new());

        // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
        //     and representing the private key of the generated key pair.
        let private_key = CryptoKey::from_okp_handle(cx.vm(), true, private_key_data.as_slice());

        // 11. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"X25519"),
        });

        // 13. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
        private_key.set_usages(usage_intersection(
            key_usages,
            &[KeyUsage::DeriveKey, KeyUsage::DeriveBits],
        ));

        // 15. Let result be a new CryptoKeyPair dictionary.
        // 16. Set the publicKey attribute of result to be publicKey.
        // 17. Set the privateKey attribute of result to be privateKey.
        // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        }))
    }

    // https://w3c.github.io/webcrypto/#x25519-operations-import-key
    #[allow(
        clippy::collapsible_if,
        reason = "the specification checks key usage in a separate step"
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
        // NOTE: This is a parameter to the function
        // 1. Let keyData be the key data to be imported.

        let key;

        // 2. If format is "spki":
        if key_format == KeyFormat::Spki {
            // 1. If usages is not empty then throw a SyntaxError.
            if !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
            //    is not equal to the id-X25519 object identifier defined in [RFC8410], then throw a DataError.
            if !spki.has_okp_oid(OkpCurve::X25519) {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
            if spki.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let publicKey be the X25519 public key identified by the subjectPublicKey field of spki.
            let public_key = spki.raw_key();

            // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents publicKey.
            key = CryptoKey::from_okp_handle(cx.vm(), false, public_key);

            // 8. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 9. Let algorithm be a new KeyAlgorithm.
            // 10. Set the name attribute of algorithm to "X25519".
            // 11. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X25519"),
            });
        }
        // 2. If format is "pkcs8":
        else if key_format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
            //    is not equal to the id-X25519 object identifier defined in [RFC8410], then throw a DataError.
            if !private_key_info.has_okp_oid(OkpCurve::X25519) {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present, then throw a DataError.
            if private_key_info.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo,
            //    structure as the ASN.1 CurvePrivateKey structure specified in Section 7 of [RFC8410], and
            //    exactData set to true.
            // 7. If an error occurred while parsing, then throw a DataError.
            let curve_private_key_bytes = parse_octet_string(cx.vm(), private_key_info.raw_key())?;

            // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
            //    and that represents the X25519 private key identified by curvePrivateKey.
            key = CryptoKey::from_okp_handle(cx.vm(), true, curve_private_key_bytes.as_slice());

            // 9. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 10. Let algorithm be a new KeyAlgorithm.
            // 11. Set the name attribute of algorithm to "X25519".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X25519"),
            });
        }
        // 2. If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
            //    Otherwise: Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"keyData is not a JsonWebKey dictionary"));
            };

            // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
            if jwk.d.is_some() && !usages.is_empty() {
                for &usage in usages {
                    if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
            if jwk.d.is_none() && !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty if d is missing"));
            }

            // 4. If the kty field of jwk is not "OKP", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 5. If the crv field of jwk is not "X25519", then throw a DataError.
            if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"X25519")) {
                return Err(dom_error("DataError", b"Invalid curve"));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
            //    or it does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid extractable"));
            }

            // 9. If the d field is present:
            if let Some(d) = &jwk.d {
                // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037], then throw a DataError.
                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"X25519")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 32 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_none() {
                    return Err(dom_error("DataError", b"Missing d field"));
                }
                let private_key = base64_url_bytes_decode(cx.vm(), d)?;
                if private_key.as_slice().len() != 32 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                let derived_public_key = okp_generate_public_key(OkpCurve::X25519, private_key.as_slice())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                if derived_public_key.as_slice() != public_key.as_slice() {
                    return Err(dom_error("DataError", b"Invalid key pair"));
                }

                // 2. Let key be a new CryptoKey object that represents the X25519 private key identified by interpreting jwk according to Section 2 of [RFC8037].
                key = CryptoKey::from_okp_handle(cx.vm(), true, private_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "private".
                key.set_type(KeyType::Private);
            }
            // 9. Otherwise:
            else {
                // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"X25519")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 32 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_some() {
                    return Err(dom_error("DataError", b"Present d field"));
                }

                key = CryptoKey::from_okp_handle(cx.vm(), false, public_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "public".
                key.set_type(KeyType::Public);
            }

            // 10. Let algorithm be a new instance of a KeyAlgorithm object.
            // 11. Set the name attribute of algorithm to "X25519".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X25519"),
            });
        }
        // 2. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        else if key_format == KeyFormat::Raw || key_format == KeyFormat::RawPublic {
            // 1. If usages is not empty then throw a SyntaxError.
            if !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 2. If the length in bits of keyData is not 256 then throw a DataError.
            let ImportKeyData::Bytes(data) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            if data.as_slice().len() * 8 != 256 {
                return Err(dom_error("DataError", b"Invalid key length"));
            }

            // 5. Let key be a new CryptoKey representing the key data provided in keyData.
            key = CryptoKey::from_okp_handle(cx.vm(), false, data.as_slice());

            // 6. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 3. Let algorithm be a new KeyAlgorithm object.
            // 4. Set the name attribute of algorithm to "X25519".
            // 7. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X25519"),
            });
        }
        // 2. Otherwise: throw a NotSupportedError.
        else {
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 3. Return key.
        Ok(key)
    }

    // https://w3c.github.io/webcrypto/#x25519-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // NOTE: This is a parameter to the function
        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki":
        let result = if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //    Set the algorithm object identifier to the id-X25519 OID defined in [RFC8410].
            //    Set the subjectPublicKey field to keyData.
            let data = okp_export_der(cx.vm(), OkpCurve::X25519, true, key.okp_handle_bytes())?;

            // 3. Let result be the result of DER-encoding data.
            ExportKeyResult::Bytes(data)
        }
        // 3. If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
            //    Set the version field to 0.
            //    Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //    Set the algorithm object identifier to the id-X25519 OID defined in [RFC8410].
            //    Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type, as defined in Section 7 of [RFC8410],
            //    that represents the X25519 private key represented by the [[handle]] internal slot of key

            let data = okp_export_der(cx.vm(), OkpCurve::X25519, false, key.okp_handle_bytes())?;

            // 3. Let result be the result of DER-encoding data.
            ExportKeyResult::Bytes(data)
        }
        // 3. If format is "jwt":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to "OKP".
            jwk.kty = Some(Utf16String::from_ascii(b"OKP"));

            // 3. Set the crv attribute of jwk to "X25519".
            jwk.crv = Some(Utf16String::from_ascii(b"X25519"));

            // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
            if key.key_type() == KeyType::Public {
                let public_key = key.okp_handle_bytes();
                jwk.x = Some(encode_jwk_bytes(cx.vm(), public_key)?);
            } else {
                // The "x" parameter of the "epk" field is set as follows:
                // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
                // and the standard base point (as u-coordinate input).
                // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
                let public_key = okp_generate_public_key(OkpCurve::X25519, key.okp_handle_bytes())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                jwk.x = Some(encode_jwk_bytes(cx.vm(), public_key.as_slice())?);
            }

            // 5. If the [[type]] internal slot of key is "private"
            if key.key_type() == KeyType::Private {
                // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
                let private_key = key.okp_handle_bytes();
                jwk.d = Some(encode_jwk_bytes(cx.vm(), private_key)?);
            }

            // 6. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            ExportKeyResult::JsonWebKey(jwk)
        }
        // 3. If format is "raw":
        // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
        // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
        else if format == KeyFormat::Raw || format == KeyFormat::RawPublic {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an octet string representing the X25519 public key represented by the [[handle]] internal slot of key.
            let public_key = key.okp_handle_bytes();

            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            ExportKeyResult::Bytes(Bytes::new(public_key.to_vec()))
        }
        // 3. Otherwise:
        else {
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        };

        // 4. Return result.
        Ok(result)
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(Vec::new())
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct X448 {
    parameters: Option<EcdhKeyDeriveParams>,
}

impl X448 {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, has_parameters: bool) -> WebIdlResult<Self> {
        let parameters = if has_parameters {
            Some(EcdhKeyDeriveParams::from_value(vm, object)?)
        } else {
            None
        };
        Ok(Self { parameters })
    }
}

impl AlgorithmMethods for X448 {
    // https://wicg.github.io/webcrypto-secure-curves/#x448-operations
    fn derive_bits(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 2. Let publicKey be the public member of normalizedAlgorithm.
        let public_key = self
            .parameters
            .as_ref()
            .expect("the operation uses key agreement parameters")
            .public_key;

        // 3. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
        if public_key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Public key is not a public key"));
        }

        // 4. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
        //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
        if key.algorithm_name() != public_key.algorithm_name() {
            return Err(dom_error("InvalidAccessError", b"Algorithm mismatch"));
        }

        // 5. Let secret be the result of performing the X448 function specified in [RFC7748] Section 5
        //    with key as the X448 private key k and the X448 public key represented by the [[handle]]
        //    internal slot of publicKey as the X448 public key u.
        let private_key = key.okp_handle_bytes();
        let public_key_data = public_key.okp_handle_bytes();

        let mut secret: Option<Bytes> = None;
        // SAFETY: The input bytes and output slot are lent for the call.
        let maybe_secret = unsafe {
            web_crypto_okp_compute_coordinate(
                OkpCurve::X448,
                Span::new(private_key),
                Span::new(public_key_data),
                core::ptr::from_mut(&mut secret).cast(),
                receive_bytes,
            )
        };
        if !maybe_secret {
            return Err(dom_error("OperationError", b"Failed to compute secret"));
        }

        let secret = secret.expect("LibCrypto returns the shared secret");

        // 6. If secret is the all-zero value, then throw a OperationError. This check must be performed in constant-time, as per [RFC7748] Section 6.2.
        // NOTE: The check may be performed by ORing all the bytes together and checking whether the result is zero,
        //       as this eliminates standard side-channels in software implementations.
        let mut or_bytes = 0;
        for &byte in secret.as_slice() {
            or_bytes |= byte;
        }

        if or_bytes == 0 {
            return Err(dom_error("OperationError", b"Secret is the all-zero value"));
        }

        // 7. If length is null: Return secret
        let Some(length) = length_optional else {
            let result = Bytes::new(copy_bytes(cx.vm(), secret.as_slice())?);
            return Ok(result);
        };

        // Otherwise: Return an octet string containing the first length bits of secret.
        let length = length as usize;
        if secret.as_slice().len() * 8 < length {
            return Err(dom_error("OperationError", b"Secret is too short"));
        }

        let mut slice = copy_bytes(cx.vm(), &secret.as_slice()[..length.div_ceil(8)])?;
        if !length.is_multiple_of(8) {
            // Zero out the last bits
            *slice.last_mut().expect("a partial byte is present") &= 0xFF << (8 - (length % 8));
        }

        Ok(Bytes::new(slice))
    }

    // https://wicg.github.io/webcrypto-secure-curves/#x448-operations
    fn generate_key(
        &self,
        cx: &CallContext,
        _: &AlgorithmParams,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2. Generate an X448 key pair, with the private key being 56 random bytes, and the public key being X448(a, 5), as defined in [RFC7748], section 6.2.
        let maybe_private_key = okp_generate_private_key(OkpCurve::X448);
        let Ok(private_key_data) = maybe_private_key else {
            return Err(dom_error("OperationError", b"Failed to generate private key"));
        };
        let maybe_public_key = okp_generate_public_key(OkpCurve::X448, private_key_data.as_slice());
        let Ok(public_key_data) = maybe_public_key else {
            return Err(dom_error("OperationError", b"Failed to generate public key"));
        };

        // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML], and representing the public key of the generated key pair.
        let public_key = CryptoKey::from_okp_handle(cx.vm(), false, public_key_data.as_slice());

        // 6. Set the [[type]] internal slot of publicKey to "public"
        public_key.set_type(KeyType::Public);

        // 3. Let algorithm be a new KeyAlgorithm object.
        // 4. Set the name attribute of algorithm to "X448".
        // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"X448"),
        });

        // 8. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 9. Set the [[usages]] internal slot of publicKey to be the empty list.
        public_key.set_usages(Vec::new());

        // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML], and representing the private key of the generated key pair.
        let private_key = CryptoKey::from_okp_handle(cx.vm(), true, private_key_data.as_slice());

        // 11. Set the [[type]] internal slot of privateKey to "private"
        private_key.set_type(KeyType::Private);

        // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Key {
            name: Utf16String::from_ascii(b"X448"),
        });

        // 13. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
        private_key.set_usages(usage_intersection(usages, &[KeyUsage::DeriveKey, KeyUsage::DeriveBits]));

        // 15. Let result be a new CryptoKeyPair dictionary.
        // 16. Set the publicKey attribute of result to be publicKey.
        // 17. Set the privateKey attribute of result to be privateKey.
        let result = CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        };

        // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(result))
    }

    // https://wicg.github.io/webcrypto-secure-curves/#x448-operations
    #[allow(
        clippy::field_reassign_with_default,
        reason = "the algorithm sets JWK members in separate steps"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible

        // 3. If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //      * Set the algorithm object identifier to the id-X448 OID defined in [RFC8410].
            //    * Set the subjectPublicKey field to keyData.
            let data = okp_export_der(cx.vm(), OkpCurve::X448, true, key.okp_handle_bytes())?;

            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            return Ok(ExportKeyResult::Bytes(data));
        }

        // 3. If format is "pkcs8":
        if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
            //    * Set the version field to 0.
            //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //      * Set the algorithm object identifier to the id-X448 OID defined in [RFC8410].
            //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type, as defined in Section 7 of [RFC8410], that represents the X448 private key represented by the [[handle]] internal slot of key

            let data = okp_export_der(cx.vm(), OkpCurve::X448, false, key.okp_handle_bytes())?;

            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            return Ok(ExportKeyResult::Bytes(data));
        }

        // 3. If format is "jwk":
        if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Set the kty attribute of jwk to "OKP".
            jwk.kty = Some(Utf16String::from_ascii(b"OKP"));

            // 3. Set the crv attribute of jwk to "X448".
            jwk.crv = Some(Utf16String::from_ascii(b"X448"));

            // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
            if key.key_type() == KeyType::Public {
                jwk.x = Some(encode_jwk_bytes(cx.vm(), key.okp_handle_bytes())?);
            } else {
                let public_key = okp_generate_public_key(OkpCurve::X448, key.okp_handle_bytes())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                jwk.x = Some(encode_jwk_bytes(cx.vm(), public_key.as_slice())?);
            }

            // 5. If the [[type]] internal slot of key is "private"
            if key.key_type() == KeyType::Private {
                // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
                jwk.d = Some(encode_jwk_bytes(cx.vm(), key.okp_handle_bytes())?);
            }

            // 6. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|usage| Utf16String::from_ascii(KEY_USAGES[*usage as usize]))
                    .collect(),
            );

            // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
            return Ok(ExportKeyResult::JsonWebKey(jwk));
        }

        // 3. If format is "raw":
        if format == KeyFormat::Raw {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be an octet string representing the X448 public key represented by the [[handle]] internal slot of key.
            // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
            return Ok(ExportKeyResult::Bytes(Bytes::new(key.okp_handle_bytes().to_vec())));
        }

        // 3. Otherwise:
        //    throw a NotSupportedError.
        Err(dom_error("NotSupportedError", b"Invalid key format"))
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(Vec::new())
    }

    // https://wicg.github.io/webcrypto-secure-curves/#x448-operations
    #[allow(
        clippy::collapsible_if,
        reason = "the specification checks key usage in a separate step"
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

        // 2. If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If usages is not empty then throw a SyntaxError.
            if !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki is not equal to the id-X448 object identifier defined in [RFC8410], then throw a DataError.
            if !spki.has_okp_oid(OkpCurve::X448) {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
            if spki.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let publicKey be the X448 public key identified by the subjectPublicKey field of spki.
            let public_key = spki.raw_key();

            // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents publicKey.
            let key = CryptoKey::from_okp_handle(cx.vm(), false, public_key);

            // 8. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 9. Let algorithm be a new KeyAlgorithm.
            // 10. Set the name attribute of algorithm to "X448".
            // 11. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X448"),
            });

            return Ok(key);
        }

        // 3. If format is "pkcs8":
        if format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo is not equal to the id-X448 object identifier defined in [RFC8410], then throw a DataError.
            if !private_key_info.has_okp_oid(OkpCurve::X448) {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present, then throw a DataError.
            if private_key_info.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey
            //    structure specified in Section 7 of [RFC8410], and exactData set to true.
            // 7. If an error occurred while parsing, then throw a DataError.
            let curve_private_key_bytes = parse_octet_string(cx.vm(), private_key_info.raw_key())?;

            // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents the X448 private key identified by curvePrivateKey.
            let key = CryptoKey::from_okp_handle(cx.vm(), true, curve_private_key_bytes.as_slice());

            // 9. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 10. Let algorithm be a new KeyAlgorithm.
            // 11. Set the name attribute of algorithm to "X448".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X448"),
            });

            return Ok(key);
        }

        // 3. If format is "jwk":
        if format == KeyFormat::Jwk {
            // 1. If keyData is a JsonWebKey dictionary:
            //    Let jwk equal keyData.
            //    Otherwise:
            //    Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"Data is not a JsonWebKey dictionary"));
            };

            // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
            if jwk.d.is_some() {
                for &usage in usages {
                    if usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits {
                        return Err(Exception::Dom(
                            "SyntaxError",
                            libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                        ));
                    }
                }
            }

            // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
            if jwk.d.is_none() && !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 4. If the kty field of jwk is not "OKP", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 5. If the crv field of jwk is not "X448", then throw a DataError.
            if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"X448")) {
                return Err(dom_error("DataError", b"Invalid curve"));
            }

            // 6. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|key_use| *key_use != Utf16String::from_ascii(b"enc"))
            {
                return Err(dom_error("DataError", b"Invalid use"));
            }

            // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
            //    or it does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid extractable"));
            }

            let key;

            // 9. If the d field is present:
            if let Some(d) = &jwk.d {
                // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037], then throw a DataError.

                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"X448")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 56 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_none() {
                    return Err(dom_error("DataError", b"Missing d field"));
                }
                let private_key = base64_url_bytes_decode(cx.vm(), d)?;
                if private_key.as_slice().len() != 56 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                let derived_public_key = okp_generate_public_key(OkpCurve::X448, private_key.as_slice())
                    .map_err(|()| out_of_memory(cx.vm()))?;
                if derived_public_key.as_slice() != public_key.as_slice() {
                    return Err(dom_error("DataError", b"Invalid key pair"));
                }

                // 2. Let key be a new CryptoKey object that represents the X448 private key identified by interpreting jwk according to Section 2 of [RFC8037].
                key = CryptoKey::from_okp_handle(cx.vm(), true, private_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "private".
                key.set_type(KeyType::Private);
            }
            // Otherwise:
            else {
                // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
                // o  The parameter "kty" MUST be "OKP".
                if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"OKP")) {
                    return Err(dom_error("DataError", b"Invalid key type"));
                }

                // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
                // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
                if jwk.crv.as_ref() != Some(&Utf16String::from_ascii(b"X448")) {
                    return Err(dom_error("DataError", b"Invalid curve"));
                }

                // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
                if jwk.x.is_none() {
                    return Err(dom_error("DataError", b"Missing x field"));
                }
                let public_key = base64_url_bytes_decode(cx.vm(), jwk.x.as_ref().expect("the coordinate is present"))?;
                if public_key.as_slice().len() != 56 {
                    return Err(dom_error("DataError", b"Invalid key length"));
                }

                // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
                //    This parameter MUST NOT be present for public keys.
                if jwk.d.is_some() {
                    return Err(dom_error("DataError", b"Present d field"));
                }

                // 2. Let key be a new CryptoKey object that represents the X448 public key identified by interpreting jwk according to Section 2 of [RFC8037].
                key = CryptoKey::from_okp_handle(cx.vm(), false, public_key.as_slice());

                // 3. Set the [[type]] internal slot of Key to "public".
                key.set_type(KeyType::Public);
            }

            // 10. Let algorithm be a new instance of a KeyAlgorithm object.
            // 11. Set the name attribute of algorithm to "X448".
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X448"),
            });

            return Ok(key);
        }

        // 3. If format is "raw":
        if format == KeyFormat::Raw {
            // 1. If usages is not empty then throw a SyntaxError.
            if !usages.is_empty() {
                return Err(dom_error("SyntaxError", b"Usages must be empty"));
            }

            // 2.Let data be keyData.
            let ImportKeyData::Bytes(data) = key_data else {
                unreachable!("raw key data contains bytes")
            };

            // 3. If the length in bits of data is not 448 then throw a DataError.
            if data.as_slice().len() * 8 != 448 {
                return Err(dom_error("DataError", b"Invalid key length"));
            }

            // 6. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents data.
            let key = CryptoKey::from_okp_handle(cx.vm(), false, data.as_slice());

            // 7. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 4. Let algorithm be a new KeyAlgorithm object.
            // 5. Set the name attribute of algorithm to "X448".
            // 8. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: Utf16String::from_ascii(b"X448"),
            });

            return Ok(key);
        }

        let _ = extractable;

        // 3. Otherwise:
        //    throw a NotSupportedError.
        Err(dom_error("NotSupportedError", b"Invalid key format"))
    }
}

type ContextParams = Ed448Params;

#[derive(Clone, Copy)]
#[repr(i32)]
enum MlDsaSize {
    MlDsa44,
    MlDsa65,
    MlDsa87,
}

const ML_DSA_44_OID: [u32; 9] = [2, 16, 840, 1, 101, 3, 4, 3, 17];
const ML_DSA_65_OID: [u32; 9] = [2, 16, 840, 1, 101, 3, 4, 3, 18];
const ML_DSA_87_OID: [u32; 9] = [2, 16, 840, 1, 101, 3, 4, 3, 19];

unsafe extern "C" {
    fn web_crypto_mldsa_sign(
        size: MlDsaSize,
        key: *const c_void,
        message: Span<'_, u8>,
        context: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_mldsa_verify(
        size: MlDsaSize,
        key: *const c_void,
        message: Span<'_, u8>,
        signature: Span<'_, u8>,
        context: Span<'_, u8>,
        verified: &mut bool,
    ) -> bool;
    fn web_crypto_mldsa_generate_key_pair(
        size: MlDsaSize,
        seed: Span<'_, u8>,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> usize;
    fn web_crypto_asn1_key_info_identifier(structure: Asn1Structure, key_info: *const c_void, oid: &mut Span<'_, u32>);
    fn web_crypto_mldsa_key_info_read_private_key(key_info: *const c_void) -> *mut c_void;
    fn web_crypto_mldsa_read_public_key(bytes: Span<'_, u8>) -> *mut c_void;
    fn web_crypto_mldsa_public_bytes(key: *const c_void, bytes: &mut Span<'_, u8>);
    fn web_crypto_mldsa_seed_bytes(key: *const c_void, bytes: &mut Span<'_, u8>);
    fn web_crypto_mldsa_export_der(
        key: *const c_void,
        is_public: bool,
        oid: Span<'_, u32>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
}

impl Asn1KeyInfo {
    fn identifier(&self) -> &[u32] {
        let mut oid = Span::new(&[]);
        // SAFETY: This object owns the structure that the identifier borrows from.
        unsafe { web_crypto_asn1_key_info_identifier(self.structure, self.key_info.as_ptr(), &mut oid) };
        oid.as_slice()
    }

    fn mldsa_private_key(&self) -> UninstalledKey {
        assert!(matches!(self.structure, Asn1Structure::PrivateKeyInfo));
        // SAFETY: This object owns a live PrivateKeyInfo.
        let key = unsafe { web_crypto_mldsa_key_info_read_private_key(self.key_info.as_ptr()) };
        UninstalledKey(NonNull::new(key).expect("LibCrypto returns an ML-DSA private key"))
    }
}

fn mldsa_read_public_key(vm: &Vm, bytes: &[u8]) -> Ref<CryptoKey> {
    // SAFETY: The input bytes are lent for the call.
    let key = unsafe { web_crypto_mldsa_read_public_key(Span::new(bytes)) };
    // SAFETY: The service transfers ownership of the live key.
    unsafe { CryptoKey::create_with_handle(vm, NonNull::new(key).expect("LibCrypto returns an ML-DSA public key")) }
}

fn mldsa_generate_key_pair(size: MlDsaSize, seed: &[u8]) -> Result<NativeKeyPair, Utf16String> {
    let mut public_key = null_mut();
    let mut private_key = null_mut();
    // SAFETY: The seed and output slots are lent for the call.
    let error = unsafe { web_crypto_mldsa_generate_key_pair(size, Span::new(seed), &mut public_key, &mut private_key) };
    if error != 0 {
        // SAFETY: LibCrypto transfers ownership of the error message.
        return Err(unsafe { Utf16String::from_raw_owned(error) });
    }
    Ok(NativeKeyPair {
        public_key: UninstalledKey(NonNull::new(public_key).expect("LibCrypto returns a public key")),
        private_key: UninstalledKey(NonNull::new(private_key).expect("LibCrypto returns a private key")),
    })
}

fn generate_mldsa_key_pair_from_seed(name: &Utf16String, seed: &Bytes) -> Result<NativeKeyPair, Utf16String> {
    if matches!(name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
        return mldsa_generate_key_pair(MlDsaSize::MlDsa44, seed.as_slice());
    }
    if matches!(name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
        return mldsa_generate_key_pair(MlDsaSize::MlDsa65, seed.as_slice());
    }
    if matches!(name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
        return mldsa_generate_key_pair(MlDsaSize::MlDsa87, seed.as_slice());
    }
    unreachable!("the name identifies an ML-DSA parameter set")
}

fn mldsa_export_der(vm: &Vm, key: Ref<CryptoKey>, is_public: bool, oid: &[u32]) -> WebIdlResult<Bytes> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The key handle, OID and output slot are lent for the call.
    if !unsafe {
        web_crypto_mldsa_export_der(
            key.handle().as_ptr(),
            is_public,
            Span::new(oid),
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    } {
        return Err(out_of_memory(vm));
    }
    Ok(result.expect("LibCrypto returns the DER bytes"))
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MlDsa {
    parameters: Option<ContextParams>,
}

impl MlDsa {
    pub(crate) fn from_value(vm: &Vm, object: Gc<Object>, has_parameters: bool) -> WebIdlResult<Self> {
        let parameters = if has_parameters {
            Some(ContextParams::from_value(vm, object)?)
        } else {
            None
        };
        Ok(Self { parameters })
    }
}

impl AlgorithmMethods for MlDsa {
    // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-sign
    fn validate_sign_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses ContextParams");

        // 1. If the context member of normalizedAlgorithm is present and has a length greater than 255 bytes, then throw
        //    an OperationError.
        if normalized_algorithm
            .context
            .as_ref()
            .is_some_and(|context| context.as_slice().len() > 255)
        {
            return Err(dom_error("OperationError", b"Context is too long"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-sign
    fn sign(
        &self,
        cx: &CallContext,
        params: &AlgorithmParams,
        key: Ref<CryptoKey>,
        message: &Bytes,
    ) -> WebIdlResult<Bytes> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses ContextParams");

        // NB: Step 1 is performed by validate_sign_parameters().
        self.validate_sign_parameters(cx.realm())?;

        // 2. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
        }

        // 3. Let context be the context member of normalizedAlgorithm or the empty octet string if the
        //    context member of normalizedAlgorithm is not present.
        let context = normalized_algorithm.context.as_ref().map_or(&[][..], Bytes::as_slice);

        // 4. Let result be the result of performing the ML-DSA.Sign signing algorithm, as specified in
        //    Section 5.2 of [FIPS-204], with the parameter set indicated by the name member of normalizedAlgorithm,
        //    using the ML-DSA private key associated with key as sk, message as M and context as ctx.
        let size = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
            MlDsaSize::MlDsa44
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
            MlDsaSize::MlDsa65
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
            MlDsaSize::MlDsa87
        } else {
            unreachable!("the name identifies an ML-DSA parameter set")
        };
        let mut signature: Option<Bytes> = None;
        // SAFETY: The key, input bytes and output slot are lent for the call.
        let maybe_result = unsafe {
            web_crypto_mldsa_sign(
                size,
                key.handle().as_ptr(),
                Span::new(message.as_slice()),
                Span::new(context),
                core::ptr::from_mut(&mut signature).cast(),
                receive_bytes,
            )
        };

        // 5. If the ML-DSA.Sign algorithm returned an error, throw an OperationError.
        if !maybe_result {
            return Err(dom_error("OperationError", b"Failed to sign message"));
        }

        // 6. Return result.
        Ok(signature.expect("LibCrypto returns a signature"))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-verify
    fn validate_verify_parameters(&self, _: Gc<Realm>) -> WebIdlResult<()> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses ContextParams");

        // 1. If the context member of normalizedAlgorithm is present and has a length greater than 255 bytes, then throw
        //    an OperationError.
        if normalized_algorithm
            .context
            .as_ref()
            .is_some_and(|context| context.as_slice().len() > 255)
        {
            return Err(dom_error("OperationError", b"Context is too long"));
        }

        Ok(())
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-verify
    fn verify(
        &self,
        cx: &CallContext,
        params: &AlgorithmParams,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        message: &Bytes,
    ) -> WebIdlResult<bool> {
        let normalized_algorithm = self.parameters.as_ref().expect("the operation uses ContextParams");

        // NB: Step 1 is performed by validate_verify_parameters().
        self.validate_verify_parameters(cx.realm())?;

        // 2. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
        }

        // 3. Let context be the context member of normalizedAlgorithm or the empty octet string if the
        //    context member of normalizedAlgorithm is not present.
        let context = normalized_algorithm.context.as_ref().map_or(&[][..], Bytes::as_slice);

        // 4. Let result be the result of performing the ML-DSA.Verify verification algorithm, as specified in
        //    Section 5.3 of [FIPS-204], with the parameter set indicated by the name member of normalizedAlgorithm,
        //    using the ML-DSA public key associated with key as pk, message as M, signature as σ and context as ctx.
        let size = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
            MlDsaSize::MlDsa44
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
            MlDsaSize::MlDsa65
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
            MlDsaSize::MlDsa87
        } else {
            unreachable!("the name identifies an ML-DSA parameter set")
        };
        let mut verified = false;
        // SAFETY: The key, input bytes and output slot are lent for the call.
        let maybe_result = unsafe {
            web_crypto_mldsa_verify(
                size,
                key.handle().as_ptr(),
                Span::new(message.as_slice()),
                Span::new(signature.as_slice()),
                Span::new(context),
                &mut verified,
            )
        };

        // 5. If the ML-DSA.Verify algorithm returned an error, throw an OperationError.
        if !maybe_result {
            return Err(dom_error("OperationError", b"Failed to verify message"));
        }

        // 6. Return result.
        Ok(verified)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        params: &AlgorithmParams,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "sign" or "verify", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::Sign && usage != KeyUsage::Verify {
                return Err(Exception::Dom(
                    "SyntaxError",
                    libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                ));
            }
        }

        // 2. Generate an ML-DSA key pair, as described in Section 5.1 of [FIPS-204], with the parameter set indicated by the name member of normalizedAlgorithm.
        let maybe_key_pair = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
            mldsa_generate_key_pair(MlDsaSize::MlDsa44, &[])
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
            mldsa_generate_key_pair(MlDsaSize::MlDsa65, &[])
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
            mldsa_generate_key_pair(MlDsaSize::MlDsa87, &[])
        } else {
            unreachable!("the name identifies an ML-DSA parameter set")
        };

        // 3. If the key generation step fails, then throw an OperationError.
        let Ok(key_pair) = maybe_key_pair else {
            return Err(dom_error("OperationError", b"Key generation failed"));
        };

        // 6. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
        let public_key = key_pair.public_key.install(cx.vm());

        // 7. Set the [[type]] internal slot of publicKey to "public".
        public_key.set_type(KeyType::Public);

        // 4. Let algorithm be a new KeyAlgorithm object.
        // 5. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 8. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Key {
            name: params.name.clone(),
        });

        // 9. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 10. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
        public_key.set_usages(usage_intersection(usages, &[KeyUsage::Verify]));

        // 11. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
        let private_key = key_pair.private_key.install(cx.vm());

        // 12. Set the [[type]] internal slot of privateKey to "private".
        private_key.set_type(KeyType::Private);

        // 13. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Key {
            name: params.name.clone(),
        });

        // 14. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 15. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
        private_key.set_usages(usage_intersection(usages, &[KeyUsage::Sign]));

        // 16. Let result be a new CryptoKeyPair dictionary.
        // 17. Set the publicKey attribute of result to be publicKey.
        // 18. Set the privateKey attribute of result to be privateKey.
        let result = CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        };

        // 19. Return result.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(result))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        params: &AlgorithmParams,
        key_format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        let key;

        // 1. Let keyData be the key data to be imported.
        // 2. -> If format is "spki":
        if key_format == KeyFormat::Spki {
            // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the name member of normalizedAlgorithm is "ML-DSA-44":
            let expected_oid = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
                // Let expectedOid be id-ml-dsa-44 (2.16.840.1.101.3.4.3.17).
                ML_DSA_44_OID
            }
            // If the name member of normalizedAlgorithm is "ML-DSA-65":
            else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
                // Let expectedOid be id-ml-dsa-65 (2.16.840.1.101.3.4.3.18).
                ML_DSA_65_OID
            }
            // If the name member of normalizedAlgorithm is "ML-DSA-87":
            else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
                // Let expectedOid be id-ml-dsa-87 (2.16.840.1.101.3.4.3.19).
                ML_DSA_87_OID
            }
            // Otherwise:
            else {
                // throw a NotSupportedError.
                return Err(dom_error("NotSupportedError", b"Invalid key format"));
            };

            // 5. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki is not equal
            //    to expectedOid, then throw a DataError.
            if spki.identifier() != expected_oid {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 6. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
            if spki.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 7. Let publicKey be the ML-DSA public key identified by the subjectPublicKey field of spki.
            let public_key = spki.raw_key();

            // 8. Let key be a new CryptoKey that represents publicKey.
            key = mldsa_read_public_key(cx.vm(), public_key);

            // 9. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 10. Let algorithm be a new KeyAlgorithm.
            // 11. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        // -> If format is "pkcs8":
        else if key_format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Sign {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. => If the name member of normalizedAlgorithm is "ML-DSA-44":
            //       Let expectedOid be id-ml-dsa-44 (2.16.840.1.101.3.4.3.17).
            //       Let asn1Structure be the ASN.1 ML-DSA-44-PrivateKey structure.
            //    => If the name member of normalizedAlgorithm is "ML-DSA-65":
            //       Let expectedOid be id-ml-dsa-65 (2.16.840.1.101.3.4.3.18).
            //       Let asn1Structure be the ASN.1 ML-DSA-65-PrivateKey structure.
            //    => If the name member of normalizedAlgorithm is "ML-DSA-87":
            //       Let expectedOid be id-ml-dsa-87 (2.16.840.1.101.3.4.3.19).
            //       Let asn1Structure be the ASN.1 ML-DSA-87-PrivateKey structure.
            //    => Otherwise:
            //       throw a NotSupportedError.
            let expected_oid = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
                ML_DSA_44_OID
            } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
                ML_DSA_65_OID
            } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
                ML_DSA_87_OID
            } else {
                return Err(dom_error("NotSupportedError", b"Invalid algorithm"));
            };

            // 5. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of
            //    privateKeyInfo is not equal to expectedOid, then throw a DataError.
            if private_key_info.identifier() != expected_oid {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 6. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of
            //    privateKeyInfo is present, then throw a DataError.
            if private_key_info.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 7. Let mlDsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm, with
            //    data as the privateKey field of privateKeyInfo, structure as asn1Structure, and exactData set to
            //    true.
            // NOTE: We already did this in parse_a_private_key_info
            // 8. If an error occurred while parsing, then throw a DataError.
            // FIXME: 9. If mlDsaPrivateKey represents an ML-DSA key in the expandedKey format, or if mlDsaPrivateKey
            //    represents an ML-DSA key in the both format and the both format is not supported, throw a
            //    NotSupportedError.
            // 10. If mlDsaPrivateKey represents an ML-DSA key in the both format, and the seed field does not
            //    correspond to the expandedKey field, throw a DataError.
            // NB: LibCrypto checks the correspondence between the seed and expanded key while parsing.
            let ml_dsa_private_key = private_key_info.mldsa_private_key();

            // 9. Let key be a new CryptoKey that represents the ML-DSA private key identified by
            //    mlDsaPrivateKey.
            key = ml_dsa_private_key.install(cx.vm());

            // 10. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 11. Let algorithm be a new KeyAlgorithm.
            // 12. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // 13. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        //    -> If format is "raw-public":
        else if key_format == KeyFormat::RawPublic {
            // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Verify {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 4. Let key be a new CryptoKey representing the key data provided in keyData.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("raw key data contains bytes")
            };
            key = mldsa_read_public_key(cx.vm(), key_data.as_slice());

            // 5. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 2. Let algorithm be a new KeyAlgorithm object.
            // 3. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // 6. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        //  -> If format is "raw-seed":
        else if key_format == KeyFormat::RawSeed {
            // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::Sign {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let data be keyData.
            let ImportKeyData::Bytes(data) = key_data else {
                unreachable!("raw key data contains bytes")
            };

            // 3. If the length in bits of data is not 256 then throw a DataError.
            if data.as_slice().len() * 8 != 256 {
                return Err(dom_error("DataError", b"Seed must be 256 bits long"));
            }

            // 4. Let privateKey be the result of performing the ML-DSA.KeyGen_internal function described in Section 6.1
            //    of [FIPS-204] with the parameter set indicated by the name member of normalizedAlgorithm, using data as ξ.
            let maybe_key_pair = generate_mldsa_key_pair_from_seed(&params.name, &data);
            let key_pair = maybe_key_pair.map_err(|error| {
                Exception::Dom(
                    "OperationError",
                    libjs_rust::api::utf16_formatted("Key generation failed: {}", &[&error]),
                )
            })?;
            let private_key = key_pair.private_key;

            // 5. Let key be a new CryptoKey that represents the ML-DSA private key identified by privateKey.
            key = private_key.install(cx.vm());

            // 6. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 7. Let algorithm be a new KeyAlgorithm.
            // 8. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        //  -> If format is "jwk":
        else if key_format == KeyFormat::Jwk {
            // 1.  => If keyData is a JsonWebKey dictionary:
            //        Let jwk equal keyData.
            //     => Otherwise:
            //        Throw a DataError.
            let ImportKeyData::JsonWebKey(jwk) = key_data else {
                return Err(dom_error("DataError", b"Data is not a JsonWebKey dictionary"));
            };

            // 2. If the priv field is present and usages contains an entry which is not "sign", or, if the priv field is
            //    not present and usages contains an entry which is not "verify" then throw a SyntaxError.
            if (jwk.r#priv.is_some() && usages.iter().any(|&usage| usage != KeyUsage::Sign))
                || (jwk.r#priv.is_none() && usages.iter().any(|&usage| usage != KeyUsage::Verify))
            {
                return Err(dom_error("SyntaxError", b"Invalid usage"));
            }

            // 3. If the kty field of jwk is not "AKP", then throw a DataError.
            if jwk.kty.as_ref() != Some(&Utf16String::from_ascii(b"AKP")) {
                return Err(dom_error("DataError", b"Invalid key type"));
            }

            // 4. If the alg field of jwk is not equal to the name member of normalizedAlgorithm, then throw a DataError.
            if jwk.alg.as_ref() != Some(&params.name) {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 5. If usages is non-empty and the use field of jwk is present and is not equal to "sig", then throw a DataError.
            if !usages.is_empty()
                && jwk
                    .r#use
                    .as_ref()
                    .is_some_and(|usage| usage != &Utf16String::from_ascii(b"sig"))
            {
                return Err(dom_error("DataError", b"Invalid usage type"));
            }

            // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web
            //    Key [JWK], or it does not contain all of the specified usages values, then throw a DataError.
            validate_jwk_key_ops(&jwk, usages)?;

            // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
            if jwk.ext == Some(false) && extractable {
                return Err(dom_error("DataError", b"Invalid ext field"));
            }

            // 8. => If the priv field of jwk is present:
            if let Some(private_seed) = &jwk.r#priv {
                // 1. If the priv attribute of jwk does not contain a valid base64url encoded seed
                //    representing an ML-DSA private key, then throw a DataError.
                let seed = base64_url_bytes_decode(cx.vm(), private_seed)?;
                if seed.as_slice().len() * 8 != 256 {
                    return Err(dom_error("DataError", b"Seed must be 256 bits long"));
                }

                // 2. Let key be a new CryptoKey object that represents the ML-DSA private key
                //    identified by interpreting the priv attribute of jwk as a base64url encoded seed.
                let maybe_key_pair = generate_mldsa_key_pair_from_seed(&params.name, &seed);
                let key_pair = maybe_key_pair.map_err(|error| {
                    Exception::Dom(
                        "DataError",
                        libjs_rust::api::utf16_formatted("Invalid seed: {}", &[&error]),
                    )
                })?;
                let NativeKeyPair {
                    public_key,
                    private_key,
                } = key_pair;
                key = private_key.install(cx.vm());

                // 3. Set the [[type]] internal slot of key to "private".
                key.set_type(KeyType::Private);

                // 4. If the pub attribute of jwk does not contain the base64url encoded public key
                //    representing the ML-DSA public key corresponding to key, then throw a DataError.
                let Some(public_bytes) = &jwk.r#pub else {
                    return Err(dom_error("DataError", b"JsonWebKey does not contain public key"));
                };

                let pub_decoded = base64_url_bytes_decode(cx.vm(), public_bytes);
                let mut expected = Span::new(&[]);
                // SAFETY: The native candidate owns the bytes through the comparison.
                unsafe { web_crypto_mldsa_public_bytes(public_key.0.as_ptr(), &mut expected) };
                if !pub_decoded.is_ok_and(|bytes| bytes.as_slice() == expected.as_slice()) {
                    return Err(dom_error("DataError", b"JsonWebKey public key does not match"));
                }
            }
            //    => Otherwise:
            else {
                // 1. If the pub attribute of jwk does not contain a valid base64url encoded public key
                //    representing an ML-DSA public key, then throw a DataError.
                let Some(public_bytes) = &jwk.r#pub else {
                    return Err(dom_error("DataError", b"JsonWebKey does not contain public key"));
                };

                let public_key = base64_url_bytes_decode(cx.vm(), public_bytes)?;

                // 2. Let key be a new CryptoKey object that represents the ML-DSA public key
                //    identified by interpreting the pub attribute of jwk as a base64url encoded public key.
                key = mldsa_read_public_key(cx.vm(), public_key.as_slice());

                // 3. Set the [[type]] internal slot of key to "public".
                key.set_type(KeyType::Public);
            }

            // 9. Let algorithm be a new instance of a KeyAlgorithm object.
            // 10. Set the name attribute of algorithm to the name member of normalizedAlgorithm.
            // 11. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        //    -> Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 3. Return key.
        Ok(key)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-export-key
    #[allow(
        clippy::field_reassign_with_default,
        reason = "keep the dictionary assignments in specification order"
    )]
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        let vm = cx.vm();

        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
        // Note: In our impl this is always accessible
        let handle = key.handle();

        // 3. -> If format is "spki":
        let result = if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

            // 3. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //      * -> If the name member of keyAlgorithm is "ML-DSA-44":
            //           Set the algorithm object identifier to the id-ml-dsa-44 (2.16.840.1.101.3.4.3.17) OID.
            //        -> If the name member of keyAlgorithm is "ML-DSA-65":
            //           Set the algorithm object identifier to the id-ml-dsa-65 (2.16.840.1.101.3.4.3.18) OID.
            //        -> If the name member of keyAlgorithm is "ML-DSA-87":
            //           Set the algorithm object identifier to the id-ml-dsa-87 (2.16.840.1.101.3.4.3.19) OID.
            //        -> Otherwise:
            //           throw a NotSupportedError.
            //    * Set the subjectPublicKey field to keyData.
            let algorithm_oid = if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
                ML_DSA_44_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
                ML_DSA_65_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
                ML_DSA_87_OID
            } else {
                return Err(dom_error("NotSupportedError", b"Invalid algorithm"));
            };

            let data = mldsa_export_der(vm, key, true, &algorithm_oid)?;

            // 4. Let result be the result of DER-encoding data.
            ExportKeyResult::Bytes(data)
        }
        //   -> If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

            // 3. Let data be an instance of the PrivateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
            //    * Set the version field to 0.
            //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //      * => If the name member of keyAlgorithm is "ML-DSA-44":
            //           Set the algorithm object identifier to the id-ml-dsa-44 (2.16.840.1.101.3.4.3.17) OID.
            //      * => If the name member of keyAlgorithm is "ML-DSA-65":
            //           Set the algorithm object identifier to the id-ml-dsa-65 (2.16.840.1.101.3.4.3.18) OID.
            //      * => If the name member of keyAlgorithm is "ML-DSA-87":
            //           Set the algorithm object identifier to the id-ml-dsa-87 (2.16.840.1.101.3.4.3.19) OID.
            //      * => Otherwise:
            //           throw a NotSupportedError.
            //    * Set the privateKey field as follows:
            //      * => If the name member of keyAlgorithm is "ML-DSA-44":
            //           Set the privateKey field to the result of DER-encoding a ML-DSA-44-PrivateKey
            //           ASN.1 type that represents the ML-DSA private key seed represented by the
            //           [[handle]] internal slot of key using the seed-only format (using a context-
            //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
            //      * => If the name member of keyAlgorithm is "ML-DSA-65":
            //           Set the privateKey field to the result of DER-encoding a ML-DSA-65-PrivateKey
            //           ASN.1 type that represents the ML-DSA private key seed represented by the
            //           [[handle]] internal slot of key using the seed-only format (using a context-
            //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
            //      * => If the name member of keyAlgorithm is "ML-DSA-87":
            //           Set the privateKey field to the result of DER-encoding a ML-DSA-87-PrivateKey
            //           ASN.1 type that represents the ML-DSA private key seed represented by the
            //           [[handle]] internal slot of key using the seed-only format (using a context-
            //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
            //      * => Otherwise:
            //           throw a NotSupportedError.
            let algorithm_oid = if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-DSA-44")) {
                ML_DSA_44_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-DSA-65")) {
                ML_DSA_65_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-DSA-87")) {
                ML_DSA_87_OID
            } else {
                return Err(dom_error("NotSupportedError", b"Invalid algorithm"));
            };

            let data = mldsa_export_der(vm, key, false, &algorithm_oid)?;

            // 4. Let result be the result of DER-encoding data.
            ExportKeyResult::Bytes(data)
        }
        //   -> If format is "raw-public":
        else if format == KeyFormat::RawPublic {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be a byte sequence containing the ML-DSA public key represented by the [[handle]] internal slot of key.
            let mut bytes = Span::new(&[]);
            // SAFETY: The key owns the bytes through the copy.
            unsafe { web_crypto_mldsa_public_bytes(handle.as_ptr(), &mut bytes) };
            let data = Bytes::new(copy_bytes(vm, bytes.as_slice())?);

            // 3. Let result be data.
            ExportKeyResult::Bytes(data)
        }
        //   -> If format is "raw-seed":
        else if format == KeyFormat::RawSeed {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let data be a byte sequence containing the ξ seed variable of the key represented by the [[handle]] internal slot of key.
            let mut bytes = Span::new(&[]);
            // SAFETY: The key owns the bytes through the copy.
            unsafe { web_crypto_mldsa_seed_bytes(handle.as_ptr(), &mut bytes) };
            let data = Bytes::new(copy_bytes(vm, bytes.as_slice())?);

            // 3. Let result be data.
            ExportKeyResult::Bytes(data)
        }
        //   -> If format is "jwk":
        else if format == KeyFormat::Jwk {
            // 1. Let jwk be a new JsonWebKey dictionary.
            let mut jwk = JsonWebKey::default();

            // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

            // 3. Set the kty attribute of jwk to "AKP".
            jwk.kty = Some(Utf16String::from_ascii(b"AKP"));

            // 4. Set the alg attribute of jwk to the name member of keyAlgorithm.
            jwk.alg = Some(key.algorithm_name());

            // 5. Set the pub attribute of jwk to the base64url encoded public key corresponding to the [[handle]] internal slot of key.
            let mut public_bytes = Span::new(&[]);
            // SAFETY: The key owns the bytes through the encoding.
            unsafe { web_crypto_mldsa_public_bytes(handle.as_ptr(), &mut public_bytes) };
            jwk.r#pub = Some(encode_jwk_bytes(vm, public_bytes.as_slice())?);

            // 6. -> If the [[type]] internal slot of key is "private":
            //       Set the priv attribute of jwk to the base64url encoded seed represented by the [[handle]] internal slot of key.
            if key.key_type() == KeyType::Private {
                let mut seed = Span::new(&[]);
                // SAFETY: The key owns the seed through the encoding.
                unsafe { web_crypto_mldsa_seed_bytes(handle.as_ptr(), &mut seed) };
                jwk.r#priv = Some(encode_jwk_bytes(vm, seed.as_slice())?);
            }

            // 7. Set the key_ops attribute of jwk to the usages attribute of key.
            jwk.key_ops = Some(
                key.internal_usages()
                    .iter()
                    .map(|&usage| Utf16String::from_ascii(key_usage_to_string(usage).as_bytes()))
                    .collect(),
            );

            // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
            jwk.ext = Some(key.is_extractable());

            // 9. Let result be jwk.
            ExportKeyResult::JsonWebKey(jwk)
        }
        //   -> Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        };

        // 4. Return result.
        Ok(result)
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::Verify])
    }
}

#[derive(Clone, Copy)]
#[repr(i32)]
enum MlKemSize {
    MlKem512,
    MlKem768,
    MlKem1024,
}

const ML_KEM_512_OID: [u32; 9] = [2, 16, 840, 1, 101, 3, 4, 4, 1];
const ML_KEM_768_OID: [u32; 9] = [2, 16, 840, 1, 101, 3, 4, 4, 2];
const ML_KEM_1024_OID: [u32; 9] = [2, 16, 840, 1, 101, 3, 4, 4, 3];

unsafe extern "C" {
    fn web_crypto_mlkem_generate_key_pair(
        size: MlKemSize,
        seed: Span<'_, u8>,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> usize;
    fn web_crypto_mlkem_key_info_read_private_key(key_info: *const c_void) -> *mut c_void;
    fn web_crypto_mlkem_read_public_key(bytes: Span<'_, u8>) -> *mut c_void;
    fn web_crypto_mlkem_public_bytes(key: *const c_void, state: *mut c_void, receive: ReceiveBytes);
    fn web_crypto_mlkem_seed_bytes(key: *const c_void, bytes: &mut Span<'_, u8>);
    fn web_crypto_mlkem_export_der(
        key: *const c_void,
        is_public: bool,
        oid: Span<'_, u32>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    fn web_crypto_mlkem_encapsulate(
        size: MlKemSize,
        key: *const c_void,
        shared_key: *mut c_void,
        ciphertext: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
    fn web_crypto_mlkem_decapsulate(
        size: MlKemSize,
        key: *const c_void,
        ciphertext: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
}

impl Asn1KeyInfo {
    fn mlkem_private_key(&self) -> UninstalledKey {
        assert!(matches!(self.structure, Asn1Structure::PrivateKeyInfo));
        // SAFETY: This object owns a live PrivateKeyInfo.
        let key = unsafe { web_crypto_mlkem_key_info_read_private_key(self.key_info.as_ptr()) };
        UninstalledKey(NonNull::new(key).expect("LibCrypto returns an ML-KEM private key"))
    }
}

fn mlkem_read_public_key(vm: &Vm, bytes: &[u8]) -> Ref<CryptoKey> {
    // SAFETY: The input bytes are lent for the call.
    let key = unsafe { web_crypto_mlkem_read_public_key(Span::new(bytes)) };
    // SAFETY: The service transfers ownership of the live key.
    unsafe { CryptoKey::create_with_handle(vm, NonNull::new(key).expect("LibCrypto returns an ML-KEM public key")) }
}

fn mlkem_generate_key_pair(size: MlKemSize, seed: &[u8]) -> Result<NativeKeyPair, Utf16String> {
    let mut public_key = null_mut();
    let mut private_key = null_mut();
    // SAFETY: The seed and output slots are lent for the call.
    let error = unsafe { web_crypto_mlkem_generate_key_pair(size, Span::new(seed), &mut public_key, &mut private_key) };
    if error != 0 {
        // SAFETY: LibCrypto transfers ownership of the error message.
        return Err(unsafe { Utf16String::from_raw_owned(error) });
    }
    Ok(NativeKeyPair {
        public_key: UninstalledKey(NonNull::new(public_key).expect("LibCrypto returns a public key")),
        private_key: UninstalledKey(NonNull::new(private_key).expect("LibCrypto returns a private key")),
    })
}

fn mlkem_export_der(vm: &Vm, key: Ref<CryptoKey>, is_public: bool, oid: &[u32]) -> WebIdlResult<Bytes> {
    let mut result: Option<Bytes> = None;
    // SAFETY: The key handle, OID and output slot are lent for the call.
    if !unsafe {
        web_crypto_mlkem_export_der(
            key.handle().as_ptr(),
            is_public,
            Span::new(oid),
            core::ptr::from_mut(&mut result).cast(),
            receive_bytes,
        )
    } {
        return Err(out_of_memory(vm));
    }
    Ok(result.expect("LibCrypto returns the DER bytes"))
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MlKem;

impl AlgorithmMethods for MlKem {
    // https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-generate-key
    fn generate_key(
        &self,
        cx: &CallContext,
        params: &AlgorithmParams,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        // 1. If usages contains an entry which is not one of "encapsulateKey", "encapsulateBits", "decapsulateKey" or
        //    "decapsulateBits", then throw a SyntaxError.
        for &usage in usages {
            if usage != KeyUsage::EncapsulateKey
                && usage != KeyUsage::EncapsulateBits
                && usage != KeyUsage::DecapsulateKey
                && usage != KeyUsage::DecapsulateBits
            {
                return Err(dom_error("SyntaxError", b"Invalid key usage"));
            }
        }

        // 2. Generate an ML-KEM key pair, as described in Section 7.1 of [FIPS-203], with the parameter set indicated
        //    by the name member of normalizedAlgorithm.
        let size = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
            MlKemSize::MlKem512
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
            MlKemSize::MlKem768
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
            MlKemSize::MlKem1024
        } else {
            unreachable!("the name identifies an ML-KEM parameter set")
        };
        let maybe_key_pair = mlkem_generate_key_pair(size, &[]);

        // 3. If the key generation step fails, then throw an OperationError.
        let key_pair = maybe_key_pair.map_err(|error| {
            Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Key generation failed: {}", &[&error]),
            )
        })?;

        // 6. Let publicKey be a new CryptoKey representing the encapsulation key of the generated key pair.
        let public_key = key_pair.public_key.install(cx.vm());

        // 7. Set the [[type]] internal slot of publicKey to "public".
        public_key.set_type(KeyType::Public);

        // 4. Let algorithm be a new KeyAlgorithm object.
        // 5. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 8. Set the [[algorithm]] internal slot of publicKey to algorithm.
        public_key.set_algorithm(InternalAlgorithmData::Key {
            name: params.name.clone(),
        });

        // 9. Set the [[extractable]] internal slot of publicKey to true.
        public_key.set_extractable(true);

        // 10. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "encapsulateKey",
        //     "encapsulateBits" ].
        public_key.set_usages(usage_intersection(
            usages,
            &[KeyUsage::EncapsulateKey, KeyUsage::EncapsulateBits],
        ));

        // 11. Let privateKey be a new CryptoKey representing the decapsulation key of the generated key pair.
        let private_key = key_pair.private_key.install(cx.vm());

        // 12. Set the [[type]] internal slot of privateKey to "private".
        private_key.set_type(KeyType::Private);

        // 13. Set the [[algorithm]] internal slot of privateKey to algorithm.
        private_key.set_algorithm(InternalAlgorithmData::Key {
            name: params.name.clone(),
        });

        // 14. Set the [[extractable]] internal slot of privateKey to extractable.
        private_key.set_extractable(extractable);

        // 15. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "decapsulateKey",
        //     "decapsulateBits" ].
        private_key.set_usages(usage_intersection(
            usages,
            &[KeyUsage::DecapsulateKey, KeyUsage::DecapsulateBits],
        ));

        // 16. Let result be a new CryptoKeyPair dictionary.
        // 17. Set the publicKey attribute of result to be publicKey.
        // 18. Set the privateKey attribute of result to be privateKey.
        let result = CryptoKeyPair {
            public_key: Some(public_key),
            private_key: Some(private_key),
        };

        // 19. Return result.
        Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(result))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-import-key
    fn import_key(
        &self,
        cx: &CallContext,
        params: &AlgorithmParams,
        format: KeyFormat,
        key_data: ImportKeyData,
        _: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        let key;

        // 1. Let keyData be the key data to be imported.
        // 2. -> If format is "spki":
        if format == KeyFormat::Spki {
            // 1. If usages contains an entry which is not "encapsulateKey" or "encapsulateBits" then throw
            //    a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::EncapsulateKey && usage != KeyUsage::EncapsulateBits {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let spki = Asn1KeyInfo::parse(&key_data, Asn1Structure::SubjectPublicKeyInfo)?;

            // 4. If the name member of normalizedAlgorithm is "ML-KEM-512":
            let expected_oid = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
                // Let expectedOid be id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1).
                ML_KEM_512_OID
            }
            // If the name member of normalizedAlgorithm is "ML-KEM-768":
            else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
                // Let expectedOid be id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2).
                ML_KEM_768_OID
            }
            // If the name member of normalizedAlgorithm is "ML-KEM-1024":
            else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
                // Let expectedOid be id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3).
                ML_KEM_1024_OID
            }
            // Otherwise:
            else {
                // throw a NotSupportedError.
                return Err(dom_error("NotSupportedError", b"Invalid key format"));
            };

            // 5. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki is not equal
            //    to expectedOid, then throw a DataError.
            if spki.identifier() != expected_oid {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 6. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
            if spki.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 7. Let publicKey be the ML-KEM public key identified by the subjectPublicKey field of spki.
            let public_key = spki.raw_key();

            // 8. Let key be a new CryptoKey that represents publicKey.
            key = mlkem_read_public_key(cx.vm(), public_key);

            // 9. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 10. Let algorithm be a new KeyAlgorithm.
            // 11. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // 12. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        //    -> If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If usages contains an entry which is not "decapsulateKey" or "decapsulateBits" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::DecapsulateKey && usage != KeyUsage::DecapsulateBits {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
            // 3. If an error occurred while parsing, then throw a DataError.
            let ImportKeyData::Bytes(key_data) = key_data else {
                unreachable!("DER key data contains bytes")
            };
            let private_key_info = Asn1KeyInfo::parse(&key_data, Asn1Structure::PrivateKeyInfo)?;

            // 4. => If the name member of normalizedAlgorithm is "ML-KEM-512":
            //       Let expectedOid be id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1).
            //       Let asn1Structure be the ASN.1 ML-KEM-512-PrivateKey structure.
            //    => If the name member of normalizedAlgorithm is "ML-KEM-768":
            //       Let expectedOid be id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2).
            //       Let asn1Structure be the ASN.1 ML-KEM-768-PrivateKey structure.
            //    => If the name member of normalizedAlgorithm is "ML-KEM-1024":
            //       Let expectedOid be id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3).
            //       Let asn1Structure be the ASN.1 ML-KEM-1024-PrivateKey structure.
            //    => Otherwise:
            //       throw a NotSupportedError.
            let expected_oid = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
                ML_KEM_512_OID
            } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
                ML_KEM_768_OID
            } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
                ML_KEM_1024_OID
            } else {
                return Err(dom_error("NotSupportedError", b"Invalid algorithm"));
            };

            // 5. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of
            //    privateKeyInfo is not equal to expectedOid, then throw a DataError.
            if private_key_info.identifier() != expected_oid {
                return Err(dom_error("DataError", b"Invalid algorithm"));
            }

            // 6. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of
            //    privateKeyInfo is present, then throw a DataError.
            if private_key_info.ec_parameters().is_some() {
                return Err(dom_error("DataError", b"Invalid algorithm parameters"));
            }

            // 7. Let mlKemPrivateKey be the result of performing the parse an ASN.1 structure algorithm, with
            //    data as the privateKey field of privateKeyInfo, structure as asn1Structure, and exactData set to
            //    true.
            // NOTE: We already did this in parse_a_private_key_info
            // 8. If an error occurred while parsing, then throw a DataError.
            // FIXME: 9. If mlKemPrivateKey represents an ML-KEM key in the expandedKey format, or if mlKemPrivateKey
            //    represents an ML-KEM key in the both format and the both format is not supported, throw a
            //    NotSupportedError.
            // 10. If mlKemPrivateKey represents an ML-KEM key in the both format, and the seed field does not
            //    correspond to the expandedKey field, throw a DataError.
            // NB: LibCrypto checks the correspondence between the seed and expanded key while parsing.
            let ml_kem_private_key = private_key_info.mlkem_private_key();

            // 9. Let key be a new CryptoKey that represents the ML-KEM private key identified by
            //    mlKemPrivateKey.
            key = ml_kem_private_key.install(cx.vm());

            // 10. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 11. Let algorithm be a new KeyAlgorithm.
            // 12. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // 13. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        //    -> If format is "raw-public":
        else if format == KeyFormat::RawPublic {
            // 1. If usages contains an entry which is not "encapsulateKey" or "encapsulateBits" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::EncapsulateKey && usage != KeyUsage::EncapsulateBits {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let data be keyData.
            let ImportKeyData::Bytes(data) = key_data else {
                unreachable!("raw key data contains bytes")
            };

            // 3. Let key be a new CryptoKey that represents the ML-KEM public key data in data.
            key = mlkem_read_public_key(cx.vm(), data.as_slice());

            // 4. Set the [[type]] internal slot of key to "public"
            key.set_type(KeyType::Public);

            // 5. Let algorithm be a new KeyAlgorithm.
            // 6. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // 6. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        //    -> If format is "raw-seed":
        else if format == KeyFormat::RawSeed {
            // 1. If usages contains an entry which is not "decapsulateKey" or "decapsulateBits" then throw a SyntaxError.
            for &usage in usages {
                if usage != KeyUsage::DecapsulateKey && usage != KeyUsage::DecapsulateBits {
                    return Err(Exception::Dom(
                        "SyntaxError",
                        libjs_rust::api::utf16_formatted("Invalid key usage '{}'", &[&key_usage_to_string(usage)]),
                    ));
                }
            }

            // 2. Let data be keyData.
            let ImportKeyData::Bytes(data) = key_data else {
                unreachable!("raw key data contains bytes")
            };

            // 3. If the length in bits of data is not 512 then throw a DataError.
            if data.as_slice().len() * 8 != 512 {
                return Err(dom_error("DataError", b"Invalid key format"));
            }

            // 4. Let privateKey be the result of performing the ML-KEM.KeyGen_internal function described in
            //    Section 6.1 of [FIPS-203] with the parameter set indicated by the name member of
            //    normalizedAlgorithm, using the first 256 bits of data as d and the last 256 bits of data as z.
            let size = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
                MlKemSize::MlKem512
            } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
                MlKemSize::MlKem768
            } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
                MlKemSize::MlKem1024
            } else {
                unreachable!("the name identifies an ML-KEM parameter set")
            };
            let maybe_key_pair = mlkem_generate_key_pair(size, data.as_slice());
            let key_pair = maybe_key_pair.map_err(|error| {
                Exception::Dom(
                    "OperationError",
                    libjs_rust::api::utf16_formatted("Key generation failed: {}", &[&error]),
                )
            })?;

            // 5. Let key be a new CryptoKey that represents the ML-KEM private key identified by privateKey.
            key = key_pair.private_key.install(cx.vm());

            // 6. Set the [[type]] internal slot of key to "private"
            key.set_type(KeyType::Private);

            // 7. Let algorithm be a new KeyAlgorithm.
            // 8. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
            // 9. Set the [[algorithm]] internal slot of key to algorithm.
            key.set_algorithm(InternalAlgorithmData::Key {
                name: params.name.clone(),
            });
        }
        // FIXME: -> If format is "jwk":
        //    -> Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        }

        // 3. Return key.
        Ok(key)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-export-key
    fn export_key(&self, cx: &CallContext, format: KeyFormat, key: Ref<CryptoKey>) -> WebIdlResult<ExportKeyResult> {
        let vm = cx.vm();

        // 1. Let key be the CryptoKey to be exported.

        // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be
        //    accessed, then throw an OperationError.
        // Note: In our impl this is always accessible
        let handle = key.handle();

        // 3. -> If format is "spki":
        let result = if format == KeyFormat::Spki {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

            // 3. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
            //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
            //      * -> If the name member of keyAlgorithm is "ML-KEM-512":
            //           Set the algorithm object identifier to the id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1) OID.
            //        -> If the name member of keyAlgorithm is "ML-KEM-768":
            //           Set the algorithm object identifier to the id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2) OID.
            //        -> If the name member of keyAlgorithm is "ML-KEM-1024":
            //           Set the algorithm object identifier to the id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3) OID.
            //        -> Otherwise:
            //           throw a NotSupportedError.
            //    * Set the subjectPublicKey field to keyData.
            let algorithm_oid = if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
                ML_KEM_512_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
                ML_KEM_768_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
                ML_KEM_1024_OID
            } else {
                return Err(dom_error("NotSupportedError", b"Invalid algorithm"));
            };

            let data = mlkem_export_der(vm, key, true, &algorithm_oid)?;

            // 4. Let result be the result of DER-encoding data.
            ExportKeyResult::Bytes(data)
        }
        //   -> If format is "pkcs8":
        else if format == KeyFormat::Pkcs8 {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

            // 3. Let data be an instance of the PrivateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
            //    * Set the version field to 0.
            //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
            //      * => If the name member of keyAlgorithm is "ML-KEM-512":
            //           Set the algorithm object identifier to the id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1) OID.
            //      * => If the name member of keyAlgorithm is "ML-KEM-768":
            //           Set the algorithm object identifier to the id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2) OID.
            //      * => If the name member of keyAlgorithm is "ML-KEM-1024":
            //           Set the algorithm object identifier to the id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3) OID.
            //      * => Otherwise:
            //           throw a NotSupportedError.
            //    * Set the privateKey field as follows:
            //      * => If the name member of keyAlgorithm is "ML-KEM-512":
            //           Set the privateKey field to the result of DER-encoding a ML-KEM-512-PrivateKey
            //           ASN.1 type that represents the ML-KEM private key seed represented by the
            //           [[handle]] internal slot of key using the seed-only format (using a context-
            //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
            //      * => If the name member of keyAlgorithm is "ML-KEM-768":
            //           Set the privateKey field to the result of DER-encoding a ML-KEM-65-PrivateKey
            //           ASN.1 type that represents the ML-KEM private key seed represented by the
            //           [[handle]] internal slot of key using the seed-only format (using a context-
            //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
            //      * => If the name member of keyAlgorithm is "ML-KEM-1024":
            //           Set the privateKey field to the result of DER-encoding a ML-KEM-1024-PrivateKey
            //           ASN.1 type that represents the ML-KEM private key seed represented by the
            //           [[handle]] internal slot of key using the seed-only format (using a context-
            //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
            //      * => Otherwise:
            //           throw a NotSupportedError.
            let algorithm_oid = if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
                ML_KEM_512_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
                ML_KEM_768_OID
            } else if matches!(key.algorithm_name().as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
                ML_KEM_1024_OID
            } else {
                return Err(dom_error("NotSupportedError", b"Invalid algorithm"));
            };

            let data = mlkem_export_der(vm, key, false, &algorithm_oid)?;

            // 4. Let result be the result of DER-encoding data.
            ExportKeyResult::Bytes(data)
        }
        //   -> If format is "raw-public":
        else if format == KeyFormat::RawPublic {
            // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Public {
                return Err(dom_error("InvalidAccessError", b"Key is not a public key"));
            }

            // 2. Let data be a byte sequence containing the raw octets of the key represented by the [[handle]] internal slot of key.
            let mut data: Option<Bytes> = None;
            // SAFETY: The handle and output slot are lent for the call.
            unsafe {
                web_crypto_mlkem_public_bytes(handle.as_ptr(), core::ptr::from_mut(&mut data).cast(), receive_bytes);
            };
            let data = data.expect("LibCrypto returns the public key bytes");

            // 3. Let result be data.
            ExportKeyResult::Bytes(data)
        }
        //   -> If format is "raw-seed":
        else if format == KeyFormat::RawSeed {
            // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
            if key.key_type() != KeyType::Private {
                return Err(dom_error("InvalidAccessError", b"Key is not a private key"));
            }

            // 2. Let data be a byte sequence containing the concatenation of the d and z seed variables of the
            //    key represented by the [[handle]] internal slot of key.
            let mut seed = Span::new(&[]);
            // SAFETY: The key owns the seed through the copy.
            unsafe { web_crypto_mlkem_seed_bytes(handle.as_ptr(), &mut seed) };
            let data = Bytes::new(copy_bytes(vm, seed.as_slice())?);

            // 3. Let result be data.
            ExportKeyResult::Bytes(data)
        }
        // FIXME:  -> If format is "jwk":
        //   -> Otherwise:
        else {
            // throw a NotSupportedError.
            return Err(dom_error("NotSupportedError", b"Invalid key format"));
        };

        // 4. Return result.
        Ok(result)
    }

    fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        Some(vec![KeyUsage::EncapsulateKey, KeyUsage::EncapsulateBits])
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-encapsulate
    fn encapsulate(
        &self,
        _: Gc<Realm>,
        params: &AlgorithmParams,
        key: Ref<CryptoKey>,
    ) -> WebIdlResult<EncapsulatedBitsData> {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Public {
            return Err(dom_error("InvalidAccessError", b"Invalid key type"));
        }

        // 2. Perform the encapsulation key check described in Section 7.2 of [FIPS-203] with the parameter set indicated
        //    by the name member of algorithm, using the key represented by the [[handle]] internal slot of key as the ek
        //    input parameter.
        // 3. If the encapsulation key check failed, throw an OperationError.
        // NOTE: Presumably done by OpenSSL for us

        // 4. Let sharedKey and ciphertext be the outputs that result from performing the ML-KEM.Encaps function
        //    described in Section 7.2 of [FIPS-203] with the parameter set indicated by the name member of algorithm,
        //    using the key represented by the [[handle]] internal slot of key as the ek input parameter.
        let size = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
            MlKemSize::MlKem512
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
            MlKemSize::MlKem768
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
            MlKemSize::MlKem1024
        } else {
            unreachable!("the name identifies an ML-KEM parameter set")
        };
        let mut shared_key: Option<Bytes> = None;
        let mut ciphertext: Option<Bytes> = None;
        // SAFETY: The handle and output slots are lent for the call.
        let maybe_encapsulation = unsafe {
            web_crypto_mlkem_encapsulate(
                size,
                key.handle().as_ptr(),
                core::ptr::from_mut(&mut shared_key).cast(),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        };

        // 5. If the ML-KEM.Encaps function returned an error, throw an OperationError.
        if maybe_encapsulation != 0 {
            // SAFETY: LibCrypto transfers ownership of the error message.
            let error = unsafe { Utf16String::from_raw_owned(maybe_encapsulation) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Key encapsulation failed: {}", &[&error]),
            ));
        }

        // 6. Let result be a new EncapsulatedBits dictionary.
        // 7. Set the sharedKey attribute of result to the result of creating an ArrayBuffer containing sharedKey.
        // 8. Set the ciphertext attribute of result to the result of creating an ArrayBuffer containing ciphertext.
        let result = EncapsulatedBitsData { shared_key, ciphertext };

        // 9. Return result.
        Ok(result)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-decapsulate
    fn decapsulate(
        &self,
        _: Gc<Realm>,
        params: &AlgorithmParams,
        key: Ref<CryptoKey>,
        ciphertext: &Bytes,
    ) -> WebIdlResult<Bytes> {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if key.key_type() != KeyType::Private {
            return Err(dom_error("InvalidAccessError", b"Invalid key type"));
        }

        // 2. Perform the decapsulation input check described in Section 7.3 of [FIPS-203] with the parameter set
        //    indicated by the name member of algorithm, using the key represented by the [[handle]] internal slot of key as
        //    the dk input parameter, and ciphertext as the c input parameter.
        // 3. If the decapsulation key check failed, return an OperationError.
        // NOTE: Presumably done by OpenSSL for us

        // 4. Let sharedKey be the output that results from performing the ML-KEM.Decaps function described in Section
        //    7.3 of [FIPS-203] with the parameter set indicated by the name member of algorithm, using the key
        //    represented by the [[handle]] internal slot of key as the dk input parameter, and ciphertext as the c input
        //    parameter.
        let size = if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-512")) {
            MlKemSize::MlKem512
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-768")) {
            MlKemSize::MlKem768
        } else if matches!(params.name.as_units(), Utf16StringUnits::Ascii(b"ML-KEM-1024")) {
            MlKemSize::MlKem1024
        } else {
            unreachable!("the name identifies an ML-KEM parameter set")
        };
        let mut shared_key: Option<Bytes> = None;
        // SAFETY: The handle, ciphertext and output slot are lent for the call.
        let maybe_shared_key = unsafe {
            web_crypto_mlkem_decapsulate(
                size,
                key.handle().as_ptr(),
                Span::new(ciphertext.as_slice()),
                core::ptr::from_mut(&mut shared_key).cast(),
                receive_bytes,
            )
        };

        if maybe_shared_key != 0 {
            // SAFETY: LibCrypto transfers ownership of the error message.
            let error = unsafe { Utf16String::from_raw_owned(maybe_shared_key) };
            return Err(Exception::Dom(
                "OperationError",
                libjs_rust::api::utf16_formatted("Key decapsulation failed: {}", &[&error]),
            ));
        }

        let shared_key = shared_key.expect("LibCrypto returns the shared key");

        // 5. Return sharedKey.
        Ok(shared_key)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-get-shared-key-length
    fn get_shared_key_length(&self, _: Gc<Realm>) -> WebIdlResult<u32> {
        // 1. Return 256.
        Ok(256)
    }
}

unsafe extern "C" fn receive_bytes(state: *mut c_void, bytes: Span<'_, u8>) {
    // SAFETY: The state is the caller's slot, and the service lends the bytes.
    unsafe { *state.cast::<Option<Bytes>>() = Some(Bytes::new(bytes.as_slice().to_vec())) };
}

pub(crate) fn public_key_for_private_key(vm: &Vm, key: Ref<CryptoKey>) -> Option<Ref<CryptoKey>> {
    let name = key.algorithm_name();
    let algorithm = key.internal_algorithm();
    let named_curve = if let InternalAlgorithmData::Ec { named_curve, .. } = &*algorithm {
        named_curve.raw_identity()
    } else {
        0
    };
    // SAFETY: The key handle and algorithm metadata are lent for the call.
    let public_key =
        unsafe { web_crypto_public_key_for_private_key(key.handle().as_ptr(), name.raw_identity(), named_curve) };
    NonNull::new(public_key).map(|handle| {
        // SAFETY: The service transfers ownership of the native key.
        unsafe { CryptoKey::create_with_handle(vm, handle) }
    })
}
