/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::{Span, Utf16String};
use libcrypto_rust::{KeyDecodeError, KeyHandle};
use libjs_rust::api::{
    Array, CellRoot, ErrorKind, ErrorType, Gc, GcRefCell, GcWeak, IntegrityLevel, MarkedVec, Must, Object,
    PrimitiveString, PropertyKey, Trace, Value, Vm,
};

use crate::bindings::conversions::create_uint8_array;
use crate::bindings::interface::{
    Ref, Wrappable, allocate, cell_from_abi, cell_into_abi, create_dictionary_object, realm_arguments, wrap,
};
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
    #[gc(untraced)]
    algorithm_name: RefCell<Utf16String>,
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
            algorithm_name: RefCell::default(),
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

    pub(crate) fn from_okp_handle(vm: &Vm, is_private: bool, bytes: &[u8]) -> Ref<Self> {
        Self::create(vm, KeyHandle::from_okp_bytes(is_private, bytes))
    }

    pub(crate) fn okp_handle_bytes(&self) -> &[u8] {
        self.handle.okp_bytes()
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
        self.algorithm_name.replace(Utf16String::default());
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
        let mut algorithm_name = self.algorithm_name.borrow_mut();
        if algorithm_name.is_empty() {
            *algorithm_name = self.algorithm.borrow().name().clone();
        }
        algorithm_name.clone()
    }
}

#[derive(Default)]
#[repr(C)]
struct RawAlgorithm<'a> {
    r#type: u8,
    name: usize,
    modulus_length: u32,
    public_exponent: Span<'a, u8>,
    hash: usize,
    named_curve: usize,
    length: u32,
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

/// The tags of a serialized handle, Crypto::HandleTag, which are part of the stable storage serialization format.
const HANDLE_EC_PRIVATE_KEY: u8 = 4;

/// The number of buffers of the handle of each tag, Crypto::HandleTag. An EC private key has one, then optionally its
/// parameters and its public key, whose two buffers follow.
const HANDLE_BUFFER_COUNTS: [usize; 11] = [1, 2, 8, 2, 1, 1, 3, 1, 3, 1, 1];

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
        let fields = algorithm.fields();
        serialized.write_u8(fields.r#type);
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
        self.handle().with_components(|fields| {
            serialized.write_u8(fields.tag);
            let buffers = fields.buffers();
            if fields.tag != HANDLE_EC_PRIVATE_KEY {
                for buffer in buffers {
                    serialized.write_bytes(buffer);
                }
                return;
            }

            serialized.write_bytes(buffers[0]);
            serialized.write_bool(fields.ec_parameters.is_some());
            if let Some(parameters) = fields.ec_parameters {
                serialized.write_u64(parameters.len() as u64);
                for &parameter in parameters {
                    serialized.write_u32(parameter);
                }
            }
            serialized.write_bool(fields.has_ec_public_key);
            if fields.has_ec_public_key {
                serialized.write_bytes(buffers[1]);
                serialized.write_bytes(buffers[2]);
            }
        });
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
        let handle_tag = serialized.read_u8()?;
        let Some(&buffer_count) = HANDLE_BUFFER_COUNTS.get(usize::from(handle_tag)) else {
            return Err(data_clone_error(b"Invalid CryptoKey handle tag"));
        };
        let mut buffers: Vec<&[u8]> = Vec::new();
        let (mut has_ec_parameters, mut ec_parameters, mut has_ec_public_key) = (false, Vec::new(), false);
        if handle_tag == HANDLE_EC_PRIVATE_KEY {
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
            for _ in 0..buffer_count {
                buffers.push(serialized.read_borrowed_bytes()?);
            }
        }

        let handle = KeyHandle::from_components(
            handle_tag,
            &buffers,
            has_ec_parameters.then_some(ec_parameters.as_slice()),
            has_ec_public_key,
        );
        let handle = match handle {
            Ok(handle) => handle,
            Err(KeyDecodeError::OutOfMemory) => {
                return cx
                    .vm()
                    .throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .map_err(Into::into);
            }
            Err(KeyDecodeError::InvalidComponents) => {
                return Err(data_clone_error(b"Invalid CryptoKey handle components"));
            }
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
        key.set_algorithm(InternalAlgorithmData::from_fields(
            &RawAlgorithm {
                r#type: algorithm_type,
                modulus_length,
                length,
                ..RawAlgorithm::default()
            },
            name,
            hash,
            named_curve,
            public_exponent_bytes,
        ));
        key.set_usages(usages);
        Ok(key)
    }
}

