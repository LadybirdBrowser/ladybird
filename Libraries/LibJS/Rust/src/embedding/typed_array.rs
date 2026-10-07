/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The TypedArray kinds and the views they give of the bytes of their buffers.
//!
//! A kind crosses as the u8 of TypedArrayBase::Kind, one of the JS_LAYOUT_TYPED_ARRAY_KIND_* constants of
//! LibJS/Embedding/Layout.h, which also has the offsets of the slots an embedder may read directly. Every function here
//! must be called on the thread that runs the VM.

use crate::embedding::abi_types::{JSRealm, cell_from_abi, completion_into_abi, object_into_abi, vm_from_abi};
use crate::embedding::array_buffer::{
    JSByteLength, byte_length_from_abi, byte_length_into_abi, cell_of_class_from_abi, order_from_abi,
};
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM};
use crate::runtime::array_buffer::ArrayBuffer;
use crate::runtime::typed_array::{
    Kind, TypedArrayBase, TypedArrayWithBufferWitness, create_typed_array, create_typed_array_on_buffer,
    is_typed_array_out_of_bounds, make_typed_array_with_buffer_witness_record, typed_array_byte_length,
    typed_array_length,
};

/// The TypedArray With Buffer Witness Record of the spec, which C++ calls TypedArrayWithBufferWitness.
#[repr(C)]
pub struct JSTypedArrayWithBufferWitness {
    pub typed_array: *mut JSObject,
    pub cached_buffer_byte_length: JSByteLength,
}

/// # Safety
///
/// `typed_array` must be the address of a live typed array.
unsafe fn typed_array_from_abi(typed_array: *mut JSObject) -> Gc<TypedArrayBase> {
    // SAFETY: The caller guarantees that the pointer is the address of a live object.
    unsafe { cell_of_class_from_abi(typed_array, "TypedArray") }
}

/// # Safety
///
/// `record` must point to a record of a live typed array.
unsafe fn witness_record_from_abi(record: *const JSTypedArrayWithBufferWitness) -> TypedArrayWithBufferWitness {
    assert!(!record.is_null(), "the embedder passes a witness record");
    // SAFETY: The caller passes a valid record.
    let record = unsafe { &*record };
    TypedArrayWithBufferWitness {
        // SAFETY: The caller passes the record of a live typed array.
        object: unsafe { typed_array_from_abi(record.typed_array) },
        cached_buffer_byte_length: byte_length_from_abi(record.cached_buffer_byte_length),
    }
}

/// TypedArray<T>::create(Realm&, u32 length) for the kind: a typed array of `length` zero elements over a new
/// ArrayBuffer of its own, which is the payload of the normal completion. Throws a RangeError when there is not enough
/// memory. Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    kind: u8,
    length: u32,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM and realm.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi(realm)) };
    completion_into_abi(create_typed_array(vm, realm, Kind::from_u8(kind), length))
}

/// TypedArray<T>::create(Realm&, u32 length, ArrayBuffer&) for the kind: a typed array of `length` elements from the
/// start of `buffer`, which must have room for them. Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live, and `buffer` a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_create_on_buffer(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    kind: u8,
    length: u32,
    buffer: *mut JSObject,
) -> *mut JSObject {
    // SAFETY: The caller passes a live VM, realm and buffer.
    let (vm, realm, buffer) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            cell_of_class_from_abi::<ArrayBuffer>(buffer, "ArrayBuffer"),
        )
    };
    let element_size = Kind::from_u8(kind).element_type().size();
    assert!(
        (length as usize)
            .checked_mul(element_size)
            .is_some_and(|byte_length| byte_length <= buffer.byte_length()),
        "the embedder makes a typed array that fits in its buffer"
    );
    object_into_abi(create_typed_array_on_buffer(
        vm,
        realm,
        Kind::from_u8(kind),
        length,
        buffer,
    ))
}

