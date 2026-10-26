/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Safe passing of structured data, https://html.spec.whatwg.org/multipage/structured-data.html
//!
//! The algorithms run here. The records they write and read stay LibWeb's C++ HTML::StructuredSerializeWriter and
//! HTML::StructuredSerializeReader, which the serialization steps of the interfaces implemented in C++ write and read
//! as well (cpp.rs).

pub(crate) mod cpp;
pub(crate) mod exports;

use ak::Utf16String;
use libjs_rust::api::{
    Array, ArrayBuffer, ArrayBufferOrder, ArrayBufferShared, BigIntObject, BooleanObject, ByteLength, DataView, Date,
    ErrorKind, ErrorType as JsErrorType, Gc, Map, MarkedVec, Must, NumberObject, Object, PrimitiveString, PropertyKey,
    PropertyKind, Realm, RegExpObject, Set, StringObject, TypedArrayBase, TypedArrayKind, Value, Vm,
    create_byte_data_block, detach_array_buffer, is_typed_array_out_of_bounds, is_view_out_of_bounds,
    length_of_array_like, make_data_view_with_buffer_witness_record, make_typed_array_with_buffer_witness_record,
    regexp_create,
};

use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::structured_serialize::cpp::{
    DeserializationMemory, OwnedDeserializationMemory, SerializationMemory, SerializationType,
    SerializedTransferRecord, StructuredSerializeReader, StructuredSerializeWriter, TransferDataDecoder,
    TransferDataHolder, TransferredValues, create_serialized_platform_object, create_transferred_platform_object,
    current_settings_object_agent_cluster, current_settings_object_cross_origin_isolated_capability,
    data_clone_error_from_serialization_error, deserialization_steps, deserialize_rust_platform_object, is_exposed,
    is_platform_object, is_transferable, is_transferable_interface_exposed, realm_agent_cluster,
    serializable_interface_is_implemented_in_rust, serializable_interface_name, serializable_payload_version,
    serialization_steps, transfer_steps, transferable_is_detached, transferable_primary_interface,
    transferable_set_detached,
};

/// HTML::ValueTag, the tags of the values of a record, which are part of the stable storage serialization format.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum ValueTag {
    UndefinedPrimitive = 1,
    NullPrimitive = 2,
    BooleanPrimitive = 3,
    NumberPrimitive = 4,
    StringPrimitive = 5,
    BigIntPrimitive = 6,
    BooleanObject = 7,
    NumberObject = 8,
    StringObject = 9,
    BigIntObject = 10,
    DateObject = 11,
    RegExpObject = 12,
    MapObject = 13,
    SetObject = 14,
    ArrayObject = 15,
    ErrorObject = 16,
    Object = 17,
    ObjectReference = 18,
    GrowableSharedArrayBuffer = 19,
    SharedArrayBuffer = 20,
    ResizeableArrayBuffer = 21,
    ArrayBuffer = 22,
    ArrayBufferView = 23,
    SerializableObject = 24,
    Int32Primitive = 25,
    EndObject = 0xFF,
}

impl ValueTag {
    /// The tag of a value with the raw value, if values can have it: only tags listed here decode as values, as
    /// value_tag_is_decodable() says. Empty and EndObject are reserved.
    fn decodable_from(raw_tag: u8) -> Option<Self> {
        Some(match raw_tag {
            1 => Self::UndefinedPrimitive,
            2 => Self::NullPrimitive,
            3 => Self::BooleanPrimitive,
            4 => Self::NumberPrimitive,
            5 => Self::StringPrimitive,
            6 => Self::BigIntPrimitive,
            7 => Self::BooleanObject,
            8 => Self::NumberObject,
            9 => Self::StringObject,
            10 => Self::BigIntObject,
            11 => Self::DateObject,
            12 => Self::RegExpObject,
            13 => Self::MapObject,
            14 => Self::SetObject,
            15 => Self::ArrayObject,
            16 => Self::ErrorObject,
            17 => Self::Object,
            18 => Self::ObjectReference,
            19 => Self::GrowableSharedArrayBuffer,
            20 => Self::SharedArrayBuffer,
            21 => Self::ResizeableArrayBuffer,
            22 => Self::ArrayBuffer,
            23 => Self::ArrayBufferView,
            24 => Self::SerializableObject,
            25 => Self::Int32Primitive,
            _ => return None,
        })
    }
}

/// HTML::TransferType, the types of the data holders of a transfer.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum TransferType {
    ArrayBuffer = 2,
    ResizableArrayBuffer = 3,
}

/// Error, followed by the native errors of LibJS, in the order of JS_ENUMERATE_NATIVE_ERRORS, which IPC records hold.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ErrorType {
    Error,
    EvalError,
    InternalError,
    RangeError,
    ReferenceError,
    SyntaxError,
    TypeError,
    URIError,
}

const NATIVE_ERROR_TYPES: [(ErrorType, &str); 7] = [
    (ErrorType::EvalError, "EvalError"),
    (ErrorType::InternalError, "InternalError"),
    (ErrorType::RangeError, "RangeError"),
    (ErrorType::ReferenceError, "ReferenceError"),
    (ErrorType::SyntaxError, "SyntaxError"),
    (ErrorType::TypeError, "TypeError"),
    (ErrorType::URIError, "URIError"),
];

fn error_type_to_name(error_type: ErrorType) -> &'static str {
    if error_type == ErrorType::Error {
        return "Error";
    }
    NATIVE_ERROR_TYPES
        .iter()
        .find(|(native_error_type, _)| *native_error_type == error_type)
        .map(|(_, name)| *name)
        .expect("every error type has a name")
}

fn error_type_from_ipc(raw_error_type: u8) -> Option<ErrorType> {
    Some(match raw_error_type {
        0 => ErrorType::Error,
        1 => ErrorType::EvalError,
        2 => ErrorType::InternalError,
        3 => ErrorType::RangeError,
        4 => ErrorType::ReferenceError,
        5 => ErrorType::SyntaxError,
        6 => ErrorType::TypeError,
        7 => ErrorType::URIError,
        _ => return None,
    })
}

fn error_name_to_type(name: &Utf16String) -> ErrorType {
    NATIVE_ERROR_TYPES
        .iter()
        .find(|(_, native_error_name)| *name == Utf16String::from_utf8(native_error_name))
        .map_or(ErrorType::Error, |(native_error_type, _)| *native_error_type)
}

fn error_kind_of(error_type: ErrorType) -> ErrorKind {
    match error_type {
        ErrorType::Error => ErrorKind::Error,
        ErrorType::EvalError => ErrorKind::EvalError,
        ErrorType::InternalError => ErrorKind::InternalError,
        ErrorType::RangeError => ErrorKind::RangeError,
        ErrorType::ReferenceError => ErrorKind::ReferenceError,
        ErrorType::SyntaxError => ErrorKind::SyntaxError,
        ErrorType::TypeError => ErrorKind::TypeError,
        ErrorType::URIError => ErrorKind::URIError,
    }
}

/// An IPC record holds an error type as its index, and a storage record as its name.
fn encode_error_type(serialized: StructuredSerializeWriter, error_type: ErrorType) {
    if serialized.serialization_type() == SerializationType::Ipc {
        serialized.encode_u8(error_type as u8);
    } else {
        serialized.encode_utf16_string(&Utf16String::from_utf8(error_type_to_name(error_type)));
    }
}

fn decode_error_type(serialized: StructuredSerializeReader) -> WebIdlResult<ErrorType> {
    if serialized.serialization_type() == SerializationType::Ipc {
        let decoded = serialized.decode_u8()?;
        return match error_type_from_ipc(decoded) {
            Some(error_type) => Ok(error_type),
            None => Err(data_clone_error_from_serialization_error(
                serialized.realm(),
                "Invalid structured serialize error type",
            )),
        };
    }

    let name = serialized.decode_utf16_string()?;
    Ok(error_name_to_type(&name))
}

/// The typed array constructors, in the order of JS_ENUMERATE_TYPED_ARRAYS.
const TYPED_ARRAY_KINDS: [(TypedArrayKind, &str); 12] = [
    (TypedArrayKind::Uint8Array, "Uint8Array"),
    (TypedArrayKind::Uint8ClampedArray, "Uint8ClampedArray"),
    (TypedArrayKind::Uint16Array, "Uint16Array"),
    (TypedArrayKind::Uint32Array, "Uint32Array"),
    (TypedArrayKind::BigUint64Array, "BigUint64Array"),
    (TypedArrayKind::Int8Array, "Int8Array"),
    (TypedArrayKind::Int16Array, "Int16Array"),
    (TypedArrayKind::Int32Array, "Int32Array"),
    (TypedArrayKind::BigInt64Array, "BigInt64Array"),
    (TypedArrayKind::Float16Array, "Float16Array"),
    (TypedArrayKind::Float32Array, "Float32Array"),
    (TypedArrayKind::Float64Array, "Float64Array"),
];

fn typed_array_name(kind: TypedArrayKind) -> &'static str {
    TYPED_ARRAY_KINDS
        .iter()
        .find(|(typed_array_kind, _)| *typed_array_kind == kind)
        .map(|(_, name)| *name)
        .expect("every typed array kind has a name")
}

fn call_stack_size_exceeded(vm: &Vm) -> Exception {
    match vm.throw_completion::<()>(ErrorKind::InternalError, JsErrorType::CallStackSizeExceeded, &[]) {
        Err(throw) => throw.into(),
        Ok(()) => unreachable!("throwing a completion throws"),
    }
}

