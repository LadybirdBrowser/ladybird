/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libcrypto_rust::{HandleTag, KeyHandle, OkpCurve};
use libjs_rust::api::{
    Array, CellRoot, ErrorKind, ErrorType, Gc, GcRefCell, GcWeak, IntegrityLevel, MarkedVec, Must, Object,
    PrimitiveString, PropertyKey, Trace, Value, Vm,
};

use crate::bindings::conversions::create_uint8_array;
use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate, create_dictionary_object};
use crate::bindings::setlike::WorldEntries;
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum KeyType {
    Public,
    Private,
    Secret,
}

const KEY_TYPE_VALUES: [KeyType; 3] = [KeyType::Public, KeyType::Private, KeyType::Secret];

#[derive(Clone, Copy, PartialEq, Eq, Trace)]
#[gc(crate = libjs_rust)]
#[repr(u8)]
pub(crate) enum KeyUsage {
    Encrypt,
    Decrypt,
    Sign,
    Verify,
    DeriveKey,
    DeriveBits,
    WrapKey,
    UnwrapKey,
    EncapsulateKey,
    EncapsulateBits,
    DecapsulateKey,
    DecapsulateBits,
}

const KEY_USAGE_VALUES: [KeyUsage; 12] = [
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
];

#[derive(Clone)]
pub(crate) enum InternalAlgorithmData {
    Key {
        name: Utf16String,
    },
    Rsa {
        name: Utf16String,
        modulus_length: u32,
        public_exponent: Vec<u8>,
    },
    RsaHashed {
        name: Utf16String,
        modulus_length: u32,
        public_exponent: Vec<u8>,
        hash: Utf16String,
    },
    Ec {
        name: Utf16String,
        named_curve: Utf16String,
    },
    Aes {
        name: Utf16String,
        length: u16,
    },
    Hmac {
        name: Utf16String,
        hash: Utf16String,
        length: u32,
    },
    Kmac {
        name: Utf16String,
        length: u32,
    },
}

impl InternalAlgorithmData {
    fn name(&self) -> &Utf16String {
        match self {
            Self::Key { name }
            | Self::Rsa { name, .. }
            | Self::RsaHashed { name, .. }
            | Self::Ec { name, .. }
            | Self::Aes { name, .. }
            | Self::Hmac { name, .. }
            | Self::Kmac { name, .. } => name,
        }
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct CachedAlgorithmObject {
    wrapper: GcWeak<Object>,
    object: Gc<Object>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CryptoKey {
    #[gc(untraced)]
    r#type: Cell<Option<KeyType>>,
    #[gc(untraced)]
    extractable: Cell<bool>,
    #[gc(untraced)]
    algorithm: RefCell<InternalAlgorithmData>,
    #[gc(untraced)]
    key_usages: RefCell<Vec<KeyUsage>>,
    #[gc(untraced)]
    handle: KeyHandle,
    cached_algorithm_objects: GcRefCell<Vec<CachedAlgorithmObject>>,
    cached_usages_objects: WorldEntries<Object>,
}

impl Wrappable for CryptoKey {}

fn create_key_algorithm_object(cx: &CallContext, name: &Utf16String) -> WebIdlResult<Gc<Object>> {
    let object = create_dictionary_object(cx);
    object.create_data_property_or_throw(
        cx.vm(),
        &PropertyKey::from_utf8("name"),
        Value::from_string(PrimitiveString::create(cx.vm(), name.clone())),
    )?;
    Ok(object)
}

fn create_algorithm_object(cx: &CallContext, algorithm: &InternalAlgorithmData) -> WebIdlResult<Gc<Object>> {
    let vm = cx.vm();
    match algorithm {
        InternalAlgorithmData::Key { name } => create_key_algorithm_object(cx, name),
        InternalAlgorithmData::Rsa {
            name,
            modulus_length,
            public_exponent,
        } => {
            let object = create_key_algorithm_object(cx, name)?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("modulusLength"),
                Value::from_f64(f64::from(*modulus_length)),
            )?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("publicExponent"),
                Value::from_object(create_uint8_array(cx, public_exponent)?),
            )?;
            Ok(object)
        }
        InternalAlgorithmData::RsaHashed {
            name,
            modulus_length,
            public_exponent,
            hash,
        } => {
            let object = create_key_algorithm_object(cx, name)?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("modulusLength"),
                Value::from_f64(f64::from(*modulus_length)),
            )?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("publicExponent"),
                Value::from_object(create_uint8_array(cx, public_exponent)?),
            )?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("hash"),
                Value::from_object(create_key_algorithm_object(cx, hash)?),
            )?;
            Ok(object)
        }
        InternalAlgorithmData::Ec { name, named_curve } => {
            let object = create_key_algorithm_object(cx, name)?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("namedCurve"),
                Value::from_string(PrimitiveString::create(vm, named_curve.clone())),
            )?;
            Ok(object)
        }
        InternalAlgorithmData::Aes { name, length } => {
            let object = create_key_algorithm_object(cx, name)?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("length"),
                Value::from_i32(i32::from(*length)),
            )?;
            Ok(object)
        }
        InternalAlgorithmData::Hmac { name, hash, length } => {
            let object = create_key_algorithm_object(cx, name)?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("hash"),
                Value::from_object(create_key_algorithm_object(cx, hash)?),
            )?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("length"),
                Value::from_f64(f64::from(*length)),
            )?;
            Ok(object)
        }
        InternalAlgorithmData::Kmac { name, length } => {
            let object = create_key_algorithm_object(cx, name)?;
            object.create_data_property_or_throw(
                vm,
                &PropertyKey::from_utf8("length"),
                Value::from_f64(f64::from(*length)),
            )?;
            Ok(object)
        }
    }
}

