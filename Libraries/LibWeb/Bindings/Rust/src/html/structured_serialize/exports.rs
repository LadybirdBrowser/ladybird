/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points through which LibWeb's C++ code runs structured serialization: the functions of
//! Libraries/LibWeb/HTML/StructuredSerialize.h, which keep their C++ signatures for the C++ code that calls them.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, MarkedVec, Object, RawNativeFunctionResult, ThrowCompletionOr, Value, Vm};
use libwebcommon_rust::html::serialization_records::SerializationType;

use crate::bindings::interface::realm_arguments;
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::structured_serialize::cpp::{
    IPCSerializationRecord, ReceivedIPCSerializationRecord, SerializedTransferRecord, SharedBuffers,
    StorageSerializationRecord, TransferDataDecoder, TransferredValues,
};
use crate::html::structured_serialize::memory::{DeserializationMemory, SerializationMemory};
use crate::html::structured_serialize::reader::StructuredSerializeReader;
use crate::html::structured_serialize::writer::StructuredSerializeWriter;
use crate::html::structured_serialize::{
    AllowSharedArrayBuffers, structured_deserialize_internal, structured_deserialize_with_transfer,
    structured_deserialize_with_transfer_internal, structured_serialize_internal, structured_serialize_with_transfer,
};

/// The completion of an algorithm, as the entry points return it to C++.
fn completion(cx: &CallContext, result: WebIdlResult<Value>) -> RawNativeFunctionResult {
    let result: ThrowCompletionOr<Value> = result.map_err(|exception| exception.throw(cx));
    RawNativeFunctionResult::from_completion(result)
}

/// StructuredSerializeInternal(value, forStorage) in the current realm, with a new memory.
fn structured_serialize_in_current_realm<'vm>(
    vm: &'vm Vm,
    serialized: &StructuredSerializeWriter<'vm>,
    value: Value,
    for_storage: bool,
    allow_shared_array_buffers: AllowSharedArrayBuffers,
) -> Result<(), RawNativeFunctionResult> {
    let memory = SerializationMemory::new(vm);
    // NB: Code outside of any script serializes values too, with no current realm. An object's exception is created in
    //     its realm then; a Symbol throws too, but only script serializes one.
    let realm = vm
        .current_realm()
        .or_else(|| value.is_object().then(|| value.as_object().shape().realm()));
    structured_serialize_internal(
        vm,
        realm,
        serialized,
        value,
        for_storage,
        &memory,
        allow_shared_array_buffers,
    )
    .map_err(|exception| {
        let cx = CallContext::new(vm, realm.expect("what throws has a realm"));
        completion(&cx, Err(exception))
    })
}

/// StructuredSerialize, which runs in the current realm, and sets the record. The record shares the cross-process
/// shared memory of SharedArrayBuffers through the list, if there is one.
///
/// # Safety
///
/// The pointers must be the VM, a list that C++ lends or null, and a record that C++ lends. The value must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_serialize(
    vm: *mut c_void,
    value: u64,
    allow_shared_array_buffers: u8,
    shared_buffers: *mut c_void,
    record: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    // SAFETY: The caller passes a live value, and lends the list and the record.
    let (value, record) = unsafe { (Value::from_raw(value), IPCSerializationRecord::from_raw(record)) };
    let serialized = if shared_buffers.is_null() {
        StructuredSerializeWriter::new(vm, SerializationType::Ipc)
    } else {
        // SAFETY: The caller lends the list.
        StructuredSerializeWriter::new_with_shared_buffers(vm, unsafe { SharedBuffers::from_raw(shared_buffers) })
    };
    if let Err(result) = structured_serialize_in_current_realm(
        vm,
        &serialized,
        value,
        false,
        AllowSharedArrayBuffers::from_abi(allow_shared_array_buffers),
    ) {
        return result;
    }
    serialized.finish_ipc_record(record);
    RawNativeFunctionResult::from_completion(Ok(Value::UNDEFINED))
}

/// StructuredSerializeForStorage, which runs in the current realm, and sets the record.
///
/// # Safety
///
/// The pointers must be the VM and a record that C++ lends. The value must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_serialize_for_storage(
    vm: *mut c_void,
    value: u64,
    record: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    // SAFETY: The caller passes a live value, and lends the record.
    let (value, record) = unsafe { (Value::from_raw(value), StorageSerializationRecord::from_raw(record)) };
    let serialized = StructuredSerializeWriter::new(vm, SerializationType::Storage);
    if let Err(result) = structured_serialize_in_current_realm(
        vm,
        &serialized,
        value,
        true,
        AllowSharedArrayBuffers::CrossOriginIsolatedOnly,
    ) {
        return result;
    }
    serialized.finish_storage_record(record);
    RawNativeFunctionResult::from_completion(Ok(Value::UNDEFINED))
}

