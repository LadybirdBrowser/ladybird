/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The serialization steps of CryptoKey, https://w3c.github.io/webcrypto/#cryptokey-interface, which LibWeb implements in
//! C++ (Libraries/LibWeb/Crypto/CryptoKey.cpp).

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, RawNativeFunctionResult, Realm};

use crate::bindings::{Exception, WebIdlResult};
use crate::html::structured_serialize::cpp::{
    DecodedBytes, SerializationType, StructuredSerializeReader, StructuredSerializeWriter,
};

/// The fields of a CryptoKey, as Libraries/LibWeb/Crypto/CryptoKey.cpp hands them over and creates a key from. A key
/// that it hands over owns its strings for the receiver, and borrows its bytes from the key for the call.
#[repr(C)]
struct RawFields<'a> {
    /// Crypto::KeyType, in the order of Bindings::KeyType.
    r#type: u8,
    extractable: bool,
    algorithm_type: u8,
    algorithm_name: usize,
    modulus_length: u32,
    public_exponent: ak::Span<'a, u8>,
    hash: usize,
    named_curve: usize,
    length: u32,
    /// Bindings::KeyUsage.
    usages: ak::Span<'a, u8>,
    handle_tag: u8,
    handle_buffers: [ak::Span<'a, u8>; MAX_HANDLE_BUFFERS],
    handle_buffer_count: usize,
    has_ec_parameters: bool,
    ec_parameters: ak::Span<'a, u32>,
    has_ec_public_key: bool,
}