#[allow(
    clippy::unnecessary_wraps,
    reason = "the generated bindings can propagate exceptions from every getter"
)]
impl CryptoKey {
    pub(crate) fn create(vm: &Vm, handle: KeyHandle) -> Ref<Self> {
        allocate(vm, |_| Self {
            r#type: Cell::new(None),
            extractable: Cell::new(false),
            algorithm: RefCell::new(InternalAlgorithmData::Key {
                name: Utf16String::default(),
            }),
            key_usages: RefCell::default(),
            handle,
            cached_algorithm_objects: GcRefCell::default(),
            cached_usages_objects: WorldEntries::default(),
        })
    }

    pub(crate) fn from_byte_handle(vm: &Vm, bytes: &[u8]) -> Ref<Self> {
        Self::create(vm, KeyHandle::from_secret_bytes(bytes))
    }

    pub(crate) fn byte_handle_bytes(&self) -> &[u8] {
        self.handle.secret_bytes()
    }

    pub(crate) fn handle(&self) -> &KeyHandle {
        &self.handle
    }

    pub(crate) fn internal_algorithm(&self) -> core::cell::Ref<'_, InternalAlgorithmData> {
        self.algorithm.borrow()
    }

    pub(crate) fn internal_usages(&self) -> core::cell::Ref<'_, [KeyUsage]> {
        core::cell::Ref::map(self.key_usages.borrow(), Vec::as_slice)
    }

    pub(crate) fn is_extractable(&self) -> bool {
        self.extractable.get()
    }

    pub(crate) fn set_extractable(&self, extractable: bool) {
        self.extractable.set(extractable);
    }

    pub(crate) fn set_type(&self, key_type: KeyType) {
        self.r#type.set(Some(key_type));
    }

    pub(crate) fn set_algorithm(&self, algorithm: InternalAlgorithmData) {
        self.algorithm.replace(algorithm);
        self.cached_algorithm_objects.replace(Vec::new());
    }

    pub(crate) fn set_usages(&self, usages: Vec<KeyUsage>) {
        self.key_usages.replace(usages);
    }

    pub(crate) fn key_type(&self) -> KeyType {
        self.r#type
            .get()
            .expect("the key type is initialized before it is read")
    }

    pub(crate) fn extractable(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.extractable.get())
    }

    pub(crate) fn bindings_type(&self, _: &mut CallContext) -> WebIdlResult<KeyType> {
        Ok(self.key_type())
    }

    pub(crate) fn algorithm(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        let wrapper = cx.vm().this_value().as_object();
        self.cached_algorithm_objects
            .borrow_mut(cx.vm())
            .retain(|entry| entry.wrapper.get().is_some());

        let cached_object = self
            .cached_algorithm_objects
            .borrow()
            .iter()
            .find_map(|entry| (entry.wrapper.get() == Some(wrapper)).then_some(entry.object));
        if let Some(object) = cached_object {
            return Ok(Value::from_object(object));
        }

        let object = create_algorithm_object(cx, &self.algorithm.borrow())?;
        self.cached_algorithm_objects
            .borrow_mut(cx.vm())
            .push(CachedAlgorithmObject {
                wrapper: GcWeak::new(cx.vm().heap(), wrapper),
                object,
            });
        Ok(Value::from_object(object))
    }

    pub(crate) fn usages(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        let usages = self.cached_usages_objects.get_or_create(cx, || {
            let vm = cx.vm();
            let values = MarkedVec::new(vm);
            for usage in self.key_usages.borrow().iter() {
                values.push(Value::from_string(PrimitiveString::create_from_ascii(
                    vm,
                    KEY_USAGES[*usage as usize],
                )));
            }
            let usages = CellRoot::new(Array::create_from_list(vm, cx.realm(), &values));
            usages.cell().set_integrity_level(vm, IntegrityLevel::Frozen).must();
            usages.cell().upcast()
        });
        Ok(Value::from_object(usages))
    }

    pub(crate) fn algorithm_name(&self) -> Utf16String {
        self.algorithm.borrow().name().clone()
    }
}

