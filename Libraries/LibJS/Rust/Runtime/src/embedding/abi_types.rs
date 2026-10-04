/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The types the ABI passes that LibJS/HostObjectABI.h does not define, and conversions between the C types and the
//! runtime's own.
//!
//! The conventions every area of the ABI follows:
//! - Cells cross as pointers to the opaque C type of their kind, such as JSObject or JSPrimitiveString, whose address
//!   is the cell's. A pointer the runtime returns stays valid while something keeps the cell alive, which for a pointer
//!   on the embedder's stack is conservative stack scanning, as for any engine pointer.
//! - Operations that can throw return a JSCompletion, as LibJS/HostObjectABI.h describes it, whichever side runs the
//!   operation. The payload of a normal completion is the result when that is a JSValue, a bool as 0 or 1, one of the
//!   enumerators the ABI defines, such as JS_HANDLED_BY_HOST_HANDLED, or a pointer, with null standing for none, and 0
//!   for an operation without a result. Any other result, such as a number, a property key or an owned string, goes to
//!   an out parameter, which a throw leaves untouched. The payload of a throw completion is the thrown value.
//!   completion_into_abi() and completion_writing_result_to() build both kinds.
//! - Borrowed strings cross as a JSUtf16View. An owned AK::Utf16String crosses as its raw word, a
//!   JSOwnedUtf16String: the sender gives up its reference with AK::Utf16String::into_raw(), and the receiver adopts it
//!   with AK::Utf16String::adopt_raw(), so the string's storage is never copied.
//! - Property keys cross as the one word of the engine's own property key, which both runtimes encode the same way.

use core::ffi::c_void;
use core::mem::ManuallyDrop;
use core::ptr::NonNull;

use ak::Utf16String;

