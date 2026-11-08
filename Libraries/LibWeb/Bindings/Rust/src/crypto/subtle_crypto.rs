/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;
use std::collections::HashMap;
use std::sync::LazyLock;

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferShared, ErrorKind, ErrorType, Gc, Must, Object, PrimitiveString, PropertyKey, Realm, Trace,
    Value, Vm,
};

use crate::bindings::conversions::BufferSource;
#[cfg(web_idl_bindings)]
use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{
    Ref, Wrappable, create_dictionary_object, with_temporary_execution_context,
    with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, create_rejected_promise, deferred_invoke, promise_realm,
    queue_a_global_task, reject_promise, resolve_promise,
};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::crypto::crypto_algorithms::{
    AesCbc, AesCtr, AesGcm, AesKw, AesOcb, AesParameterType, AlgorithmMethods, AlgorithmParams, Argon2, Argon2Params,
    Bytes, CShakeParams, ChaCha20Poly1305, EcParameterType, Ecdh, Ecdsa, Ed448, Ed25519, EncapsulatedKeyData,
    ExportKeyResult, Hkdf, HkdfParams, Hmac, HmacImportParams, HmacKeyGenParams, ImportKeyData, Kmac, KmacImportParams,
    KmacKeyGenParams, KmacParams, MlDsa, MlKem, Pbkdf2, Pbkdf2Params, RsaOaep, RsaParameterType, RsaPss, RsassaPkcs1,
    Sha, X448, X25519, normalized_key_usages, public_key_for_private_key,
};
#[allow(
    unused_imports,
    reason = "the generated bindings name definitions through their IDL file"
)]
pub(crate) use crate::crypto::crypto_bindings::{
    CryptoKeyPair, EncapsulatedBits, EncapsulatedKey, JsonWebKey, RsaOtherPrimesInfo,
};
use crate::crypto::crypto_bindings::{
    array_buffer, crypto_key_pair, encapsulated_bits as encapsulated_bits_value,
    encapsulated_key as encapsulated_key_value, parse_json_web_key, resolve_crypto_key_promise,
};
use crate::crypto::crypto_key::{CryptoKey, KeyType, KeyUsage};

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AlgorithmIdentifier {
    Object(Gc<Object>),
    DOMString(Utf16String),
}

pub(crate) enum KeyDataType {
    BufferSource(BufferSource),
    JsonWebKey(JsonWebKey),
}

pub(crate) enum CryptoKeyOrCryptoKeyPair {
    CryptoKey(Ref<CryptoKey>),
    CryptoKeyPair(CryptoKeyPair),
}

pub(crate) enum ArrayBufferOrJsonWebKey {
    ArrayBuffer(Gc<libjs_rust::api::ArrayBuffer>),
    JsonWebKey(JsonWebKey),
}

#[derive(Clone, Copy, PartialEq, Eq, Trace)]
#[gc(crate = libjs_rust)]
#[repr(u8)]
pub(crate) enum KeyFormat {
    RawPublic,
    RawPrivate,
    RawSeed,
    RawSecret,
    Raw,
    Spki,
    Pkcs8,
    Jwk,
}

#[derive(Clone, Copy)]
enum RegisteredAlgorithm {
    Sha,
    Hmac,
    HmacKeyGen,
    HmacImport,
    Kmac,
    KmacSignOrVerify,
    KmacKeyGen,
    KmacImport,
    Hkdf,
    HkdfParams,
    Pbkdf2,
    Pbkdf2Params,
    Argon2,
    Argon2Params,
    AesCbc(AesParameterType),
    AesCtr(AesParameterType),
    AesGcm(AesParameterType),
    AesKw(AesParameterType),
    AesOcb(AesParameterType),
    ChaCha20Poly1305,
    ChaCha20Poly1305Aead,
    RsaOaep(RsaParameterType),
    RsaPss(RsaParameterType),
    RsassaPkcs1(RsaParameterType),
    Ecdsa(EcParameterType),
    Ecdh(EcParameterType),
    Ed25519,
    Ed448(bool),
    MlDsa(bool),
    MlKem,
    X25519(bool),
    X448(bool),
    CShake,
}

type RegisteredAlgorithms = HashMap<&'static str, RegisteredAlgorithm>;
type SupportedAlgorithmsMap = HashMap<&'static str, RegisteredAlgorithms>;

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
enum AlgorithmImplementation {
    Sha(Sha),
    Hmac(Hmac),
    HmacKeyGen(HmacKeyGenParams),
    HmacImport(HmacImportParams),
    Kmac(Kmac),
    KmacSignOrVerify(KmacParams),
    KmacKeyGen(KmacKeyGenParams),
    KmacImport(KmacImportParams),
    Hkdf(Hkdf),
    HkdfParams(HkdfParams),
    Pbkdf2(Pbkdf2),
    Pbkdf2Params(Pbkdf2Params),
    Argon2(Argon2),
    Argon2Params(Argon2Params),
    AesCbc(AesCbc),
    AesCtr(AesCtr),
    AesGcm(AesGcm),
    AesKw(AesKw),
    AesOcb(AesOcb),
    ChaCha20Poly1305(ChaCha20Poly1305),
    RsaOaep(RsaOaep),
    RsaPss(RsaPss),
    RsassaPkcs1(RsassaPkcs1),
    Ecdsa(Ecdsa),
    Ecdh(Ecdh),
    Ed25519(Ed25519),
    Ed448(Ed448),
    MlDsa(MlDsa),
    MlKem(MlKem),
    X25519(X25519),
    X448(X448),
    CShake(CShakeParams),
}

impl Deref for AlgorithmImplementation {
    type Target = dyn AlgorithmMethods;