/// StructuredDeserialize of the IPC record in the realm. The record shares the cross-process shared memory of
/// SharedArrayBuffers through the list, if there is one.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, a record that C++ lends, and a list that C++ lends or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_deserialize(
    vm: *mut c_void,
    target_realm: *mut c_void,
    record: *const c_void,
    shared_buffers: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, target_realm) = unsafe { realm_arguments(vm, target_realm) };
    let cx = CallContext::new(vm, target_realm);
    // SAFETY: The caller lends the record, and the list if there is one.
    let (record, shared_buffers) = unsafe {
        (
            ReceivedIPCSerializationRecord::from_raw(record),
            (!shared_buffers.is_null()).then(|| SharedBuffers::from_raw(shared_buffers)),
        )
    };
    let serialized = StructuredSerializeReader::new_ipc(target_realm, record, shared_buffers);
    let memory = DeserializationMemory::new(vm);
    completion(&cx, structured_deserialize_internal(&cx, &serialized, &memory, false))
}

/// StructuredDeserialize of the bytes of the storage record in the realm, which throws a "DataCloneError" DOMException
/// for bytes after its value.
///
/// # Safety
///
/// The pointers must be the VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_deserialize_for_storage(
    vm: *mut c_void,
    target_realm: *mut c_void,
    data: ak::Span<'_, u8>,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, target_realm) = unsafe { realm_arguments(vm, target_realm) };
    let cx = CallContext::new(vm, target_realm);
    let result = StructuredSerializeReader::new_storage(target_realm, data.as_slice()).and_then(|serialized| {
        let memory = DeserializationMemory::new(vm);
        structured_deserialize_internal(&cx, &serialized, &memory, true)
    });
    completion(&cx, result)
}

/// StructuredSerializeWithTransfer in the realm, whose record the caller owns.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and `count` live objects. The value must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_serialize_with_transfer(
    vm: *mut c_void,
    realm: *mut c_void,
    value: u64,
    transfer_list: *const *mut c_void,
    count: usize,
    out_record: &mut *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let cx = CallContext::new(vm, realm);
    // SAFETY: The caller passes a live value.
    let value = unsafe { Value::from_raw(value) };
    let transfer_list_values = MarkedVec::with_capacity(vm, count);
    for index in 0..count {
        // SAFETY: The caller passes `count` live objects.
        transfer_list_values.push(unsafe {
            Gc::from_non_null(NonNull::new((*transfer_list.add(index)).cast::<Object>()).expect("C++ passes objects"))
        });
    }
    let result = structured_serialize_with_transfer(&cx, value, &transfer_list_values.values());
    completion(
        &cx,
        result.map(|record| {
            *out_record = record.into_raw();
            Value::UNDEFINED
        }),
    )
}

/// StructuredDeserializeWithTransfer in the realm, which returns [[Deserialized]], and appends [[TransferredValues]] to
/// the list.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and a record and a list that C++ lends.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_deserialize_with_transfer(
    vm: *mut c_void,
    target_realm: *mut c_void,
    record: *mut c_void,
    transferred_values: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, target_realm) = unsafe { realm_arguments(vm, target_realm) };
    let cx = CallContext::new(vm, target_realm);
    // SAFETY: The caller lends the record and the list.
    let (record, transferred_values) = unsafe {
        (
            SerializedTransferRecord::from_raw(record),
            TransferredValues::from_raw(transferred_values),
        )
    };
    let result = structured_deserialize_with_transfer(&cx, record);
    completion(
        &cx,
        result.map(|deserialize_record| {
            for value in deserialize_record.transferred_values.values().iter() {
                transferred_values.append(*value);
            }
            deserialize_record.deserialized
        }),
    )
}

/// The value that one data holder of a transfer holds, in the realm.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and a decoder that C++ lends.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_deserialize_with_transfer_internal(
    vm: *mut c_void,
    target_realm: *mut c_void,
    decoder: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, target_realm) = unsafe { realm_arguments(vm, target_realm) };
    let cx = CallContext::new(vm, target_realm);
    // SAFETY: The caller lends the decoder.
    let decoder = unsafe { TransferDataDecoder::from_raw(decoder) };
    let result = structured_deserialize_with_transfer_internal(&cx, decoder);
    completion(&cx, result)
}