use crate::gc::class::Extends;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{
    JS_COMPLETION_NORMAL, JS_COMPLETION_THROW, JSCompletion, JSObject, JSPromiseCapability, JSPropertyKey,
    JSStringSink, JSVM, JSValue, JSValueSink,
};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::big_int::BigInt;
use crate::runtime::completion::{Throw, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_data::{ErrorData, ErrorDataCell};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise_capability::PromiseCapability;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::symbol::Symbol;
use crate::utf16::Utf16View;

// The C types of cells, which the ABI only ever passes behind a pointer. Without #[repr(C)], cbindgen declares each as
// an incomplete struct. It does not expand macros, so each is written out.

/// A realm.
pub struct JSRealm {
    _opaque: [u8; 0],
}

/// A string value.
pub struct JSPrimitiveString {
    _opaque: [u8; 0],
}

/// A BigInt value.
pub struct JSBigInt {
    _opaque: [u8; 0],
}

/// A Symbol value.
pub struct JSSymbol {
    _opaque: [u8; 0],
}

/// The [[ErrorData]] of a host object that is not an Error, in a cell of its own: C++ ErrorDataCell.
pub struct JSErrorDataCell {
    _opaque: [u8; 0],
}

/// The [[ErrorData]] of an object, the call stack it was created on, which lives in the object if it is an Error and
/// in a JSErrorDataCell otherwise. It is not a cell, so a pointer to it stays valid for as long as the cell it lives
/// in.
pub struct JSErrorData {
    _opaque: [u8; 0],
}

/// The opaque C type of a kind of cell: a pointer to it is the address of such a cell.
pub trait CellAbi {
    type Cell;
}

impl CellAbi for JSObject {
    type Cell = Object;
}

impl CellAbi for JSRealm {
    type Cell = Realm;
}

impl CellAbi for JSPrimitiveString {
    type Cell = PrimitiveString;
}

impl CellAbi for JSBigInt {
    type Cell = BigInt;
}

impl CellAbi for JSSymbol {
    type Cell = Symbol;
}

impl CellAbi for JSErrorDataCell {
    type Cell = ErrorDataCell;
}

impl CellAbi for JSPromiseCapability {
    type Cell = PromiseCapability;
}

pub fn error_data_into_abi(error_data: &ErrorData) -> *const JSErrorData {
    core::ptr::from_ref(error_data).cast()
}

/// # Safety
///
/// `error_data` must point to the error data of a cell that stays alive for `'cell`.
pub unsafe fn error_data_from_abi<'cell>(error_data: *const JSErrorData) -> &'cell ErrorData {
    assert!(!error_data.is_null(), "the embedder passes error data");
    // SAFETY: The caller guarantees that the pointer is to error data, which lives as long as its cell.
    unsafe { &*error_data.cast::<ErrorData>() }
}

pub fn cell_into_abi<C: CellAbi>(cell: Gc<C::Cell>) -> *mut C {
    cell.as_ptr().cast()
}

/// The cell's pointer, or null for none.
pub fn optional_cell_into_abi<C: CellAbi>(cell: Option<Gc<C::Cell>>) -> *mut C {
    cell.map_or(core::ptr::null_mut(), cell_into_abi)
}

/// # Safety
///
/// `pointer` must be the address of a live cell of the kind C stands for.
pub unsafe fn cell_from_abi<C: CellAbi>(pointer: *mut C) -> Gc<C::Cell> {
    let pointer = NonNull::new(pointer).expect("the embedder passes a cell");
    // SAFETY: The caller guarantees that the pointer is the address of a live cell of this kind.
    unsafe { Gc::from_non_null(pointer.cast()) }
}

/// # Safety
///
/// `pointer` must be null or the address of a live cell of the kind C stands for.
pub unsafe fn optional_cell_from_abi<C: CellAbi>(pointer: *mut C) -> Option<Gc<C::Cell>> {
    // SAFETY: The caller guarantees that a pointer that is not null is the address of a live cell of this kind.
    (!pointer.is_null()).then(|| unsafe { cell_from_abi(pointer) })
}

pub fn vm_into_abi(vm: &Vm) -> *mut JSVM {
    core::ptr::from_ref(vm).cast_mut().cast()
}

/// # Safety
///
/// `vm` must be the address of a live VM, which the embedder constructed and only uses on the thread that owns it.
pub unsafe fn vm_from_abi<'vm>(vm: *mut JSVM) -> &'vm Vm {
    let vm = NonNull::new(vm).expect("the embedder passes its VM");
    // SAFETY: The caller guarantees that the pointer is the address of a live VM.
    unsafe { vm.cast::<Vm>().as_ref() }
}

/// The address of an object of any class, as the JSObject the ABI passes.
pub fn object_into_abi<T: Extends<Object>>(object: Gc<T>) -> *mut JSObject {
    cell_into_abi(object.upcast::<Object>())
}

/// The object's address, or null for none.
pub fn optional_object_into_abi<T: Extends<Object>>(object: Option<Gc<T>>) -> *mut JSObject {
    object.map_or(core::ptr::null_mut(), object_into_abi)
}

/// A result that the payload of a normal completion carries.
pub trait CompletionPayload {
    fn into_payload(self) -> u64;
}

/// No result, for which the payload is 0.
impl CompletionPayload for () {
    fn into_payload(self) -> u64 {
        0
    }
}

impl CompletionPayload for bool {
    fn into_payload(self) -> u64 {
        u64::from(self)
    }
}

impl CompletionPayload for Value {
    fn into_payload(self) -> u64 {
        self.0
    }
}

/// A pointer, such as one to a cell of one of the C types, with null standing for none.
impl<T> CompletionPayload for *mut T {
    fn into_payload(self) -> u64 {
        self.expose_provenance() as u64
    }
}

impl<T: Extends<Object>> CompletionPayload for Gc<T> {
    fn into_payload(self) -> u64 {
        object_into_abi(self).into_payload()
    }
}

impl<T: Extends<Object>> CompletionPayload for Option<Gc<T>> {
    fn into_payload(self) -> u64 {
        optional_object_into_abi(self).into_payload()
    }
}

/// The completion of an operation whose result, if it has one, is the payload of its normal completion.
pub fn completion_into_abi<T: CompletionPayload>(result: ThrowCompletionOr<T>) -> JSCompletion {
    match result {
        Ok(result) => JSCompletion {
            payload: result.into_payload(),
            variant: JS_COMPLETION_NORMAL,
        },
        Err(throw) => throw_completion_into_abi(throw),
    }
}

pub fn throw_completion_into_abi(throw: Throw) -> JSCompletion {
    JSCompletion {
        payload: throw.value().0,
        variant: JS_COMPLETION_THROW,
    }
}

/// The completion of an operation whose result goes to the out parameter `out`, which a throw leaves untouched.
///
/// # Safety
///
/// `out` must be valid for writing a T.
pub unsafe fn completion_writing_result_to<T>(result: ThrowCompletionOr<T>, out: *mut T) -> JSCompletion {
    match result {
        Ok(result) => {
            assert!(!out.is_null(), "the embedder passes an out parameter");
            // SAFETY: The caller guarantees that the out parameter is valid for writing a T.
            unsafe { out.write(result) };
            completion_into_abi(Ok(()))
        }
        Err(throw) => throw_completion_into_abi(throw),
    }
}

/// The completion an embedder's hook returned, as the runtime's own.
pub fn completion_from_abi(completion: JSCompletion) -> ThrowCompletionOr<Value> {
    match completion.variant {
        JS_COMPLETION_NORMAL => Ok(Value(completion.payload)),
        JS_COMPLETION_THROW => Err(Throw::new(Value(completion.payload))),
        variant => panic!("the embedder returned a completion of unknown variant {variant}"),
    }
}

pub fn value_into_abi(value: Value) -> JSValue {
    value.0
}

pub fn value_from_abi(value: JSValue) -> Value {
    Value(value)
}

/// A borrowed run of code units in the two storage kinds of AK::Utf16View: `data` points to `length_in_code_units`
/// ASCII bytes if `has_ascii_storage` is set, and to as many UTF-16 code units otherwise. `data` may be null when the
/// length is 0.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JSUtf16View {
    pub data: *const c_void,
    pub length_in_code_units: usize,
    pub has_ascii_storage: bool,
}