    fn deref(&self) -> &Self::Target {
        match self {
            Self::Sha(algorithm) => algorithm,
            Self::Hmac(algorithm) => algorithm,
            Self::HmacKeyGen(algorithm) => algorithm,
            Self::HmacImport(algorithm) => algorithm,
            Self::Kmac(algorithm) => algorithm,
            Self::KmacSignOrVerify(algorithm) => algorithm,
            Self::KmacKeyGen(algorithm) => algorithm,
            Self::KmacImport(algorithm) => algorithm,
            Self::Hkdf(algorithm) => algorithm,
            Self::HkdfParams(algorithm) => algorithm,
            Self::Pbkdf2(algorithm) => algorithm,
            Self::Pbkdf2Params(algorithm) => algorithm,
            Self::Argon2(algorithm) => algorithm,
            Self::Argon2Params(algorithm) => algorithm,
            Self::AesCbc(algorithm) => algorithm,
            Self::AesCtr(algorithm) => algorithm,
            Self::AesGcm(algorithm) => algorithm,
            Self::AesKw(algorithm) => algorithm,
            Self::AesOcb(algorithm) => algorithm,
            Self::ChaCha20Poly1305(algorithm) => algorithm,
            Self::RsaOaep(algorithm) => algorithm,
            Self::RsaPss(algorithm) => algorithm,
            Self::RsassaPkcs1(algorithm) => algorithm,
            Self::Ecdsa(algorithm) => algorithm,
            Self::Ecdh(algorithm) => algorithm,
            Self::Ed25519(algorithm) => algorithm,
            Self::Ed448(algorithm) => algorithm,
            Self::MlDsa(algorithm) => algorithm,
            Self::MlKem(algorithm) => algorithm,
            Self::X25519(algorithm) => algorithm,
            Self::X448(algorithm) => algorithm,
            Self::CShake(algorithm) => algorithm,
        }
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct NormalizedAlgorithmAndParameter {
    pub(crate) parameter: AlgorithmParams,
    methods: AlgorithmImplementation,
}

// NB: Getting a copy of the bytes held by a buffer source can only fail when allocating the copy fails.
pub(super) fn get_a_copy_of_the_bytes(vm: &Vm, buffer_source: BufferSource) -> WebIdlResult<Bytes> {
    // SAFETY: Copying the bytes neither runs script nor allocates a cell.
    let data = unsafe {
        buffer_source.with_bytes(|bytes| {
            let mut data = Vec::new();
            data.try_reserve_exact(bytes.len())?;
            data.extend_from_slice(bytes);
            Ok::<_, std::collections::TryReserveError>(data)
        })
    };
    match data {
        Ok(bytes) => Ok(Bytes::new(bytes)),
        Err(_) => vm
            .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            .map_err(Into::into),
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SubtleCrypto;

impl Wrappable for SubtleCrypto {}

fn export_key_result_to_js_value(vm: &Vm, realm: Gc<Realm>, result: ExportKeyResult) -> WebIdlResult<Value> {
    let result = match result {
        ExportKeyResult::Bytes(bytes) => ArrayBufferOrJsonWebKey::ArrayBuffer(ArrayBuffer::create_from_bytes(
            vm,
            realm,
            bytes.as_slice(),
            ArrayBufferShared::No,
        )),
        ExportKeyResult::JsonWebKey(key) => ArrayBufferOrJsonWebKey::JsonWebKey(key),
    };
    #[cfg(web_idl_bindings)]
    return result.to_js(&CallContext::new(vm, realm)).map_err(Into::into);
    #[cfg(not(web_idl_bindings))]
    crate::crypto::crypto_bindings::bindings_unavailable(result)
}

#[cfg_attr(not(web_idl_bindings), allow(clippy::unnecessary_wraps))]
fn export_key_result_to_bytes(vm: &Vm, result: ExportKeyResult) -> WebIdlResult<Bytes> {
    if let ExportKeyResult::Bytes(bytes) = result {
        return Ok(bytes);
    }
    let ExportKeyResult::JsonWebKey(key) = result else {
        unreachable!()
    };
    #[cfg(web_idl_bindings)]
    let json = key.to_json(vm)?;
    #[cfg(not(web_idl_bindings))]
    let json: Utf16String = crate::crypto::crypto_bindings::bindings_unavailable((vm, key));
    Ok(Bytes::new(String::from_utf16_lossy(&json.to_utf16()).into_bytes()))
}

pub(super) fn dom_error(name: &'static str, message: &[u8]) -> Exception {
    Exception::Dom(name, Utf16String::from_ascii(message))
}

fn create_rejected_promise_from_exception(vm: &Vm, realm: Gc<Realm>, exception: Exception) -> WebIdlPromise {
    with_temporary_execution_context(realm, || {
        let reason = exception.throw(&CallContext::new(vm, realm)).value();
        create_rejected_promise(vm, realm, reason)
    })
}

fn queue_crypto_rejection(vm: &Vm, realm: Gc<Realm>, promise: WebIdlPromise, exception: Exception) {
    let value = exception.throw(&CallContext::new(vm, realm)).value();
    queue_a_global_task(
        vm,
        realm.global_object(),
        TaskSource::Crypto,
        (realm, promise, value),
        |vm, (realm, promise, value)| {
            with_temporary_execution_context_and_callbacks(realm, || {
                reject_promise(vm, promise, value);
            });
        },
    );
}

fn reject_promise_with_exception(vm: &Vm, promise: WebIdlPromise, exception: Exception) {
    let realm = promise_realm(promise);
    with_temporary_execution_context(realm, || {
        let reason = exception.throw(&CallContext::new(vm, realm)).value();
        reject_promise(vm, promise, reason);
    });
}

// https://w3c.github.io/webcrypto/#dfn-normalize-an-algorithm
pub(crate) fn normalize_an_algorithm(
    vm: &Vm,
    realm: Gc<Realm>,
    algorithm: &AlgorithmIdentifier,
    operation: &str,
) -> WebIdlResult<NormalizedAlgorithmAndParameter> {
    // If alg is an instance of a DOMString:
    if let AlgorithmIdentifier::DOMString(algorithm) = algorithm {
        // Return the result of running the normalize an algorithm algorithm,
        // with the alg set to a new Algorithm dictionary whose name attribute is alg, and with the op set to op.
        let dictionary = create_algorithm_dictionary(vm, realm, algorithm)?;
        return normalize_an_algorithm(vm, realm, &AlgorithmIdentifier::Object(dictionary), operation);
    }
    let AlgorithmIdentifier::Object(algorithm) = algorithm else {
        unreachable!()
    };

    // If alg is an object:
    // 1. Let registeredAlgorithms be the associative container stored at the op key of supportedAlgorithms.
    let internal_object = supported_algorithms();
    let maybe_registered_algorithms = internal_object.get(operation);

    // NB: supports() can normalize with op set to "deriveKey", for which there is no container and thus no registered
    //     algorithm.
    let Some(registered_algorithms) = maybe_registered_algorithms else {
        return Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8(&format!("Operation '{operation}' is not supported")),
        ));
    };

    // 2. Let initialAlg be the result of converting the ECMAScript object represented by alg to the IDL dictionary type
    //    Algorithm, as defined by [WebIDL].
    // 3. If an error occurred, return the error and terminate this algorithm.
    // Note: We're not going to bother creating an Algorithm object, all we want is the name attribute so that we can
    //       fetch the actual algorithm factory from the registeredAlgorithms map.
    let initial_algorithm = algorithm.get(vm, &PropertyKey::from_utf8("name"))?;

    if initial_algorithm.is_undefined() {
        return vm
            .throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Algorithm"])
            .map_err(Into::into);
    }

    // 4. Let algName be the value of the name attribute of initialAlg.
    let mut algorithm_name = initial_algorithm.to_utf16_string(vm)?;

    let mut desired_type = None;

    // 5. If registeredAlgorithms contains a key that is a case-insensitive string match for algName:
    let algorithm_name_utf8 = String::from_utf16_lossy(&algorithm_name.to_utf16());
    for (&key, &registered_algorithm) in registered_algorithms {
        if !key.eq_ignore_ascii_case(&algorithm_name_utf8) {
            continue;
        }

        // 1. Set algName to the value of the matching key.
        algorithm_name = Utf16String::from_ascii(key.as_bytes());

        // 2. Let desiredType be the IDL dictionary type stored at algName in registeredAlgorithms.
        desired_type = Some(registered_algorithm);
        break;
    }

    let Some(desired_type) = desired_type else {
        // Otherwise:
        // Return a new NotSupportedError and terminate this algorithm.
        return Err(Exception::Dom(
            "NotSupportedError",
            libjs_rust::api::utf16_formatted(
                "Algorithm '{}' is not supported for operation '{}'",
                &[&algorithm_name, &operation],
            ),
        ));
    };

    // 6. Let normalizedAlgorithm be the result of converting the ECMAScript object represented by alg to the IDL
    //    dictionary type desiredType, as defined by [WebIDL].
    // 8. If an error occurred, return the error and terminate this algorithm.
    // 9. Let dictionaries be a list consisting of the IDL dictionary type desiredType and all of desiredType's
    //    inherited dictionaries, in order from least to most derived.
    // 10. For each dictionary dictionary in dictionaries:
    //    1. For each dictionary member member declared on dictionary, in order:
    //       1. Let key be the identifier of member.
    //       2. Let idlValue be the value of the dictionary member with key name of key on normalizedAlgorithm.
    //          3. If member is of the type BufferSource and is present:
    //             Set the dictionary member on normalizedAlgorithm with key name key to the result of getting a copy of
    //             the bytes held by idlValue, replacing the current value.
    //          3. If member is of the type HashAlgorithmIdentifier:
    //             Set the dictionary member on normalizedAlgorithm with key name key to the result of normalizing an
    //             algorithm, with the alg set to idlValue and the op set to "digest".
    //          3. If member is of the type AlgorithmIdentifier:
    //             Set the dictionary member on normalizedAlgorithm with key name key to the result of normalizing an
    //             algorithm, with the alg set to idlValue and the op set to the operation defined by the specification
    //             that defines the algorithm identified by algName.
    //       4. If an error occurred, return the error and terminate this algorithm.
    // NB: The registered parameter converters normalize their own members.
    let methods = match desired_type {
        RegisteredAlgorithm::Sha => AlgorithmImplementation::Sha(Sha),
        RegisteredAlgorithm::Hmac => AlgorithmImplementation::Hmac(Hmac),
        RegisteredAlgorithm::HmacKeyGen => {
            AlgorithmImplementation::HmacKeyGen(HmacKeyGenParams::from_value(vm, realm, *algorithm)?)
        }
        RegisteredAlgorithm::HmacImport => {
            AlgorithmImplementation::HmacImport(HmacImportParams::from_value(vm, realm, *algorithm)?)
        }
        RegisteredAlgorithm::Kmac => AlgorithmImplementation::Kmac(Kmac),
        RegisteredAlgorithm::KmacSignOrVerify => {
            AlgorithmImplementation::KmacSignOrVerify(KmacParams::from_value(vm, *algorithm)?)
        }
        RegisteredAlgorithm::KmacKeyGen => {
            AlgorithmImplementation::KmacKeyGen(KmacKeyGenParams::from_value(vm, *algorithm)?)
        }
        RegisteredAlgorithm::KmacImport => {
            AlgorithmImplementation::KmacImport(KmacImportParams::from_value(vm, *algorithm)?)
        }
        RegisteredAlgorithm::Hkdf => AlgorithmImplementation::Hkdf(Hkdf),
        RegisteredAlgorithm::HkdfParams => {
            AlgorithmImplementation::HkdfParams(HkdfParams::from_value(vm, realm, *algorithm)?)
        }
        RegisteredAlgorithm::Pbkdf2 => AlgorithmImplementation::Pbkdf2(Pbkdf2),
        RegisteredAlgorithm::Pbkdf2Params => {
            AlgorithmImplementation::Pbkdf2Params(Pbkdf2Params::from_value(vm, realm, *algorithm)?)
        }
        RegisteredAlgorithm::Argon2 => AlgorithmImplementation::Argon2(Argon2),
        RegisteredAlgorithm::Argon2Params => {
            AlgorithmImplementation::Argon2Params(Argon2Params::from_value(vm, *algorithm)?)
        }
        RegisteredAlgorithm::AesCbc(parameter_type) => {
            AlgorithmImplementation::AesCbc(AesCbc::from_value(vm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::AesCtr(parameter_type) => {
            AlgorithmImplementation::AesCtr(AesCtr::from_value(vm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::AesGcm(parameter_type) => {
            AlgorithmImplementation::AesGcm(AesGcm::from_value(vm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::AesKw(parameter_type) => {
            AlgorithmImplementation::AesKw(AesKw::from_value(vm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::AesOcb(parameter_type) => {
            AlgorithmImplementation::AesOcb(AesOcb::from_value(vm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::ChaCha20Poly1305 => {
            AlgorithmImplementation::ChaCha20Poly1305(ChaCha20Poly1305::from_value(vm, *algorithm, false)?)
        }
        RegisteredAlgorithm::ChaCha20Poly1305Aead => {
            AlgorithmImplementation::ChaCha20Poly1305(ChaCha20Poly1305::from_value(vm, *algorithm, true)?)
        }
        RegisteredAlgorithm::RsaOaep(parameter_type) => {
            AlgorithmImplementation::RsaOaep(RsaOaep::from_value(vm, realm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::RsaPss(parameter_type) => {
            AlgorithmImplementation::RsaPss(RsaPss::from_value(vm, realm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::RsassaPkcs1(parameter_type) => {
            AlgorithmImplementation::RsassaPkcs1(RsassaPkcs1::from_value(vm, realm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::Ecdsa(parameter_type) => {
            AlgorithmImplementation::Ecdsa(Ecdsa::from_value(vm, realm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::Ecdh(parameter_type) => {
            AlgorithmImplementation::Ecdh(Ecdh::from_value(vm, realm, *algorithm, parameter_type)?)
        }
        RegisteredAlgorithm::Ed25519 => AlgorithmImplementation::Ed25519(Ed25519),
        RegisteredAlgorithm::Ed448(has_parameters) => {
            AlgorithmImplementation::Ed448(Ed448::from_value(vm, *algorithm, has_parameters)?)
        }
        RegisteredAlgorithm::MlDsa(has_parameters) => {
            AlgorithmImplementation::MlDsa(MlDsa::from_value(vm, *algorithm, has_parameters)?)
        }
        RegisteredAlgorithm::MlKem => AlgorithmImplementation::MlKem(MlKem),
        RegisteredAlgorithm::X25519(has_parameters) => {
            AlgorithmImplementation::X25519(X25519::from_value(vm, *algorithm, has_parameters)?)
        }
        RegisteredAlgorithm::X448(has_parameters) => {
            AlgorithmImplementation::X448(X448::from_value(vm, *algorithm, has_parameters)?)
        }
        RegisteredAlgorithm::CShake => AlgorithmImplementation::CShake(CShakeParams::from_value(vm, *algorithm)?),
    };

    // 7. Set the name attribute of normalizedAlgorithm to algName.
    let normalized_algorithm = NormalizedAlgorithmAndParameter {
        methods,
        parameter: AlgorithmParams { name: algorithm_name },
    };

    // 11. Return normalizedAlgorithm.
    Ok(normalized_algorithm)
}

#[allow(
    clippy::too_many_arguments,
    clippy::unnecessary_wraps,
    reason = "the generated bindings pass the IDL arguments and can propagate an exception"
)]
impl SubtleCrypto {
    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-encrypt
    pub(crate) fn encrypt(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        key: Ref<CryptoKey>,
        data_parameter: BufferSource,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let algorithm and key be the algorithm and key parameters passed to the encrypt() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "encrypt".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "encrypt");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let data be the result of getting a copy of the bytes held by the data parameter passed to the encrypt() method.
        let data_or_error = get_a_copy_of_the_bytes(vm, data_parameter);
        if let Err(error) = data_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let data = data_or_error.must();

        // 5. Let realm be the relevant realm of this.

        // 6. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 7. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_algorithm.must(), promise, key, data),
            |vm, (realm, normalized_algorithm, promise, key, data)| {
                with_temporary_execution_context(realm, || {
                    // 8. If the following steps or referenced procedures say to throw an error, queue a global task on
                    //    the crypto task source, given realm's global object, to reject promise with the returned
                    //    error; and then terminate the algorithm.
                    // 9. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch"),
                        );
                        return;
                    }

                    // 10. If the [[usages]] internal slot of key does not contain an entry that is "encrypt", then throw an InvalidAccessError.
                    if !key.internal_usages().contains(&KeyUsage::Encrypt) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support encryption"),
                        );
                        return;
                    }

                    // 11. Let ciphertext be the result of performing the encrypt operation specified by normalizedAlgorithm using algorithm and key and with data as plaintext.
                    let cipher_text = normalized_algorithm
                        .methods
                        .encrypt(&CallContext::new(vm, realm), key, &data);
                    if let Err(error) = cipher_text {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }

                    // 12. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, cipher_text.must()),
                        |vm, (realm, promise, cipher_text_bytes)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 13. Let result be the result of creating an ArrayBuffer in realm, containing ciphertext.
                                // 14. Resolve promise with result.
                                resolve_promise(vm, promise, array_buffer(vm, realm, &cipher_text_bytes));
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-decrypt
    pub(crate) fn decrypt(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        key: Ref<CryptoKey>,
        data_parameter: BufferSource,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let algorithm and key be the algorithm and key parameters passed to the decrypt() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "decrypt".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "decrypt");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let data be the result of getting a copy of the bytes held by the data parameter passed to the decrypt() method.
        let data_or_error = get_a_copy_of_the_bytes(vm, data_parameter);
        if let Err(error) = data_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let data = data_or_error.must();

        // 5. Let realm be the relevant realm of this.

        // 6. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 7. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_algorithm.must(), promise, key, data),
            |vm, (realm, normalized_algorithm, promise, key, data)| {
                with_temporary_execution_context(realm, || {
                    // 8. If the following steps or referenced procedures say to throw an error, queue a global task on
                    //    the crypto task source, given realm's global object, to reject promise with the returned
                    //    error; and then terminate the algorithm.
                    // 9. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch"),
                        );
                        return;
                    }

                    // 10. If the [[usages]] internal slot of key does not contain an entry that is "decrypt", then throw an InvalidAccessError.
                    if !key.internal_usages().contains(&KeyUsage::Decrypt) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support decryption"),
                        );
                        return;
                    }

                    // 11. Let plaintext be the result of performing the decrypt operation specified by normalizedAlgorithm using key and algorithm and with data as ciphertext.
                    let plain_text = normalized_algorithm
                        .methods
                        .decrypt(&CallContext::new(vm, realm), key, &data);
                    if let Err(error) = plain_text {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }

                    // 12. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, plain_text.must()),
                        |vm, (realm, promise, plain_text_bytes)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 13. Let result be the result of creating an ArrayBuffer in realm, containing plaintext.
                                // 14. Resolve promise with result.
                                resolve_promise(vm, promise, array_buffer(vm, realm, &plain_text_bytes));
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-digest
    pub(crate) fn digest(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        data: BufferSource,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let algorithm be the algorithm parameter passed to the digest() method.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "digest".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "digest");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        // FIXME: Spec bug: link to https://webidl.spec.whatwg.org/#a-promise-rejected-with
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let data be the result of getting a copy of the bytes held by the data parameter passed to the digest() method.
        let data_buffer_or_error = get_a_copy_of_the_bytes(vm, data);
        if let Err(error) = data_buffer_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let data_buffer = data_buffer_or_error.must();

        // 5. Let realm be the relevant realm of this.

        // 6. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 7. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_algorithm.must(), promise, data_buffer),
            |vm, (realm, algorithm_object, promise, data_buffer)| {
                with_temporary_execution_context(realm, || {
                    // 8. If the following steps or referenced procedures say to throw an error, queue a global task on
                    //    the crypto task source, given realm's global object, to reject promise with the returned
                    //    error; and then terminate the algorithm.
                    // 9. Let digest be the result of performing the digest operation specified by normalizedAlgorithm using algorithm, with data as message.
                    let digest = algorithm_object
                        .methods
                        .digest(realm, &algorithm_object.parameter, &data_buffer);

                    if let Err(error) = digest {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }

                    // 10. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, digest.must()),
                        |vm, (realm, promise, digest_bytes)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 11. Let result be the result of creating an ArrayBuffer in realm, containing digest.
                                // 12. Resolve promise with result.
                                resolve_promise(vm, promise, array_buffer(vm, realm, &digest_bytes));
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-generateKey
    pub(crate) fn generate_key(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        extractable: bool,
        key_usages: Vec<KeyUsage>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());
        // 1. Let algorithm, extractable and usages be the algorithm, extractable and keyUsages
        //    parameters passed to the generateKey() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm,
        //    with alg set to algorithm and op set to "generateKey".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "generateKey");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let realm be the relevant realm of this.
        // 5. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 6. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_algorithm.must(), promise, extractable, key_usages),
            |vm, (realm, normalized_algorithm, promise, extractable, key_usages)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // FIXME: 7. If the following steps or referenced procedures say to throw an error, queue a global
                    //    task on the crypto task source, given realm's global object, to reject promise with the
                    //    returned error; and then terminate the algorithm.
                    // NB: Errors are rejected directly in the deferred invocation.

                    // 8. Let result be the result of performing the generate key operation specified by
                    //    normalizedAlgorithm using algorithm, extractable and usages.
                    let result_or_error = normalized_algorithm.methods.generate_key(
                        &CallContext::new(vm, realm),
                        &normalized_algorithm.parameter,
                        extractable,
                        &key_usages,
                    );

                    if let Err(error) = result_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }
                    let result = result_or_error.must();

                    match result {
                        // 9. If result is a CryptoKey object:
                        //    If the [[type]] internal slot of result is "secret" or "private" and usages is empty,
                        //    then throw a SyntaxError.
                        CryptoKeyOrCryptoKeyPair::CryptoKey(key) => {
                            if (key.key_type() == KeyType::Secret || key.key_type() == KeyType::Private)
                                && key_usages.is_empty()
                            {
                                reject_promise(
                                    vm,
                                    promise,
                                    dom_error("SyntaxError", b"usages must not be empty")
                                        .throw(&CallContext::new(vm, realm))
                                        .value(),
                                );
                                return;
                            }
                            // FIXME: 10. Queue a global task on the crypto task source, given realm's global object, to
                            //    perform the remaining steps.
                            // 11. Let result be the result of converting result to an ECMAScript Object in realm, as
                            //    defined by [WebIDL].
                            // 12. Resolve promise with result.
                            resolve_crypto_key_promise(vm, realm, promise, key);
                        }
                        // 9. If result is a CryptoKeyPair object:
                        //    If the [[usages]] internal slot of the privateKey attribute of result is the empty sequence,
                        //    then throw a SyntaxError.
                        CryptoKeyOrCryptoKeyPair::CryptoKeyPair(key_pair) => {
                            if key_pair
                                .private_key
                                .expect("a generated pair has a private key")
                                .internal_usages()
                                .is_empty()
                            {
                                reject_promise(
                                    vm,
                                    promise,
                                    dom_error("SyntaxError", b"usages must not be empty")
                                        .throw(&CallContext::new(vm, realm))
                                        .value(),
                                );
                                return;
                            }
                            // FIXME: 10. Queue a global task on the crypto task source, given realm's global object, to
                            //    perform the remaining steps.
                            // 11. Let result be the result of converting result to an ECMAScript Object in realm, as
                            //    defined by [WebIDL].
                            let object = crypto_key_pair(vm, realm, &key_pair);
                            if let Err(error) = object {
                                reject_promise_with_exception(vm, promise, error.into());
                                return;
                            }
                            // 12. Resolve promise with result.
                            resolve_promise(vm, promise, object.must());
                        }
                    }
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-importKey
    pub(crate) fn import_key(
        &self,
        cx: &mut CallContext,
        format: KeyFormat,
        key_data: KeyDataType,
        algorithm: AlgorithmIdentifier,
        extractable: bool,
        key_usages: Vec<KeyUsage>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());
        // 1. Let format, algorithm, extractable and usages, be the format, algorithm, extractable and keyUsages
        //    parameters passed to the importKey() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "importKey".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "importKey");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. If format is equal to the string "jwk":
        let real_key_data = if format == KeyFormat::Jwk {
            // 1. If the keyData parameter passed to the importKey() method is not a JsonWebKey dictionary, throw a TypeError.
            if !matches!(&key_data, KeyDataType::JsonWebKey(_)) {
                return Ok(create_rejected_promise_from_exception(
                    vm,
                    realm,
                    Exception::from(
                        vm.throw_completion::<()>(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"JsonWebKey"])
                            .expect_err("a throw completion is an error"),
                    ),
                ));
            }

            // 2. Let keyData be the keyData parameter passed to the importKey() method.
            let KeyDataType::JsonWebKey(key) = key_data else {
                unreachable!()
            };
            ImportKeyData::JsonWebKey(key)
        }
        // Otherwise:
        else {
            // 1. If the keyData parameter passed to the importKey() method is a JsonWebKey dictionary, throw a TypeError.
            if matches!(&key_data, KeyDataType::JsonWebKey(_)) {
                return Ok(create_rejected_promise_from_exception(
                    vm,
                    realm,
                    Exception::from(
                        vm.throw_completion::<()>(
                            ErrorKind::TypeError,
                            ErrorType::NotAnObjectOfType,
                            &[&"BufferSource"],
                        )
                        .expect_err("a throw completion is an error"),
                    ),
                ));
            }

            // 2. Let keyData be the result of getting a copy of the bytes held by the keyData parameter passed to the
            //    importKey() method.
            let KeyDataType::BufferSource(buffer_source) = key_data else {
                unreachable!()
            };
            let key_data_or_error = get_a_copy_of_the_bytes(vm, buffer_source);
            if let Err(error) = key_data_or_error {
                return Ok(create_rejected_promise_from_exception(vm, realm, error));
            }
            ImportKeyData::Bytes(key_data_or_error.must())
        };

        // 5. Let realm be the relevant realm of this.

        // 6. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 7. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                real_key_data,
                normalized_algorithm.must(),
                promise,
                format,
                extractable,
                key_usages,
                algorithm,
            ),
            |vm, (realm, real_key_data, normalized_algorithm, promise, format, extractable, key_usages, _algorithm)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // FIXME: 8. If the following steps or referenced procedures say to throw an error, queue a global task on the
                    //    crypto task source, given realm's global object, to reject promise with the returned error; and then
                    //    terminate the algorithm.

                    // 9. Let result be the CryptoKey object that results from performing the import key operation
                    //    specified by normalizedAlgorithm using keyData, algorithm, format, extractable and usages.
                    let maybe_result = normalized_algorithm.methods.import_key(
                        &CallContext::new(vm, realm),
                        &normalized_algorithm.parameter,
                        format,
                        real_key_data,
                        extractable,
                        &key_usages,
                    );
                    if let Err(error) = maybe_result {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }
                    let result = maybe_result.must();

                    // 10. If the [[type]] internal slot of result is "secret" or "private" and usages is empty, then throw a SyntaxError.
                    if (result.key_type() == KeyType::Secret || result.key_type() == KeyType::Private)
                        && key_usages.is_empty()
                    {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("SyntaxError", b"usages must not be empty")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 11. Set the [[extractable]] internal slot of result to extractable.
                    result.set_extractable(extractable);

                    // 12. Set the [[usages]] internal slot of result to the normalized value of usages.
                    result.set_usages(normalized_key_usages(&key_usages));

                    // FIXME: 13. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    // FIXME: 14. Let result be the result of converting result to an ECMAScript Object in realm, as defined by [WebIDL].

                    // 15. Resolve promise with result.
                    resolve_crypto_key_promise(vm, realm, promise, result);
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-exportKey
    pub(crate) fn export_key(
        &self,
        cx: &mut CallContext,
        format: KeyFormat,
        key: Ref<CryptoKey>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());
        // 1. Let format and key be the format and key parameters passed to the exportKey() method, respectively.

        // 2. Let realm be the relevant realm of this.
        // 3. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 4. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, key, promise, format),
            |vm, (realm, key, promise, format)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // FIXME: 5. If the following steps or referenced procedures say to throw an error, queue a global
                    //    task on the crypto task source, given realm's global object, to reject promise with the
                    //    returned error; and then terminate the algorithm.
                    // NB: Errors are rejected directly in the deferred invocation.

                    // 6. If the name member of the [[algorithm]] internal slot of key does not identify a registered
                    //    algorithm that supports the export key operation, then throw a NotSupportedError.
                    // Note: Handled by the base AlgorithmMethods implementation
                    // FIXME: Stash the AlgorithmMethods on the KeyAlgorithm
                    let normalized_algorithm_or_error = normalize_an_algorithm(
                        vm,
                        realm,
                        &AlgorithmIdentifier::DOMString(key.algorithm_name()),
                        "exportKey",
                    );
                    if let Err(error) = normalized_algorithm_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }
                    let normalized_algorithm = normalized_algorithm_or_error.must();

                    // 7. If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError.
                    if !key.is_extractable() {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Key is not extractable")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 8. Let result be the result of performing the export key operation specified by the [[algorithm]]
                    //    internal slot of key using key and format.
                    let result_or_error =
                        normalized_algorithm
                            .methods
                            .export_key(&CallContext::new(vm, realm), format, key);
                    if let Err(error) = result_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    // FIXME: 9. Queue a global task on the crypto task source, given realm's global object, to perform
                    //    the remaining steps.
                    // 10. If format is equal to the string "jwk":
                    //     Let result be the result of converting result to an ECMAScript Object in realm, as defined by [WebIDL].
                    // 10. Otherwise:
                    //     Let result be the result of creating an ArrayBuffer in realm, containing result.
                    let result = export_key_result_to_js_value(vm, realm, result_or_error.must());
                    if let Err(error) = result {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }
                    // 11. Resolve promise with result.
                    resolve_promise(vm, promise, result.must());
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-sign
    pub(crate) fn sign(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        key: Ref<CryptoKey>,
        data_parameter: BufferSource,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let algorithm and key be the algorithm and key parameters passed to the sign() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "sign".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "sign");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let data be the result of getting a copy of the bytes held by the data parameter passed to the sign() method.
        let data_or_error = get_a_copy_of_the_bytes(vm, data_parameter);
        if let Err(error) = data_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let data = data_or_error.must();

        // 5. Let realm be the relevant realm of this.

        // 6. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 7. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_algorithm.must(), promise, key, data),
            |vm, (realm, normalized_algorithm, promise, key, data)| {
                with_temporary_execution_context(realm, || {
                    // 8. If the following steps or referenced procedures say to throw an error, queue a global task on the
                    //    crypto task source, given realm's global object, to reject promise with the returned error; and then
                    //    terminate the algorithm.
                    // 9. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]]
                    //    internal slot of key then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch"),
                        );
                        return;
                    }

                    // 10. If the [[usages]] internal slot of key does not contain an entry that is "sign", then throw an InvalidAccessError.
                    if !key.internal_usages().contains(&KeyUsage::Sign) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support signing"),
                        );
                        return;
                    }

                    // 11. Let signature be the result of performing the sign operation specified by normalizedAlgorithm using key
                    //     and algorithm and with data as message.
                    let signature = normalized_algorithm.methods.sign(
                        &CallContext::new(vm, realm),
                        &normalized_algorithm.parameter,
                        key,
                        &data,
                    );
                    if let Err(error) = signature {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }

                    // 12. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, signature.must()),
                        |vm, (realm, promise, signature_bytes)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 13. Let result be the result of creating an ArrayBuffer in realm, containing signature.
                                // 14. Resolve promise with result.
                                resolve_promise(vm, promise, array_buffer(vm, realm, &signature_bytes));
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-verify
    pub(crate) fn verify(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        key: Ref<CryptoKey>,
        signature_data: BufferSource,
        data_parameter: BufferSource,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let algorithm and key be the algorithm and key parameters passed to the verify() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "verify".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "verify");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let signature be the result of getting a copy of the bytes held by the signature parameter passed to the verify() method.
        let signature_or_error = get_a_copy_of_the_bytes(vm, signature_data);
        if let Err(error) = signature_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let signature = signature_or_error.must();

        // 5. Let data be the result of getting a copy of the bytes held by the data parameter passed to the verify() method.
        let data_or_error = get_a_copy_of_the_bytes(vm, data_parameter);
        if let Err(error) = data_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let data = data_or_error.must();

        // 6. Let realm be the relevant realm of this.

        // 7. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 8. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_algorithm.must(), promise, key, signature, data),
            |vm, (realm, normalized_algorithm, promise, key, signature, data)| {
                with_temporary_execution_context(realm, || {
                    // 9. If the following steps or referenced procedures say to throw an error, queue a global task on the
                    //    crypto task source, given realm's global object, to reject promise with the returned error; and then
                    //    terminate the algorithm.
                    // 10. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]]
                    //     internal slot of key then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch"),
                        );
                        return;
                    }

                    // 11. If the [[usages]] internal slot of key does not contain an entry that is "verify", then throw an InvalidAccessError.
                    if !key.internal_usages().contains(&KeyUsage::Verify) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support verification"),
                        );
                        return;
                    }

                    // 12. Let result be the result of performing the verify operation specified by normalizedAlgorithm using key,
                    //     algorithm and signature and with data as message.
                    let result = normalized_algorithm.methods.verify(
                        &CallContext::new(vm, realm),
                        &normalized_algorithm.parameter,
                        key,
                        &signature,
                        &data,
                    );
                    if let Err(error) = result {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }

                    // 13. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, result.must()),
                        |vm, (realm, promise, verification_result)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 14. Resolve promise with result.
                                resolve_promise(vm, promise, Value::from_bool(verification_result));
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-deriveBits
    pub(crate) fn derive_bits(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        base_key: Ref<CryptoKey>,
        length_optional: Option<u32>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());
        // 1. Let algorithm, baseKey and length, be the algorithm, baseKey and length parameters passed to the deriveBits() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "deriveBits".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "deriveBits");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let realm be the relevant realm of this.
        // 5. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 6. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_algorithm.must(), promise, base_key, length_optional),
            |vm, (realm, normalized_algorithm, promise, base_key, length_optional)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // FIXME: 7. If the following steps or referenced procedures say to throw an error, queue a global
                    //    task on the crypto task source, given realm's global object, to reject promise with the
                    //    returned error; and then terminate the algorithm.
                    // NB: Errors are rejected directly in the deferred invocation.

                    // 8. If the name member of normalizedAlgorithm is not equal to the name attribute of the
                    //    [[algorithm]] internal slot of baseKey then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != base_key.algorithm_name() {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 9. If the [[usages]] internal slot of baseKey does not contain an entry that is "deriveBits",
                    //    then throw an InvalidAccessError.
                    if !base_key.internal_usages().contains(&KeyUsage::DeriveBits) {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support deriving bits")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 10. Let bits be the result of performing the derive bits operation specified by
                    //    normalizedAlgorithm using baseKey, algorithm and length.
                    let result = normalized_algorithm.methods.derive_bits(
                        &CallContext::new(vm, realm),
                        &normalized_algorithm.parameter,
                        base_key,
                        length_optional,
                    );
                    if let Err(error) = result {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    // FIXME: 11. Queue a global task on the crypto task source, given realm's global object, to perform
                    //    the remaining steps.
                    // 12. Let result be the result of creating an ArrayBuffer in realm, containing bits.
                    // 13. Resolve promise with result.
                    resolve_promise(vm, promise, array_buffer(vm, realm, &result.must()));
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-deriveKey
    pub(crate) fn derive_key(
        &self,
        cx: &mut CallContext,
        algorithm: AlgorithmIdentifier,
        base_key: Ref<CryptoKey>,
        derived_key_type: AlgorithmIdentifier,
        extractable: bool,
        key_usages: Vec<KeyUsage>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());
        // 1. Let algorithm, baseKey, derivedKeyType, extractable and usages be the algorithm, baseKey, derivedKeyType, extractable and keyUsages parameters passed to the deriveKey() method, respectively.

        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "deriveBits".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "deriveBits");

        // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if let Err(error) = normalized_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let normalizedDerivedKeyAlgorithmImport be the result of normalizing an algorithm, with alg set to derivedKeyType and op set to "importKey".
        let normalized_derived_key_algorithm_import = normalize_an_algorithm(vm, realm, &derived_key_type, "importKey");

        // 5. If an error occurred, return a Promise rejected with normalizedDerivedKeyAlgorithmImport.
        if let Err(error) = normalized_derived_key_algorithm_import {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 6. Let normalizedDerivedKeyAlgorithmLength be the result of normalizing an algorithm, with alg set to derivedKeyType and op set to "get key length".
        let normalized_derived_key_algorithm_length =
            normalize_an_algorithm(vm, realm, &derived_key_type, "get key length");

        // 7. If an error occurred, return a Promise rejected with normalizedDerivedKeyAlgorithmLength.
        if let Err(error) = normalized_derived_key_algorithm_length {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 8. Let realm be the relevant realm of this.
        // 9. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 10. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                normalized_algorithm.must(),
                promise,
                normalized_derived_key_algorithm_import.must(),
                normalized_derived_key_algorithm_length.must(),
                base_key,
                extractable,
                key_usages,
            ),
            |vm,
             (
                realm,
                normalized_algorithm,
                promise,
                normalized_derived_key_algorithm_import,
                normalized_derived_key_algorithm_length,
                base_key,
                extractable,
                key_usages,
            )| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // FIXME: 11. If the following steps or referenced procedures say to throw an error, queue a global
                    //    task on the crypto task source, given realm's global object, to reject promise with the
                    //    returned error; and then terminate the algorithm.
                    // NB: Errors are rejected directly in the deferred invocation.

                    // 12. If the name member of normalizedAlgorithm is not equal to the name attribute of the
                    //    [[algorithm]] internal slot of baseKey then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != base_key.algorithm_name() {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 13. If the [[usages]] internal slot of baseKey does not contain an entry that is "deriveKey",
                    //    then throw an InvalidAccessError.
                    if !base_key.internal_usages().contains(&KeyUsage::DeriveKey) {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support deriving keys")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 14. Let length be the result of performing the get key length algorithm specified by
                    //    normalizedDerivedKeyAlgorithmLength using derivedKeyType.
                    let length_result = normalized_derived_key_algorithm_length
                        .methods
                        .get_key_length(realm, &normalized_derived_key_algorithm_length.parameter);
                    if let Err(error) = length_result {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    let length = length_result.must();

                    // 15. Let secret be the result of performing the derive bits operation specified by
                    //    normalizedAlgorithm using key, algorithm and length.
                    let secret = normalized_algorithm.methods.derive_bits(
                        &CallContext::new(vm, realm),
                        &normalized_algorithm.parameter,
                        base_key,
                        length,
                    );
                    if let Err(error) = secret {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    // 16. Let result be the result of performing the import key operation specified by
                    //    normalizedDerivedKeyAlgorithmImport using "raw" as format, secret as keyData, derivedKeyType
                    //    as algorithm and using extractable and usages.
                    let secret_bytes = secret.must();
                    let result_or_error = normalized_derived_key_algorithm_import.methods.import_key(
                        &CallContext::new(vm, realm),
                        &normalized_derived_key_algorithm_import.parameter,
                        KeyFormat::Raw,
                        ImportKeyData::Bytes(secret_bytes),
                        extractable,
                        &key_usages,
                    );
                    if let Err(error) = result_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }
                    let result = result_or_error.must();

                    // 17. If the [[type]] internal slot of result is "secret" or "private" and usages is empty, then
                    //    throw a SyntaxError.
                    if (result.key_type() == KeyType::Secret || result.key_type() == KeyType::Private)
                        && key_usages.is_empty()
                    {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("SyntaxError", b"usages must not be empty")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 18. Set the [[extractable]] internal slot of result to extractable.
                    result.set_extractable(extractable);

                    // 19. Set the [[usages]] internal slot of result to the normalized value of usages.
                    result.set_usages(normalized_key_usages(&key_usages));

                    // FIXME: 20. Queue a global task on the crypto task source, given realm's global object, to perform
                    //    the remaining steps.
                    // 21. Let result be the result of converting result to an ECMAScript Object in realm, as defined by
                    //    [WebIDL].
                    // 22. Resolve promise with result.
                    resolve_crypto_key_promise(vm, realm, promise, result);
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-wrapKey
    pub(crate) fn wrap_key(
        &self,
        cx: &mut CallContext,
        format: KeyFormat,
        key: Ref<CryptoKey>,
        wrapping_key: Ref<CryptoKey>,
        algorithm: AlgorithmIdentifier,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());
        // 5. Let realm be the relevant realm of this.
        // 1. Let format, key, wrappingKey and algorithm be the format, key, wrappingKey and wrapAlgorithm parameters passed to the wrapKey() method, respectively.

        let mut operation = "";
        let normalized_algorithm_or_error = (|| -> WebIdlResult<NormalizedAlgorithmAndParameter> {
            // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "wrapKey".
            let normalized_algorithm_wrap_key_or_error = normalize_an_algorithm(vm, realm, &algorithm, "wrapKey");

            // 3. If an error occurred, let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "encrypt".
            // 4. If an error occurred, return a Promise rejected with normalizedAlgorithm.
            if normalized_algorithm_wrap_key_or_error.is_err() {
                let normalized_algorithm_encrypt_or_error = normalize_an_algorithm(vm, realm, &algorithm, "encrypt");
                if let Err(error) = normalized_algorithm_encrypt_or_error {
                    return Err(error);
                }

                operation = "encrypt";
                normalized_algorithm_encrypt_or_error
            } else {
                operation = "wrapKey";
                normalized_algorithm_wrap_key_or_error
            }
        })();
        if let Err(error) = normalized_algorithm_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        let normalized_algorithm = normalized_algorithm_or_error.must();

        // 6. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 7. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                normalized_algorithm,
                promise,
                wrapping_key,
                key,
                format,
                operation,
            ),
            |vm, (realm, normalized_algorithm, promise, wrapping_key, key, format, operation)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // FIXME: 8. If the following steps or referenced procedures say to throw an error, queue a global task on the crypto task source, given realm's global object, to reject promise with the returned error; and then terminate the algorithm.

                    // 9. If the name member of normalizedAlgorithm is not equal to the name attribute
                    //    of the [[algorithm]] internal slot of wrappingKey then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != wrapping_key.algorithm_name() {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 10. If the [[usages]] internal slot of wrappingKey does not contain an entry that is "wrapKey", then throw an InvalidAccessError.
                    if !wrapping_key.internal_usages().contains(&KeyUsage::WrapKey) {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support wrapping keys")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 11. If the algorithm identified by the [[algorithm]] internal slot of key does not support the export key operation, then throw a NotSupportedError.
                    // Note: Handled by the base AlgorithmMethods implementation

                    // 12. If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError.
                    // Note: Because the wrapKey method effectively exports the key, only keys marked as extractable may
                    //       be wrapped. In particular, this means that this API cannot create a wrapped JWK key that is
                    //       marked as non-extractable using the ext JWK member. However, the unwrapKey method does
                    //       support the ext JWK member, so that wrapped non-extractable keys created elsewhere, for
                    //       example by a server, can be unwrapped using this API.
                    if !key.is_extractable() {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Key is not extractable")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 13. Let exportedKey be the result of performing the export key operation specified by the [[algorithm]] internal slot of key using key and format.
                    // NOTE: The spec does not mention we need to normalize this, but it's the only way we have to get to export_key.
                    let normalized_key_algorithm = normalize_an_algorithm(
                        vm,
                        realm,
                        &AlgorithmIdentifier::DOMString(key.algorithm_name()),
                        "exportKey",
                    );
                    if let Err(error) = normalized_key_algorithm {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    let exported_key_or_error =
                        normalized_key_algorithm
                            .must()
                            .methods
                            .export_key(&CallContext::new(vm, realm), format, key);
                    if let Err(error) = exported_key_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    // 14. If format is equal to the string "jwk":
                    //     1. Let json be the result of representing exportedKey as a UTF-16 string conforming to the JSON grammar;
                    //        for example, by executing the JSON.stringify algorithm specified in [ECMA-262] in the context of a new
                    //        global object.
                    //     2. Let bytes be the result of UTF-8 encoding json.
                    //     Otherwise:
                    //     Let bytes be exportedKey.
                    // Note: The key wrapping operations for some algorithms place constraints on the payload size. For
                    //       example AES-KW requires the payload to be a multiple of 8 bytes in length and RSA-OAEP
                    //       places a restriction on the length. For key formats that offer flexibility in serialization
                    //       of a given key (for example JWK), implementations may choose to adapt the serialization to
                    //       the constraints of the wrapping algorithm. This is why JSON.stringify is not normatively
                    //       required, as otherwise it would prohibit implementations from introducing added padding.
                    let bytes_or_error = export_key_result_to_bytes(vm, exported_key_or_error.must());
                    if let Err(error) = bytes_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }
                    let bytes = bytes_or_error.must();

                    // 15. If normalizedAlgorithm supports the wrap key operation:
                    let result = if operation == "wrapKey" {
                        // Let result be the result of performing the wrap key operation specified by normalizedAlgorithm
                        // using algorithm, wrappingKey as key and bytes as plaintext.
                        let result_or_error = normalized_algorithm.methods.wrap_key(realm, wrapping_key, &bytes);
                        if let Err(error) = result_or_error {
                            reject_promise_with_exception(vm, promise, error);
                            return;
                        }

                        result_or_error.must()
                    }
                    // Otherwise, if normalizedAlgorithm supports the encrypt operation:
                    else if operation == "encrypt" {
                        // Let result be the result of performing the encrypt operation specified by normalizedAlgorithm
                        // using algorithm, wrappingKey as key and bytes as plaintext.
                        let result_or_error =
                            normalized_algorithm
                                .methods
                                .encrypt(&CallContext::new(vm, realm), wrapping_key, &bytes);
                        if let Err(error) = result_or_error {
                            reject_promise_with_exception(vm, promise, error);
                            return;
                        }

                        result_or_error.must()
                    }
                    // Otherwise:
                    else {
                        // throw a NotSupportedError.
                        reject_promise(
                            vm,
                            promise,
                            dom_error("NotSupportedError", b"Algorithm does not support wrapping")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    };

                    // FIXME: 16. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    // FIXME: 17. Let result be the result of creating an ArrayBuffer in realm, containing result.
                    // 18. Resolve promise with result.
                    resolve_promise(vm, promise, array_buffer(vm, realm, &result));
                });
            },
        );

        Ok(promise)
    }

    // https://w3c.github.io/webcrypto/#SubtleCrypto-method-unwrapKey
    pub(crate) fn unwrap_key(
        &self,
        cx: &mut CallContext,
        format: KeyFormat,
        wrapped_key: BufferSource,
        unwrapping_key: Ref<CryptoKey>,
        algorithm: AlgorithmIdentifier,
        unwrapped_key_algorithm: AlgorithmIdentifier,
        extractable: bool,
        key_usages: Vec<KeyUsage>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());
        // 8. Let realm be the relevant realm of this.
        // 1. Let format, unwrappingKey, algorithm, unwrappedKeyAlgorithm, extractable and usages, be the format, unwrappingKey, unwrapAlgorithm,
        //    unwrappedKeyAlgorithm, extractable and keyUsages parameters passed to the unwrapKey() method, respectively.

        let mut operation = "";
        let normalized_algorithm_or_error = (|| -> WebIdlResult<NormalizedAlgorithmAndParameter> {
            // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "unwrapKey".
            let normalized_algorithm_unwrap_key_or_error = normalize_an_algorithm(vm, realm, &algorithm, "unwrapKey");

            // 3. If an error occurred, let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "decrypt".
            // 4. If an error occurred, return a Promise rejected with normalizedAlgorithm.
            if normalized_algorithm_unwrap_key_or_error.is_err() {
                let normalized_algorithm_decrypt_or_error = normalize_an_algorithm(vm, realm, &algorithm, "decrypt");
                if let Err(error) = normalized_algorithm_decrypt_or_error {
                    return Err(error);
                }

                operation = "decrypt";
                normalized_algorithm_decrypt_or_error
            } else {
                operation = "unwrapKey";
                normalized_algorithm_unwrap_key_or_error
            }
        })();
        if let Err(error) = normalized_algorithm_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        let normalized_algorithm = normalized_algorithm_or_error.must();

        // 5. Let normalizedKeyAlgorithm be the result of normalizing an algorithm, with alg set to unwrappedKeyAlgorithm and op set to "importKey".
        let normalized_key_algorithm_or_error =
            normalize_an_algorithm(vm, realm, &unwrapped_key_algorithm, "importKey");
        if let Err(error) = normalized_key_algorithm_or_error {
            // 6. If an error occurred, return a Promise rejected with normalizedKeyAlgorithm.
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        let normalized_key_algorithm = normalized_key_algorithm_or_error.must();

        // 7. Let wrappedKey be the result of getting a copy of the bytes held by the wrappedKey parameter passed to the unwrapKey() method.
        let real_wrapped_key_or_error = get_a_copy_of_the_bytes(vm, wrapped_key);
        if let Err(error) = real_wrapped_key_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let real_wrapped_key = real_wrapped_key_or_error.must();

        // 9. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 10. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                normalized_algorithm,
                promise,
                unwrapping_key,
                real_wrapped_key,
                operation,
                format,
                (extractable, key_usages, normalized_key_algorithm),
            ),
            |vm,
             (
                realm,
                normalized_algorithm,
                promise,
                unwrapping_key,
                real_wrapped_key,
                operation,
                format,
                (extractable, key_usages, normalized_key_algorithm),
            )| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // FIXME: 11. If the following steps or referenced procedures say to throw an error, queue a global task on the crypto task source, given realm's global object, to reject promise with the returned error; and then terminate the algorithm.

                    // 12. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot
                    //     of unwrappingKey then throw an InvalidAccessError.
                    if normalized_algorithm.parameter.name != unwrapping_key.algorithm_name() {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Algorithm mismatch")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 13. If the [[usages]] internal slot of unwrappingKey does not contain an entry that is "unwrapKey", then throw an InvalidAccessError.
                    if !unwrapping_key.internal_usages().contains(&KeyUsage::UnwrapKey) {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("InvalidAccessError", b"Key does not support unwrapping keys")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 14. If normalizedAlgorithm supports an unwrap key operation:
                    let bytes_or_error = if operation == "unwrapKey" {
                        // Let bytes be the result of performing the unwrap key operation specified by normalizedAlgorithm
                        // using algorithm, unwrappingKey as key and wrappedKey as ciphertext.
                        normalized_algorithm
                            .methods
                            .unwrap_key(realm, unwrapping_key, &real_wrapped_key)
                    }
                    // Otherwise, if normalizedAlgorithm supports a decrypt operation:
                    else if operation == "decrypt" {
                        // Let bytes be the result of performing the decrypt operation specified by normalizedAlgorithm
                        // using algorithm, unwrappingKey as key and wrappedKey as ciphertext.
                        normalized_algorithm.methods.decrypt(
                            &CallContext::new(vm, realm),
                            unwrapping_key,
                            &real_wrapped_key,
                        )
                    }
                    // Otherwise:
                    else {
                        // throw a NotSupportedError.
                        Err(dom_error("NotSupportedError", b"Algorithm does not support wrapping"))
                    };
                    if let Err(error) = bytes_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    let bytes = bytes_or_error.must();

                    // 15. If format is equal to the string "jwk":
                    let key = if format == KeyFormat::Jwk {
                        // Let key be the result of executing the parse a JWK algorithm, with bytes as the data to be parsed.
                        let maybe_parsed = parse_json_web_key(vm, bytes.as_slice());
                        if let Err(error) = maybe_parsed {
                            reject_promise_with_exception(vm, promise, error);
                            return;
                        }

                        ImportKeyData::JsonWebKey(maybe_parsed.must())
                    }
                    // Otherwise:
                    else {
                        // Let key be bytes.
                        ImportKeyData::Bytes(bytes)
                    };

                    // 16. Let result be the result of performing the import key operation specified by normalizedKeyAlgorithm
                    //    using unwrappedKeyAlgorithm as algorithm, format, usages and extractable and with key as keyData.
                    let result_or_error = normalized_key_algorithm.methods.import_key(
                        &CallContext::new(vm, realm),
                        &normalized_key_algorithm.parameter,
                        format,
                        key,
                        extractable,
                        &key_usages,
                    );
                    if let Err(error) = result_or_error {
                        reject_promise_with_exception(vm, promise, error);
                        return;
                    }

                    let result = result_or_error.must();

                    // 17. If the [[type]] internal slot of result is "secret" or "private" and usages is empty, then throw a SyntaxError.
                    if (result.key_type() == KeyType::Secret || result.key_type() == KeyType::Private)
                        && key_usages.is_empty()
                    {
                        reject_promise(
                            vm,
                            promise,
                            dom_error("SyntaxError", b"Usages must not be empty")
                                .throw(&CallContext::new(vm, realm))
                                .value(),
                        );
                        return;
                    }

                    // 18. Set the [[extractable]] internal slot of result to extractable.
                    result.set_extractable(extractable);

                    // 19. Set the [[usages]] internal slot of result to the normalized value of usages.
                    result.set_usages(normalized_key_usages(&key_usages));

                    // FIXME: 20. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    // FIXME: 21. Let result be the result of converting result to an ECMAScript Object in realm, as defined by [WebIDL].
                    // 22. Resolve promise with result.
                    resolve_crypto_key_promise(vm, realm, promise, result);
                });
            },
        );

        Ok(promise)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#dfn-SubtleCrypto-method-encapsulateKey
    pub(crate) fn encapsulate_key(
        &self,
        cx: &mut CallContext,
        encapsulation_algorithm: AlgorithmIdentifier,
        encapsulation_key: Ref<CryptoKey>,
        shared_key_algorithm: AlgorithmIdentifier,
        extractable: bool,
        key_usages: Vec<KeyUsage>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let encapsulationAlgorithm, encapsulationKey, sharedKeyAlgorithm, extractable and usages be the
        //    encapsulationAlgorithm, encapsulationKey, sharedKeyAlgorithm, extractable and keyUsages parameters passed to
        //    the encapsulateKey() method, respectively.

        // 2. Let normalizedEncapsulationAlgorithm be the result of normalizing an algorithm, with alg set to
        //    encapsulationAlgorithm and op set to "encapsulate".
        let maybe_normalized_encapsulation_algorithm =
            normalize_an_algorithm(vm, realm, &encapsulation_algorithm, "encapsulate");

        // 3. If an error occurred, return a Promise rejected with normalizedEncapsulationAlgorithm.
        if let Err(error) = maybe_normalized_encapsulation_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let normalized_encapsulation_algorithm = maybe_normalized_encapsulation_algorithm.must();

        // 4. Let normalizedSharedKeyAlgorithm be the result of normalizing an algorithm, with alg
        //    set to sharedKeyAlgorithm and op set to "importKey".
        let maybe_normalized_shared_key_algorithm =
            normalize_an_algorithm(vm, realm, &shared_key_algorithm, "importKey");

        // 5. If an error occurred, return a Promise rejected with normalizedSharedKeyAlgorithm.
        if let Err(error) = maybe_normalized_shared_key_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let normalized_shared_key_algorithm = maybe_normalized_shared_key_algorithm.must();

        // 6. Let realm be the relevant realm of this.

        // 7. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 8. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                normalized_encapsulation_algorithm,
                encapsulation_key,
                promise,
                normalized_shared_key_algorithm,
                extractable,
                key_usages,
            ),
            |vm,
             (
                realm,
                normalized_encapsulation_algorithm,
                encapsulation_key,
                promise,
                normalized_shared_key_algorithm,
                extractable,
                usages,
            )| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // 9. If the following steps or referenced procedures say to throw an error, queue a global task on the crypto task
                    //    source, given realm's global object, to reject promise with the returned error; and then terminate the algorithm.
                    // 10. If the name member of normalizedEncapsulationAlgorithm is not equal to the name attribute of the [[algorithm]]
                    //     internal slot of encapsulationKey then throw an InvalidAccessError.
                    if normalized_encapsulation_algorithm.parameter.name != encapsulation_key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid encapsulation key algorithm"),
                        );
                        return;
                    }

                    // 11. If the [[usages]] internal slot of encapsulationKey does not contain an entry that is "encapsulateKey", then
                    //     throw an InvalidAccessError.
                    if !encapsulation_key.internal_usages().contains(&KeyUsage::EncapsulateKey) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid encapsulation key usage"),
                        );
                        return;
                    }

                    // 12. Let encapsulatedBits be the result of performing the encapsulate operation specified by the [[algorithm]]
                    //     internal slot of encapsulationKey using encapsulationKey.
                    let maybe_encapsulated_bits = normalized_encapsulation_algorithm.methods.encapsulate(
                        realm,
                        &normalized_encapsulation_algorithm.parameter,
                        encapsulation_key,
                    );
                    if let Err(error) = maybe_encapsulated_bits {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }
                    let encapsulated_bits = maybe_encapsulated_bits.must();

                    // 13. Let sharedKey be the result of performing the import key operation specified by
                    //     normalizedSharedKeyAlgorithm using "raw-secret" as format, the sharedKey field of encapsulatedBits as
                    //     keyData, sharedKeyAlgorithm as algorithm and using extractable and usages.
                    let maybe_shared_key = normalized_shared_key_algorithm.methods.import_key(
                        &CallContext::new(vm, realm),
                        &normalized_shared_key_algorithm.parameter,
                        KeyFormat::RawSecret,
                        ImportKeyData::Bytes(
                            encapsulated_bits
                                .shared_key
                                .expect("encapsulation returns a shared key"),
                        ),
                        extractable,
                        &usages,
                    );
                    if let Err(error) = maybe_shared_key {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }
                    let shared_key = maybe_shared_key.must();

                    // 14. If usages is empty, then throw a SyntaxError.
                    if usages.is_empty() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("SyntaxError", b"usages must not be empty"),
                        );
                        return;
                    }

                    // 15. Set the [[extractable]] internal slot of sharedKey to extractable.
                    shared_key.set_extractable(extractable);

                    // 16. Set the [[usages]] internal slot of sharedKey to the normalized value of usages.
                    shared_key.set_usages(normalized_key_usages(&usages));

                    // 17. Let encapsulatedKey be a new EncapsulatedKey dictionary with sharedKey set to sharedKey and
                    //    ciphertext set to the ciphertext field of encapsulatedBits.
                    let encapsulated_key = EncapsulatedKeyData {
                        shared_key,
                        ciphertext: encapsulated_bits.ciphertext,
                    };

                    // 18. Queue a global task on the crypto task source, given realm's global object, to perform the
                    //    remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, encapsulated_key),
                        |vm, (realm, promise, encapsulated_key)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 19. Let result be the result of converting encapsulatedKey to an ECMAScript Object in
                                //    realm, as defined by [WebIDL].
                                let maybe_result = encapsulated_key_value(vm, realm, &encapsulated_key);
                                if let Err(error) = maybe_result {
                                    reject_promise(vm, promise, error.value());
                                    return;
                                }
                                let result = maybe_result.must();

                                // 20. Resolve promise with result.
                                resolve_promise(vm, promise, result);
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#dfn-SubtleCrypto-method-encapsulateBits
    pub(crate) fn encapsulate_bits(
        &self,
        cx: &mut CallContext,
        encapsulation_algorithm: AlgorithmIdentifier,
        encapsulation_key: Ref<CryptoKey>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let encapsulationAlgorithm and encapsulationKey be the encapsulationAlgorithm and encapsulationKey
        //    parameters passed to the encapsulateBits() method, respectively.

        // 2. Let normalizedEncapsulationAlgorithm be the result of normalizing an algorithm, with alg set to
        //    encapsulationAlgorithm and op set to "encapsulate".
        let maybe_normalized_encapsulation_algorithm =
            normalize_an_algorithm(vm, realm, &encapsulation_algorithm, "encapsulate");

        // 3. If an error occurred, return a Promise rejected with normalizedEncapsulationAlgorithm.
        if let Err(error) = maybe_normalized_encapsulation_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let normalized_encapsulation_algorithm = maybe_normalized_encapsulation_algorithm.must();

        // 4. Let realm be the relevant realm of this.

        // 5. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 6. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (realm, normalized_encapsulation_algorithm, promise, encapsulation_key),
            |vm, (realm, normalized_encapsulation_algorithm, promise, encapsulation_key)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // 7. If the following steps or referenced procedures say to throw an error, queue a global task on the crypto task
                    //    source, given realm's global object, to reject promise with the returned error; and then terminate the algorithm.
                    // 8. If the name member of normalizedEncapsulationAlgorithm is not equal to the name attribute of the [[algorithm]]
                    //    internal slot of encapsulationKey then throw an InvalidAccessError.
                    if normalized_encapsulation_algorithm.parameter.name != encapsulation_key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid encapsulation key algorithm"),
                        );
                        return;
                    }

                    // 9. If the [[usages]] internal slot of encapsulationKey does not contain an entry that is "encapsulateBits", then
                    //    throw an InvalidAccessError.
                    if !encapsulation_key.internal_usages().contains(&KeyUsage::EncapsulateBits) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid encapsulation key usages"),
                        );
                        return;
                    }

                    // 10. Let encapsulatedBits be the result of performing the encapsulate operation specified by the [[algorithm]]
                    //     internal slot of encapsulationKey using encapsulationKey.
                    let maybe_encapsulated_bits = normalized_encapsulation_algorithm.methods.encapsulate(
                        realm,
                        &normalized_encapsulation_algorithm.parameter,
                        encapsulation_key,
                    );
                    if let Err(error) = maybe_encapsulated_bits {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }
                    let encapsulated_bits = maybe_encapsulated_bits.must();

                    // 11. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, encapsulated_bits),
                        |vm, (realm, promise, encapsulated_bits)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 12. Let result be the result of converting encapsulatedBits to an ECMAScript Object in realm, as defined by [WebIDL].
                                let maybe_result = encapsulated_bits_value(vm, realm, &encapsulated_bits);
                                if let Err(error) = maybe_result {
                                    reject_promise(vm, promise, error.value());
                                    return;
                                }
                                let result = maybe_result.must();

                                // 13. Resolve promise with result.
                                resolve_promise(vm, promise, result);
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#dfn-SubtleCrypto-method-decapsulateKey
    pub(crate) fn decapsulate_key(
        &self,
        cx: &mut CallContext,
        decapsulation_algorithm: AlgorithmIdentifier,
        decapsulation_key: Ref<CryptoKey>,
        ciphertext: BufferSource,
        shared_key_algorithm: AlgorithmIdentifier,
        extractable: bool,
        usages: Vec<KeyUsage>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let decapsulationAlgorithm, decapsulationKey, sharedKeyAlgorithm, extractable and usages be the
        //    decapsulationAlgorithm, decapsulationKey, sharedKeyAlgorithm, extractable and keyUsages parameters passed to
        //    the decapsulateKey() method, respectively.

        // 2. Let normalizedDecapsulationAlgorithm be the result of normalizing an algorithm, with alg set to
        //    decapsulationAlgorithm and op set to "decapsulate".
        let normalized_decapsulation_algorithm =
            normalize_an_algorithm(vm, realm, &decapsulation_algorithm, "decapsulate");

        // 3. If an error occurred, return a Promise rejected with normalizedDecapsulationAlgorithm.
        if let Err(error) = normalized_decapsulation_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let normalizedSharedKeyAlgorithm be the result of normalizing an algorithm, with alg
        //    set to sharedKeyAlgorithm and op set to "importKey".
        let normalized_shared_key_algorithm = normalize_an_algorithm(vm, realm, &shared_key_algorithm, "importKey");

        // 5. If an error occurred, return a Promise rejected with normalizedSharedKeyAlgorithm.
        if let Err(error) = normalized_shared_key_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 6. Let ciphertext be the result of getting a copy of the bytes held by the ciphertext parameter passed to the
        //    decapsulateKey() method.
        let cipher_text_or_error = get_a_copy_of_the_bytes(vm, ciphertext);
        if let Err(error) = cipher_text_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let cipher_text = cipher_text_or_error.must();

        // 7. Let realm be the relevant realm of this.

        // 8. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 9. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                normalized_decapsulation_algorithm.must(),
                promise,
                decapsulation_key,
                cipher_text,
                normalized_shared_key_algorithm.must(),
                extractable,
                usages,
            ),
            |vm,
             (
                realm,
                normalized_decapsulation_algorithm,
                promise,
                decapsulation_key,
                cipher_text,
                normalized_shared_key_algorithm,
                extractable,
                usages,
            )| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // 10. If the following steps or referenced procedures say to throw an error, queue a global task on the crypto task
                    //     source, given realm's global object, to reject promise with the returned error; and then terminate the algorithm.
                    // 11. If the name member of normalizedDecapsulationAlgorithm is not equal to the name attribute of the [[algorithm]]
                    //     internal slot of decapsulationKey then throw an InvalidAccessError.
                    if normalized_decapsulation_algorithm.parameter.name != decapsulation_key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid algorithm name"),
                        );
                        return;
                    }

                    // 12. If the [[usages]] internal slot of decapsulationKey does not contain an entry that is "decapsulateKey", then
                    //     throw an InvalidAccessError.
                    if !decapsulation_key.internal_usages().contains(&KeyUsage::DecapsulateKey) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid key usages"),
                        );
                        return;
                    }
                    // 13. Let decapsulatedBits be the result of performing the decapsulate operation specified by the [[algorithm]]
                    //     internal slot of decapsulationKey using decapsulationKey and ciphertext.
                    let maybe_decapsulated_bits = normalized_decapsulation_algorithm.methods.decapsulate(
                        realm,
                        &normalized_decapsulation_algorithm.parameter,
                        decapsulation_key,
                        &cipher_text,
                    );
                    if let Err(error) = maybe_decapsulated_bits {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }
                    let decapsulated_bits = maybe_decapsulated_bits.must();

                    // 14. Let sharedKey be the result of performing the import key operation specified by
                    //     normalizedSharedKeyAlgorithm using "raw-secret" as format, the decapsulatedBits as keyData,
                    //     sharedKeyAlgorithm as algorithm and using extractable and usages.
                    let maybe_shared_key = normalized_shared_key_algorithm.methods.import_key(
                        &CallContext::new(vm, realm),
                        &normalized_shared_key_algorithm.parameter,
                        KeyFormat::RawSecret,
                        ImportKeyData::Bytes(decapsulated_bits),
                        extractable,
                        &usages,
                    );
                    if let Err(error) = maybe_shared_key {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }
                    let shared_key = maybe_shared_key.must();

                    // 15. If usages is empty, then throw a SyntaxError.
                    if usages.is_empty() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("SyntaxError", b"usages must not be empty"),
                        );
                        return;
                    }

                    // 16. Set the [[extractable]] internal slot of sharedKey to extractable.
                    shared_key.set_extractable(extractable);

                    // 17. Set the [[usages]] internal slot of sharedKey to the normalized value of usages.
                    shared_key.set_usages(normalized_key_usages(&usages));

                    // 18. Queue a global task on the crypto task source, given realm's global object, to perform the
                    //    remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, shared_key),
                        |vm, (realm, promise, shared_key)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 19. Let result be the result of converting sharedKey to an ECMAScript Object in
                                //    realm, as defined by [WebIDL].
                                // 20. Resolve promise with result.
                                resolve_crypto_key_promise(vm, realm, promise, shared_key);
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#dfn-SubtleCrypto-method-decapsulateBits
    pub(crate) fn decapsulate_bits(
        &self,
        cx: &mut CallContext,
        decapsulation_algorithm: AlgorithmIdentifier,
        decapsulation_key: Ref<CryptoKey>,
        ciphertext: BufferSource,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let decapsulationAlgorithm and decapsulationKey be the decapsulationAlgorithm and decapsulationKey
        //    parameters passed to the decapsulateBits() method, respectively.

        // 2. Let normalizedDecapsulationAlgorithm be the result of normalizing an algorithm, with alg set to
        //    decapsulationAlgorithm and op set to "decapsulate".
        let normalized_decapsulation_algorithm =
            normalize_an_algorithm(vm, realm, &decapsulation_algorithm, "decapsulate");

        // 3. If an error occurred, return a Promise rejected with normalizedDecapsulationAlgorithm.
        if let Err(error) = normalized_decapsulation_algorithm {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let ciphertext be the result of getting a copy of the bytes held by the ciphertext parameter passed to the
        //    decapsulateBits() method.
        let cipher_text_or_error = get_a_copy_of_the_bytes(vm, ciphertext);
        if let Err(error) = cipher_text_or_error {
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }
        let cipher_text = cipher_text_or_error.must();

        // 5. Let realm be the relevant realm of this.

        // 6. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 7. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                normalized_decapsulation_algorithm.must(),
                promise,
                decapsulation_key,
                cipher_text,
            ),
            |vm, (realm, normalized_decapsulation_algorithm, promise, decapsulation_key, cipher_text)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    // 8. If the following steps or referenced procedures say to throw an error, queue a global task on the crypto task
                    //    source, given realm's global object, to reject promise with the returned error; and then terminate the algorithm.
                    // 9. If the name member of normalizedDecapsulationAlgorithm is not equal to the name attribute of the [[algorithm]]
                    //    internal slot of decapsulationKey then throw an InvalidAccessError.
                    if normalized_decapsulation_algorithm.parameter.name != decapsulation_key.algorithm_name() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid algorithm name"),
                        );
                        return;
                    }

                    // 10. If the [[usages]] internal slot of decapsulationKey does not contain an entry that is "decapsulateBits", then
                    //     throw an InvalidAccessError.
                    if !decapsulation_key.internal_usages().contains(&KeyUsage::DecapsulateBits) {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Invalid key usages"),
                        );
                        return;
                    }

                    // 11. Let decapsulatedBits be the result of performing the decapsulate operation specified by the [[algorithm]]
                    //     internal slot of decapsulationKey using decapsulationKey and ciphertext.
                    let maybe_decapsulated_bits = normalized_decapsulation_algorithm.methods.decapsulate(
                        realm,
                        &normalized_decapsulation_algorithm.parameter,
                        decapsulation_key,
                        &cipher_text,
                    );
                    if let Err(error) = maybe_decapsulated_bits {
                        queue_crypto_rejection(vm, realm, promise, error);
                        return;
                    }
                    let decapsulated_bits = maybe_decapsulated_bits.must();

                    // 12. Queue a global task on the crypto task source, given realm's global object, to perform the remaining steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, decapsulated_bits),
                        |vm, (realm, promise, decapsulated_bits)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 13. Let result be the result of creating an ArrayBuffer in realm, containing decapsulatedBits.
                                // 14. Resolve promise with result.
                                resolve_promise(vm, promise, array_buffer(vm, realm, &decapsulated_bits));
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#dfn-SubtleCrypto-method-getPublicKey
    pub(crate) fn get_public_key(
        &self,
        cx: &mut CallContext,
        key: Ref<CryptoKey>,
        key_usages: Vec<KeyUsage>,
    ) -> WebIdlResult<WebIdlPromise> {
        let (vm, realm) = (cx.vm(), cx.realm());

        // 1. Let key and usages be the key and keyUsages parameters passed to the getPublicKey() method, respectively.

        // 2. Let algorithm be the [[algorithm]] internal slot of key.
        // NB: Every algorithm that supports deriving a public key registers an export key operation, which lets us find
        //     the algorithm's methods.
        let normalized_algorithm = normalize_an_algorithm(
            vm,
            realm,
            &AlgorithmIdentifier::DOMString(key.algorithm_name()),
            "exportKey",
        );

        // 3. If the cryptographic algorithm identified by algorithm does not support deriving a public key from a private
        //    key, then return a Promise rejected with a NotSupportedError.
        let mut public_key_usages = None;
        if let Ok(normalized_algorithm) = normalized_algorithm {
            public_key_usages = normalized_algorithm.methods.public_key_usages();
        }
        if !public_key_usages.is_some() {
            let error = Exception::Dom(
                "NotSupportedError",
                libjs_rust::api::utf16_formatted(
                    "Algorithm '{}' does not support deriving a public key",
                    &[&key.algorithm_name()],
                ),
            );
            return Ok(create_rejected_promise_from_exception(vm, realm, error));
        }

        // 4. Let realm be the relevant realm of this.

        // 5. Let promise be a new Promise.
        let promise = create_promise(vm, realm);

        // 6. Return promise and perform the remaining steps in parallel.
        deferred_invoke(
            vm,
            (
                realm,
                promise,
                key,
                key_usages,
                public_key_usages.expect("the algorithm supports public keys"),
            ),
            |vm, (realm, promise, key, key_usages, public_key_usages)| {
                with_temporary_execution_context(realm, || {
                    // 7. If the following steps or referenced procedures say to throw an error, queue a global task on the crypto
                    //    task source, given realm's global object, to reject promise with the returned error; and then terminate
                    //    the algorithm.
                    // 8. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
                    if key.key_type() != KeyType::Private {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("InvalidAccessError", b"Key is not a private key"),
                        );
                        return;
                    }

                    // 9. If usages contains an entry which is not supported for a public key by the algorithm identified by
                    //    algorithm, then throw a SyntaxError.
                    for usage in &key_usages {
                        if !public_key_usages.contains(usage) {
                            queue_crypto_rejection(
                                vm,
                                realm,
                                promise,
                                dom_error("SyntaxError", b"Invalid key usage for a public key"),
                            );
                            return;
                        }
                    }

                    // 10. Let publicKey be a new CryptoKey representing the public key corresponding to the private key represented
                    //     by the [[handle]] internal slot of key.
                    let public_key_data = public_key_for_private_key(vm, key);

                    // 11. If an error occurred, then throw a OperationError.
                    if public_key_data.is_none() {
                        queue_crypto_rejection(
                            vm,
                            realm,
                            promise,
                            dom_error("OperationError", b"Failed to derive the public key"),
                        );
                        return;
                    }
                    let public_key = public_key_data.expect("the public key was derived");

                    // 12. Set the [[type]] internal slot of publicKey to "public".
                    public_key.set_type(KeyType::Public);

                    // 13. Set the [[algorithm]] internal slot of publicKey to algorithm.
                    public_key.set_algorithm(key.internal_algorithm().clone());

                    // 14. Set the [[extractable]] internal slot of publicKey to true.
                    public_key.set_extractable(true);

                    // 15. Set the [[usages]] internal slot of publicKey to the normalized value of usages.
                    public_key.set_usages(normalized_key_usages(&key_usages));

                    // 16. Queue a global task on the crypto task source, given realm's global object, to perform the remaining
                    //     steps.
                    queue_a_global_task(
                        vm,
                        realm.global_object(),
                        TaskSource::Crypto,
                        (realm, promise, public_key),
                        |vm, (realm, promise, public_key)| {
                            with_temporary_execution_context_and_callbacks(realm, || {
                                // 17. Let result be the result of converting publicKey to an ECMAScript Object in realm, as defined by
                                //     [WebIDL].
                                // 18. Resolve promise with result.
                                resolve_crypto_key_promise(vm, realm, promise, public_key);
                            });
                        },
                    );
                });
            },
        );

        Ok(promise)
    }

    // https://wicg.github.io/webcrypto-modern-algos/#dfn-SubtleCrypto-method-supports
    pub(crate) fn supports_length(
        cx: &mut CallContext,
        operation: Utf16String,
        algorithm: AlgorithmIdentifier,
        length: Option<u32>,
    ) -> WebIdlResult<bool> {
        let (vm, realm) = (cx.vm(), cx.realm());
        let operation = String::from_utf16_lossy(&operation.to_utf16());
        // 1. If operation is not one of "encrypt", "decrypt", "sign", "verify", "digest", "generateKey", "deriveKey",
        //    "deriveBits", "importKey", "exportKey", "wrapKey", "unwrapKey", "encapsulateKey", "encapsulateBits",
        //    "decapsulateKey", "decapsulateBits" or "getPublicKey", return false.
        if !is_a_supported_operation(&operation) {
            return Ok(false);
        }

        // 2. Return the result of checking support for an algorithm, with op set to operation, alg set to algorithm, and
        //    length set to length.
        Ok(check_support_for_an_algorithm(
            vm, realm, &operation, &algorithm, length,
        ))
    }

    // https://wicg.github.io/webcrypto-modern-algos/#dfn-SubtleCrypto-method-supports-additionalAlgorithm
    pub(crate) fn supports_additional_algorithm(
        cx: &mut CallContext,
        operation: Utf16String,
        algorithm: AlgorithmIdentifier,
        additional_algorithm: AlgorithmIdentifier,
    ) -> WebIdlResult<bool> {
        let (vm, realm) = (cx.vm(), cx.realm());
        let operation = String::from_utf16_lossy(&operation.to_utf16());
        // 1. If operation is not one of "encrypt", "decrypt", "sign", "verify", "digest", "generateKey", "deriveKey",
        //    "deriveBits", "importKey", "exportKey", "wrapKey", "unwrapKey", "encapsulateKey", "encapsulateBits",
        //    "decapsulateKey", "decapsulateBits" or "getPublicKey", return false.
        if !is_a_supported_operation(&operation) {
            return Ok(false);
        }

        let mut op = operation.as_str();

        // 2. If operation is "deriveKey" or "unwrapKey":
        if matches!(op, "deriveKey" | "unwrapKey") {
            // If the result of checking support for an algorithm with op set to "importKey" and alg set to
            // additionalAlgorithm is false, return false.
            if !check_support_for_an_algorithm(vm, realm, "importKey", &additional_algorithm, None) {
                return Ok(false);
            }
        }

        // 2. If operation is "wrapKey":
        //    If the result of checking support for an algorithm with op set to "exportKey" and alg set to
        //    additionalAlgorithm is false, return false.
        if op == "wrapKey" {
            // If the result of checking support for an algorithm with op set to "exportKey" and alg set to
            // additionalAlgorithm is false, return false.
            if !check_support_for_an_algorithm(vm, realm, "exportKey", &additional_algorithm, None) {
                return Ok(false);
            }
        }

        // 3. If operation is "encapsulateKey" or "decapsulateKey":
        if matches!(op, "encapsulateKey" | "decapsulateKey") {
            // 1. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set
            //    to "get shared key length".
            let normalized_algorithm = normalize_an_algorithm(vm, realm, &algorithm, "get shared key length");

            // 2. If an error occurred, return false.
            if normalized_algorithm.is_err() {
                return Ok(false);
            }

            // 3. Let sharedKeyLength be the result of performing the get shared key length algorithm specified by
            //    normalizedAlgorithm using algorithm.
            let shared_key_length = normalized_algorithm.must().methods.get_shared_key_length(realm);
            if shared_key_length.is_err() {
                return Ok(false);
            }

            // 4. Let normalizedAdditionalAlgorithm be the result of normalizing an algorithm, with alg set to
            //    additionalAlgorithm and op set to "importKey".
            let normalized_additional_algorithm = normalize_an_algorithm(vm, realm, &additional_algorithm, "importKey");

            // 5. If an error occurred, return false.
            if normalized_additional_algorithm.is_err() {
                return Ok(false);
            }

            // 6. If the result of determining support from operation steps with op set to "importKey" and
            //    normalizedAlgorithm set to normalizedAdditionalAlgorithm, and length set to null is false, return false.
            let normalized_additional_algorithm = normalized_additional_algorithm.must();
            if !determine_support_from_operation_steps(realm, "importKey", &normalized_additional_algorithm, None) {
                return Ok(false);
            }

            // 7. If the import key operation specified by normalizedAdditionalAlgorithm would throw an error for every value
            //    of keyData that is a byte sequence whose length in bits is sharedKeyLength when format is "raw-secret",
            //    return false.
            // NB: Whether importing secret key data fails depends on its length only, so importing zeroes of that length
            //     tells whether every such keyData fails.
            let key_data = ImportKeyData::Bytes(Bytes::new(vec![0; shared_key_length.must().div_ceil(8) as usize]));
            let import_algorithm = normalized_additional_algorithm;
            if import_algorithm
                .methods
                .import_key(
                    &CallContext::new(vm, realm),
                    &import_algorithm.parameter,
                    KeyFormat::RawSecret,
                    key_data,
                    false,
                    &[],
                )
                .is_err()
            {
                return Ok(false);
            }
        }

        // 4. Let length be null.
        let mut length = None;

        // 5. If operation is "deriveKey":
        if op == "deriveKey" {
            // 1. If the result of checking support for an algorithm with op set to "get key length" and alg set to
            //    additionalAlgorithm is false, return false.
            if !check_support_for_an_algorithm(vm, realm, "get key length", &additional_algorithm, None) {
                return Ok(false);
            }

            // 2. Let normalizedAdditionalAlgorithm be the result of normalizing an algorithm, with alg set to
            //    additionalAlgorithm and op set to "get key length".
            // NB: Normalizing can run author code, so this second normalization can fail even though the first succeeded.
            let normalized_additional_algorithm =
                normalize_an_algorithm(vm, realm, &additional_algorithm, "get key length");
            if normalized_additional_algorithm.is_err() {
                return Ok(false);
            }

            // 3. Let length be the result of performing the get key length algorithm specified by additionalAlgorithm
            //    using normalizedAdditionalAlgorithm.
            let normalized_length_algorithm = normalized_additional_algorithm.must();
            let key_length = normalized_length_algorithm
                .methods
                .get_key_length(realm, &normalized_length_algorithm.parameter);
            if key_length.is_err() {
                return Ok(false);
            }
            length = key_length.must();

            // 4. Set operation to "deriveBits".
            op = "deriveBits";
        }

        // 6. Return the result of checking support for an algorithm, with op set to operation, alg set to algorithm,
        //    and length set to length.
        Ok(check_support_for_an_algorithm(vm, realm, op, &algorithm, length))
    }
}

// https://wicg.github.io/webcrypto-modern-algos/#dfn-determine-support-from-operation-steps
fn determine_support_from_operation_steps(
    realm: Gc<Realm>,
    op: &str,
    normalized_algorithm: &NormalizedAlgorithmAndParameter,
    length: Option<u32>,
) -> bool {
    let methods = &normalized_algorithm.methods;

    // 1. If the specified operation or normalized algorithm (or one of its parameter values) is expected to fail (for
    //    any key and/or data) for an implementation-specific reason (e.g. known nonconformance to the specification),
    //    return false.
    // NB: The validate_*_parameters() methods of the algorithms also reject parameters that we do not support.

    // 2. If op is "generateKey" or "importKey", let usages be the empty list.
    // NB: An empty list of usages never makes a step of these operations throw.

    // 3. For each of the steps of the operation specified by op of the algorithm specified by normalizedAlgorithm:
    //    If the step says to throw an error:
    //       Return false.
    //    If the step says to generate a key:
    //       Return true.
    //    If the step relies on an unavailable parameter, such as key, plaintext or ciphertext:
    //       Return true.
    //    If the step says to return a value:
    //       Return true.
    //    Otherwise:
    //       Execute the step.
    // Note: The steps in the referenced algorithm may access the normalizedAlgorithm, length, and usages variables from
    //       this context.
    // NB: The validate_*_parameters() methods of an algorithm perform the steps of its operations that come before the
    //     first step that relies on an unavailable parameter, generates a key or returns a value. The operations that
    //     have no such method rely on an unavailable parameter in their first steps. The get key length operation
    //     relies on its parameters only, so we perform it in full.
    let result = (|| -> WebIdlResult<()> {
        if op == "encrypt" {
            return methods.validate_encrypt_parameters(realm);
        }
        if op == "decrypt" {
            return methods.validate_decrypt_parameters(realm);
        }
        if op == "sign" {
            return methods.validate_sign_parameters(realm);
        }
        if op == "verify" {
            return methods.validate_verify_parameters(realm);
        }
        if op == "digest" {
            return methods.validate_digest_parameters(realm);
        }
        if op == "generateKey" {
            return methods.validate_generate_key_parameters(realm);
        }
        if op == "importKey" {
            return methods.validate_import_key_parameters(realm);
        }
        if op == "deriveBits" {
            return methods.validate_derive_bits_parameters(realm, &normalized_algorithm.parameter, length);
        }
        if op == "get key length" {
            methods.get_key_length(realm, &normalized_algorithm.parameter)?;
        }
        Ok(())
    })();
    // 4. Assert: this step is never reached, because one of the steps of the operation will have said to return a value
    //    or throw an error, causing us to return true or false, respectively.
    // NB: The validators stop at the first step that determines support.
    result.is_ok()
}

// https://wicg.github.io/webcrypto-modern-algos/#dfn-check-support-for-algorithm
fn check_support_for_an_algorithm(
    vm: &Vm,
    realm: Gc<Realm>,
    mut op: &str,
    alg: &AlgorithmIdentifier,
    length: Option<u32>,
) -> bool {
    // 1. If op is "encapsulateKey" or "encapsulateBits", set op to "encapsulate".
    if matches!(op, "encapsulateKey" | "encapsulateBits") {
        op = "encapsulate";
    }

    // 2. If op is "decapsulateKey" or "decapsulateBits", set op to "decapsulate".
    if matches!(op, "decapsulateKey" | "decapsulateBits") {
        op = "decapsulate";
    }

    // 3. If op is "getPublicKey":
    if op == "getPublicKey" {
        // 1. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to alg and op set to
        //    "exportKey".
        let normalized_algorithm = normalize_an_algorithm(vm, realm, alg, "exportKey");

        // 2. If an error occurred, return false.
        if normalized_algorithm.is_err() {
            return false;
        }

        // 3. If the cryptographic algorithm identified by normalizedAlgorithm does not support deriving a public key
        //    from a private key, then return false.
        // 4. Otherwise, return true.
        return normalized_algorithm.must().methods.public_key_usages().is_some();
    }

    // 4. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to alg and op set to op.
    let normalized_algorithm = normalize_an_algorithm(vm, realm, alg, op);

    // 5. If an error occurred:
    if normalized_algorithm.is_err() {
        // 1. If op is "wrapKey", return the result of checking support for an algorithm with op set to "encrypt" and
        //    alg set to alg.
        if op == "wrapKey" {
            return check_support_for_an_algorithm(vm, realm, "encrypt", alg, None);
        }

        // 2. If op is "unwrapKey", return the result of checking support for an algorithm with op set to "decrypt" and
        //    alg set to alg.
        if op == "unwrapKey" {
            return check_support_for_an_algorithm(vm, realm, "decrypt", alg, None);
        }

        // 3. Otherwise, return false.
        return false;
    }

    // 6. Return the result of determining support from operation steps, with op set to op, normalizedAlgorithm set to
    //    normalizedAlgorithm, and length set to length.
    determine_support_from_operation_steps(realm, op, &normalized_algorithm.must(), length)
}

fn is_a_supported_operation(operation: &str) -> bool {
    matches!(
        operation,
        "encrypt"
            | "decrypt"
            | "sign"
            | "verify"
            | "digest"
            | "generateKey"
            | "deriveKey"
            | "deriveBits"
            | "importKey"
            | "exportKey"
            | "wrapKey"
            | "unwrapKey"
            | "encapsulateKey"
            | "encapsulateBits"
            | "decapsulateKey"
            | "decapsulateBits"
            | "getPublicKey"
    )
}

// https://w3c.github.io/webcrypto/#algorithm-normalization-internal
fn supported_algorithms() -> &'static SupportedAlgorithmsMap {
    static SUPPORTED_ALGORITHMS: LazyLock<SupportedAlgorithmsMap> = LazyLock::new(|| {
        let mut internal_object = HashMap::new();

        // 1. For each value, v in the List of supported operations,
        // set the v key of the internal object supportedAlgorithms to a new associative container.
        let supported_operations = [
            "encrypt",
            "decrypt",
            "sign",
            "verify",
            "digest",
            "deriveBits",
            "wrapKey",
            "unwrapKey",
            "generateKey",
            "importKey",
            "exportKey",
            "get key length",
            "encapsulate",
            "decapsulate",
            "get shared key length",
        ];

        for operation in supported_operations {
            internal_object.insert(operation, HashMap::new());
        }

        // https://w3c.github.io/webcrypto/#algorithm-conventions

        // https://w3c.github.io/webcrypto/#rsassa-pkcs1-registration
        for (operation, registered_algorithm) in [
            ("sign", RegisteredAlgorithm::RsassaPkcs1(RsaParameterType::Base)),
            ("verify", RegisteredAlgorithm::RsassaPkcs1(RsaParameterType::Base)),
            (
                "generateKey",
                RegisteredAlgorithm::RsassaPkcs1(RsaParameterType::KeyGen),
            ),
            ("importKey", RegisteredAlgorithm::RsassaPkcs1(RsaParameterType::Import)),
            ("exportKey", RegisteredAlgorithm::RsassaPkcs1(RsaParameterType::Base)),
        ] {
            define_an_algorithm(
                &mut internal_object,
                operation,
                "RSASSA-PKCS1-v1_5",
                registered_algorithm,
            );
        }

        // https://w3c.github.io/webcrypto/#rsa-pss-registration
        for (operation, registered_algorithm) in [
            ("sign", RegisteredAlgorithm::RsaPss(RsaParameterType::Pss)),
            ("verify", RegisteredAlgorithm::RsaPss(RsaParameterType::Pss)),
            ("generateKey", RegisteredAlgorithm::RsaPss(RsaParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::RsaPss(RsaParameterType::Import)),
            ("exportKey", RegisteredAlgorithm::RsaPss(RsaParameterType::Base)),
        ] {
            define_an_algorithm(&mut internal_object, operation, "RSA-PSS", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#rsa-oaep-registration
        for (operation, registered_algorithm) in [
            ("encrypt", RegisteredAlgorithm::RsaOaep(RsaParameterType::Oaep)),
            ("decrypt", RegisteredAlgorithm::RsaOaep(RsaParameterType::Oaep)),
            ("generateKey", RegisteredAlgorithm::RsaOaep(RsaParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::RsaOaep(RsaParameterType::Import)),
            ("exportKey", RegisteredAlgorithm::RsaOaep(RsaParameterType::Base)),
        ] {
            define_an_algorithm(&mut internal_object, operation, "RSA-OAEP", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#ecdsa-registration
        for (operation, registered_algorithm) in [
            ("sign", RegisteredAlgorithm::Ecdsa(EcParameterType::SignOrVerify)),
            ("verify", RegisteredAlgorithm::Ecdsa(EcParameterType::SignOrVerify)),
            ("generateKey", RegisteredAlgorithm::Ecdsa(EcParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::Ecdsa(EcParameterType::Import)),
            ("exportKey", RegisteredAlgorithm::Ecdsa(EcParameterType::Base)),
        ] {
            define_an_algorithm(&mut internal_object, operation, "ECDSA", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#ecdh-registration
        for (operation, registered_algorithm) in [
            ("importKey", RegisteredAlgorithm::Ecdh(EcParameterType::Import)),
            ("exportKey", RegisteredAlgorithm::Ecdh(EcParameterType::Base)),
            ("deriveBits", RegisteredAlgorithm::Ecdh(EcParameterType::Derive)),
            ("generateKey", RegisteredAlgorithm::Ecdh(EcParameterType::KeyGen)),
        ] {
            define_an_algorithm(&mut internal_object, operation, "ECDH", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#aes-ctr-registration
        for (operation, registered_algorithm) in [
            ("encrypt", RegisteredAlgorithm::AesCtr(AesParameterType::Ctr)),
            ("decrypt", RegisteredAlgorithm::AesCtr(AesParameterType::Ctr)),
            ("generateKey", RegisteredAlgorithm::AesCtr(AesParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::AesCtr(AesParameterType::Base)),
            ("exportKey", RegisteredAlgorithm::AesCtr(AesParameterType::Base)),
            (
                "get key length",
                RegisteredAlgorithm::AesCtr(AesParameterType::DerivedKey),
            ),
        ] {
            define_an_algorithm(&mut internal_object, operation, "AES-CTR", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#aes-cbc-registration
        for (operation, registered_algorithm) in [
            ("encrypt", RegisteredAlgorithm::AesCbc(AesParameterType::Cbc)),
            ("decrypt", RegisteredAlgorithm::AesCbc(AesParameterType::Cbc)),
            ("generateKey", RegisteredAlgorithm::AesCbc(AesParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::AesCbc(AesParameterType::Base)),
            ("exportKey", RegisteredAlgorithm::AesCbc(AesParameterType::Base)),
            (
                "get key length",
                RegisteredAlgorithm::AesCbc(AesParameterType::DerivedKey),
            ),
        ] {
            define_an_algorithm(&mut internal_object, operation, "AES-CBC", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#aes-gcm-registration
        for (operation, registered_algorithm) in [
            ("encrypt", RegisteredAlgorithm::AesGcm(AesParameterType::Gcm)),
            ("decrypt", RegisteredAlgorithm::AesGcm(AesParameterType::Gcm)),
            ("generateKey", RegisteredAlgorithm::AesGcm(AesParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::AesGcm(AesParameterType::Base)),
            ("exportKey", RegisteredAlgorithm::AesGcm(AesParameterType::Base)),
            (
                "get key length",
                RegisteredAlgorithm::AesGcm(AesParameterType::DerivedKey),
            ),
        ] {
            define_an_algorithm(&mut internal_object, operation, "AES-GCM", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#aes-kw-registration
        for (operation, registered_algorithm) in [
            ("wrapKey", RegisteredAlgorithm::AesKw(AesParameterType::Base)),
            ("unwrapKey", RegisteredAlgorithm::AesKw(AesParameterType::Base)),
            ("generateKey", RegisteredAlgorithm::AesKw(AesParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::AesKw(AesParameterType::Base)),
            ("exportKey", RegisteredAlgorithm::AesKw(AesParameterType::Base)),
            (
                "get key length",
                RegisteredAlgorithm::AesKw(AesParameterType::DerivedKey),
            ),
        ] {
            define_an_algorithm(&mut internal_object, operation, "AES-KW", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#hmac-registration
        for (operation, registered_algorithm) in [
            ("sign", RegisteredAlgorithm::Hmac),
            ("verify", RegisteredAlgorithm::Hmac),
            ("generateKey", RegisteredAlgorithm::HmacKeyGen),
            ("importKey", RegisteredAlgorithm::HmacImport),
            ("exportKey", RegisteredAlgorithm::Hmac),
            ("get key length", RegisteredAlgorithm::HmacImport),
        ] {
            define_an_algorithm(&mut internal_object, operation, "HMAC", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#sha-registration
        define_an_algorithm(&mut internal_object, "digest", "SHA-1", RegisteredAlgorithm::Sha);
        define_an_algorithm(&mut internal_object, "digest", "SHA-256", RegisteredAlgorithm::Sha);
        define_an_algorithm(&mut internal_object, "digest", "SHA-384", RegisteredAlgorithm::Sha);
        define_an_algorithm(&mut internal_object, "digest", "SHA-512", RegisteredAlgorithm::Sha);

        // https://wicg.github.io/webcrypto-modern-algos/#sha3-registration
        define_an_algorithm(&mut internal_object, "digest", "SHA3-256", RegisteredAlgorithm::Sha);
        define_an_algorithm(&mut internal_object, "digest", "SHA3-384", RegisteredAlgorithm::Sha);
        define_an_algorithm(&mut internal_object, "digest", "SHA3-512", RegisteredAlgorithm::Sha);

        // https://wicg.github.io/webcrypto-modern-algos/#cshake-registration
        define_an_algorithm(&mut internal_object, "digest", "cSHAKE128", RegisteredAlgorithm::CShake);
        define_an_algorithm(&mut internal_object, "digest", "cSHAKE256", RegisteredAlgorithm::CShake);

        // https://w3c.github.io/webcrypto/#hkdf-registration
        for (operation, registered_algorithm) in [
            ("deriveBits", RegisteredAlgorithm::HkdfParams),
            ("importKey", RegisteredAlgorithm::Hkdf),
            ("get key length", RegisteredAlgorithm::Hkdf),
        ] {
            define_an_algorithm(&mut internal_object, operation, "HKDF", registered_algorithm);
        }

        // https://w3c.github.io/webcrypto/#pbkdf2-registration
        for (operation, registered_algorithm) in [
            ("deriveBits", RegisteredAlgorithm::Pbkdf2Params),
            ("importKey", RegisteredAlgorithm::Pbkdf2),
            ("get key length", RegisteredAlgorithm::Pbkdf2),
        ] {
            define_an_algorithm(&mut internal_object, operation, "PBKDF2", registered_algorithm);
        }

        // https://wicg.github.io/webcrypto-secure-curves/#x25519-registration
        for (operation, registered_algorithm) in [
            ("deriveBits", RegisteredAlgorithm::X25519(true)),
            ("generateKey", RegisteredAlgorithm::X25519(false)),
            ("importKey", RegisteredAlgorithm::X25519(false)),
            ("exportKey", RegisteredAlgorithm::X25519(false)),
        ] {
            define_an_algorithm(&mut internal_object, operation, "X25519", registered_algorithm);
        }

        // https://wicg.github.io/webcrypto-secure-curves/#x448-registration
        for (operation, registered_algorithm) in [
            ("deriveBits", RegisteredAlgorithm::X448(true)),
            ("generateKey", RegisteredAlgorithm::X448(false)),
            ("importKey", RegisteredAlgorithm::X448(false)),
            ("exportKey", RegisteredAlgorithm::X448(false)),
        ] {
            define_an_algorithm(&mut internal_object, operation, "X448", registered_algorithm);
        }

        // https://wicg.github.io/webcrypto-secure-curves/#ed25519-registration
        for (operation, registered_algorithm) in [
            ("sign", RegisteredAlgorithm::Ed25519),
            ("verify", RegisteredAlgorithm::Ed25519),
            ("generateKey", RegisteredAlgorithm::Ed25519),
            ("importKey", RegisteredAlgorithm::Ed25519),
            ("exportKey", RegisteredAlgorithm::Ed25519),
        ] {
            define_an_algorithm(&mut internal_object, operation, "Ed25519", registered_algorithm);
        }

        // https://wicg.github.io/webcrypto-secure-curves/#ed448-registration
        for (operation, registered_algorithm) in [
            ("sign", RegisteredAlgorithm::Ed448(true)),
            ("verify", RegisteredAlgorithm::Ed448(true)),
            ("generateKey", RegisteredAlgorithm::Ed448(false)),
            ("importKey", RegisteredAlgorithm::Ed448(false)),
            ("exportKey", RegisteredAlgorithm::Ed448(false)),
        ] {
            define_an_algorithm(&mut internal_object, operation, "Ed448", registered_algorithm);
        }

        // https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-registration
        for name in ["ML-DSA-44", "ML-DSA-65", "ML-DSA-87"] {
            for (operation, registered_algorithm) in [
                ("sign", RegisteredAlgorithm::MlDsa(true)),
                ("verify", RegisteredAlgorithm::MlDsa(true)),
                ("generateKey", RegisteredAlgorithm::MlDsa(false)),
                ("importKey", RegisteredAlgorithm::MlDsa(false)),
                ("exportKey", RegisteredAlgorithm::MlDsa(false)),
            ] {
                define_an_algorithm(&mut internal_object, operation, name, registered_algorithm);
            }
        }

        // https://wicg.github.io/webcrypto-modern-algos/#ml-kem-registration
        for name in ["ML-KEM-512", "ML-KEM-768", "ML-KEM-1024"] {
            for (operation, registered_algorithm) in [
                ("generateKey", RegisteredAlgorithm::MlKem),
                ("importKey", RegisteredAlgorithm::MlKem),
                ("exportKey", RegisteredAlgorithm::MlKem),
                ("encapsulate", RegisteredAlgorithm::MlKem),
                ("decapsulate", RegisteredAlgorithm::MlKem),
                ("get shared key length", RegisteredAlgorithm::MlKem),
            ] {
                define_an_algorithm(&mut internal_object, operation, name, registered_algorithm);
            }
        }

        // https://wicg.github.io/webcrypto-modern-algos/#argon2-registration
        for algorithm in ["Argon2d", "Argon2i", "Argon2id"] {
            for (operation, registered_algorithm) in [
                ("importKey", RegisteredAlgorithm::Argon2),
                ("deriveBits", RegisteredAlgorithm::Argon2Params),
                ("get key length", RegisteredAlgorithm::Argon2),
            ] {
                define_an_algorithm(&mut internal_object, operation, algorithm, registered_algorithm);
            }
        }

        // https://wicg.github.io/webcrypto-modern-algos/#chacha20-poly1305-registration
        for (operation, registered_algorithm) in [
            ("encrypt", RegisteredAlgorithm::ChaCha20Poly1305Aead),
            ("decrypt", RegisteredAlgorithm::ChaCha20Poly1305Aead),
            ("generateKey", RegisteredAlgorithm::ChaCha20Poly1305),
            ("importKey", RegisteredAlgorithm::ChaCha20Poly1305),
            ("exportKey", RegisteredAlgorithm::ChaCha20Poly1305),
            ("get key length", RegisteredAlgorithm::ChaCha20Poly1305),
        ] {
            define_an_algorithm(
                &mut internal_object,
                operation,
                "ChaCha20-Poly1305",
                registered_algorithm,
            );
        }

        // https://wicg.github.io/webcrypto-modern-algos/#aes-ocb-registration
        for (operation, registered_algorithm) in [
            ("encrypt", RegisteredAlgorithm::AesOcb(AesParameterType::Aead)),
            ("decrypt", RegisteredAlgorithm::AesOcb(AesParameterType::Aead)),
            ("generateKey", RegisteredAlgorithm::AesOcb(AesParameterType::KeyGen)),
            ("importKey", RegisteredAlgorithm::AesOcb(AesParameterType::Base)),
            ("exportKey", RegisteredAlgorithm::AesOcb(AesParameterType::Base)),
            (
                "get key length",
                RegisteredAlgorithm::AesOcb(AesParameterType::DerivedKey),
            ),
        ] {
            define_an_algorithm(&mut internal_object, operation, "AES-OCB", registered_algorithm);
        }

        // https://wicg.github.io/webcrypto-modern-algos/#kmac-registration
        for algorithm in ["KMAC128", "KMAC256"] {
            for (operation, registered_algorithm) in [
                ("sign", RegisteredAlgorithm::KmacSignOrVerify),
                ("verify", RegisteredAlgorithm::KmacSignOrVerify),
                ("generateKey", RegisteredAlgorithm::KmacKeyGen),
                ("importKey", RegisteredAlgorithm::KmacImport),
                ("exportKey", RegisteredAlgorithm::Kmac),
                ("get key length", RegisteredAlgorithm::KmacImport),
            ] {
                define_an_algorithm(&mut internal_object, operation, algorithm, registered_algorithm);
            }
        }

        internal_object
    });
    &SUPPORTED_ALGORITHMS
}

// https://w3c.github.io/webcrypto/#concept-define-an-algorithm
fn define_an_algorithm(
    internal_object: &mut SupportedAlgorithmsMap,
    op: &'static str,
    algorithm: &'static str,
    registered_algorithm: RegisteredAlgorithm,
) {
    // 1. Let registeredAlgorithms be the associative container stored at the op key of supportedAlgorithms.
    // NOTE: There should always be a container at the op key.
    let registered_algorithms = internal_object.get_mut(op).expect("the operation has a container");

    // 2. Set the alg key of registeredAlgorithms to the IDL dictionary type type.
    registered_algorithms.insert(algorithm, registered_algorithm);
}

fn create_algorithm_dictionary(vm: &Vm, realm: Gc<Realm>, name: &Utf16String) -> WebIdlResult<Gc<Object>> {
    let dictionary = create_dictionary_object(&CallContext::new(vm, realm));
    dictionary.create_data_property(
        vm,
        &PropertyKey::from_utf8("name"),
        Value::from_string(PrimitiveString::create(vm, name.clone())),
        None,
        None,
    )?;
    Ok(dictionary)
}