fn data_clone_error(message: &str) -> Exception {
    Exception::Dom("DataCloneError", Utf16String::from_utf8(message))
}

/// AllowSharedArrayBuffers of StructuredSerialize.h.
// AD-HOC: Serializing a SharedArrayBuffer is normally gated on the current settings object's
//         cross-origin isolated capability (a Spectre mitigation for data that may reach another
//         agent). SameAgentAlways bypasses that gate for internal clones that never leave the
//         surrounding agent, where the gate's threat model does not apply. Web-visible entry points
//         must use CrossOriginIsolatedOnly.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AllowSharedArrayBuffers {
    CrossOriginIsolatedOnly,
    SameAgentAlways,
}

impl AllowSharedArrayBuffers {
    pub(crate) fn from_abi(value: u8) -> Self {
        match value {
            0 => Self::CrossOriginIsolatedOnly,
            1 => Self::SameAgentAlways,
            _ => unreachable!("LibWeb has no other AllowSharedArrayBuffers"),
        }
    }
}

// The [[AgentCluster]] of a shared buffer's record: the surrounding agent's agent cluster, which deserialization holds
// the target realm's against — so the buffer never leaves the cluster it was created in. A SameAgentAlways clone never
// leaves its agent, and names no cluster.
fn serialize_agent_cluster(
    data_holder: StructuredSerializeWriter,
    allow_shared_array_buffers: AllowSharedArrayBuffers,
) -> WebIdlResult<()> {
    if allow_shared_array_buffers == AllowSharedArrayBuffers::SameAgentAlways {
        data_holder.encode_bool(false);
        return Ok(());
    }

    let Some(agent_cluster) = current_settings_object_agent_cluster() else {
        return Err(data_clone_error(
            "Cannot serialize SharedArrayBuffer outside an agent cluster",
        ));
    };
    data_holder.encode_bool(true);
    data_holder.encode_u64(agent_cluster);
    Ok(())
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
fn serialize_array_buffer(
    vm: &Vm,
    data_holder: StructuredSerializeWriter,
    array_buffer: Gc<ArrayBuffer>,
    for_storage: bool,
    allow_shared_array_buffers: AllowSharedArrayBuffers,
) -> WebIdlResult<()> {
    // 14. Otherwise, if value has an [[ArrayBufferData]] internal slot:

    // 1. If IsSharedArrayBuffer(value) is true:
    if array_buffer.is_shared_array_buffer() {
        // 1. If the current settings object's cross-origin isolated capability is false, then throw a "DataCloneError" DOMException.
        // NOTE: This check is only needed when serializing (and not when deserializing) as the cross-origin isolated capability cannot change
        //       over time and a SharedArrayBuffer cannot leave an agent cluster.
        // AD-HOC: SameAgentAlways clones never leave the surrounding agent, so the gate's threat model does not apply to
        //         them; see AllowSharedArrayBuffers.
        if allow_shared_array_buffers == AllowSharedArrayBuffers::CrossOriginIsolatedOnly
            && !current_settings_object_cross_origin_isolated_capability()
        {
            return Err(data_clone_error(
                "Cannot serialize SharedArrayBuffer when not cross-origin isolated",
            ));
        }
        if !array_buffer.is_fixed_length() {
            // 2. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "GrowableSharedArrayBuffer",
            //           [[ArrayBufferData]]: value.[[ArrayBufferData]], [[ArrayBufferByteLengthData]]: value.[[ArrayBufferByteLengthData]],
            //           [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]] }.
            // To share with a same-process target, stash the source buffer in the side table. The byte copy keeps the
            // wire format self-contained for records that cross a process boundary.
            // AD-HOC: A growable SharedArrayBuffer isn't backed by cross-process shared memory (its storage is
            //         process-local) — so only a same-process target can share it. A record that crosses a process
            //         boundary hands the peer a copy.
            // FIXME: Share it across processes too: a max-sized shared mapping with a shared length word (#11682).
            data_holder.encode_u8(ValueTag::GrowableSharedArrayBuffer as u8);
            serialize_agent_cluster(data_holder, allow_shared_array_buffers)?;
            data_holder.encode_u32(data_holder.add_shared_array_buffer(array_buffer));
            data_holder.encode_bytes(&array_buffer.copy_all_to_byte_buffer());
            data_holder.encode_u64(array_buffer.max_byte_length() as u64);
        } else {
            // 3. Otherwise, set serialized to { [[Type]]: "SharedArrayBuffer", [[ArrayBufferData]]: value.[[ArrayBufferData]],
            //           [[ArrayBufferByteLength]]: value.[[ArrayBufferByteLength]] }.
            // To share (rather than copy) [[ArrayBufferData]] with a same-process target, stash the source buffer in the
            // record's side table and encode its index. A record that crosses a process boundary arrives with an empty
            // side table — and shares through the file-descriptor side list below instead, or falls back to a byte copy.
            data_holder.encode_u8(ValueTag::SharedArrayBuffer as u8);
            serialize_agent_cluster(data_holder, allow_shared_array_buffers)?;
            data_holder.encode_u32(data_holder.add_shared_array_buffer(array_buffer));
            // AD-HOC: When backed by cross-process shared memory and a side list is available (the messaging path),
            //         transfer the buffer by appending it to shared_buffers, and encoding its index. That lets the file
            //         descriptor survive structured clone, whose main record is bytes-only, so an agent in another
            //         process references the same [[ArrayBufferData]]. Without a side list (e.g., serialization for
            //         storage) or a shared backing (e.g., a zero-length buffer), fall back to copying the bytes.
            let can_share = data_holder.can_share_array_buffer_memory(array_buffer);
            data_holder.encode_bool(can_share);
            if can_share {
                data_holder.encode_u32(data_holder.shared_buffer_count());
                // Carried beside the index — so the receiving agent can tell this object apart from any other, even
                // when it maps this same one twice, from two separate messages.
                data_holder.encode_u64(array_buffer.with_data_block(|data_block| data_block.shared_object_id()));
                data_holder.append_shared_buffer(array_buffer);
            } else {
                data_holder.encode_bytes(&array_buffer.copy_all_to_byte_buffer());
            }
        }

        // 4. Set sameAgentCluster to true.
        // 29. If forStorage is true and serialized.[[AgentCluster]] is not null, then throw a "DataCloneError" DOMException.
        // NB: A SharedArrayBuffer is the only value whose record is restricted to the same agent cluster, so step 29
        //     throws here, once its record is set.
        if for_storage {
            return Err(data_clone_error("Cannot serialize SharedArrayBuffer for storage"));
        }
    }
    // 2. Otherwise:
    else {
        // 1. If IsDetachedBuffer(value) is true, then throw a "DataCloneError" DOMException.
        if array_buffer.is_detached() {
            return Err(data_clone_error("Cannot serialize detached ArrayBuffer"));
        }

        // 2. Let size be value.[[ArrayBufferByteLength]].
        let size = array_buffer.byte_length();

        // 3. Let dataCopy be ? CreateByteDataBlock(size).
        //    NOTE: This can throw a RangeError exception upon allocation failure.
        let data_copy = create_byte_data_block(vm, size, None)?;

        // 4. Perform CopyDataBlockBytes(dataCopy, 0, value.[[ArrayBufferData]], 0, size).
        array_buffer.copy_data_to_block(&data_copy, 0, 0, size);

        // 5. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "ResizableArrayBuffer",
        //    [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size, [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]] }.
        if !array_buffer.is_fixed_length() {
            data_holder.encode_u8(ValueTag::ResizeableArrayBuffer as u8);
            data_holder.encode_bytes(&data_copy.copy_to_byte_buffer(0, size));
            data_holder.encode_u64(array_buffer.max_byte_length() as u64);
        }
        // 6. Otherwise, set serialized to { [[Type]]: "ArrayBuffer", [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size }.
        else {
            data_holder.encode_u8(ValueTag::ArrayBuffer as u8);
            data_holder.encode_bytes(&data_copy.copy_to_byte_buffer(0, size));
        }
    }
    Ok(())
}

/// A view of an ArrayBuffer: a typed array or a DataView.
#[derive(Clone, Copy)]
enum ViewType {
    TypedArray(Gc<TypedArrayBase>),
    DataView(Gc<DataView>),
}

fn serialize_byte_length(data_holder: StructuredSerializeWriter, byte_length: ByteLength) {
    assert!(!byte_length.is_detached());

    data_holder.encode_bool(byte_length.is_auto());
    if !byte_length.is_auto() {
        data_holder.encode_u32(byte_length.length());
    }
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
fn serialize_viewed_array_buffer(
    vm: &Vm,
    realm: Option<Gc<Realm>>,
    data_holder: StructuredSerializeWriter,
    view: ViewType,
    for_storage: bool,
    memory: SerializationMemory,
    allow_shared_array_buffers: AllowSharedArrayBuffers,
) -> WebIdlResult<()> {
    // 15. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot:

    // 1. If IsArrayBufferViewOutOfBounds(value) is true, then throw a "DataCloneError" DOMException.
    match view {
        ViewType::DataView(data_view) => {
            let view_record = make_data_view_with_buffer_witness_record(data_view, ArrayBufferOrder::SeqCst);
            if is_view_out_of_bounds(&view_record) {
                return Err(Exception::Dom(
                    "DataCloneError",
                    JsErrorType::BufferOutOfBounds.message(&[&"DataView"]),
                ));
            }
        }
        ViewType::TypedArray(typed_array) => {
            let typed_array_record =
                make_typed_array_with_buffer_witness_record(&typed_array, ArrayBufferOrder::SeqCst);
            if is_typed_array_out_of_bounds(&typed_array_record) {
                return Err(Exception::Dom(
                    "DataCloneError",
                    JsErrorType::BufferOutOfBounds.message(&[&"TypedArray"]),
                ));
            }
        }
    }

    // 2. Let buffer be the value of value's [[ViewedArrayBuffer]] internal slot.
    let buffer = Value::from_object(match view {
        ViewType::DataView(data_view) => data_view.viewed_array_buffer(),
        ViewType::TypedArray(typed_array) => typed_array.viewed_array_buffer(),
    });

    // 4. Assert: bufferSerialized.[[Type]] is "ArrayBuffer", "ResizableArrayBuffer", "SharedArrayBuffer", or "GrowableSharedArrayBuffer".
    // NB: buffer is an ArrayBuffer, which serializes to one of those, or to a reference to an earlier one.

    match view {
        // 5. If value has a [[DataView]] internal slot, then set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: "DataView",
        //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]], [[ByteOffset]]: value.[[ByteOffset]] }.
        ViewType::DataView(data_view) => {
            data_holder.encode_u8(ValueTag::ArrayBufferView as u8);
            // 3. Let bufferSerialized be ? StructuredSerializeInternal(buffer, forStorage, memory).
            structured_serialize_internal(
                vm,
                realm,
                data_holder,
                buffer,
                for_storage,
                memory,
                allow_shared_array_buffers,
            )?; // [[ArrayBufferSerialized]]
            data_holder.encode_utf16_string(&Utf16String::from_utf8("DataView")); // [[Constructor]]
            serialize_byte_length(data_holder, data_view.byte_length());
            data_holder.encode_u32(data_view.byte_offset());
        }
        // 6. Otherwise:
        ViewType::TypedArray(typed_array) => {
            // 1. Assert: value has a [[TypedArrayName]] internal slot.
            //    NOTE: Handled by the ViewType of the view.
            // 2. Set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: value.[[TypedArrayName]],
            //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]],
            //    [[ByteOffset]]: value.[[ByteOffset]], [[ArrayLength]]: value.[[ArrayLength]] }.
            data_holder.encode_u8(ValueTag::ArrayBufferView as u8);
            structured_serialize_internal(
                vm,
                realm,
                data_holder,
                buffer,
                for_storage,
                memory,
                allow_shared_array_buffers,
            )?; // [[ArrayBufferSerialized]]
            data_holder.encode_utf16_string(&Utf16String::from_utf8(typed_array_name(typed_array.kind()))); // [[Constructor]]
            serialize_byte_length(data_holder, typed_array.byte_length());
            data_holder.encode_u32(typed_array.byte_offset());
            serialize_byte_length(data_holder, typed_array.array_length());
        }
    }

    Ok(())
}

// Serializing and deserializing are each two passes:
// 1. Fill up the memory with all the values, but without translating references
// 2. Translate all the references into the appropriate form
struct Serializer<'vm> {
    vm: &'vm Vm,
    // NB: The current realm, which a serialization outside of any script has none of.
    realm: Option<Gc<Realm>>,
    serialized: StructuredSerializeWriter,
    memory: SerializationMemory, // JS value -> index
    for_storage: bool,
    allow_shared_array_buffers: AllowSharedArrayBuffers,
}

impl Serializer<'_> {
    // https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
    fn serialize(&self, value: Value) -> WebIdlResult<()> {
        let vm = self.vm;
        if vm.did_reach_stack_space_limit() {
            return Err(call_stack_size_exceeded(vm));
        }

        let serialized = self.serialized;

        // 1. If memory was not supplied, let memory be an empty map.
        // NB: Every caller supplies memory.

        // 2. If memory[value] exists, then return memory[value].
        if let Some(index) = self.memory.get(value) {
            self.encode_tag(ValueTag::ObjectReference);
            serialized.encode_u32(index);
            return Ok(());
        }

        // 3. Let deep be false.
        let mut deep = false;

        // 4. Let sameAgentCluster be false.
        // NB: Only a SharedArrayBuffer restricts its record to the same agent cluster, which its branch handles.

        // 5. If value is undefined, null, a Boolean, a Number, a BigInt, or a String, then return { [[Type]]: "primitive", [[Value]]: value }.
        let mut return_primitive_type = true;

        if value.is_undefined() {
            self.encode_tag(ValueTag::UndefinedPrimitive);
        } else if value.is_null() {
            self.encode_tag(ValueTag::NullPrimitive);
        } else if value.is_boolean() {
            self.encode_tag(ValueTag::BooleanPrimitive);
            serialized.encode_bool(value.as_bool());
        } else if value.is_int32() {
            self.encode_tag(ValueTag::Int32Primitive);
            serialized.encode_i32(value.as_i32());
        } else if value.is_number() {
            self.encode_tag(ValueTag::NumberPrimitive);
            serialized.encode_f64(value.as_f64());
        } else if value.is_bigint() {
            self.encode_tag(ValueTag::BigIntPrimitive);
            serialized.encode_big_int(value);
        } else if value.is_string() {
            self.encode_tag(ValueTag::StringPrimitive);
            serialized.encode_utf16_string(&value.as_string().utf16_string());
        } else {
            return_primitive_type = false;
        }

        if return_primitive_type {
            return Ok(());
        }

        // 6. If value is a Symbol, then throw a "DataCloneError" DOMException.
        if value.is_symbol() {
            return Err(data_clone_error("Cannot serialize Symbol"));
        }

        // 7. Let serialized be an uninitialized value.
        // NOTE: We created the serialized value above.
        if value.is_object() {
            let object = value.as_object();

            // 8. If value has a [[BooleanData]] internal slot, then set serialized to { [[Type]]: "Boolean", [[BooleanData]]: value.[[BooleanData]] }.
            if let Some(boolean_object) = object.downcast::<BooleanObject>() {
                self.encode_tag(ValueTag::BooleanObject);
                serialized.encode_bool(boolean_object.boolean());
            }
            // 9. Otherwise, if value has a [[NumberData]] internal slot, then set serialized to { [[Type]]: "Number", [[NumberData]]: value.[[NumberData]] }.
            else if let Some(number_object) = object.downcast::<NumberObject>() {
                self.encode_tag(ValueTag::NumberObject);
                serialized.encode_f64(number_object.number());
            }
            // 10. Otherwise, if value has a [[BigIntData]] internal slot, then set serialized to { [[Type]]: "BigInt", [[BigIntData]]: value.[[BigIntData]] }.
            else if let Some(big_int_object) = object.downcast::<BigIntObject>() {
                self.encode_tag(ValueTag::BigIntObject);
                serialized.encode_big_int(Value::from_bigint(big_int_object.bigint()));
            }
            // 11. Otherwise, if value has a [[StringData]] internal slot, then set serialized to { [[Type]]: "String", [[StringData]]: value.[[StringData]] }.
            else if let Some(string_object) = object.downcast::<StringObject>() {
                self.encode_tag(ValueTag::StringObject);
                serialized.encode_utf16_string(&string_object.primitive_string().utf16_string());
            }
            // 12. Otherwise, if value has a [[DateValue]] internal slot, then set serialized to { [[Type]]: "Date", [[DateValue]]: value.[[DateValue]] }.
            else if let Some(date) = object.downcast::<Date>() {
                self.encode_tag(ValueTag::DateObject);
                serialized.encode_f64(date.date_value());
            }
            // 13. Otherwise, if value has a [[RegExpMatcher]] internal slot, then set serialized to
            //     { [[Type]]: "RegExp", [[RegExpMatcher]]: value.[[RegExpMatcher]], [[OriginalSource]]: value.[[OriginalSource]],
            //       [[OriginalFlags]]: value.[[OriginalFlags]] }.
            else if let Some(reg_exp_object) = object.downcast::<RegExpObject>() {
                // NOTE: ECMAScriptRegex is perfectly happy to be reconstructed with just the source+flags.
                //       In the future, we could optimize the work being done on the deserialize step by serializing
                //       more of the internal state (the [[RegExpMatcher]] internal slot).
                self.encode_tag(ValueTag::RegExpObject);
                serialized.encode_utf16_string(&reg_exp_object.pattern());
                serialized.encode_utf16_string(&reg_exp_object.flags());
            }
            // 14. Otherwise, if value has an [[ArrayBufferData]] internal slot:
            else if let Some(array_buffer) = object.downcast::<ArrayBuffer>() {
                serialize_array_buffer(
                    vm,
                    serialized,
                    array_buffer,
                    self.for_storage,
                    self.allow_shared_array_buffers,
                )?;
            }
            // 15. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot:
            else if let Some(typed_array_base) = object.downcast::<TypedArrayBase>() {
                serialize_viewed_array_buffer(
                    vm,
                    self.realm,
                    serialized,
                    ViewType::TypedArray(typed_array_base),
                    self.for_storage,
                    self.memory,
                    self.allow_shared_array_buffers,
                )?;
            } else if let Some(data_view) = object.downcast::<DataView>() {
                serialize_viewed_array_buffer(
                    vm,
                    self.realm,
                    serialized,
                    ViewType::DataView(data_view),
                    self.for_storage,
                    self.memory,
                    self.allow_shared_array_buffers,
                )?;
            }
            // 16. Otherwise, if value has a [[MapData]] internal slot:
            else if object.downcast::<Map>().is_some() {
                // 1. Set serialized to { [[Type]]: "Map", [[MapData]]: a new empty List }.
                self.encode_tag(ValueTag::MapObject);

                // 2. Set deep to true.
                deep = true;
            }
            // 17. Otherwise, if value has a [[SetData]] internal slot:
            else if object.downcast::<Set>().is_some() {
                // 1. Set serialized to { [[Type]]: "Set", [[SetData]]: a new empty List }.
                self.encode_tag(ValueTag::SetObject);

                // 2. Set deep to true.
                deep = true;
            }
            // 18. Otherwise, if value has an [[ErrorData]] internal slot and value is not a platform object:
            else if object.downcast::<libjs_rust::api::Error>().is_some() && !is_platform_object(object) {
                // 1. Let name be ? Get(value, "name").
                let name = object.get(vm, &vm.names.name)?;

                // 2. If name is not one of "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", or "URIError", then set name to "Error".
                let mut error_type = ErrorType::Error;
                if name.is_string() {
                    error_type = error_name_to_type(&name.as_string().utf16_string());
                }
                // NB: InternalError is a native error of LibJS, but it is not one of those names.
                if error_type == ErrorType::InternalError {
                    error_type = ErrorType::Error;
                }

                // 3. Let valueMessageDesc be ? value.[[GetOwnProperty]]("message").
                let value_message_descriptor = object.internal_get_own_property(vm, &vm.names.message)?;

                // 4. Let message be undefined if IsDataDescriptor(valueMessageDesc) is false, and ? ToString(valueMessageDesc.[[Value]]) otherwise.
                let mut message = None;
                if let Some(descriptor) = &value_message_descriptor
                    && descriptor.is_data_descriptor()
                {
                    message = Some(descriptor.value.unwrap_or(Value::UNDEFINED).to_utf16_string(vm)?);
                }

                // FIXME: Spec bug - https://github.com/whatwg/html/issues/11321
                // MISSING STEP: Let valueCauseDesc be ? value.[[GetOwnProperty]]("cause").
                let value_cause_descriptor = object.internal_get_own_property(vm, &vm.names.cause)?;

                // MISSING STEP: Let cause be undefined if IsDataDescriptor(valueCauseDesc) is false, and ? ToString(valueCauseDesc.[[Value]]) otherwise.
                let mut cause = None;
                if let Some(descriptor) = &value_cause_descriptor
                    && descriptor.is_data_descriptor()
                {
                    cause = Some(descriptor.value.unwrap_or(Value::UNDEFINED).to_utf16_string(vm)?);
                }

                // FIXME: 5. Let stack be an implementation-defined string that represents value.[[Stack]].
                // 6. Set serialized to { [[Type]]: "Error", [[Name]]: name, [[Message]]: message, [[Stack]]: stack }.
                // FIXME: 7. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified to serialized.
                self.encode_tag(ValueTag::ErrorObject);
                encode_error_type(serialized, error_type);
                serialized.encode_optional_utf16_string(message.as_ref());
                serialized.encode_optional_utf16_string(cause.as_ref());
            }
            // 19. Otherwise, if value is an Array exotic object:
            else if object.downcast::<Array>().is_some() {
                // 1. Let valueLenDescriptor be ? OrdinaryGetOwnProperty(value, "length").
                // 2. Let valueLen be valueLenDescriptor.[[Value]].
                // NON-STANDARD: Array objects in LibJS do not have a real length property, so it must be accessed the usual way
                let length = length_of_array_like(vm, &object).must();

                // 3. Set serialized to { [[Type]]: "Array", [[Length]]: valueLen, [[Properties]]: a new empty List }.
                self.encode_tag(ValueTag::ArrayObject);
                serialized.encode_u64(length);

                // 4. Set deep to true.
                deep = true;
            }
            // 20. Otherwise, if value is a platform object that is a serializable object:
            else if let Some(interface_name) = serializable_interface_name(object) {
                // 1. If value has a [[Detached]] internal slot whose value is true, then throw a "DataCloneError" DOMException.
                // NB: The platform objects that have a [[Detached]] internal slot are the transferable objects.
                if is_transferable(object) && transferable_is_detached(object) {
                    return Err(data_clone_error("Cannot serialize a detached platform object"));
                }

                // 2. Let typeString be the identifier of the primary interface of value.
                // 3. Set serialized to { [[Type]]: typeString }.
                self.encode_tag(ValueTag::SerializableObject);
                serialized.encode_interface_name(interface_name);
                if self.for_storage {
                    serialized.encode_u64(serializable_payload_version());
                }

                // FIXME: 4. If value's primary interface is decorated with [SameAgentCluster], then set sameAgentCluster to true.

                // 5. Set deep to true.
                deep = true;
            }
            // 21. Otherwise, if value is a platform object, then throw a "DataCloneError" DOMException.
            else if is_platform_object(object) {
                return Err(data_clone_error("Cannot serialize platform objects"));
            }
            // 22. Otherwise, if IsCallable(value) is true, then throw a "DataCloneError" DOMException.
            else if value.is_function() {
                return Err(data_clone_error("Cannot serialize functions"));
            }
            // 23. Otherwise, if value has any internal slot other than [[Prototype]], [[Extensible]], or [[PrivateElements]], then throw a "DataCloneError" DOMException.
            else if object.has_internal_slots() {
                return Err(data_clone_error("Cannot serialize an object with internal slots"));
            }
            // 24. Otherwise, if value is an exotic object and value is not the %Object.prototype% intrinsic object associated with any realm, then throw a "DataCloneError" DOMException.
            // NB: Step 19 serializes an Array, and step 23 throws for every other exotic object but an immutable prototype
            //     exotic object, of which LibJS has only %Object.prototype%, and LibWeb only platform objects.
            // 25. Otherwise:
            else {
                // 1. Set serialized to { [[Type]]: "Object", [[Properties]]: a new empty List }.
                self.encode_tag(ValueTag::Object);

                // 2. Set deep to true.
                deep = true;
            }
        }

        // 26. If sameAgentCluster is true, then restrict serialized to the same agent cluster; otherwise, set serialized.[[AgentCluster]] to null.
        // NB: The SharedArrayBuffer branch encodes the agent cluster with its record.

        // 27. Set memory[value] to serialized.
        // Nested sub-serialization above may already have claimed earlier ids.
        self.memory.set(value, self.memory.size());

        // 28. If deep is true:
        if deep {
            let object = value.as_object();

            // 1. If value has a [[MapData]] internal slot:
            if let Some(map) = object.downcast::<Map>() {
                // 1. Let copiedList be a new empty List.
                let copied_list = MarkedVec::with_capacity(vm, map.map_size() * 2);

                // 2. For each Record { [[Key]], [[Value]] } entry of value.[[MapData]]:
                map.for_each_entry(|key, value| {
                    // 1. Let copiedEntry be a new Record { [[Key]]: entry.[[Key]], [[Value]]: entry.[[Value]] }.
                    // 2. If copiedEntry.[[Key]] is not the special value empty, append copiedEntry to copiedList.
                    copied_list.push(key);
                    copied_list.push(value);
                });

                serialized.encode_u64(map.map_size() as u64);

                // 3. For each Record { [[Key]], [[Value]] } entry of copiedList:
                for copied_value in copied_list.values().iter().copied() {
                    // 1. Let serializedKey be ? StructuredSerializeInternal(entry.[[Key]], forStorage, memory).
                    // 2. Let serializedValue be ? StructuredSerializeInternal(entry.[[Value]], forStorage, memory).
                    structured_serialize_internal(
                        vm,
                        self.realm,
                        serialized,
                        copied_value,
                        self.for_storage,
                        self.memory,
                        self.allow_shared_array_buffers,
                    )?;

                    // 3. Append { [[Key]]: serializedKey, [[Value]]: serializedValue } to serialized.[[MapData]].
                }
            }
            // 2. Otherwise, if value has a [[SetData]] internal slot:
            else if let Some(set) = object.downcast::<Set>() {
                // 1. Let copiedList be a new empty List.
                let copied_list = MarkedVec::with_capacity(vm, set.set_size());

                // 2. For each entry of value.[[SetData]]:
                set.for_each_value(|entry| {
                    // 1. If entry is not the special value empty, append entry to copiedList.
                    copied_list.push(entry);
                });

                serialized.encode_u64(set.set_size() as u64);

                // 3. For each entry of copiedList:
                for copied_value in copied_list.values().iter().copied() {
                    // 1. Let serializedEntry be ? StructuredSerializeInternal(entry, forStorage, memory).
                    structured_serialize_internal(
                        vm,
                        self.realm,
                        serialized,
                        copied_value,
                        self.for_storage,
                        self.memory,
                        self.allow_shared_array_buffers,
                    )?;

                    // 2. Append serializedEntry to serialized.[[SetData]].
                }
            }
            // 3. Otherwise, if value is a platform object that is a serializable object, then perform the serialization steps for value's primary interface, given value, serialized, and forStorage.
            else if serializable_interface_name(object).is_some() {
                let realm = self.realm.unwrap_or_else(|| object.shape().realm());
                serialization_steps(
                    &CallContext::new(vm, realm),
                    object,
                    serialized,
                    self.for_storage,
                    self.memory,
                )?;
            }
            // 4. Otherwise, for each key in ! EnumerableOwnProperties(value, key):
            else {
                let keys = object.enumerable_own_property_names(vm, PropertyKind::Key).must();
                for key in keys.values().iter().copied() {
                    let property_key = PropertyKey::from_value(vm, key).must();

                    // 1. If ! HasOwnProperty(value, key) is true:
                    if object.has_own_property(vm, &property_key).must() {
                        // 1. Let inputValue be ? value.[[Get]](key, value).
                        let input_value = object.get(vm, &property_key)?;

                        // 2. Let outputValue be ? StructuredSerializeInternal(inputValue, forStorage, memory).
                        structured_serialize_internal(
                            vm,
                            self.realm,
                            serialized,
                            input_value,
                            self.for_storage,
                            self.memory,
                            self.allow_shared_array_buffers,
                        )?;

                        // 3. Append { [[Key]]: key, [[Value]]: outputValue } to serialized.[[Properties]].
                        serialized.encode_utf16_string(&key.as_string().utf16_string());
                    }
                }

                self.encode_tag(ValueTag::EndObject);
            }
        }

        // 29. If forStorage is true and serialized.[[AgentCluster]] is not null, then throw a "DataCloneError" DOMException.
        // NB: The SharedArrayBuffer branch throws, once its record is set.

        // 30. Return serialized.
        Ok(())
    }

    fn encode_tag(&self, tag: ValueTag) {
        self.serialized.encode_u8(tag as u8);
    }
}

