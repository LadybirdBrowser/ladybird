/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Miscellaneous abstract operations, https://streams.spec.whatwg.org/#misc-abstract-ops

use core::ffi::c_void;

use libjs_rust::api::{
    ArrayBuffer, Gc, Must, RawNativeFunctionResult, Value, clone_array_buffer, construct, same_value,
};

use crate::bindings::conversions::{ArrayBufferView, BufferSource};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    /// StructuredDeserialize(? StructuredSerialize(value), realm).
    fn web_streams_structured_clone(realm: *mut c_void, value: u64) -> RawNativeFunctionResult;
}

// https://streams.spec.whatwg.org/#can-transfer-array-buffer
pub(crate) fn can_transfer_array_buffer(array_buffer: &ArrayBuffer) -> bool {
    // 1. Assert: O is an Object.
    // 2. Assert: O has an [[ArrayBufferData]] internal slot.

    // 3. If ! IsDetachedBuffer(O) is true, return false.
    if array_buffer.is_detached() {
        return false;
    }

    // 4. If SameValue(O.[[ArrayBufferDetachKey]], undefined) is false, return false.
    if !same_value(array_buffer.detach_key(), Value::UNDEFINED) {
        return false;
    }

    // 5. Return true.
    true
}

// https://streams.spec.whatwg.org/#is-non-negative-number
pub(crate) fn is_non_negative_number(value: Value) -> bool {
    // 1. If v is not a Number, return false.
    if !value.is_number() {
        return false;
    }

    // 2. If v is NaN, return false.
    if value.is_nan() {
        return false;
    }

    // 3. If v < 0, return false.
    if value.as_f64() < 0.0 {
        return false;
    }

    // 4. Return true.
    true
}

// https://streams.spec.whatwg.org/#transfer-array-buffer
pub(crate) fn transfer_array_buffer(cx: &CallContext, buffer: Gc<ArrayBuffer>) -> WebIdlResult<Gc<ArrayBuffer>> {
    // 1. Assert: ! IsDetachedBuffer(O) is false.
    assert!(!buffer.is_detached());

    // 2. Let arrayBufferData be O.[[ArrayBufferData]].
    // 3. Let arrayBufferByteLength be O.[[ArrayBufferByteLength]].
    // 4. Perform ? DetachArrayBuffer(O).
    // NB: The data block is taken from the buffer as it is detached, so the transfer copies nothing.
    let block = buffer.detach_and_take_data_block(cx.vm())?;

    // 5. Return a new ArrayBuffer object, created in the current Realm, whose [[ArrayBufferData]] internal slot value
    //    is arrayBufferData and whose [[ArrayBufferByteLength]] internal slot value is arrayBufferByteLength.
    Ok(ArrayBuffer::create_from_data_block(cx.vm(), cx.realm(), block))
}

// https://streams.spec.whatwg.org/#abstract-opdef-cloneasuint8array
pub(crate) fn clone_as_uint8_array(cx: &CallContext, view: ArrayBufferView) -> WebIdlResult<Value> {
    let vm = cx.vm();
    let extent = BufferSource::from(view).extent();

    // 1. Assert: O is an Object.
    // 2. Assert: O has an [[ViewedArrayBuffer]] internal slot.

    // 3. Assert: ! IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is false.
    assert!(!extent.buffer.is_detached());

    // 4. Let buffer be ? CloneArrayBuffer(O.[[ViewedArrayBuffer]], O.[[ByteOffset]], O.[[ByteLength]], %ArrayBuffer%).
    let buffer = clone_array_buffer(vm, extent.buffer, extent.byte_offset, extent.byte_length)?;

    // 5. Let array be ! Construct(%Uint8Array%, « buffer »).
    let array = construct(
        vm,
        cx.realm().intrinsics().uint8_array_constructor(vm),
        &[Value::from_object(buffer)],
        None,
    )
    .must();

    // 6. Return array.
    Ok(Value::from_object(array))
}

// https://streams.spec.whatwg.org/#abstract-opdef-structuredclone
pub(crate) fn structured_clone(cx: &CallContext, value: Value) -> WebIdlResult<Value> {
    // 1. Let serialized be ? StructuredSerialize(v).
    // 2. Return ? StructuredDeserialize(serialized, the current Realm).
    // SAFETY: The realm and the value are live.
    Ok(unsafe { web_streams_structured_clone(cx.realm().as_ptr().cast(), value.to_raw()) }.into_completion()?)
}

// https://streams.spec.whatwg.org/#abstract-opdef-cancopydatablockbytes
pub(crate) fn can_copy_data_block_bytes_buffer(
    to_buffer: Gc<ArrayBuffer>,
    to_index: u64,
    from_buffer: Gc<ArrayBuffer>,
    from_index: u64,
    count: u64,
) -> bool {
    // 1. Assert: toBuffer is an Object.
    // 2. Assert: toBuffer has an [[ArrayBufferData]] internal slot.
    // 3. Assert: fromBuffer is an Object.
    // 4. Assert: fromBuffer has an [[ArrayBufferData]] internal slot.

    // 5. If toBuffer is fromBuffer, return false.
    if to_buffer == from_buffer {
        return false;
    }

    // 6. If ! IsDetachedBuffer(toBuffer) is true, return false.
    if to_buffer.is_detached() {
        return false;
    }

    // 7. If ! IsDetachedBuffer(fromBuffer) is true, return false.
    if from_buffer.is_detached() {
        return false;
    }

    // 8. If toIndex + count > toBuffer.[[ArrayBufferByteLength]], return false.
    if to_index + count > to_buffer.byte_length() as u64 {
        return false;
    }

    // 9. If fromIndex + count > fromBuffer.[[ArrayBufferByteLength]], return false.
    if from_index + count > from_buffer.byte_length() as u64 {
        return false;
    }

    // 10. Return true.
    true
}