impl JSUtf16View {
    pub fn of(view: Utf16View<'_>) -> Self {
        match view {
            Utf16View::Ascii(bytes) => Self {
                data: bytes.as_ptr().cast(),
                length_in_code_units: bytes.len(),
                has_ascii_storage: true,
            },
            Utf16View::Utf16(code_units) => Self {
                data: code_units.as_ptr().cast(),
                length_in_code_units: code_units.len(),
                has_ascii_storage: false,
            },
        }
    }

    /// # Safety
    ///
    /// Unless the length is 0, `data` must point to that many code units of the view's storage kind, which stay
    /// unchanged for `'a`.
    pub unsafe fn as_view<'a>(&self) -> Utf16View<'a> {
        if self.length_in_code_units == 0 {
            return Utf16View::EMPTY;
        }
        assert!(!self.data.is_null(), "a view of code units has its data");
        if self.has_ascii_storage {
            // SAFETY: The caller guarantees that the data is this many ASCII bytes.
            Utf16View::Ascii(unsafe { core::slice::from_raw_parts(self.data.cast(), self.length_in_code_units) })
        } else {
            // SAFETY: The caller guarantees that the data is this many UTF-16 code units.
            Utf16View::Utf16(unsafe { core::slice::from_raw_parts(self.data.cast(), self.length_in_code_units) })
        }
    }
}

/// The raw word of an AK::Utf16String, along with the one reference to its storage that the receiver adopts.
pub type JSOwnedUtf16String = usize;

pub fn owned_utf16_string_into_abi(string: Utf16String) -> JSOwnedUtf16String {
    string.into_raw()
}

/// # Safety
///
/// `string` must be the raw word of an AK::Utf16String whose reference the caller gives up.
pub unsafe fn owned_utf16_string_from_abi(string: JSOwnedUtf16String) -> Utf16String {
    // SAFETY: The caller transfers the reference of a valid raw string.
    unsafe { Utf16String::from_raw_owned(string) }
}

// A property key and its C form are the same word, so a borrowed one can be viewed as the other.
const _: () = assert!(size_of::<JSPropertyKey>() == size_of::<PropertyKey>());
const _: () = assert!(align_of::<JSPropertyKey>() == align_of::<PropertyKey>());

/// Gives the key's reference, if it holds a string, to the embedder.
pub fn property_key_into_abi(key: PropertyKey) -> JSPropertyKey {
    let key = ManuallyDrop::new(key);
    // SAFETY: A property key is one word, which the key no longer releases.
    JSPropertyKey {
        bits: unsafe { core::ptr::from_ref(&*key).cast::<usize>().read() },
    }
}

