/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBufferOrder, ErrorType, Throw, Trace, TypedArrayKind, Value, Vm, is_typed_array_out_of_bounds,
    make_typed_array_with_buffer_witness_record, typed_array_byte_length,
};

use crate::bindings::conversions::{ArrayBufferView, BufferSource};
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::crypto::subtle_crypto::SubtleCrypto;
use crate::webidl::dom_exception::DOMExceptionFields;
use crate::webidl::quota_exceeded_error::{self, QuotaExceededErrorFields};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Crypto {
    subtle: Ref<SubtleCrypto>,
}

impl Wrappable for Crypto {}

#[allow(
    clippy::unnecessary_wraps,
    reason = "the generated bindings can propagate exceptions from every getter"
)]
impl Crypto {
    pub(crate) fn subtle(&self, _: &mut CallContext) -> WebIdlResult<Ref<SubtleCrypto>> {
        Ok(self.subtle)
    }

    // https://w3c.github.io/webcrypto/#dfn-Crypto-method-getRandomValues
    pub(crate) fn get_random_values(
        &self,
        cx: &mut CallContext,
        array: ArrayBufferView,
    ) -> WebIdlResult<ArrayBufferView> {
        let array_view = BufferSource::from(array);

        // 1. If array is not an Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, BigInt64Array, or BigUint64Array, then throw a TypeMismatchError and terminate the algorithm.
        let type_mismatch_error = || {
            Exception::Dom("TypeMismatchError", Utf16String::from_ascii(b"array must be one of Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, BigInt64Array, or BigUint64Array"))
        };
        let ArrayBufferView::TypedArray(typed_array) = array else {
            return Err(type_mismatch_error());
        };

        use TypedArrayKind::{
            BigInt64Array, BigUint64Array, Int8Array, Int16Array, Int32Array, Uint8Array, Uint8ClampedArray,
            Uint16Array, Uint32Array,
        };
        if !matches!(
            typed_array.kind(),
            Int8Array
                | Uint8Array
                | Uint8ClampedArray
                | Int16Array
                | Uint16Array
                | Int32Array
                | Uint32Array
                | BigInt64Array
                | BigUint64Array
        ) {
            return Err(type_mismatch_error());
        }

        let typed_array_record = make_typed_array_with_buffer_witness_record(&typed_array, ArrayBufferOrder::SeqCst);

        // IMPLEMENTATION DEFINED: If the viewed array buffer is out-of-bounds, throw a InvalidStateError and terminate the algorithm.
        if is_typed_array_out_of_bounds(&typed_array_record) {
            return Err(Exception::Dom(
                "InvalidStateError",
                ErrorType::BufferOutOfBounds.message(&[&"TypedArray"]),
            ));
        }

        // 2. Let byteLength be the byte length of array.
        // 3. If byteLength is greater than 65536, throw a QuotaExceededError and terminate the algorithm.
        if typed_array_byte_length(&typed_array_record) > 65536 {
            let fields = QuotaExceededErrorFields {
                dom_exception: DOMExceptionFields {
                    name: Utf16String::from_ascii(b"QuotaExceededError"),
                    message: Utf16String::from_ascii(b"array's byteLength may not be greater than 65536"),
                },
                quota: None,
                requested: None,
            };
            return Err(Throw::new(Value::from_object(quota_exceeded_error::create(cx.realm(), &fields))).into());
        }

        // 4. Let bytes be a byte sequence of length byteLength.
        let extent = array_view.extent();
        let mut random_bytes = vec![0; extent.byte_length];
        // 5. Fill bytes with cryptographically secure random bytes.
        libcrypto_rust::fill_with_random(&mut random_bytes);
        // 6. Write bytes into array.
        extent.buffer.overwrite(extent.byte_offset, &random_bytes);

        // 7. Return array.
        Ok(array)
    }

    // https://w3c.github.io/webcrypto/#dfn-Crypto-method-randomUUID
    pub(crate) fn random_uuid(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(generate_random_uuid())
    }
}

// https://w3c.github.io/webcrypto/#dfn-generate-a-random-uuid
fn generate_random_uuid() -> Utf16String {
    // 1. Let bytes be a byte sequence of length 16.
    // 2. Fill bytes with cryptographically secure random bytes.
    // 3. Set the 4 most significant bits of bytes[6], which represent the UUID version, to 0100.
    // 4. Set the 2 most significant bits of bytes[8], which represent the UUID variant, to 10.
    // 5. Return the string concatenation of «
    //    hexadecimal representation of bytes[0], hexadecimal representation of bytes[1], hexadecimal representation of
    //    bytes[2], hexadecimal representation of bytes[3],
    //    "-",
    //    hexadecimal representation of bytes[4], hexadecimal representation of bytes[5],
    //    "-",
    //    hexadecimal representation of bytes[6], hexadecimal representation of bytes[7],
    //    "-",
    //    hexadecimal representation of bytes[8], hexadecimal representation of bytes[9],
    //    "-",
    //    hexadecimal representation of bytes[10], hexadecimal representation of bytes[11], hexadecimal representation
    //    of bytes[12], hexadecimal representation of bytes[13], hexadecimal representation of bytes[14], hexadecimal
    //    representation of bytes[15]
    //    ».
    // NB: The native service performs these steps.
    libcrypto_rust::generate_random_uuid()
}

/// # Safety
///
/// The VM must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_crypto_create(vm: *const c_void) -> *mut c_void {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(allocate(vm, |_| Crypto {
        subtle: allocate(vm, |_| SubtleCrypto),
    }))
}