struct Deserializer<'a, 'vm> {
    cx: &'a CallContext<'vm>,
    serialized: StructuredSerializeReader,
    memory: DeserializationMemory,
}

impl Deserializer<'_, '_> {
    // https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
    fn deserialize(&self) -> WebIdlResult<Value> {
        self.deserialize_value(self.serialized.decode_u8()?)
    }

    // Step 1 of deserializing a "SharedArrayBuffer" or "GrowableSharedArrayBuffer" record: it names the agent cluster it
    // was serialized in, and a target realm in any other cluster can't have the buffer. A record that names none (a
    // SameAgentAlways clone's) is deserialized in the agent that serialized it.
    fn deserialize_agent_cluster(&self) -> WebIdlResult<()> {
        if !self.serialized.decode_bool()? {
            return Ok(());
        }
        let agent_cluster = self.serialized.decode_u64()?;
        let target_agent_cluster = realm_agent_cluster(self.cx.realm());
        if target_agent_cluster != Some(agent_cluster) {
            return Err(data_clone_error(
                "Cannot deserialize SharedArrayBuffer outside its agent cluster",
            ));
        }
        Ok(())
    }

    fn deserialize_byte_length(&self) -> WebIdlResult<ByteLength> {
        let is_auto = self.serialized.decode_bool()?;
        if is_auto {
            return Ok(ByteLength::auto_());
        }

        let length = self.serialized.decode_u32()?;
        Ok(ByteLength::from(length))
    }