impl InternalAlgorithmData {
    fn fields(&self) -> RawAlgorithm<'_> {
        let mut fields = RawAlgorithm {
            name: self.name().raw_identity(),
            ..RawAlgorithm::default()
        };
        fields.r#type = match self {
            Self::Key { .. } => ALGORITHM_KEY,
            Self::Rsa {
                modulus_length,
                public_exponent,
                ..
            } => {
                fields.modulus_length = *modulus_length;
                fields.public_exponent = Span::new(public_exponent);
                ALGORITHM_RSA
            }
            Self::RsaHashed {
                modulus_length,
                public_exponent,
                hash,
                ..
            } => {
                fields.modulus_length = *modulus_length;
                fields.public_exponent = Span::new(public_exponent);
                fields.hash = hash.raw_identity();
                ALGORITHM_RSA_HASHED
            }
            Self::Ec { named_curve, .. } => {
                fields.named_curve = named_curve.raw_identity();
                ALGORITHM_EC
            }
            Self::Aes { length, .. } => {
                fields.length = u32::from(*length);
                ALGORITHM_AES
            }
            Self::Hmac { hash, length, .. } => {
                fields.hash = hash.raw_identity();
                fields.length = *length;
                ALGORITHM_HMAC
            }
            Self::Kmac { length, .. } => {
                fields.length = *length;
                ALGORITHM_KMAC
            }
        };
        fields
    }

    fn from_fields(
        fields: &RawAlgorithm<'_>,
        name: Utf16String,
        hash: Utf16String,
        named_curve: Utf16String,
        public_exponent: Vec<u8>,
    ) -> Self {
        let RawAlgorithm {
            r#type,
            modulus_length,
            length,
            ..
        } = *fields;
        match r#type {
            ALGORITHM_KEY => Self::Key { name },
            ALGORITHM_RSA => Self::Rsa {
                name,
                modulus_length,
                public_exponent,
            },
            ALGORITHM_RSA_HASHED => Self::RsaHashed {
                name,
                modulus_length,
                public_exponent,
                hash,
            },
            ALGORITHM_EC => Self::Ec { name, named_curve },
            ALGORITHM_AES => Self::Aes {
                name,
                length: u16::try_from(length).expect("an AES key has a 16-bit length"),
            },
            ALGORITHM_HMAC => Self::Hmac { name, hash, length },
            ALGORITHM_KMAC => Self::Kmac { name, length },
            _ => unreachable!("the type is that of a key algorithm"),
        }
    }
}

/// # Safety
///
/// The VM is live and the caller transfers ownership of the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_create(vm: *const c_void, handle: KeyHandle) -> *mut c_void {
    // SAFETY: The VM is live and the handle is owned.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(CryptoKey::create(vm, handle))
}

/// # Safety
///
/// The VM, realm, and CryptoKey cell must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_wrap(
    vm: *mut c_void,
    realm: *mut c_void,
    key: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller supplies the live VM, realm, and CryptoKey cell.
    let ((vm, realm), key) = unsafe { (realm_arguments(vm, realm), cell_from_abi::<CryptoKey>(key)) };
    wrap(&CallContext::new(vm, realm), key).as_ptr().cast()
}

/// # Safety
///
/// The key must be a live CryptoKey cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_handle(key: *mut c_void) -> *mut c_void {
    // SAFETY: LibWeb supplies a live CryptoKey cell.
    unsafe { cell_from_abi::<CryptoKey>(key) }.handle().as_raw()
}

