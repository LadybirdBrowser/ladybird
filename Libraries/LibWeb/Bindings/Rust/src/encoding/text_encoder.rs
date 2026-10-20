/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TextEncoder, https://encoding.spec.whatwg.org/#interface-textencoder

use core::mem::MaybeUninit;

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferOrder, ArrayBufferShared, ErrorType, Gc, Trace, TypedArrayBase, TypedArrayKind,
    create_typed_array_on_buffer, is_typed_array_out_of_bounds, make_typed_array_with_buffer_witness_record,
    typed_array_byte_length,
};

use crate::bindings::conversions::UsvString;
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::encoding::utf8::{self, EncodeIntoProgress};

/// A TextEncoder has no state: it always encodes UTF-8.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TextEncoder;

impl Wrappable for TextEncoder {}

/// The dictionary TextEncoderEncodeIntoResult of Encoding/TextEncoder.idl, whose members the bindings convert.
pub(crate) struct TextEncoderEncodeIntoResult {
    pub(crate) read: Option<u64>,
    pub(crate) written: Option<u64>,
}

fn encode_into_result(progress: EncodeIntoProgress) -> TextEncoderEncodeIntoResult {
    TextEncoderEncodeIntoResult {
        read: Some(progress.read as u64),
        written: Some(progress.written as u64),
    }
}

/// Every code unit encodes to at most three bytes, so a source of that many bytes fits a destination without the
/// encoding being measured first.
const MAX_UTF8_BYTES_PER_CODE_UNIT: usize = 3;

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl TextEncoder {
    // https://encoding.spec.whatwg.org/#dom-textencoder
    pub(crate) fn construct(_: &mut CallContext) -> WebIdlResult<Self> {
        // The new TextEncoder() constructor steps are to do nothing.
        Ok(Self)
    }

    // https://encoding.spec.whatwg.org/#dom-textencoder-encoding
    pub(crate) fn encoding(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The encoding getter steps are to return "utf-8".
        Ok(Utf16String::from_ascii(b"utf-8"))
    }

    // https://encoding.spec.whatwg.org/#dom-textencoder-encode
    pub(crate) fn encode(&self, cx: &mut CallContext, input: UsvString) -> WebIdlResult<Gc<TypedArrayBase>> {
        // 1. Convert input to an I/O queue of scalar values.
        // 2. Let output be the I/O queue of bytes « end-of-queue ».
        // 3. While true:
        //     1. Let item be the result of reading from input.
        //     2. Let result be the result of processing an item with item, an instance of the UTF-8 encoder,
        //        input, output, and "fatal".
        //     3. Assert: result is not an error.
        //     4. If result is finished, then convert output into a byte sequence and return a Uint8Array object
        //        wrapping an ArrayBuffer containing output.
        let length = input.utf8_length();
        let array_length = u32::try_from(length)
            .map_err(|_| Exception::RangeError(ErrorType::InvalidLength.message(&[&"typed array"])))?;
        let buffer =
            ArrayBuffer::try_create_written_by(cx.vm(), cx.realm(), length, ArrayBufferShared::No, |output| {
                // SAFETY: The buffer has the length of the encoding.
                unsafe { input.encode_utf8(output) }
            })?;
        Ok(create_typed_array_on_buffer(
            cx.vm(),
            cx.realm(),
            TypedArrayKind::Uint8Array,
            array_length,
            buffer,
        ))
    }

    // https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
    pub(crate) fn encode_into(
        &self,
        _: &mut CallContext,
        source: UsvString,
        destination: Gc<TypedArrayBase>,
    ) -> WebIdlResult<TextEncoderEncodeIntoResult> {
        // AD-HOC: Return early if destination is detached. This is not explicitly handled in the spec, however no
        //         bytes are copied as the destination's size is always zero in this case.
        //         See: https://github.com/whatwg/encoding/issues/324
        let buffer = destination.viewed_array_buffer();
        if buffer.is_detached() {
            return Ok(encode_into_result(EncodeIntoProgress { read: 0, written: 0 }));
        }
        let destination_record = make_typed_array_with_buffer_witness_record(&destination, ArrayBufferOrder::SeqCst);
        if is_typed_array_out_of_bounds(&destination_record) {
            return Ok(encode_into_result(EncodeIntoProgress { read: 0, written: 0 }));
        }
        let destination_byte_length = typed_array_byte_length(&destination_record) as usize;
        if destination_byte_length == 0 {
            return Ok(encode_into_result(EncodeIntoProgress { read: 0, written: 0 }));
        }
        let destination_bytes =
            buffer.with_data_block(|data_block| data_block.data_at(destination.byte_offset() as usize));
        assert!(!destination_bytes.is_null());

        // OPTIMIZATION: Encode the whole source at once into a destination that is not shared when it fits.
        //               Shared destinations stay on the incremental path, which keeps their writes the same.
        if !buffer.is_shared_array_buffer() {
            let fits = source.length_in_code_units() * MAX_UTF8_BYTES_PER_CODE_UNIT <= destination_byte_length
                || source.utf8_length() <= destination_byte_length;
            if fits {
                // SAFETY: The destination views `destination_byte_length` bytes of the buffer from its byte offset,
                //         which nothing else accesses while this runs, as the buffer is not shared, and which the
                //         encoding fits.
                let written = unsafe {
                    let output = ak::slice_from_raw_parts_mut(
                        destination_bytes.cast::<MaybeUninit<u8>>(),
                        destination_byte_length,
                    );
                    source.encode_utf8(output)
                };
                return Ok(encode_into_result(EncodeIntoProgress {
                    read: source.length_in_code_units(),
                    written,
                }));
            }
        }

        // 1. Let read be 0.
        // 2. Let written be 0.
        // 3. Let encoder be an instance of the UTF-8 encoder.
        // 4. Let unused be the I/O queue of scalar values « end-of-queue ».
        // 5. Convert source to an I/O queue of scalar values.
        // 6. While true:
        //     1. Let item be the result of reading from source.
        //     2. Let result be the result of running encoder's handler on unused and item.
        //     3. If result is finished, then break.
        //     4. Otherwise:
        //         1. If destination's byte length − written is greater than or equal to the number of bytes in
        //            result, then:
        //             1. If item is greater than U+FFFF, then increment read by 2.
        //             2. Otherwise, increment read by 1.
        //             3. Write the bytes in result into destination, with startingOffset set to written.
        //             4. Increment written by the number of bytes in result.
        //         2. Otherwise, break.
        // 7. Return «[ "read" → read, "written" → written ]».
        let units = source.units();
        let progress = if buffer.is_shared_array_buffer() {
            // WARNING: See the warning for SharedArrayBuffer objects at
            //          https://encoding.spec.whatwg.org/#sharedarraybuffer-warning. Another thread can access the
            //          bytes at any time, so they are only written through raw copies.
            utf8::encode_into(&units, destination_byte_length, |offset, bytes| {
                // SAFETY: The encoder writes within the destination's byte length.
                unsafe {
                    core::ptr::copy_nonoverlapping(bytes.as_ptr(), destination_bytes.add(offset), bytes.len());
                }
            })
        } else {
            // SAFETY: Nothing else accesses the bytes of a buffer that is not shared while this runs.
            let output = unsafe { ak::slice_from_raw_parts_mut(destination_bytes, destination_byte_length) };
            utf8::encode_into(&units, destination_byte_length, |offset, bytes| {
                output[offset..offset + bytes.len()].copy_from_slice(bytes);
            })
        };
        Ok(encode_into_result(progress))
    }
}
