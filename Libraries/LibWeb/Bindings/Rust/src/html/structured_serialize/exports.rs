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

use crate::bindings::interface::realm_arguments;
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::structured_serialize::cpp::{
    DeserializationMemory, SerializationMemory, SerializedTransferRecord, StructuredSerializeReader,
    StructuredSerializeWriter, TransferDataDecoder, TransferredValues,
};
use crate::html::structured_serialize::{
    AllowSharedArrayBuffers, structured_deserialize_internal, structured_deserialize_with_transfer,
    structured_deserialize_with_transfer_internal, structured_serialize_internal, structured_serialize_with_transfer,
};

/// The completion of an algorithm, as the entry points return it to C++.
fn completion(cx: &CallContext, result: WebIdlResult<Value>) -> RawNativeFunctionResult {
    let result: ThrowCompletionOr<Value> = result.map_err(|exception| exception.throw(cx));
    RawNativeFunctionResult::from_completion(result)
}

/// StructuredSerializeInternal, which runs in the current realm.
///
/// # Safety
///
/// The pointers must be the VM, and a writer and a memory that C++ lends. The value must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_serialize_internal(
    vm: *mut c_void,
    serialized: *mut c_void,
    value: u64,
    for_storage: bool,
    memory: *mut c_void,
    allow_shared_array_buffers: u8,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    // SAFETY: The caller lends the writer and the memory, and passes a live value.
    let (serialized, memory, value) = unsafe {
        (
            StructuredSerializeWriter::from_raw(serialized),
            SerializationMemory::from_raw(memory),
            Value::from_raw(value),
        )
    };
    // NB: Code outside of any script serializes values too, with no current realm. An object's exception is created in
    //     its realm then; a Symbol throws too, but only script serializes one.
    let realm = vm
        .current_realm()
        .or_else(|| value.is_object().then(|| value.as_object().shape().realm()));
    let result = structured_serialize_internal(
        vm,
        realm,
        serialized,
        value,
        for_storage,
        memory,
        AllowSharedArrayBuffers::from_abi(allow_shared_array_buffers),
    );
    match result {
        Ok(()) => RawNativeFunctionResult::from_completion(Ok(Value::UNDEFINED)),
        Err(exception) => {
            let cx = CallContext::new(vm, realm.expect("what throws has a realm"));
            completion(&cx, Err(exception))
        }
    }
}

/// StructuredDeserialize in the realm, which throws a "DataCloneError" DOMException for a storage record with bytes
/// after its value if `check_fully_consumed` is true.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and a reader and a memory that C++ lends.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_deserialize_internal(
    vm: *mut c_void,
    target_realm: *mut c_void,
    serialized: *mut c_void,
    memory: *mut c_void,
    check_fully_consumed: bool,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, target_realm) = unsafe { realm_arguments(vm, target_realm) };
    let cx = CallContext::new(vm, target_realm);
    // SAFETY: The caller lends the reader and the memory.
    let (serialized, memory) = unsafe {
        (
            StructuredSerializeReader::from_raw(serialized, target_realm),
            DeserializationMemory::from_raw(memory),
        )
    };
    let result = structured_deserialize_internal(&cx, serialized, memory, check_fully_consumed);
    completion(&cx, result)
}

/// StructuredSerializeWithTransfer in the realm, which writes [[Serialized]] with the writer, and appends
/// [[TransferDataHolders]] to the record.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, a writer and a record that C++ lends, and `count` live objects. The
/// value must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_serialize_with_transfer(
    vm: *mut c_void,
    realm: *mut c_void,
    serialized: *mut c_void,
    record: *mut c_void,
    value: u64,
    transfer_list: *const *mut c_void,
    count: usize,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let cx = CallContext::new(vm, realm);
    // SAFETY: The caller lends the writer and the record, and passes a live value.
    let (serialized, record, value) = unsafe {
        (
            StructuredSerializeWriter::from_raw(serialized),
            SerializedTransferRecord::from_raw(record),
            Value::from_raw(value),
        )
    };
    let transfer_list_values = MarkedVec::with_capacity(vm, count);
    for index in 0..count {
        // SAFETY: The caller passes `count` live objects.
        transfer_list_values.push(unsafe {
            Gc::from_non_null(NonNull::new((*transfer_list.add(index)).cast::<Object>()).expect("C++ passes objects"))
        });
    }
    let result = structured_serialize_with_transfer(&cx, serialized, record, value, &transfer_list_values.values());
    completion(&cx, result.map(|()| Value::UNDEFINED))
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
    let result = structured_deserialize_with_transfer(&cx, record, transferred_values);
    completion(&cx, result)
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
