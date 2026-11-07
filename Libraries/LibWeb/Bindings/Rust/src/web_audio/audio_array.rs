/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_rust::api::{
    ArrayBufferOrder, TypedArrayBase, is_typed_array_out_of_bounds, make_typed_array_with_buffer_witness_record,
    typed_array_length,
};

// Utilities for moving sample data in and out of the Float32Arrays exposed by the Web Audio API. A Float32Array's
// contents can change from JavaScript at any time, so audio data destined for the rendering thread is always snapshotted
// into an owned buffer first.

// The current element length of a Float32Array, or 0 if it is out of bounds of its backing ArrayBuffer.
pub(crate) fn float32_array_length(array: &TypedArrayBase) -> usize {
    let record = make_typed_array_with_buffer_witness_record(array, ArrayBufferOrder::SeqCst);
    if is_typed_array_out_of_bounds(&record) {
        return 0;
    }
    typed_array_length(&record)
}

// Snapshots the live contents of a Float32Array into an owned buffer.
pub(crate) fn copy_float32_array(array: &TypedArrayBase) -> Vec<f32> {
    let length = float32_array_length(array);
    let mut values = vec![0.; length];
    let bytes = unsafe { ak::slice_from_raw_parts_mut(values.as_mut_ptr().cast::<u8>(), length * size_of::<f32>()) };
    array.viewed_array_buffer().copy_to(array.byte_offset(), bytes);
    values
}

// Overwrites the leading elements of a Float32Array with the given samples.
pub(crate) fn overwrite_float32_array(array: &TypedArrayBase, values: &[f32]) {
    let bytes = unsafe { ak::slice_from_raw_parts(values.as_ptr().cast::<u8>(), size_of_val(values)) };
    array.viewed_array_buffer().overwrite(array.byte_offset(), bytes);
}