    // The Object/Array property loop is the only caller allowed to pre-read EndObject.
    fn deserialize_value(&self, raw_tag: u8) -> WebIdlResult<Value> {
        let vm = self.cx.vm();
        if vm.did_reach_stack_space_limit() {
            return Err(call_stack_size_exceeded(vm));
        }

        let realm = self.cx.realm();

        // 1. If memory was not supplied, let memory be an empty map.
        // NB: Every caller supplies memory.

        let Some(tag) = ValueTag::decodable_from(raw_tag) else {
            return Err(data_clone_error_from_serialization_error(
                realm,
                "Unexpected structured serialize value tag",
            ));
        };

        // 2. If memory[serialized] exists, then return memory[serialized].
        if tag == ValueTag::ObjectReference {
            let index = self.serialized.decode_u32()?;
            if index == u32::MAX {
                return Ok(Value::from_object(Object::create(vm, realm, None)));
            }
            if index as usize >= self.memory.size() {
                return Err(data_clone_error_from_serialization_error(
                    realm,
                    "ObjectReference index out of range",
                ));
            }
            return Ok(self.memory.get(index as usize));
        }

        // 5. Let deep be false.
        let mut deep = false;

        // 6. Let value be an uninitialized value.
        let value: Value;

        let mut is_primitive = false;

        let decode_string = || -> WebIdlResult<Gc<PrimitiveString>> {
            let string = self.serialized.decode_utf16_string()?;
            Ok(PrimitiveString::create(vm, string))
        };

        match tag {
            // 3. If serialized.[[Type]] is "primitive", then return serialized.[[Value]].
            ValueTag::UndefinedPrimitive => {
                value = Value::UNDEFINED;
                is_primitive = true;
            }
            ValueTag::NullPrimitive => {
                value = Value::NULL;
                is_primitive = true;
            }
            ValueTag::BooleanPrimitive => {
                value = Value::from_bool(self.serialized.decode_bool()?);
                is_primitive = true;
            }
            ValueTag::Int32Primitive => {
                value = Value::from_i32(self.serialized.decode_i32()?);
                is_primitive = true;
            }
            ValueTag::NumberPrimitive => {
                value = Value::from_f64(self.serialized.decode_f64()?);
                is_primitive = true;
            }
            ValueTag::BigIntPrimitive => {
                value = self.serialized.decode_big_int()?;
                is_primitive = true;
            }
            ValueTag::StringPrimitive => {
                value = Value::from_string(decode_string()?);
                is_primitive = true;
            }

            // 7. If serialized.[[Type]] is "Boolean", then set value to a new Boolean object in targetRealm whose [[BooleanData]] internal slot value is serialized.[[BooleanData]].
            ValueTag::BooleanObject => {
                value = Value::from_object(BooleanObject::create(vm, realm, self.serialized.decode_bool()?));
            }

            // 8. Otherwise, if serialized.[[Type]] is "Number", then set value to a new Number object in targetRealm whose [[NumberData]] internal slot value is serialized.[[NumberData]].
            ValueTag::NumberObject => {
                value = Value::from_object(NumberObject::create(vm, realm, self.serialized.decode_f64()?));
            }

            // 9. Otherwise, if serialized.[[Type]] is "BigInt", then set value to a new BigInt object in targetRealm whose [[BigIntData]] internal slot value is serialized.[[BigIntData]].
            ValueTag::BigIntObject => {
                let big_int = self.serialized.decode_big_int()?.as_bigint();
                value = Value::from_object(BigIntObject::create(vm, realm, big_int));
            }

            // 10. Otherwise, if serialized.[[Type]] is "String", then set value to a new String object in targetRealm whose [[StringData]] internal slot value is serialized.[[StringData]].
            ValueTag::StringObject => {
                let string = decode_string()?;
                value = Value::from_object(StringObject::create(vm, realm, string, realm.string_prototype(vm)));
            }

            // 11. Otherwise, if serialized.[[Type]] is "Date", then set value to a new Date object in targetRealm whose [[DateValue]] internal slot value is serialized.[[DateValue]].
            ValueTag::DateObject => {
                value = Value::from_object(Date::create(vm, realm, self.serialized.decode_f64()?));
            }

            // 12. Otherwise, if serialized.[[Type]] is "RegExp", then set value to a new RegExp object in targetRealm whose [[RegExpMatcher]] internal slot value is serialized.[[RegExpMatcher]],
            //     whose [[OriginalSource]] internal slot value is serialized.[[OriginalSource]], and whose [[OriginalFlags]] internal slot value is serialized.[[OriginalFlags]].
            ValueTag::RegExpObject => {
                let pattern = decode_string()?;
                let flags = decode_string()?;

                let Ok(regexp) = regexp_create(vm, Value::from_string(pattern), Value::from_string(flags)) else {
                    return Err(data_clone_error_from_serialization_error(
                        realm,
                        "Invalid RegExp pattern or flags",
                    ));
                };
                value = Value::from_object(regexp);
            }

            // 13. Otherwise, if serialized.[[Type]] is "SharedArrayBuffer", then set value to a new SharedArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]] and whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]].
            ValueTag::SharedArrayBuffer => 'shared_array_buffer: {
                // 4. If serialized.[[AgentCluster]] is not null and targetRealm's corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.
                // NB: The agent cluster is encoded with the record of a SharedArrayBuffer, the only record restricted to one.
                self.deserialize_agent_cluster()?;

                let side_table_index = self.serialized.decode_u32()?;

                // AD-HOC: When the backing was transferred as cross-process shared memory, map it from the side list by
                //         index — so both agents reference the same bytes.
                let is_shared_backed = self.serialized.decode_bool()?;
                if is_shared_backed {
                    let index = self.serialized.decode_u32()?;
                    let shared_object_id = self.serialized.decode_u64()?;
                    value = self
                        .serialized
                        .array_buffer_from_shared_memory(index, shared_object_id)?;
                    break 'shared_array_buffer;
                }

                let buffer = self.serialized.decode_byte_buffer()?;

                if let Some(array_buffer) =
                    self.serialized
                        .array_buffer_sharing_side_table_entry(side_table_index, buffer.len(), false)
                {
                    value = Value::from_object(array_buffer);
                    break 'shared_array_buffer;
                }

                // A buffer with no cross-process shared memory to share (a zero-length one, e.g.) whose record crossed a
                // process boundary arrives with an empty side table — so it's reconstructed from the byte copy.
                value = Value::from_object(buffer.into_array_buffer(realm, ArrayBufferShared::Yes));
            }

            // 14. Otherwise, if serialized.[[Type]] is "GrowableSharedArrayBuffer", then set value to a new SharedArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], whose [[ArrayBufferByteLengthData]] internal slot value is serialized.[[ArrayBufferByteLengthData]], and whose [[ArrayBufferMaxByteLength]] internal slot value is serialized.[[ArrayBufferMaxByteLength]].
            ValueTag::GrowableSharedArrayBuffer => 'growable_shared_array_buffer: {
                // 4. If serialized.[[AgentCluster]] is not null and targetRealm's corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.
                // NB: The agent cluster is encoded with the record of a SharedArrayBuffer, the only record restricted to one.
                self.deserialize_agent_cluster()?;

                let side_table_index = self.serialized.decode_u32()?;
                let buffer = self.serialized.decode_byte_buffer()?;
                let max_byte_length = self.serialized.decode_size()?;

                if let Some(array_buffer) =
                    self.serialized
                        .array_buffer_sharing_side_table_entry(side_table_index, buffer.len(), true)
                {
                    let data = array_buffer
                        .downcast::<ArrayBuffer>()
                        .expect("the side table holds ArrayBuffers");
                    data.set_max_byte_length(max_byte_length);
                    value = Value::from_object(array_buffer);
                    break 'growable_shared_array_buffer;
                }

                let data = buffer.into_array_buffer(realm, ArrayBufferShared::Yes);
                data.set_max_byte_length(max_byte_length);

                value = Value::from_object(data);
            }

