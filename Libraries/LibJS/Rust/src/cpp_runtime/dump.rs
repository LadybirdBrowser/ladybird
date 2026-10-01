/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use super::rust_panic::abort_on_panic;
use crate::bytecode::dump::FFIBytecodeDumpCallbacks;
use crate::bytecode::dump::FFIBytecodeDumpMetadata;
use crate::bytecode::dump::FFIDumpExceptionHandler;
use crate::bytecode::dump::count_basic_blocks;
use crate::bytecode::dump::dump_bytecode;

/// Count basic blocks in a validated bytecode instruction stream.
///
/// # Safety
/// `bytecode_ptr` must point to `bytecode_len` bytes of validated bytecode, or
/// be null when `bytecode_len` is zero. `exception_handlers` must point to
/// `exception_handler_count` valid entries, or be null when the count is zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_count_basic_blocks(
    bytecode_ptr: *const u8,
    bytecode_len: usize,
    exception_handlers: *const FFIDumpExceptionHandler,
    exception_handler_count: usize,
) -> usize {
    abort_on_panic(|| unsafe {
        let bytecode = if bytecode_len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(bytecode_ptr, bytecode_len)
        };
        let exception_handlers = if exception_handler_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(exception_handlers, exception_handler_count)
        };
        count_basic_blocks(bytecode, exception_handlers)
    })
}

/// Dump a validated bytecode instruction stream through C++ formatting callbacks.
///
/// # Safety
/// `bytecode_ptr` must point to `bytecode_len` bytes of validated bytecode, or
/// be null when `bytecode_len` is zero. `exception_handlers` must point to
/// `exception_handler_count` valid entries, or be null when the count is zero.
/// `metadata` and `callbacks` must point to valid structs, and every callback
/// must remain callable for the duration of this function. `metadata.constants`
/// must point to `metadata.constant_count` encoded Values, or be null when the
/// count is zero. When `metadata.dump_interpreter` is true,
/// `metadata.interpreter_handler_ranges` must point to 256 valid entries whose
/// pointers delimit readable native code. `ctx` is passed through to callbacks
/// and must remain valid for their requirements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dump_bytecode(
    bytecode_ptr: *const u8,
    bytecode_len: usize,
    exception_handlers: *const FFIDumpExceptionHandler,
    exception_handler_count: usize,
    metadata: *const FFIBytecodeDumpMetadata,
    ctx: *mut c_void,
    callbacks: *const FFIBytecodeDumpCallbacks,
) {
    abort_on_panic(|| unsafe {
        let bytecode = if bytecode_len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(bytecode_ptr, bytecode_len)
        };
        let exception_handlers = if exception_handler_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(exception_handlers, exception_handler_count)
        };
        let metadata = metadata.as_ref().expect("rust_dump_bytecode metadata must not be null");
        let constants = if metadata.constant_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(metadata.constants, metadata.constant_count)
        };
        let callbacks = callbacks
            .as_ref()
            .expect("rust_dump_bytecode callbacks must not be null");
        dump_bytecode(bytecode, exception_handlers, metadata, constants, ctx, callbacks);
    });
}
