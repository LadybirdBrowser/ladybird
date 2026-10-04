/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Errors: creating and throwing them with messages the embedder formats, and the [[ErrorData]] of objects. Every
//! function runs on the thread that owns the VM.

use core::ffi::c_void;

use crate::embedding::abi_types::{
    JSErrorData, JSErrorDataCell, JSErrorKind, JSOwnedUtf16String, JSRealm, JSUtf16View, cell_from_abi, cell_into_abi,
    completion_into_abi, error_data_from_abi, error_data_into_abi, error_kind_from_abi, optional_cell_from_abi,
    optional_cell_into_abi, owned_utf16_string_from_abi, owned_utf16_string_into_abi, value_from_abi, vm_from_abi,
};
use crate::gc::class::{Class, GcCell};
use crate::interpreter::vm::TypeErrorRealmOverride;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue};
use crate::layout::object::Object;
use crate::runtime::error::Error;
use crate::runtime::error_data::{CompactTraceback, ErrorData, ErrorDataCell};
use crate::utf16::Utf16View;

/// The error data that the embedder passes as a JSErrorData: an address js_error_data_of gave it, or a JSErrorDataCell
/// itself, which is where a pointer to the C++ ErrorDataCell points as a pointer to the ErrorData it derives from.
///
/// # Safety
///
/// `error_data` must be the error data or the error data cell of a live cell.
unsafe fn error_data_or_error_data_cell_from_abi<'cell>(error_data: *const JSErrorData) -> &'cell ErrorData {
    assert!(!error_data.is_null(), "the embedder passes error data");
    // A cell starts with its class. Error data starts with a part of its traceback's Vec or a cell pointer, neither
    // of which is ever the address of a class.
    // SAFETY: Both start with an initialized word.
    let first_word = unsafe { error_data.cast::<*const Class>().read_unaligned() };
    if core::ptr::eq(first_word, ErrorDataCell::CLASS) {
        // SAFETY: The pointer is the address of a live error data cell.
        return unsafe { &*error_data.cast::<ErrorDataCell>() }.error_data();
    }
    // SAFETY: The caller passes error data of a live cell.
    unsafe { error_data_from_abi(error_data) }
}

/// The error data that the error_data hook of a host class returned for `object`: error data or an error data cell
/// as js_error_data_stack_string takes it, or null for none.
///
/// # Safety
///
/// `error_data` must be null or the error data or error data cell of a cell that `object` keeps alive.
pub unsafe fn error_data_from_host_hook(_object: &Object, error_data: *mut c_void) -> Option<&ErrorData> {
    // SAFETY: The caller guarantees that the object keeps the error data alive, as long as the object itself.
    (!error_data.is_null()).then(|| unsafe { error_data_or_error_data_cell_from_abi(error_data.cast_const().cast()) })
}

/// Throws a new error of `kind` whose message is a copy of the code units `message` views, created the way the
/// runtime creates the errors it throws: in the current realm, or for a TypeError in the realm a
/// js_error_type_error_realm_scope_enter overrides it with, and with the current call stack as its error data. Call on
/// the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, with a realm on its execution context stack, and `message` a view of code units
/// that stay unchanged during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_throw(vm: *mut JSVM, kind: JSErrorKind, message: JSUtf16View) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a view of code units that stay unchanged during the call.
    let message = unsafe { message.as_view() }.to_utf16_string();
    completion_into_abi::<()>(vm.throw_completion_with_utf16_message(error_kind_from_abi(kind), message))
}

/// js_error_throw with a message the caller gives up, whose storage the error's message adopts. Call on the VM's
/// thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, with a realm on its execution context stack, and `message` an AK::Utf16String
/// whose reference the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_throw_with_owned_message(
    vm: *mut JSVM,
    kind: JSErrorKind,
    message: JSOwnedUtf16String,
) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller gives up its reference to the message.
    let message = unsafe { owned_utf16_string_from_abi(message) };
    completion_into_abi::<()>(vm.throw_completion_with_utf16_message(error_kind_from_abi(kind), message))
}

