/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The source code that scripts, modules and functions are compiled from, which embedders share with the runtime by
//! reference counting, as C++ shares RefPtr<SourceCode const>.
//!
//! A JSSourceCode is the SourceCode of an Rc, and every function that hands one out returns the same pointer for the
//! same source code, so its address identifies it. Its reference count is not atomic: only the VM's thread may retain or
//! release it, or read it.

use std::rc::Rc;

use crate::bytecode::executable::Executable;
use crate::embedding::abi_types::{JSOwnedUtf16String, JSSourceCode, owned_utf16_string_from_abi};
use crate::embedding::execution_context::JSExecutionContext;
use crate::layout::execution_context::ExecutionContext;
use crate::source_code::SourceCode;

pub fn source_code_into_abi(source_code: &Rc<SourceCode>) -> *const JSSourceCode {
    Rc::as_ptr(source_code).cast()
}

/// # Safety
///
/// `source_code` must be the SourceCode of a live Rc, such as one the ABI handed out, which stays alive for `'a`.
pub unsafe fn source_code_from_abi<'a>(source_code: *const JSSourceCode) -> &'a SourceCode {
    assert!(!source_code.is_null(), "the embedder passes source code");
    // SAFETY: The caller guarantees that the pointer is that of a live SourceCode.
    unsafe { &*source_code.cast::<SourceCode>() }
}

/// # Safety
///
/// As for source_code_from_abi(). The returned Rc is one more owner of the source code.
pub unsafe fn shared_source_code_from_abi(source_code: *const JSSourceCode) -> Rc<SourceCode> {
    let source_code = source_code.cast::<SourceCode>();
    assert!(!source_code.is_null(), "the embedder passes source code");
    // SAFETY: The caller guarantees that the pointer is that of a live Rc, which then has one more owner.
    unsafe {
        Rc::increment_strong_count(source_code);
        Rc::from_raw(source_code)
    }
}

/// SourceCode::create(filename, code): new source code whose one reference the caller owns and gives up with
/// js_source_code_release(). Adopts both strings, which the caller gives up with AK::Utf16String::into_raw(). Only the
/// VM's thread may call this.
///
/// # Safety
///
/// Both strings must be raw AK::Utf16Strings whose references the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_create(
    filename: JSOwnedUtf16String,
    code: JSOwnedUtf16String,
) -> *const JSSourceCode {
    // SAFETY: The caller transfers both references.
    let (filename, code) = unsafe { (owned_utf16_string_from_abi(filename), owned_utf16_string_from_abi(code)) };
    Rc::into_raw(SourceCode::create(filename, code)).cast()
}

/// Adds a reference to the source code, which the caller gives up with js_source_code_release(). Only the VM's thread
/// may call this.
///
/// # Safety
///
/// `source_code` must be live source code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_retain(source_code: *const JSSourceCode) {
    assert!(!source_code.is_null(), "the embedder passes source code");
    // SAFETY: The caller passes the SourceCode of a live Rc.
    unsafe { Rc::increment_strong_count(source_code.cast::<SourceCode>()) };
}

/// Gives up a reference to the source code that js_source_code_create() or js_source_code_retain() gave the caller.
/// Only the VM's thread may call this.
///
/// # Safety
///
/// `source_code` must be live source code of which the caller owns a reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_release(source_code: *const JSSourceCode) {
    assert!(!source_code.is_null(), "the embedder passes source code");
    // SAFETY: The caller gives up one of its references to the SourceCode of a live Rc.
    unsafe { Rc::decrement_strong_count(source_code.cast::<SourceCode>()) };
}

/// SourceCode::length_in_code_units(). Only the VM's thread may call this.
///
/// # Safety
///
/// `source_code` must be live source code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_length_in_code_units(source_code: *const JSSourceCode) -> usize {
    // SAFETY: The caller passes live source code.
    unsafe { source_code_from_abi(source_code) }.length_in_code_units()
}

/// SourceCode::filename() as C++ returns it, by reference: the address of the source code's AK::Utf16String, which
/// stays where it is, unchanged, for as long as the source code lives. Only the VM's thread may call this.
///
/// # Safety
///
/// `source_code` must be live source code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_filename_address(
    source_code: *const JSSourceCode,
) -> *const JSOwnedUtf16String {
    // SAFETY: The caller passes live source code.
    let source_code = unsafe { source_code_from_abi(source_code) };
    core::ptr::from_ref(source_code.filename()).cast()
}

/// SourceCode::code() as C++ returns it, by reference: the address of the source code's AK::Utf16String, which stays
/// where it is, unchanged, for as long as the source code lives. Only the VM's thread may call this.
///
/// # Safety
///
/// `source_code` must be live source code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_code_address(source_code: *const JSSourceCode) -> *const JSOwnedUtf16String {
    // SAFETY: The caller passes live source code.
    let source_code = unsafe { source_code_from_abi(source_code) };
    core::ptr::from_ref(source_code.code()).cast()
}

/// SourceCode::utf16_data(): the js_source_code_length_in_code_units() UTF-16 code units of the code, widened on the
/// first call for code in the ASCII storage kind. They stay where they are, unchanged, for as long as the source code
/// lives, so a worker thread may read them while the VM's thread keeps the source code alive, as it may parse them with
/// js_compile_parse(). Null for empty code. Only the VM's thread may call this.
///
/// # Safety
///
/// `source_code` must be live source code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_utf16_data(source_code: *const JSSourceCode) -> *const u16 {
    // SAFETY: The caller passes live source code.
    let code_units = unsafe { source_code_from_abi(source_code) }.utf16_code_units();
    if code_units.is_empty() {
        return core::ptr::null();
    }
    code_units.as_ptr()
}

/// ExecutionContext::source_code(): the source code of the bytecode the context runs, or null if it runs none, as for a
/// native function, or if that bytecode has no source code. The context's executable keeps it alive, and a caller that
/// keeps it longer retains it. Only the VM's thread may call this.
///
/// # Safety
///
/// `execution_context` must be a live execution context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_source_code_of_execution_context(
    execution_context: *const JSExecutionContext,
) -> *const JSSourceCode {
    assert!(!execution_context.is_null(), "the embedder passes an execution context");
    // SAFETY: The caller passes a live execution context.
    let execution_context = unsafe { &*execution_context.cast::<ExecutionContext>() };
    let Some(executable) = execution_context.executable.get() else {
        return core::ptr::null();
    };
    Executable::from_head(executable)
        .source_code
        .as_ref()
        .map_or(core::ptr::null(), source_code_into_abi)
}