            // 15. Otherwise, if serialized.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], and whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]].
            //     If this throws an exception, catch it, and then throw a "DataCloneError" DOMException.
            // NB: The bytes were allocated when they were decoded, which throws a "DataCloneError" DOMException if it fails.
            ValueTag::ArrayBuffer => {
                let buffer = self.serialized.decode_byte_buffer()?;
                value = Value::from_object(buffer.into_array_buffer(realm, ArrayBufferShared::No));
            }

            // 16. Otherwise, if serialized.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]], and whose [[ArrayBufferMaxByteLength]] internal slot value is serialized.[[ArrayBufferMaxByteLength]].
            //     If this throws an exception, catch it, and then throw a "DataCloneError" DOMException.
            // NB: The bytes were allocated when they were decoded, which throws a "DataCloneError" DOMException if it fails.
            ValueTag::ResizeableArrayBuffer => {
                let buffer = self.serialized.decode_byte_buffer()?;
                let max_byte_length = self.serialized.decode_size()?;

                let data = buffer.into_array_buffer(realm, ArrayBufferShared::No);
                data.set_max_byte_length(max_byte_length);

                value = Value::from_object(data);
            }

            // 17. Otherwise, if serialized.[[Type]] is "ArrayBufferView":
            ValueTag::ArrayBufferView => {
                // 1. Let deserializedArrayBuffer be ? StructuredDeserialize(serialized.[[ArrayBufferSerialized]], targetRealm, memory).
                let array_buffer_value = self.deserialize()?;
                let array_buffer = if array_buffer_value.is_object() {
                    array_buffer_value.as_object().downcast::<ArrayBuffer>()
                } else {
                    None
                };
                let Some(array_buffer) = array_buffer else {
                    return Err(data_clone_error_from_serialization_error(
                        realm,
                        "ArrayBufferView is not backed by an ArrayBuffer",
                    ));
                };

                let constructor_name = self.serialized.decode_utf16_string()?;
                let byte_length = self.deserialize_byte_length()?;
                let byte_offset = self.serialized.decode_u32()?;

                // Validate in 64 bits; the engine's element-access check works in u32.
                let length_fits_buffer = |length: ByteLength, element_size: u64| -> bool {
                    if length.is_auto() {
                        return !array_buffer.is_fixed_length() && byte_offset as usize <= array_buffer.byte_length();
                    }
                    let end = u64::from(length.length())
                        .checked_mul(element_size)
                        .and_then(|end| end.checked_add(u64::from(byte_offset)));
                    end.is_some_and(|end| end <= array_buffer.byte_length() as u64)
                };

                // 2. If serialized.[[Constructor]] is "DataView", then set value to a new DataView object in targetRealm whose [[ViewedArrayBuffer]] internal slot value is deserializedArrayBuffer,
                //    whose [[ByteLength]] internal slot value is serialized.[[ByteLength]], and whose [[ByteOffset]] internal slot value is serialized.[[ByteOffset]].
                if constructor_name == Utf16String::from_utf8("DataView") {
                    if !length_fits_buffer(byte_length, 1) {
                        return Err(data_clone_error_from_serialization_error(
                            realm,
                            "DataView does not fit its ArrayBuffer",
                        ));
                    }
                    value = Value::from_object(DataView::create(
                        vm,
                        realm,
                        array_buffer,
                        byte_length,
                        byte_offset as usize,
                    ));
                }
                // 3. Otherwise, set value to a new typed array object in targetRealm, using the constructor given by serialized.[[Constructor]], whose [[ViewedArrayBuffer]] internal slot value is
                //    deserializedArrayBuffer, whose [[TypedArrayName]] internal slot value is serialized.[[Constructor]], whose [[ByteLength]] internal slot value is serialized.[[ByteLength]],
                //    whose [[ByteOffset]] internal slot value is serialized.[[ByteOffset]], and whose [[ArrayLength]] internal slot value is serialized.[[ArrayLength]].
                else {
                    let array_length = self.deserialize_byte_length()?;

                    let kind = TYPED_ARRAY_KINDS
                        .iter()
                        .find(|(_, name)| constructor_name == Utf16String::from_utf8(name))
                        .map(|(kind, _)| *kind);

                    let Some(kind) = kind else {
                        return Err(data_clone_error_from_serialization_error(
                            realm,
                            "Unknown ArrayBufferView constructor",
                        ));
                    };

                    let element_size = kind.element_type().size() as u64;

                    // Reject combinations a real typed array could not produce.
                    let mut consistent_slots = byte_length.is_auto() == array_length.is_auto()
                        && u64::from(byte_offset).is_multiple_of(element_size);
                    if consistent_slots && !array_length.is_auto() {
                        let expected_byte_length = u64::from(array_length.length()).checked_mul(element_size);
                        consistent_slots = expected_byte_length == Some(u64::from(byte_length.length()));
                    }
                    if !consistent_slots || !length_fits_buffer(array_length, element_size) {
                        return Err(data_clone_error_from_serialization_error(
                            realm,
                            "ArrayBufferView does not fit its ArrayBuffer",
                        ));
                    }

                    value = Value::from_object(TypedArrayBase::create_from_slots(
                        vm,
                        realm,
                        kind,
                        array_buffer,
                        array_length,
                        byte_length,
                        byte_offset,
                    ));
                }
            }