/// # Safety
///
/// `key` must point to the word of a property key that stays alive for `'key`.
pub unsafe fn property_key_from_abi<'key>(key: *const JSPropertyKey) -> &'key PropertyKey {
    assert!(!key.is_null(), "the embedder passes a property key");
    // SAFETY: The caller guarantees that the word is a live property key, and the two types have the same layout.
    unsafe { &*key.cast::<PropertyKey>() }
}

/// The constructor of an error that the embedder creates or throws: %Error%, a NativeError constructor, or
/// %AggregateError% or %SuppressedError%.
pub type JSErrorKind = u8;

pub const JS_ERROR_KIND_ERROR: JSErrorKind = 0;
pub const JS_ERROR_KIND_EVAL_ERROR: JSErrorKind = 1;
pub const JS_ERROR_KIND_INTERNAL_ERROR: JSErrorKind = 2;
pub const JS_ERROR_KIND_RANGE_ERROR: JSErrorKind = 3;
pub const JS_ERROR_KIND_REFERENCE_ERROR: JSErrorKind = 4;
pub const JS_ERROR_KIND_SYNTAX_ERROR: JSErrorKind = 5;
pub const JS_ERROR_KIND_TYPE_ERROR: JSErrorKind = 6;
pub const JS_ERROR_KIND_URI_ERROR: JSErrorKind = 7;
pub const JS_ERROR_KIND_AGGREGATE_ERROR: JSErrorKind = 8;
pub const JS_ERROR_KIND_SUPPRESSED_ERROR: JSErrorKind = 9;

pub fn error_kind_from_abi(kind: JSErrorKind) -> ErrorKind {
    match kind {
        JS_ERROR_KIND_ERROR => ErrorKind::Error,
        JS_ERROR_KIND_EVAL_ERROR => ErrorKind::EvalError,
        JS_ERROR_KIND_INTERNAL_ERROR => ErrorKind::InternalError,
        JS_ERROR_KIND_RANGE_ERROR => ErrorKind::RangeError,
        JS_ERROR_KIND_REFERENCE_ERROR => ErrorKind::ReferenceError,
        JS_ERROR_KIND_SYNTAX_ERROR => ErrorKind::SyntaxError,
        JS_ERROR_KIND_TYPE_ERROR => ErrorKind::TypeError,
        JS_ERROR_KIND_URI_ERROR => ErrorKind::URIError,
        JS_ERROR_KIND_AGGREGATE_ERROR => ErrorKind::AggregateError,
        JS_ERROR_KIND_SUPPRESSED_ERROR => ErrorKind::SuppressedError,
        kind => panic!("the embedder passed an unknown error kind {kind}"),
    }
}

/// Where the runtime writes bytes, such as printed text, which C++ appends to an AK::Stream. `append` returns false
/// when it could not take the bytes, which stops the operation that writes them.
#[repr(C)]
pub struct JSByteSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, length: usize) -> bool>,
}

/// A SourceCode, which the runtime shares through reference counting. Its address identifies it: every function that
/// hands one out returns the same pointer for the same source code.
pub struct JSSourceCode {
    _opaque: [u8; 0],
}

/// Hands `value` to the embedder's sink.
pub fn append_to_value_sink(sink: &JSValueSink, value: Value) {
    let append = sink.append.expect("a value sink has an append function");
    // SAFETY: The embedder's sink takes values with the context it came with.
    unsafe { append(sink.context, value.0) };
}

/// Hands `string` to the embedder's sink, which takes UTF-16 code units.
pub fn append_to_string_sink(sink: &JSStringSink, string: Utf16View<'_>) {
    let append = sink.append.expect("a string sink has an append function");
    let widened_ascii;
    let code_units = match string {
        Utf16View::Ascii(bytes) => {
            widened_ascii = bytes.iter().map(|&byte| u16::from(byte)).collect::<Vec<u16>>();
            widened_ascii.as_slice()
        }
        Utf16View::Utf16(code_units) => code_units,
    };
    // SAFETY: The embedder's sink takes the code units with the context it came with, and only reads them during the
    //         call.
    unsafe { append(sink.context, code_units.as_ptr(), code_units.len()) };
}