/// # Safety
///
/// The key must be a live CryptoKey cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_extractable(key: *mut c_void) -> bool {
    // SAFETY: LibWeb supplies a live CryptoKey cell.
    unsafe { cell_from_abi::<CryptoKey>(key) }.extractable.get()
}

/// # Safety
///
/// The key must be a live CryptoKey cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_set_extractable(key: *mut c_void, extractable: bool) {
    // SAFETY: LibWeb supplies a live CryptoKey cell.
    unsafe { cell_from_abi::<CryptoKey>(key) }.extractable.set(extractable);
}

/// # Safety
///
/// The key must be a live CryptoKey cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_type(key: *mut c_void) -> u8 {
    // SAFETY: LibWeb supplies a live CryptoKey cell.
    unsafe { cell_from_abi::<CryptoKey>(key) }.key_type() as u8
}

/// # Safety
///
/// The key must be a live CryptoKey cell and the type must be a valid KeyType.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_set_type(key: *mut c_void, r#type: u8) {
    // SAFETY: LibWeb supplies a live CryptoKey cell.
    let key = unsafe { cell_from_abi::<CryptoKey>(key) };
    key.r#type.set(Some(KEY_TYPE_VALUES[usize::from(r#type)]));
}

/// # Safety
///
/// The key must be a live CryptoKey cell and fields must point to writable storage.
#[unsafe(no_mangle)]
unsafe extern "C" fn web_rust_crypto_key_get_algorithm(key: *mut c_void, fields: *mut c_void) {
    // SAFETY: LibWeb supplies a live CryptoKey cell.
    let key = unsafe { cell_from_abi::<CryptoKey>(key) };
    let algorithm = key.algorithm.borrow();
    // SAFETY: LibWeb reads the fields before mutating the key or letting it die.
    unsafe { fields.cast::<RawAlgorithm<'_>>().write(algorithm.fields()) };
}

/// # Safety
///
/// The key must be a live CryptoKey cell and fields must own their strings.
#[unsafe(no_mangle)]
unsafe extern "C" fn web_rust_crypto_key_set_algorithm(key: *mut c_void, fields: &RawAlgorithm<'_>) {
    // SAFETY: LibWeb supplies a live CryptoKey cell and transfers the strings.
    let (key, name, hash, named_curve) = unsafe {
        (
            cell_from_abi::<CryptoKey>(key),
            Utf16String::from_raw_owned(fields.name),
            Utf16String::from_raw_owned(fields.hash),
            Utf16String::from_raw_owned(fields.named_curve),
        )
    };
    key.set_algorithm(InternalAlgorithmData::from_fields(
        fields,
        name,
        hash,
        named_curve,
        fields.public_exponent.as_slice().to_vec(),
    ));
}

/// # Safety
///
/// The key must be a live CryptoKey cell. The caller takes ownership of the returned string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_key_algorithm_name(key: *mut c_void) -> usize {
    // SAFETY: LibWeb supplies a live CryptoKey cell.
    unsafe { cell_from_abi::<CryptoKey>(key) }.algorithm_name().into_raw()
}

/// # Safety
///
/// The key must be a live CryptoKey cell and steps must not mutate it.
#[unsafe(no_mangle)]
unsafe extern "C" fn web_rust_crypto_key_with_usages(
    key: *mut c_void,
    state: *mut c_void,
    steps: unsafe extern "C" fn(*mut c_void, Span<'_, KeyUsage>),
) {
    // SAFETY: LibWeb supplies a live CryptoKey cell and the usages are lent for the callback.
    let key = unsafe { cell_from_abi::<CryptoKey>(key) };
    unsafe { steps(state, Span::new(&key.key_usages.borrow())) };
}

/// # Safety
///
/// The key must be a live CryptoKey cell and usages must contain valid KeyUsage values.
#[unsafe(no_mangle)]
unsafe extern "C" fn web_rust_crypto_key_set_usages(key: *mut c_void, usages: Span<'_, KeyUsage>) {
    // SAFETY: LibWeb supplies a live CryptoKey cell and lends the usages for the call.
    unsafe { cell_from_abi::<CryptoKey>(key) }.set_usages(usages.as_slice().to_vec());
}