            // 18. Otherwise, if serialized.[[Type]] is "Map":
            ValueTag::MapObject => {
                // 1. Set value to a new Map object in targetRealm whose [[MapData]] internal slot value is a new empty List.
                value = Value::from_object(Map::create(vm, realm));

                // 2. Set deep to true.
                deep = true;
            }

            // 19. Otherwise, if serialized.[[Type]] is "Set":
            ValueTag::SetObject => {
                // 1. Set value to a new Set object in targetRealm whose [[SetData]] internal slot value is a new empty List.
                value = Value::from_object(Set::create(vm, realm));

                // 2. Set deep to true.
                deep = true;
            }

            // 20. Otherwise, if serialized.[[Type]] is "Array":
            ValueTag::ArrayObject => {
                // 1. Let outputProto be targetRealm.[[Intrinsics]].[[%Array.prototype%]].
                // 2. Set value to ! ArrayCreate(serialized.[[Length]], outputProto).
                let length = self.serialized.decode_u64()?;
                let Ok(array) = Array::create(vm, realm, length, Some(realm.intrinsics().array_prototype(vm))) else {
                    return Err(data_clone_error_from_serialization_error(
                        realm,
                        "Array length out of range",
                    ));
                };
                value = Value::from_object(array);

                // 3. Set deep to true.
                deep = true;
            }

