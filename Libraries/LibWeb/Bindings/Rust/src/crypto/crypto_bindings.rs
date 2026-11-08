/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferShared, ErrorKind, ErrorType, Gc, PropertyKey, Realm, ThrowCompletionOr, Trace, Utf16View,
    Value, Vm,
};

use crate::bindings::interface::{Ref, create_dictionary_object, implementation_if_wrapper, wrap};
use crate::bindings::promise::{WebIdlPromise, resolve_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::crypto::crypto_algorithms::{Bytes, EncapsulatedBitsData, EncapsulatedKeyData};
use crate::crypto::crypto_key::CryptoKey;

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RsaOtherPrimesInfo {
    pub(crate) d: Option<Utf16String>,
    pub(crate) r: Option<Utf16String>,
    pub(crate) t: Option<Utf16String>,
}

#[derive(Clone, Default, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct JsonWebKey {
    pub(crate) alg: Option<Utf16String>,
    pub(crate) crv: Option<Utf16String>,
    pub(crate) d: Option<Utf16String>,
    pub(crate) dp: Option<Utf16String>,
    pub(crate) dq: Option<Utf16String>,
    pub(crate) e: Option<Utf16String>,
    pub(crate) k: Option<Utf16String>,
    pub(crate) kty: Option<Utf16String>,
    pub(crate) n: Option<Utf16String>,
    pub(crate) p: Option<Utf16String>,
    pub(crate) r#priv: Option<Utf16String>,
    pub(crate) r#pub: Option<Utf16String>,
    pub(crate) q: Option<Utf16String>,
    pub(crate) qi: Option<Utf16String>,
    pub(crate) r#use: Option<Utf16String>,
    pub(crate) x: Option<Utf16String>,
    pub(crate) y: Option<Utf16String>,
    pub(crate) ext: Option<bool>,
    pub(crate) key_ops: Option<Vec<Utf16String>>,
    pub(crate) oth: Option<Vec<RsaOtherPrimesInfo>>,
}

pub(crate) struct CryptoKeyPair {
    pub(crate) public_key: Option<Ref<CryptoKey>>,
    pub(crate) private_key: Option<Ref<CryptoKey>>,
}

pub(crate) struct EncapsulatedKey {
    pub(crate) shared_key: Option<Ref<CryptoKey>>,
    pub(crate) ciphertext: Option<Gc<ArrayBuffer>>,
}

pub(crate) struct EncapsulatedBits {
    pub(crate) shared_key: Option<Gc<ArrayBuffer>>,
    pub(crate) ciphertext: Option<Gc<ArrayBuffer>>,
}

// https://w3c.github.io/webcrypto/#concept-parse-a-jwk
pub(crate) fn parse_json_web_key(vm: &Vm, data: &[u8]) -> WebIdlResult<JsonWebKey> {
    // 1. Let data be the sequence of bytes to be parsed.

    // 2. Let json be the Unicode string that results from interpreting data according to UTF-8.
    // 3. Convert json to UTF-16.
    let data = data.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(data);
    let json = Utf16String::from_utf8(&String::from_utf8_lossy(data));

    // 4. Let result be the object literal that results from executing the JSON.parse internal function
    //    in the context of a new global object, with text argument set to a JavaScript String containing json.
    // 5. Let key be the result of converting result to the IDL dictionary type of JsonWebKey.
    #[cfg(web_idl_bindings)]
    let key = JsonWebKey::from_json(vm, Utf16View::of_string(&json))?;
    #[cfg(not(web_idl_bindings))]
    let key: JsonWebKey = bindings_unavailable((vm, Utf16View::of_string(&json)));

    // 6. If the kty field of key is not defined, then throw a DataError.
    if key.kty.is_none() {
        return Err(Exception::Dom(
            "DataError",
            Utf16String::from_ascii(b"kty field is not defined"),
        ));
    }

    // 7. Return key.
    Ok(key)
}

#[cfg(not(web_idl_bindings))]
pub(crate) fn bindings_unavailable<T>(_: impl Sized) -> T {
    unreachable!("Crypto dictionary conversions require the generated bindings")
}

pub(crate) fn resolve_crypto_key_promise(vm: &Vm, realm: Gc<Realm>, promise: WebIdlPromise, key: Ref<CryptoKey>) {
    resolve_promise(vm, promise, Value::from_object(wrap(&CallContext::new(vm, realm), key)));
}

pub(crate) fn encapsulated_bits(
    vm: &Vm,
    realm: Gc<Realm>,
    encapsulated_bits: &EncapsulatedBitsData,
) -> ThrowCompletionOr<Value> {
    let object = create_dictionary_object(&CallContext::new(vm, realm));

    if let Some(shared_key) = &encapsulated_bits.shared_key {
        object.create_data_property(
            vm,
            &PropertyKey::from_utf8("sharedKey"),
            array_buffer(vm, realm, shared_key),
            None,
            None,
        )?;
    }

    if let Some(ciphertext) = &encapsulated_bits.ciphertext {
        object.create_data_property(
            vm,
            &PropertyKey::from_utf8("ciphertext"),
            array_buffer(vm, realm, ciphertext),
            None,
            None,
        )?;
    }

    Ok(Value::from_object(object))
}

pub(crate) fn encapsulated_key(
    vm: &Vm,
    realm: Gc<Realm>,
    encapsulated_key: &EncapsulatedKeyData,
) -> ThrowCompletionOr<Value> {
    let cx = CallContext::new(vm, realm);
    let object = create_dictionary_object(&cx);

    object.create_data_property(
        vm,
        &PropertyKey::from_utf8("sharedKey"),
        Value::from_object(wrap(&cx, encapsulated_key.shared_key)),
        None,
        None,
    )?;

    if let Some(ciphertext) = &encapsulated_key.ciphertext {
        object.create_data_property(
            vm,
            &PropertyKey::from_utf8("ciphertext"),
            array_buffer(vm, realm, ciphertext),
            None,
            None,
        )?;
    }

    Ok(Value::from_object(object))
}

pub(crate) fn crypto_key_pair(vm: &Vm, realm: Gc<Realm>, key_pair: &CryptoKeyPair) -> ThrowCompletionOr<Value> {
    let cx = CallContext::new(vm, realm);
    let object = create_dictionary_object(&cx);
    let public_key = key_pair.public_key.expect("a generated pair has a public key");
    let private_key = key_pair.private_key.expect("a generated pair has a private key");
    object.create_data_property_or_throw(
        vm,
        &PropertyKey::from_utf8("publicKey"),
        Value::from_object(wrap(&cx, public_key)),
    )?;
    object.create_data_property_or_throw(
        vm,
        &PropertyKey::from_utf8("privateKey"),
        Value::from_object(wrap(&cx, private_key)),
    )?;
    Ok(Value::from_object(object))
}

pub(crate) fn array_buffer(vm: &Vm, realm: Gc<Realm>, bytes: &Bytes) -> Value {
    Value::from_object(ArrayBuffer::create_from_bytes(
        vm,
        realm,
        bytes.as_slice(),
        ArrayBufferShared::No,
    ))
}

pub(super) fn crypto_key_from_value(vm: &Vm, value: Value) -> ThrowCompletionOr<Ref<CryptoKey>> {
    let key_object = value.to_object(vm)?;
    let Some(key) = implementation_if_wrapper::<CryptoKey>(key_object) else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"CryptoKey"]);
    };
    Ok(key)
}