unsafe extern "C" {
    fn web_crypto_key_with_fields(
        key: *mut c_void,
        state: *mut c_void,
        steps: unsafe extern "C" fn(state: *mut c_void, fields: *const RawFields<'_>),
    );
    fn web_crypto_key_create(
        realm: *mut c_void,
        fields: &RawFields<'_>,
        out_key: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
}

const MAX_HANDLE_BUFFERS: usize = 8;

/// The identifiers by which a storage record holds a key type, in the order of Bindings::KeyType.
const KEY_TYPES: [&[u8]; 3] = [b"public", b"private", b"secret"];

/// The identifiers by which a storage record holds a key usage, in the order of Bindings::KeyUsage.
const KEY_USAGES: [&[u8]; 12] = [
    b"encrypt",
    b"decrypt",
    b"sign",
    b"verify",
    b"deriveKey",
    b"deriveBits",
    b"wrapKey",
    b"unwrapKey",
    b"encapsulateKey",
    b"encapsulateBits",
    b"decapsulateKey",
    b"decapsulateBits",
];

/// The types of a serialized algorithm, which are part of the stable storage serialization format.
const ALGORITHM_KEY: u8 = 0;
const ALGORITHM_RSA: u8 = 1;
const ALGORITHM_RSA_HASHED: u8 = 2;
const ALGORITHM_EC: u8 = 3;
const ALGORITHM_AES: u8 = 4;
const ALGORITHM_HMAC: u8 = 5;
const ALGORITHM_KMAC: u8 = 6;

/// The tags of a serialized handle, Crypto::HandleTag, which are part of the stable storage serialization format.
const HANDLE_EC_PRIVATE_KEY: u8 = 4;

/// The number of buffers of the handle of each tag, Crypto::HandleTag. An EC private key has one, then optionally its
/// parameters and its public key, whose two buffers follow.
const HANDLE_BUFFER_COUNTS: [usize; 11] = [1, 2, 8, 2, 1, 1, 3, 1, 3, 1, 1];

fn data_clone_error(message: &[u8]) -> Exception {
    Exception::Dom("DataCloneError", Utf16String::from_ascii(message))
}

fn encode_identifier(serialized: StructuredSerializeWriter, identifier: &[u8]) {
    serialized.encode_utf16_string(&Utf16String::from_ascii(identifier));
}

/// The index of the identifier that a storage record holds.
fn decode_identifier(
    serialized: StructuredSerializeReader,
    identifiers: &[&[u8]],
    unknown_message: &[u8],
) -> WebIdlResult<u8> {
    let identifier = serialized.decode_utf16_string()?;
    identifiers
        .iter()
        .position(|candidate| identifier == Utf16String::from_ascii(candidate))
        .map(|index| u8::try_from(index).expect("an identifier list is short"))
        .ok_or_else(|| data_clone_error(unknown_message))
}

/// Runs the serialization steps of the CryptoKey, given the record.
pub(crate) fn serialization_steps(key: Gc<Object>, serialized: StructuredSerializeWriter) {
    unsafe extern "C" fn steps(state: *mut c_void, fields: *const RawFields<'_>) {
        // SAFETY: The state is the writer that serialization_steps() lends, and the fields live for the call.
        let (serialized, fields) = unsafe { (*state.cast::<StructuredSerializeWriter>(), &*fields) };
        write_fields(serialized, fields);
    }
    let mut state = serialized;
    // SAFETY: The object is a live CryptoKey, and the state lives for the call.
    unsafe { web_crypto_key_with_fields(key.as_ptr().cast(), (&raw mut state).cast(), steps) };
}

fn write_fields(serialized: StructuredSerializeWriter, value: &RawFields<'_>) {
    let for_storage = serialized.serialization_type() == SerializationType::Storage;
    // SAFETY: LibWeb hands over the strings.
    let (algorithm_name, hash, named_curve) = unsafe {
        (
            Utf16String::from_raw_owned(value.algorithm_name),
            Utf16String::from_raw_owned(value.hash),
            Utf16String::from_raw_owned(value.named_curve),
        )
    };

    // 1. Set serialized.[[Type]] to the [[type]] internal slot of value.
    if for_storage {
        encode_identifier(serialized, KEY_TYPES[usize::from(value.r#type)]);
    } else {
        serialized.encode_i32(i32::from(value.r#type));
    }

    // 2. Set serialized.[[Extractable]] to the [[extractable]] internal slot of value.
    serialized.encode_bool(value.extractable);

    // 3. Set serialized.[[Algorithm]] to the sub-serialization of the [[algorithm]] internal slot of value.
    serialized.encode_u8(value.algorithm_type);
    serialized.encode_utf16_string(&algorithm_name);
    match value.algorithm_type {
        ALGORITHM_RSA | ALGORITHM_RSA_HASHED => {
            serialized.encode_u32(value.modulus_length);
            serialized.encode_bytes(value.public_exponent.as_slice());
            if value.algorithm_type == ALGORITHM_RSA_HASHED {
                serialized.encode_utf16_string(&hash);
            }
        }
        ALGORITHM_EC => serialized.encode_utf16_string(&named_curve),
        ALGORITHM_AES => serialized.encode_u16(u16::try_from(value.length).expect("an AES key has a 16-bit length")),
        ALGORITHM_HMAC => {
            serialized.encode_utf16_string(&hash);
            serialized.encode_u32(value.length);
        }
        ALGORITHM_KMAC => serialized.encode_u32(value.length),
        ALGORITHM_KEY => {}
        _ => unreachable!("LibWeb hands over the type of an algorithm that a record holds"),
    }

    // 4. Set serialized.[[Usages]] to the sub-serialization of the [[usages]] internal slot of value.
    let usages = value.usages.as_slice();
    serialized.encode_u64(usages.len() as u64);
    for &usage in usages {
        if for_storage {
            encode_identifier(serialized, KEY_USAGES[usize::from(usage)]);
        } else {
            serialized.encode_u8(usage);
        }
    }

    // 5. Set serialized.[[Handle]] to the [[handle]] internal slot of value.
    serialized.encode_u8(value.handle_tag);
    let buffers = &value.handle_buffers[..value.handle_buffer_count];
    if value.handle_tag != HANDLE_EC_PRIVATE_KEY {
        for buffer in buffers {
            serialized.encode_bytes(buffer.as_slice());
        }
        return;
    }
    serialized.encode_bytes(buffers[0].as_slice());
    serialized.encode_bool(value.has_ec_parameters);
    if value.has_ec_parameters {
        let parameters = value.ec_parameters.as_slice();
        serialized.encode_u64(parameters.len() as u64);
        for &parameter in parameters {
            serialized.encode_u32(parameter);
        }
    }
    serialized.encode_bool(value.has_ec_public_key);
    if value.has_ec_public_key {
        serialized.encode_bytes(buffers[1].as_slice());
        serialized.encode_bytes(buffers[2].as_slice());
    }
}

/// Runs the deserialization steps of CryptoKey, which create the key in the realm from what they read.
pub(crate) fn deserialization_steps(
    realm: Gc<Realm>,
    serialized: StructuredSerializeReader,
) -> WebIdlResult<Gc<Object>> {
    let for_storage = serialized.serialization_type() == SerializationType::Storage;

    // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
    let r#type = if for_storage {
        decode_identifier(serialized, &KEY_TYPES, b"Unknown structured serialize key type")?
    } else {
        u8::try_from(serialized.decode_i32()?)
            .ok()
            .filter(|&r#type| usize::from(r#type) < KEY_TYPES.len())
            .ok_or_else(|| data_clone_error(b"Invalid structured serialize key type"))?
    };

    // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
    let extractable = serialized.decode_bool()?;

    // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
    let algorithm_type = serialized.decode_u8()?;
    if algorithm_type > ALGORITHM_KMAC {
        return Err(data_clone_error(b"Invalid CryptoKey algorithm"));
    }
    let algorithm_name = serialized.decode_utf16_string()?;
    let (mut modulus_length, mut public_exponent, mut hash, mut named_curve, mut length) =
        (0, None, Utf16String::default(), Utf16String::default(), 0);
    match algorithm_type {
        ALGORITHM_RSA | ALGORITHM_RSA_HASHED => {
            modulus_length = serialized.decode_u32()?;
            public_exponent = Some(serialized.decode_byte_buffer()?);
            if algorithm_type == ALGORITHM_RSA_HASHED {
                hash = serialized.decode_utf16_string()?;
            }
        }
        ALGORITHM_EC => named_curve = serialized.decode_utf16_string()?,
        ALGORITHM_AES => length = u32::from(serialized.decode_u16()?),
        ALGORITHM_HMAC => {
            hash = serialized.decode_utf16_string()?;
            length = serialized.decode_u32()?;
        }
        ALGORITHM_KMAC => length = serialized.decode_u32()?,
        ALGORITHM_KEY => {}
        _ => unreachable!("the type is that of an algorithm, as checked above"),
    }

    // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
    let usage_count = serialized.decode_u64()?;
    let mut usages = Vec::new();
    for _ in 0..usage_count {
        let usage = if for_storage {
            decode_identifier(serialized, &KEY_USAGES, b"Unknown structured serialize key usage")?
        } else {
            Some(serialized.decode_u8()?)
                .filter(|&usage| usize::from(usage) < KEY_USAGES.len())
                .ok_or_else(|| data_clone_error(b"Invalid structured serialize key usage"))?
        };
        usages.push(usage);
    }

    // 5. Initialize the [[handle]] internal slot of value to serialized.[[Handle]].
    let handle_tag = serialized.decode_u8()?;
    let Some(&buffer_count) = HANDLE_BUFFER_COUNTS.get(usize::from(handle_tag)) else {
        return Err(data_clone_error(b"Invalid CryptoKey handle tag"));
    };
    let mut buffers: Vec<DecodedBytes> = Vec::new();
    let (mut has_ec_parameters, mut ec_parameters, mut has_ec_public_key) = (false, Vec::new(), false);
    if handle_tag == HANDLE_EC_PRIVATE_KEY {
        buffers.push(serialized.decode_byte_buffer()?);
        has_ec_parameters = serialized.decode_bool()?;
        if has_ec_parameters {
            let parameter_count = serialized.decode_u64()?;
            for _ in 0..parameter_count {
                ec_parameters.push(serialized.decode_u32()?);
            }
        }
        has_ec_public_key = serialized.decode_bool()?;
        if has_ec_public_key {
            buffers.push(serialized.decode_byte_buffer()?);
            buffers.push(serialized.decode_byte_buffer()?);
        }
    } else {
        for _ in 0..buffer_count {
            buffers.push(serialized.decode_byte_buffer()?);
        }
    }

    let mut handle_buffers = [ak::Span::default(); MAX_HANDLE_BUFFERS];
    for (span, buffer) in handle_buffers.iter_mut().zip(&buffers) {
        *span = ak::Span::new(buffer.bytes());
    }
    let fields = RawFields {
        r#type,
        extractable,
        algorithm_type,
        algorithm_name: algorithm_name.raw_identity(),
        modulus_length,
        public_exponent: public_exponent
            .as_ref()
            .map_or_else(ak::Span::default, |bytes| ak::Span::new(bytes.bytes())),
        hash: hash.raw_identity(),
        named_curve: named_curve.raw_identity(),
        length,
        usages: ak::Span::new(&usages),
        handle_tag,
        handle_buffers,
        handle_buffer_count: buffers.len(),
        has_ec_parameters,
        ec_parameters: ak::Span::new(&ec_parameters),
        has_ec_public_key,
    };
    let mut key = core::ptr::null_mut();
    // SAFETY: The realm is live, and the call borrows the fields.
    unsafe { web_crypto_key_create(realm.as_ptr().cast(), &fields, &mut key) }.into_completion()?;
    // SAFETY: LibWeb returns a live object.
    Ok(unsafe { Gc::from_non_null(NonNull::new(key.cast()).expect("LibWeb creates the key")) })
}