/// A new error of `kind` in `realm`, without a message, whose error data is the current call stack. Call on the VM's
/// thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, and `realm` a realm of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_create(vm: *mut JSVM, realm: *mut JSRealm, kind: JSErrorKind) -> *mut JSObject {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a realm of the VM.
    let realm = unsafe { cell_from_abi(realm) };
    cell_into_abi(error_kind_from_abi(kind).create_without_message(vm, realm).upcast())
}

/// A new Error, without a message, whose prototype is `prototype` rather than an intrinsic one, for the error classes
/// an embedder defines, such as WebAssembly.CompileError. It is allocated in `realm`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `realm` a realm of it, and `prototype` an object of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_create_with_prototype(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    prototype: *mut JSObject,
) -> *mut JSObject {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a realm and an object of the VM.
    let (realm, prototype) = unsafe { (cell_from_abi(realm), cell_from_abi(prototype)) };
    let error = realm.create_object(vm, Error::new(vm, Error::CLASS, prototype));
    cell_into_abi(error.upcast())
}

/// # Safety
///
/// `error` must be an Error of the embedder's VM.
unsafe fn error_from_abi(error: *mut JSObject) -> Gc<Error> {
    // SAFETY: The caller passes an object of the VM.
    let object = unsafe { cell_from_abi(error) };
    object.downcast::<Error>().expect("the embedder passes an Error")
}

/// Defines the "message" of `error` as a copy of the code units `message` views, like the Error constructors do. Call
/// on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `error` an Error of it, and `message` a view of code units that stay unchanged
/// during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_set_message(vm: *mut JSVM, error: *mut JSObject, message: JSUtf16View) {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a view of code units that stay unchanged during the call.
    let message = unsafe { message.as_view() }.to_utf16_string();
    // SAFETY: The caller passes an Error of the VM.
    unsafe { error_from_abi(error) }.set_message(vm, message);
}

/// js_error_set_message with a message the caller gives up, whose storage the message adopts. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `error` an Error of it, and `message` an AK::Utf16String whose reference the caller
/// gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_set_owned_message(vm: *mut JSVM, error: *mut JSObject, message: JSOwnedUtf16String) {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller gives up its reference to the message.
    let message = unsafe { owned_utf16_string_from_abi(message) };
    // SAFETY: The caller passes an Error of the VM.
    unsafe { error_from_abi(error) }.set_message(vm, message);
}

/// InstallErrorCause(error, options), which can throw while reading "cause" from `options`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `error` an Error of it, and `options` a value of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_install_error_cause(
    vm: *mut JSVM,
    error: *mut JSObject,
    options: JSValue,
) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes an Error of the VM.
    let result = unsafe { error_from_abi(error) }.install_error_cause(vm, value_from_abi(options));
    completion_into_abi(result)
}

/// The error data of `object`, which an Error has and a host object may have through its class's error_data hook, or
/// null. It stays valid for as long as `object` lives. Call on the VM's thread.
///
/// # Safety
///
/// `object` must be an object of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_data_of(object: *mut JSObject) -> *const JSErrorData {
    // SAFETY: The caller passes an object of the VM.
    let object = unsafe { cell_from_abi(object) };
    object.error_data().map_or(core::ptr::null(), error_data_into_abi)
}

/// The stack of `error_data` as Error.prototype.stack shows it after the name and message, one "    at" line per frame
/// but the outermost, which the caller owns. With `compact` set, more than five consecutive frames of the same function
/// show as one with a count. Like the other functions that read error data, it also takes the JSErrorDataCell that
/// holds it. Call on the VM's thread.
///
/// # Safety
///
/// `error_data` must be error data, or an error data cell, of a live cell of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_data_stack_string(
    error_data: *const JSErrorData,
    compact: bool,
) -> JSOwnedUtf16String {
    // SAFETY: The caller passes error data of a live cell.
    let error_data = unsafe { error_data_or_error_data_cell_from_abi(error_data) };
    let compact = if compact {
        CompactTraceback::Yes
    } else {
        CompactTraceback::No
    };
    owned_utf16_string_into_abi(error_data.stack_string(compact))
}

