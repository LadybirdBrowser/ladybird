/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Classic scripts: Script Records, which an embedder parses with a [[HostDefined]] of its own and runs with
//! ScriptEvaluation. compile.rs creates them from programs parsed and compiled on other threads.

use core::ffi::c_void;
use core::ptr::NonNull;

use crate::embedding::abi_types::{
    CellAbi, JSOwnedUtf16String, JSRealm, JSUtf16View, cell_from_abi, cell_into_abi, completion_into_abi,
    optional_cell_from_abi, owned_utf16_string_into_abi, vm_from_abi,
};
use crate::embedding::environment::JSEnvironment;
use crate::gc::foreign::ForeignCellSlot;
use crate::layout::host_class::{JSCompletion, JSVM};
use crate::parser_error::ParserError;
use crate::script::Script;

/// A Script Record, which C only ever sees behind a pointer. It is the cell that a JSScriptOrModule with the script tag
/// holds.
pub struct JSScript {
    _opaque: [u8; 0],
}

impl CellAbi for JSScript {
    type Cell = Script;
}

/// Where the runtime reports the syntax errors of a source text, in source order. `append` receives each error's
/// message, which it adopts with AK::Utf16String::adopt_raw(), and the line and column it points at, as C++ ParserError
/// holds them. The runtime calls it on the thread that asked for the errors, holding nothing of the VM, so on the VM's
/// thread it may run JavaScript.
#[repr(C)]
pub struct JSParserErrorSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, message: JSOwnedUtf16String, line: u32, column: u32)>,
}

/// Hands every error to the embedder's sink, or drops them if it passed none.
///
/// # Safety
///
/// `sink` must be null or point to a sink with an append function.
pub unsafe fn append_to_parser_error_sink(sink: *const JSParserErrorSink, errors: &[ParserError]) {
    // SAFETY: The caller passes null or a valid sink.
    let Some(sink) = (unsafe { sink.as_ref() }) else {
        return;
    };
    let append = sink.append.expect("a parser error sink has an append function");
    for error in errors {
        let message = owned_utf16_string_into_abi(ak::Utf16String::from_utf8(&error.message));
        // SAFETY: The embedder's sink takes the errors with the context it came with, and adopts each message.
        unsafe { append(sink.context, message, error.line, error.column) };
    }
}

/// A slot holding the embedder's [[HostDefined]] cell, or an empty one for null.
///
/// # Safety
///
/// `host_defined` must be null or the address of a live cell of the VM's heap.
pub unsafe fn host_defined_slot_from_abi(host_defined: *mut c_void) -> ForeignCellSlot {
    let slot = ForeignCellSlot::empty();
    // SAFETY: The caller passes null or a live cell, which the slot keeps alive from now on.
    unsafe { slot.set(NonNull::new(host_defined)) };
    slot
}

/// Script::parse(source_text, realm, filename, display_filename, host_defined, line_number_offset): ParseScript of
/// `source` in `realm`, as C++ runs it for a host. The script's code reports `display_filename`, or `filename` if that
/// is empty, in its stack frames and errors, and counts its lines from `line_number_offset`. Its dynamic imports resolve
/// against `filename`. `host_defined` is null or one of the embedder's GC cells, which the script keeps alive as its
/// [[HostDefined]]. Returns the script, which the caller keeps alive, or null after appending the syntax errors to
/// `errors` (which may be null). Borrows the views. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` and `realm` must be live, the views valid, `host_defined` null or a live cell, and `errors` null or a valid
/// sink.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments, reason = "C++ Script::parse takes all of these")]
pub unsafe extern "C" fn js_script_parse(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    source: JSUtf16View,
    filename: JSUtf16View,
    display_filename: JSUtf16View,
    host_defined: *mut c_void,
    line_number_offset: usize,
    errors: *const JSParserErrorSink,
) -> *mut JSScript {
    // SAFETY: The caller passes a live VM, realm and host-defined cell, and valid views.
    let (vm, realm, source, filename, display_filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            source.as_view(),
            filename.as_view(),
            display_filename.as_view(),
            host_defined_slot_from_abi(host_defined),
        )
    };
    let source: Vec<u16> = source.code_units().collect();
    match Script::parse_with_host_defined(
        vm,
        &source,
        realm,
        &filename.to_utf8(),
        display_filename.to_utf16_string(),
        host_defined,
        line_number_offset,
    ) {
        Ok(script) => cell_into_abi(script),
        Err(parser_errors) => {
            // SAFETY: The caller passes null or a valid sink.
            unsafe { append_to_parser_error_sink(errors, &parser_errors) };
            core::ptr::null_mut()
        }
    }
}

/// VM::run(Script&, lexical_environment_override): ScriptEvaluation of the script, whose execution context gets the
/// override as its LexicalEnvironment unless that is null. As in C++, the override only reaches what resolves through
/// the LexicalEnvironment, such as typeof and direct eval: identifiers that the script compiles to global variable
/// accesses still read and write the global environment directly. Like every script, it runs on top of the execution
/// context stack, which must not be empty, as a host runs it in a context of the script's realm. Only the VM's thread
/// may call this.
///
/// # Safety
///
/// `vm` and `script` must be live, and `lexical_environment_override` null or a live environment.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_script_run(
    vm: *mut JSVM,
    script: *mut JSScript,
    lexical_environment_override: *mut JSEnvironment,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM, script and environment, if any.
    let (vm, script, lexical_environment_override) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(script),
            optional_cell_from_abi(lexical_environment_override),
        )
    };
    completion_into_abi(vm.run_script(script, lexical_environment_override))
}

/// Script::host_defined(): the [[HostDefined]] cell the script was created with, or null for none. Only the VM's thread
/// may call this.
///
/// # Safety
///
/// `script` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_script_host_defined(script: *mut JSScript) -> *mut c_void {
    // SAFETY: The caller passes a live script.
    let script = unsafe { cell_from_abi(script) };
    script.host_defined().map_or(core::ptr::null_mut(), NonNull::as_ptr)
}
