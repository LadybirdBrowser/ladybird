/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use encoding_rs::{Encoding, UTF_8};

use crate::ffi::url::{RustUrlByteSlice, RustUrlInput};
use crate::rust_panic::abort_on_panic;
use crate::url::form_urlencoded;

/// A name-value tuple borrowed across the FFI boundary.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiFormTuple {
    pub name: RustUrlInput,
    pub value: RustUrlInput,
}

pub type FfiFormTupleFn = unsafe extern "C" fn(*mut c_void, RustUrlByteSlice, RustUrlByteSlice);
pub type FfiStringResultFn = unsafe extern "C" fn(*mut c_void, RustUrlByteSlice);

/// # Safety
/// `input` must borrow its bytes for the duration of this call. `on_tuple` is called once per name-value tuple, in
/// order, with UTF-8 strings that it borrows for the duration of that call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_form_urlencoded_parse(
    input: RustUrlByteSlice,
    ctx: *mut c_void,
    on_tuple: FfiFormTupleFn,
) {
    abort_on_panic(|| {
        let input = if input.data.is_null() {
            &[][..]
        } else {
            unsafe { ak::slice_from_raw_parts(input.data, input.length) }
        };
        for (name, value) in form_urlencoded::parse(input) {
            // SAFETY: on_tuple is a valid function pointer; ctx is caller-provided.
            unsafe {
                on_tuple(
                    ctx,
                    RustUrlByteSlice::from_str(&name),
                    RustUrlByteSlice::from_str(&value),
                )
            };
        }
    })
}

/// # Safety
/// `tuples` must point to `tuple_count` tuples whose strings, and `encoding_label`, are borrowed for the duration of
/// this call. `on_complete` is called exactly once, with the ASCII serialization, which it borrows for the duration of
/// that call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_form_urlencoded_serialize(
    tuples: *const FfiFormTuple,
    tuple_count: usize,
    encoding_label: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiStringResultFn,
) {
    abort_on_panic(|| {
        let tuples = if tuples.is_null() {
            &[][..]
        } else {
            unsafe { ak::slice_from_raw_parts(tuples, tuple_count) }
        };
        let encoding = unsafe { encoding_label.borrow() }
            .and_then(|label| Encoding::for_label(label.as_bytes()))
            .unwrap_or(UTF_8);
        let serialization = form_urlencoded::serialize(
            tuples
                .iter()
                .map(|tuple| unsafe { (tuple.name.borrow(), tuple.value.borrow()) }),
            encoding,
        );
        // SAFETY: on_complete is a valid function pointer; ctx is caller-provided.
        unsafe { on_complete(ctx, RustUrlByteSlice::from_str(&serialization)) };
    })
}