/// How many frames the call stack of `error_data` has, the outermost execution context included. Call on the VM's
/// thread.
///
/// # Safety
///
/// `error_data` must be error data, or an error data cell, of a live cell of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_data_traceback_length(error_data: *const JSErrorData) -> usize {
    // SAFETY: The caller passes error data of a live cell.
    unsafe { error_data_or_error_data_cell_from_abi(error_data) }
        .traceback()
        .len()
}

/// A frame of the call stack of an error, as C++ TracebackFrame: the name of the frame's function, and where in its
/// source the frame was, which is an empty filename at line and column 0 without a source range. The views stay valid
/// for as long as the error data does.
#[repr(C)]
pub struct JSTracebackFrame {
    pub function_name: JSUtf16View,
    pub filename: JSUtf16View,
    pub line: u32,
    pub column: u32,
    pub has_source_range: bool,
}

/// Writes frame `index` of the call stack of `error_data`, counting from the innermost, to `out`. Call on the VM's
/// thread.
///
/// # Safety
///
/// `error_data` must be error data, or an error data cell, of a live cell of the embedder's VM, `index` less than its
/// traceback length, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_data_traceback_frame(
    error_data: *const JSErrorData,
    index: usize,
    out: *mut JSTracebackFrame,
) {
    // SAFETY: The caller passes error data of a live cell.
    let error_data = unsafe { error_data_or_error_data_cell_from_abi(error_data) };
    let frame = &error_data.traceback()[index];
    let (filename, line, column) = frame.source_position();
    assert!(!out.is_null(), "the embedder passes an out parameter");
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe {
        out.write(JSTracebackFrame {
            function_name: JSUtf16View::of(Utf16View::of_string(&frame.function_name)),
            filename: JSUtf16View::of(filename),
            line,
            column,
            has_source_range: frame.cached_source_range.is_some(),
        });
    }
}

/// ErrorDataCell::capture(vm): error data with the current call stack, for a host object that is not an Error, such
/// as a DOMException, to keep alive and return from its class's error_data hook. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_data_cell_capture(vm: *mut JSVM) -> *mut JSErrorDataCell {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    cell_into_abi(ErrorDataCell::capture(vm))
}

/// The TypeError realm override that js_error_type_error_realm_scope_enter replaced, which
/// js_error_type_error_realm_scope_exit puts back. `previous_realm` is null for no override.
#[repr(C)]
pub struct JSTypeErrorRealmScope {
    pub previous_realm: *mut JSRealm,
    pub previous_depth: usize,
}

/// VM::TypeErrorRealmScope: has TypeErrors thrown at the current execution context stack depth created in `realm`,
/// until js_error_type_error_realm_scope_exit with the result. Callees that push execution contexts are unaffected.
/// Scopes nest, and must exit in the reverse order they entered. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, and `realm` a realm of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_type_error_realm_scope_enter(
    vm: *mut JSVM,
    realm: *mut JSRealm,
) -> JSTypeErrorRealmScope {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a realm of the VM.
    let previous = vm.override_type_error_realm(unsafe { cell_from_abi(realm) });
    JSTypeErrorRealmScope {
        previous_realm: optional_cell_into_abi(previous.realm),
        previous_depth: previous.depth,
    }
}

/// Ends the TypeError realm scope that js_error_type_error_realm_scope_enter returned `scope` for. Call on the VM's
/// thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, and `scope` the result of its innermost js_error_type_error_realm_scope_enter that
/// has not exited.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_error_type_error_realm_scope_exit(vm: *mut JSVM, scope: JSTypeErrorRealmScope) {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.restore_type_error_realm_override(TypeErrorRealmOverride {
        // SAFETY: The scope holds the realm that was overriding before, which its embedder kept alive.
        realm: unsafe { optional_cell_from_abi(scope.previous_realm) },
        depth: scope.previous_depth,
    });
}