/// The identifiers by which a storage record holds a key type.
const KEY_TYPES: [&[u8]; 3] = [b"public", b"private", b"secret"];

/// The identifiers by which a storage record holds a key usage.
pub(super) const KEY_USAGES: [&[u8]; 12] = [
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

fn data_clone_error(message: &[u8]) -> Exception {
    Exception::Dom("DataCloneError", Utf16String::from_ascii(message))
}

fn encode_identifier(serialized: &mut SerializationWriter, identifier: &[u8]) {
    serialized.write_utf16_string(&Utf16String::from_ascii(identifier));
}

/// The index of the identifier that a storage record holds.
fn decode_identifier(
    serialized: &mut SerializationReader,
    identifiers: &[&[u8]],
    unknown_message: &[u8],
) -> WebIdlResult<u8> {
    let identifier = serialized.read_utf16_string()?;
    identifiers
        .iter()
        .position(|candidate| identifier == Utf16String::from_ascii(candidate))
        .map(|index| u8::try_from(index).expect("an identifier list is short"))
        .ok_or_else(|| data_clone_error(unknown_message))
}

impl Serializable for CryptoKey {
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        for_storage: bool,
    ) -> WebIdlResult<()> {
        // 1. Set serialized.[[Type]] to the [[type]] internal slot of value.
        let key_type = self.key_type() as u8;
        if for_storage {
            encode_identifier(serialized, KEY_TYPES[usize::from(key_type)]);
        } else {
            serialized.write_i32(i32::from(key_type));
        }

        // 2. Set serialized.[[Extractable]] to the [[extractable]] internal slot of value.
        serialized.write_bool(self.extractable.get());

        // 3. Set serialized.[[Algorithm]] to the sub-serialization of the [[algorithm]] internal slot of value.
        let algorithm = self.algorithm.borrow();
        serialized.write_u8(algorithm.serialized_type());
        serialized.write_utf16_string(algorithm.name());
        match &*algorithm {
            InternalAlgorithmData::Key { .. } => {}
            InternalAlgorithmData::Rsa {
                modulus_length,
                public_exponent,
                ..
            } => {
                serialized.write_u32(*modulus_length);
                serialized.write_bytes(public_exponent);
            }
            InternalAlgorithmData::RsaHashed {
                modulus_length,
                public_exponent,
                hash,
                ..
            } => {
                serialized.write_u32(*modulus_length);
                serialized.write_bytes(public_exponent);
                serialized.write_utf16_string(hash);
            }
            InternalAlgorithmData::Ec { named_curve, .. } => serialized.write_utf16_string(named_curve),
            InternalAlgorithmData::Aes { length, .. } => serialized.write_u16(*length),
            InternalAlgorithmData::Hmac { hash, length, .. } => {
                serialized.write_utf16_string(hash);
                serialized.write_u32(*length);
            }
            InternalAlgorithmData::Kmac { length, .. } => serialized.write_u32(*length),
        }

        // 4. Set serialized.[[Usages]] to the sub-serialization of the [[usages]] internal slot of value.
        let usages = self.key_usages.borrow();
        serialized.write_u64(usages.len() as u64);
        for &usage in usages.iter() {
            if for_storage {
                encode_identifier(serialized, KEY_USAGES[usage as usize]);
            } else {
                serialized.write_u8(usage as u8);
            }
        }

        // 5. Set serialized.[[Handle]] to the [[handle]] internal slot of value.
        // NB: An EC private key's first buffer is followed by a has-parameters flag, which is false, and a
        //     has-public-key flag, which is true, before its public key's buffers.
        let stored = self.handle().to_stored();
        serialized.write_u8(stored.tag as u8);
        for (index, buffer) in stored.buffers().iter().enumerate() {
            serialized.write_bytes(buffer);
            if index == 0 && stored.tag == HandleTag::EcPrivate {
                serialized.write_bool(false);
                serialized.write_bool(true);
            }
        }
        Ok(())
    }

    fn deserialization_steps(cx: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Ref<Self>> {
        let for_storage = serialized.is_for_storage();

        // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
        let r#type = if for_storage {
            decode_identifier(serialized, &KEY_TYPES, b"Unknown structured serialize key type")?
        } else {
            u8::try_from(serialized.read_i32()?)
                .ok()
                .filter(|&r#type| usize::from(r#type) < KEY_TYPES.len())
                .ok_or_else(|| data_clone_error(b"Invalid structured serialize key type"))?
        };

        // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
        let extractable = serialized.read_bool()?;

        // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
        let algorithm_type = serialized.read_u8()?;
        if algorithm_type > ALGORITHM_KMAC {
            return Err(data_clone_error(b"Invalid CryptoKey algorithm"));
        }
        let name = serialized.read_utf16_string()?;
        let (mut modulus_length, mut public_exponent, mut hash, mut named_curve, mut length) =
            (0, &[][..], Utf16String::default(), Utf16String::default(), 0);
        match algorithm_type {
            ALGORITHM_RSA | ALGORITHM_RSA_HASHED => {
                modulus_length = serialized.read_u32()?;
                public_exponent = serialized.read_borrowed_bytes()?;
                if algorithm_type == ALGORITHM_RSA_HASHED {
                    hash = serialized.read_utf16_string()?;
                }
            }
            ALGORITHM_EC => named_curve = serialized.read_utf16_string()?,
            ALGORITHM_AES => length = u32::from(serialized.read_u16()?),
            ALGORITHM_HMAC => {
                hash = serialized.read_utf16_string()?;
                length = serialized.read_u32()?;
            }
            ALGORITHM_KMAC => length = serialized.read_u32()?,
            ALGORITHM_KEY => {}
            _ => unreachable!("the type is that of an algorithm, as checked above"),
        }

        // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
        let usage_count = serialized.read_u64()?;
        let mut usages = Vec::new();
        for _ in 0..usage_count {
            let usage = if for_storage {
                decode_identifier(serialized, &KEY_USAGES, b"Unknown structured serialize key usage")?
            } else {
                Some(serialized.read_u8()?)
                    .filter(|&usage| usize::from(usage) < KEY_USAGES.len())
                    .ok_or_else(|| data_clone_error(b"Invalid structured serialize key usage"))?
            };
            usages.push(KEY_USAGE_VALUES[usize::from(usage)]);
        }

        // 5. Initialize the [[handle]] internal slot of value to serialized.[[Handle]].
        let Ok(handle_tag) = HandleTag::try_from(serialized.read_u8()?) else {
            return Err(data_clone_error(b"Invalid CryptoKey handle tag"));
        };
        let mut buffers: Vec<&[u8]> = Vec::new();
        let (mut has_ec_parameters, mut ec_parameters, mut has_ec_public_key) = (false, Vec::new(), false);
        if handle_tag == HandleTag::EcPrivate {
            buffers.push(serialized.read_borrowed_bytes()?);
            has_ec_parameters = serialized.read_bool()?;
            if has_ec_parameters {
                let parameter_count = serialized.read_u64()?;
                for _ in 0..parameter_count {
                    ec_parameters.push(serialized.read_u32()?);
                }
            }
            has_ec_public_key = serialized.read_bool()?;
            if has_ec_public_key {
                buffers.push(serialized.read_borrowed_bytes()?);
                buffers.push(serialized.read_borrowed_bytes()?);
            }
        } else {
            for _ in 0..handle_tag.buffer_count() {
                buffers.push(serialized.read_borrowed_bytes()?);
            }
        }

        let okp_curve = [
            ("Ed25519", OkpCurve::Ed25519),
            ("Ed448", OkpCurve::Ed448),
            ("X25519", OkpCurve::X25519),
            ("X448", OkpCurve::X448),
        ]
        .into_iter()
        .find_map(|(curve_name, curve)| (name == curve_name).then_some(curve));
        let Ok(handle) = KeyHandle::from_stored(
            handle_tag,
            &buffers,
            has_ec_parameters.then_some(ec_parameters.as_slice()),
            has_ec_public_key,
            okp_curve,
        ) else {
            return Err(data_clone_error(b"Invalid CryptoKey handle components"));
        };
        let key = Self::create(cx.vm(), handle);

        let mut public_exponent_bytes = Vec::new();
        if public_exponent_bytes.try_reserve_exact(public_exponent.len()).is_err() {
            return cx
                .vm()
                .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .map_err(Into::into);
        }
        public_exponent_bytes.extend_from_slice(public_exponent);
        key.r#type.set(Some(KEY_TYPE_VALUES[usize::from(r#type)]));
        key.extractable.set(extractable);
        key.set_algorithm(match algorithm_type {
            ALGORITHM_KEY => InternalAlgorithmData::Key { name },
            ALGORITHM_RSA => InternalAlgorithmData::Rsa {
                name,
                modulus_length,
                public_exponent: public_exponent_bytes,
            },
            ALGORITHM_RSA_HASHED => InternalAlgorithmData::RsaHashed {
                name,
                modulus_length,
                public_exponent: public_exponent_bytes,
                hash,
            },
            ALGORITHM_EC => InternalAlgorithmData::Ec { name, named_curve },
            ALGORITHM_AES => InternalAlgorithmData::Aes {
                name,
                length: u16::try_from(length).expect("an AES key has a 16-bit length"),
            },
            ALGORITHM_HMAC => InternalAlgorithmData::Hmac { name, hash, length },
            ALGORITHM_KMAC => InternalAlgorithmData::Kmac { name, length },
            _ => unreachable!("the type is that of an algorithm, as checked above"),
        });
        key.set_usages(usages);
        Ok(key)
    }
}

impl InternalAlgorithmData {
    fn serialized_type(&self) -> u8 {
        match self {
            Self::Key { .. } => ALGORITHM_KEY,
            Self::Rsa { .. } => ALGORITHM_RSA,
            Self::RsaHashed { .. } => ALGORITHM_RSA_HASHED,
            Self::Ec { .. } => ALGORITHM_EC,
            Self::Aes { .. } => ALGORITHM_AES,
            Self::Hmac { .. } => ALGORITHM_HMAC,
            Self::Kmac { .. } => ALGORITHM_KMAC,
        }
    }
}

/// Gives a key with an RsaHashedKeyAlgorithm the RsaKeyAlgorithm without its hash, which no operation creates, so
/// that tests can serialize one.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_crypto_key_remove_algorithm_hash_for_testing(key: &InterfaceCell) {
    let key = key.object::<CryptoKey>();
    let algorithm = match &*key.internal_algorithm() {
        InternalAlgorithmData::RsaHashed {
            name,
            modulus_length,
            public_exponent,
            ..
        } => InternalAlgorithmData::Rsa {
            name: name.clone(),
            modulus_length: *modulus_length,
            public_exponent: public_exponent.clone(),
        },
        _ => panic!("the key has an RsaHashedKeyAlgorithm"),
    };
    key.set_algorithm(algorithm);
}