            // 21. Otherwise, if serialized.[[Type]] is "Object":
            ValueTag::Object => {
                // 1. Set value to a new Object in targetRealm.
                value = Value::from_object(Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm))));

                // 2. Set deep to true.
                deep = true;
            }

            // 22. Otherwise, if serialized.[[Type]] is "Error":
            ValueTag::ErrorObject => {
                // 1. Let prototype be %Error.prototype%.
                // 2. If serialized.[[Name]] is "EvalError", then set prototype to %EvalError.prototype%.
                // 3. If serialized.[[Name]] is "RangeError", then set prototype to %RangeError.prototype%.
                // 4. If serialized.[[Name]] is "ReferenceError", then set prototype to %ReferenceError.prototype%.
                // 5. If serialized.[[Name]] is "SyntaxError", then set prototype to %SyntaxError.prototype%.
                // 6. If serialized.[[Name]] is "TypeError", then set prototype to %TypeError.prototype%.
                // 7. If serialized.[[Name]] is "URIError", then set prototype to %URIError.prototype%.
                // NB: The error created below for the decoded type has that prototype.
                let error_type = decode_error_type(self.serialized)?;

                // 8. Let message be serialized.[[Message]].
                let message = self.serialized.decode_optional_utf16_string()?;

                // FIXME: Spec bug - https://github.com/whatwg/html/issues/11321
                // MISSING STEP: Let cause be serialized.[[Cause]].
                let cause = self.serialized.decode_optional_utf16_string()?;

                // 9. Set value to OrdinaryObjectCreate(prototype, « [[ErrorData]], [[Stack]] »).
                let error = error_kind_of(error_type).create_without_message(vm, realm);

                // 10. Let messageDesc be PropertyDescriptor { [[Value]]: message, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }.
                // 11. If message is not undefined, then perform ! OrdinaryDefineOwnProperty(value, "message", messageDesc).
                if let Some(message) = message {
                    error.set_message(vm, message);
                }

                // FIXME: 12. Set value.[[Stack]] to serialized.[[Stack]].
                // 13. Any interesting accompanying data attached to serialized should be deserialized and attached to value.
                // MISSING STEP: If cause is not undefined, then perform ! CreateNonEnumerableDataPropertyOrThrow(value, "cause", cause).
                if let Some(cause) = cause {
                    error.create_non_enumerable_data_property_or_throw(
                        vm,
                        &vm.names.cause,
                        Value::from_string(PrimitiveString::create(vm, cause)),
                    );
                }

                value = Value::from_object(error);
            }

            // 23. Otherwise:
            _ => {
                if tag != ValueTag::SerializableObject {
                    return Err(data_clone_error_from_serialization_error(
                        realm,
                        "Unexpected structured serialize value tag",
                    ));
                }

                // 1. Let interfaceName be serialized.[[Type]].
                let interface_name = self.serialized.decode_interface_name()?;

                let mut version = None;
                if self.serialized.serialization_type() == SerializationType::Storage {
                    version = Some(self.serialized.decode_u64()?);
                }

                // 2. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.
                if !is_exposed(interface_name, realm) {
                    return Err(data_clone_error("Unsupported type"));
                }

                // NB: The deserialization steps of an interface implemented in Rust create the new instance from what they
                //     read, so they run here, at the position in memory that the instance has.
                if serializable_interface_is_implemented_in_rust(interface_name) {
                    if version.is_some_and(|version| version != serializable_payload_version()) {
                        return Err(data_clone_error_from_serialization_error(
                            realm,
                            "Unsupported serializable payload version",
                        ));
                    }
                    let memory_index = self.memory.size();
                    self.memory.append(Value::UNDEFINED);
                    let value = deserialize_rust_platform_object(interface_name, self.serialized, self.memory)?;
                    self.memory.set(memory_index, value);
                    return Ok(value);
                }

                // 3. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
                value = Value::from_object(create_serialized_platform_object(interface_name, realm));

                if version.is_some_and(|version| version != serializable_payload_version()) {
                    return Err(data_clone_error_from_serialization_error(
                        realm,
                        "Unsupported serializable payload version",
                    ));
                }

                // 4. Set deep to true.
                deep = true;
            }
        }

        // 24. Set memory[serialized] to value.
        // IMPLEMENTATION DEFINED: We don't add primitive values to the memory to match the serialization indices (which also doesn't add them)
        if !is_primitive {
            self.memory.append(value);
        }

        // 25. If deep is true:
        if deep {
            // 1. If serialized.[[Type]] is "Map":
            if tag == ValueTag::MapObject {
                let map = value.as_object().downcast::<Map>().expect("value is a Map");
                let length = self.serialized.decode_u64()?;

                // 1. For each Record { [[Key]], [[Value]] } entry of serialized.[[MapData]]:
                for _ in 0..length {
                    // 1. Let deserializedKey be ? StructuredDeserialize(entry.[[Key]], targetRealm, memory).
                    let deserialized_key = self.deserialize()?;

                    // 2. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]], targetRealm, memory).
                    let deserialized_value = self.deserialize()?;

                    // 3. Append { [[Key]]: deserializedKey, [[Value]]: deserializedValue } to value.[[MapData]].
                    map.map_set(vm, deserialized_key, deserialized_value);
                }
            }
            // 2. Otherwise, if serialized.[[Type]] is "Set":
            else if tag == ValueTag::SetObject {
                let set = value.as_object().downcast::<Set>().expect("value is a Set");
                let length = self.serialized.decode_u64()?;

                // 1. For each entry of serialized.[[SetData]]:
                for _ in 0..length {
                    // 1. Let deserializedEntry be ? StructuredDeserialize(entry, targetRealm, memory).
                    let deserialized_entry = self.deserialize()?;

                    // 2. Append deserializedEntry to value.[[SetData]].
                    set.set_add(vm, deserialized_entry);
                }
            }
            // 3. Otherwise, if serialized.[[Type]] is "Array" or "Object":
            else if tag == ValueTag::ArrayObject || tag == ValueTag::Object {
                let object = value.as_object();

                // 1. For each Record { [[Key]], [[Value]] } entry of serialized.[[Properties]]:
                // EndObject ends the list; any other tag begins a property whose value precedes its untagged key.
                loop {
                    let raw_property_tag = self.serialized.decode_u8()?;
                    if raw_property_tag == ValueTag::EndObject as u8 {
                        break;
                    }
                    // 1. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]], targetRealm, memory).
                    let deserialized_value = self.deserialize_value(raw_property_tag)?;
                    let key = self.serialized.decode_utf16_string()?;

                    // 2. Let result be ! CreateDataProperty(value, entry.[[Key]], deserializedValue).
                    let result = object.create_data_property(
                        vm,
                        &PropertyKey::from_utf16_string(&key),
                        deserialized_value,
                        None,
                        None,
                    );

                    // 3. Assert: result is true.
                    // NB: A record that was not serialized from a value, as one read back from storage, can fail here, which throws instead.
                    if !matches!(result, Ok(true)) {
                        return Err(data_clone_error_from_serialization_error(
                            realm,
                            "Invalid serialized property",
                        ));
                    }
                }
            }
            // 4. Otherwise:
            else {
                // 1. Perform the appropriate deserialization steps for the interface identified by serialized.[[Type]], given serialized, value, and targetRealm.
                deserialization_steps(value.as_object(), self.serialized, self.memory)?;
            }
        }

        // 26. Return value.
        Ok(value)
    }
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
pub(crate) fn structured_serialize_with_transfer(
    cx: &CallContext,
    serialized_writer: StructuredSerializeWriter,
    record: SerializedTransferRecord,
    value: Value,
    transfer_list: &[Gc<Object>],
) -> WebIdlResult<()> {
    let vm = cx.vm();

    // 1. Let memory be an empty map.
    // NB: C++ creates the writer, and makes the record of what this returns.
    let memory = cpp::OwnedSerializationMemory::new();

    // 2. For each transferable of transferList:
    for transferable in transfer_list {
        let as_array_buffer = transferable.downcast::<ArrayBuffer>();

        // 1. If transferable has neither an [[ArrayBufferData]] internal slot nor a [[Detached]] internal slot, then throw a "DataCloneError" DOMException.
        // NB: The platform objects that have a [[Detached]] internal slot are the transferable objects.
        if as_array_buffer.is_none() && !is_transferable(*transferable) {
            return Err(data_clone_error("Cannot transfer type"));
        }

        // 2. If transferable has an [[ArrayBufferData]] internal slot and IsSharedArrayBuffer(transferable) is true, then throw a "DataCloneError" DOMException.
        if as_array_buffer.is_some_and(|array_buffer| array_buffer.is_shared_array_buffer()) {
            return Err(data_clone_error("Cannot transfer shared array buffer"));
        }

        let transferable_value = Value::from_object(*transferable);

        // 3. If memory[transferable] exists, then throw a "DataCloneError" DOMException.
        if memory.memory().contains(transferable_value) {
            return Err(data_clone_error("Cannot transfer value twice"));
        }

        // 4. Set memory[transferable] to { [[Type]]: an uninitialized value, [[AgentCluster]]: null }.
        memory.memory().set(transferable_value, memory.memory().size());
    }

    // 3. Let serialized be ? StructuredSerializeInternal(value, false, memory).
    structured_serialize_internal(
        vm,
        Some(cx.realm()),
        serialized_writer,
        value,
        false,
        memory.memory(),
        AllowSharedArrayBuffers::CrossOriginIsolatedOnly,
    )?;

    // AD-HOC: Gather the pixels of the transferred ImageBitmaps in one buffer of shared memory. Without it, as when there
    //         is not enough memory for it, the pixels travel inline.
    let image_bitmap_pixels = serialized_writer.gather_image_bitmap_pixels(transfer_list);

    // 4. Let transferDataHolders be a new empty List.
    // NB: The data holders are appended to the record that C++ returns.

    // 5. For each transferable of transferList:
    for transferable in transfer_list {
        let array_buffer = transferable.downcast::<ArrayBuffer>();
        let is_detached = array_buffer.is_some_and(|array_buffer| array_buffer.is_detached());

        // 1. If transferable has an [[ArrayBufferData]] internal slot and IsDetachedBuffer(transferable) is true, then throw a "DataCloneError" DOMException.
        if is_detached {
            return Err(data_clone_error("Cannot transfer detached buffer"));
        }

        // 2. If transferable has a [[Detached]] internal slot and transferable.[[Detached]] is true, then throw a "DataCloneError" DOMException.
        if is_transferable(*transferable) && transferable_is_detached(*transferable) {
            return Err(data_clone_error("Value already transferred"));
        }

        // 3. Let dataHolder be memory[transferable].
        // IMPLEMENTATION DEFINED: We just create a data holder here, our memory holds indices into the serialized record.
        let data_holder = TransferDataHolder::new();

        // 4. If transferable has an [[ArrayBufferData]] internal slot:
        if let Some(array_buffer) = array_buffer {
            // 1. If transferable has an [[ArrayBufferMaxByteLength]] internal slot:
            let buffer_data = array_buffer.copy_all_to_byte_buffer();

            if !array_buffer.is_fixed_length() {
                // 1. Set dataHolder.[[Type]] to "ResizableArrayBuffer".
                data_holder.encode_u8(TransferType::ResizableArrayBuffer as u8);

                // 2. Set dataHolder.[[ArrayBufferData]] to transferable.[[ArrayBufferData]].
                // 3. Set dataHolder.[[ArrayBufferByteLength]] to transferable.[[ArrayBufferByteLength]].
                data_holder.encode_bytes(&buffer_data);

                // 4. Set dataHolder.[[ArrayBufferMaxByteLength]] to transferable.[[ArrayBufferMaxByteLength]].
                data_holder.encode_size(array_buffer.max_byte_length());
            }
            // 2. Otherwise:
            else {
                // 1. Set dataHolder.[[Type]] to "ArrayBuffer".
                data_holder.encode_u8(TransferType::ArrayBuffer as u8);

                // 2. Set dataHolder.[[ArrayBufferData]] to transferable.[[ArrayBufferData]].
                // 3. Set dataHolder.[[ArrayBufferByteLength]] to transferable.[[ArrayBufferByteLength]].
                data_holder.encode_bytes(&buffer_data);
            }

            // 3. Perform ? DetachArrayBuffer(transferable).
            // NOTE: Specifications can use the [[ArrayBufferDetachKey]] internal slot to prevent ArrayBuffers from being detached. This is used in WebAssembly JavaScript Interface, for example. See: https://html.spec.whatwg.org/multipage/references.html#refsWASMJS
            detach_array_buffer(vm, array_buffer, None)?;
        }
        // 5. Otherwise:
        else {
            // 1. Assert: transferable is a platform object that is a transferable object.
            assert!(is_transferable(*transferable));
            assert!(is_platform_object(*transferable));

            // 2. Let interfaceName be the identifier of the primary interface of transferable.
            let interface_name = transferable_primary_interface(*transferable);

            // 3. Set dataHolder.[[Type]] to interfaceName.
            data_holder.encode_u8(interface_name);

            // FIXME: 4. If transferable's primary interface is decorated with [SameAgentCluster], then restrict dataHolder to the same agent cluster.

            // 5. Perform the appropriate transfer steps for the interface identified by interfaceName, given transferable and dataHolder.
            transfer_steps(cx, *transferable, &data_holder, &image_bitmap_pixels)?;

            // 6. Set transferable.[[Detached]] to true.
            transferable_set_detached(*transferable);
        }

        // 6. Append dataHolder to transferDataHolders.
        record.append_data_holder(data_holder);
    }

    // 6. Let result be { [[Serialized]]: serialized, [[TransferDataHolders]]: transferDataHolders, [[InterfaceNames]]: an empty ordered set, [[AgentCluster]]: null }.
    // FIXME: 7. For each object → record of memory:
    //     1. If object is a platform object, then append record.[[Type]] to result.[[InterfaceNames]].
    //     2. If record.[[AgentCluster]] is not null, then restrict result to the same agent cluster.
    // 8. Return result.
    Ok(())
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserializewithtransfer
pub(crate) fn structured_deserialize_with_transfer(
    cx: &CallContext,
    serialize_with_transfer_result: SerializedTransferRecord,
    transferred_values: TransferredValues,
) -> WebIdlResult<Value> {
    let target_realm = cx.realm();

    // 1. Let memory be an empty map.
    let memory = OwnedDeserializationMemory::new();

    // FIXME: 2. If serializeWithTransferResult.[[AgentCluster]] is not null and targetRealm's corresponding agent cluster is not serializeWithTransferResult.[[AgentCluster]], then throw a "DataCloneError" DOMException.
    // FIXME: 3. For each interfaceName of serializeWithTransferResult.[[InterfaceNames]]:
    //     1. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.

    // 4. Let transferredValues be a new empty List.
    // NB: C++ lends the list that it returns.

    // 5. For each transferDataHolder of serializeWithTransferResult.[[TransferDataHolders]]:
    for index in 0..serialize_with_transfer_result.data_holder_count() {
        let Some(decoder) = serialize_with_transfer_result.take_data_holder(index) else {
            continue;
        };

        // 1. Let value be an uninitialized value.
        let value = structured_deserialize_with_transfer_internal(cx, decoder.decoder())?;

        // 5. Set memory[transferDataHolder] to value.
        memory.memory().append(value);

        // 6. Append value to transferredValues.
        transferred_values.append(value.as_object());
    }

    // 6. Let deserialized be ? StructuredDeserialize(serializeWithTransferResult.[[Serialized]], targetRealm, memory).
    let deserialized = serialize_with_transfer_result.deserialize(target_realm, memory.memory())?;

    // NB: The deserialized values hold their own references to the shared memory they use. The record can outlive its
    //     delivery, as in a task that awaits garbage collection, so it must not keep that memory alive.
    serialize_with_transfer_result.clear_shared_buffers();

    // 7. Return { [[Deserialized]]: deserialized, [[TransferredValues]]: transferredValues }.
    Ok(deserialized)
}

// AD-HOC: This non-standard overload is meant to extract just one transferrable value from a serialized transfer record.
//         It's primarily useful for an object's transfer receiving steps to deserialize a nested value.
pub(crate) fn structured_deserialize_with_transfer_internal(
    cx: &CallContext,
    decoder: TransferDataDecoder,
) -> WebIdlResult<Value> {
    let target_realm = cx.realm();

    let transfer_type = decoder.decode_u8(target_realm)?;

    // 1. Let value be an uninitialized value.
    #[allow(clippy::needless_late_init, reason = "the steps let value be uninitialized first")]
    let value: Value;

    // 2. If transferDataHolder.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm
    //    whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], and
    //    whose [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]].
    // NOTE: In cases where the original memory occupied by [[ArrayBufferData]] is accessible during the deserialization,
    //       this step is unlikely to throw an exception, as no new memory needs to be allocated: the memory occupied by
    //       [[ArrayBufferData]] is instead just getting transferred into the new ArrayBuffer. This could be true, for example,
    //       when both the source and target realms are in the same process.
    if transfer_type == TransferType::ArrayBuffer as u8 {
        let buffer = decoder.decode_byte_buffer(target_realm)?;
        value = Value::from_object(buffer.into_array_buffer(target_realm, ArrayBufferShared::No));
    }
    // 3. Otherwise, if transferDataHolder.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object
    //     in targetRealm whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], whose
    //     [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]], and whose
    //     [[ArrayBufferMaxByteLength]] internal slot value is transferDataHolder.[[ArrayBufferMaxByteLength]].
    // NOTE: For the same reason as the previous step, this step is also unlikely to throw an exception.
    else if transfer_type == TransferType::ResizableArrayBuffer as u8 {
        let buffer = decoder.decode_byte_buffer(target_realm)?;
        let max_byte_length = decoder.decode_size(target_realm)?;

        let data = buffer.into_array_buffer(target_realm, ArrayBufferShared::No);
        data.set_max_byte_length(max_byte_length);

        value = Value::from_object(data);
    }
    // 4. Otherwise:
    else {
        // 1. Let interfaceName be transferDataHolder.[[Type]].
        // NB: Step 3 of StructuredDeserializeWithTransfer checks that every transferred interface is exposed before any
        //     transfer-receiving steps run. The check runs here, per data holder, instead.
        if !is_transferable_interface_exposed(transfer_type, target_realm) {
            return Err(data_clone_error("Unknown type transferred"));
        }

        // 2. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
        // 3. Perform the appropriate transfer-receiving steps for the interface identified by interfaceName given transferDataHolder and value.
        value = create_transferred_platform_object(transfer_type, target_realm, decoder)?;
    }

    Ok(value)
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
pub(crate) fn structured_serialize_internal(
    vm: &Vm,
    realm: Option<Gc<Realm>>,
    serialized: StructuredSerializeWriter,
    value: Value,
    for_storage: bool,
    memory: SerializationMemory,
    allow_shared_array_buffers: AllowSharedArrayBuffers,
) -> WebIdlResult<()> {
    let serializer = Serializer {
        vm,
        realm,
        serialized,
        memory,
        for_storage,
        allow_shared_array_buffers,
    };
    serializer.serialize(value)
}

pub(crate) fn structured_deserialize_internal(
    cx: &CallContext,
    serialized: StructuredSerializeReader,
    memory: DeserializationMemory,
    check_fully_consumed: bool,
) -> WebIdlResult<Value> {
    let deserializer = Deserializer { cx, serialized, memory };
    let value = deserializer.deserialize()?;

    // Nested sub-value decodes legitimately stop mid-record.
    if check_fully_consumed && serialized.serialization_type() == SerializationType::Storage && !serialized.is_at_end()
    {
        return Err(data_clone_error_from_serialization_error(
            cx.realm(),
            "Trailing data after structured serialize storage record",
        ));
    }

    Ok(value)
}