/// TypedArrayBase::create_from_slots(): restores a typed array from its [[ArrayLength]], [[ByteLength]] and
/// [[ByteOffset]], as structured deserialization does. Both lengths are auto, or the byte length is the array length
/// times the element size, and the byte offset is a multiple of the element size. The caller has checked that the view
/// fits in the buffer. Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live, and `buffer` a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_create_from_slots(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    kind: u8,
    buffer: *mut JSObject,
    array_length: JSByteLength,
    byte_length: JSByteLength,
    byte_offset: u32,
) -> *mut JSObject {
    // SAFETY: The caller passes a live VM, realm and buffer.
    let (vm, realm, buffer) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            cell_of_class_from_abi::<ArrayBuffer>(buffer, "ArrayBuffer"),
        )
    };
    object_into_abi(TypedArrayBase::create_from_slots(
        vm,
        realm,
        Kind::from_u8(kind),
        buffer,
        byte_length_from_abi(array_length),
        byte_length_from_abi(byte_length),
        byte_offset,
    ))
}

/// [[ViewedArrayBuffer]]. Main thread only.
///
/// # Safety
///
/// `typed_array` must be a live typed array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_viewed_array_buffer(typed_array: *mut JSObject) -> *mut JSObject {
    // SAFETY: The caller passes a live typed array.
    object_into_abi(unsafe { typed_array_from_abi(typed_array) }.viewed_array_buffer())
}

/// The [[ByteLength]] slot, which is auto for a typed array that tracks a resizable buffer. Use
/// js_typed_array_byte_length_of_witness() for the byte length the spec's operations see. Main thread only.
///
/// # Safety
///
/// `typed_array` must be a live typed array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_byte_length(typed_array: *mut JSObject) -> JSByteLength {
    // SAFETY: The caller passes a live typed array.
    byte_length_into_abi(unsafe { typed_array_from_abi(typed_array) }.byte_length())
}

/// MakeTypedArrayWithBufferWitnessRecord(obj, order), with an order of JS_ARRAY_BUFFER_ORDER_*. Main thread only.
///
/// # Safety
///
/// `typed_array` must be a live typed array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_make_witness_record(
    typed_array: *mut JSObject,
    order: u8,
) -> JSTypedArrayWithBufferWitness {
    // SAFETY: The caller passes a live typed array.
    let typed_array = unsafe { typed_array_from_abi(typed_array) };
    let record = make_typed_array_with_buffer_witness_record(&typed_array, order_from_abi(order));
    JSTypedArrayWithBufferWitness {
        typed_array: object_into_abi(record.object),
        cached_buffer_byte_length: byte_length_into_abi(record.cached_buffer_byte_length),
    }
}

/// IsTypedArrayOutOfBounds(taRecord), which is true for a typed array of a detached buffer. Main thread only.
///
/// # Safety
///
/// `record` must point to a record of a live typed array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_is_out_of_bounds(record: *const JSTypedArrayWithBufferWitness) -> bool {
    // SAFETY: The caller passes a valid record.
    is_typed_array_out_of_bounds(&unsafe { witness_record_from_abi(record) })
}

/// TypedArrayLength(taRecord), of a typed array that is not out of bounds. Main thread only.
///
/// # Safety
///
/// `record` must point to a record of a live typed array that is not out of bounds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_length_of_witness(record: *const JSTypedArrayWithBufferWitness) -> u32 {
    // SAFETY: The caller passes a valid record.
    typed_array_length(&unsafe { witness_record_from_abi(record) })
}

/// TypedArrayByteLength(taRecord), which is 0 for a typed array that is out of bounds. Main thread only.
///
/// # Safety
///
/// `record` must point to a record of a live typed array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_typed_array_byte_length_of_witness(record: *const JSTypedArrayWithBufferWitness) -> u32 {
    // SAFETY: The caller passes a valid record.
    typed_array_byte_length(&unsafe { witness_record_from_abi(record) })
}
