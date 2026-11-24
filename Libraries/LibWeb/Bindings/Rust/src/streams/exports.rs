/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which the C++ classes of Streams/ReadableStream.h, Streams/TransformStream.h and
//! Streams/WritableStream.h set streams up and read them, for the specifications that LibWeb implements in C++.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, RawNativeFunctionResult, Realm, ThrowCompletionOr, Value, Vm};

use crate::bindings::conversions::BufferSource;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::interface::{InterfaceCell, Ref, allocate, cell_into_abi};
use crate::bindings::promise::WebIdlPromise;
use crate::bindings::{CallContext, MustNotThrow, WebIdlResult};
use crate::streams::algorithms::{CancelAlgorithm, FlushAlgorithm, TransformAlgorithm, WriteAlgorithm};
use crate::streams::cpp::{
    CppBytesSteps, CppPromiseAlgorithm, CppPromiseAlgorithmWithArgument, CppReadRequest, CppValueSteps,
};
use crate::streams::readable_byte_stream_controller::readable_byte_stream_controller_enqueue_native_bytes;
use crate::streams::readable_stream::ReadableStreamReader;
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamState, is_readable_stream_locked, readable_stream_cancel,
};
use crate::streams::readable_stream_default_controller::readable_stream_default_controller_get_desired_size;
use crate::streams::readable_stream_default_reader::ReadableStreamDefaultReader;
use crate::streams::readable_stream_generic_reader::ReadableStreamGenericReader;
use crate::streams::readable_stream_tee::readable_stream_tee;
use crate::streams::transform_stream::TransformStream;
use crate::streams::transform_stream_default_controller::transform_stream_default_controller_enqueue;
use crate::streams::underlying_source::ReadableStreamController;
use crate::streams::writable_stream::WritableStream;

/// # Safety
///
/// `cell` must be null or a live cell of LibWeb's C++ code.
unsafe fn optional_cpp_cell(cell: *mut c_void) -> Option<CppCell> {
    // SAFETY: The caller passes null or a live cell.
    NonNull::new(cell).map(|cell| unsafe { CppCell::from_raw(cell.as_ptr()) })
}

fn completion(cx: &CallContext, result: WebIdlResult<()>) -> RawNativeFunctionResult {
    let result: ThrowCompletionOr<Value> = result
        .map(|()| Value::UNDEFINED)
        .map_err(|exception| exception.throw(cx));
    RawNativeFunctionResult::from_completion(result)
}

fn promise_into_abi(promise: WebIdlPromise) -> *mut c_void {
    promise.as_ptr().cast()
}

/// A new ReadableStream in the realm.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_create(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| ReadableStream::new(realm)))
}

/// Sets the stream up with byte reading support, https://streams.spec.whatwg.org/#readablestream-set-up-with-byte-reading-support,
/// given the pullAlgorithm and the cancelAlgorithm that C++ gives, if any.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, a live ReadableStream, and null or the live GC::Functions of the
/// algorithms.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_set_up_with_byte_reading_support(
    vm: &Vm,
    realm: Gc<Realm>,
    stream: &InterfaceCell,
    pull_algorithm: *mut c_void,
    cancel_algorithm: *mut c_void,
    high_water_mark: f64,
) {
    // SAFETY: The caller passes the VM, a live realm, a live stream, and null or live algorithms.
    let (stream, pull_algorithm, cancel_algorithm) = unsafe {
        (
            stream.object::<ReadableStream>(),
            optional_cpp_cell(pull_algorithm).map(CppPromiseAlgorithm),
            optional_cpp_cell(cancel_algorithm).map(CppPromiseAlgorithmWithArgument),
        )
    };
    let cx = CallContext::new(vm, realm);
    ReadableStream::set_up_with_byte_reading_support(stream, &cx, pull_algorithm, cancel_algorithm, high_water_mark);
}

/// The [[state]] of the stream: 0 for "readable", 1 for "closed", and 2 for "errored".
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_state(stream: &InterfaceCell) -> u8 {
    let stream = stream.object::<ReadableStream>();
    match stream.state() {
        ReadableStreamState::Readable => 0,
        ReadableStreamState::Closed => 1,
        ReadableStreamState::Errored => 2,
    }
}

/// https://streams.spec.whatwg.org/#readablestream-locked
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_is_locked(stream: &InterfaceCell) -> bool {
    let stream = stream.object::<ReadableStream>();
    // A ReadableStream stream is locked if ! IsReadableStreamLocked(stream) returns true.
    is_readable_stream_locked(stream)
}

/// https://streams.spec.whatwg.org/#is-readable-stream-disturbed
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_is_disturbed(stream: &InterfaceCell) -> bool {
    let stream = stream.object::<ReadableStream>();
    // A ReadableStream stream is disturbed if stream.[[disturbed]] is true.
    stream.disturbed()
}

/// Whether the [[closeRequested]] of the stream's controller is true.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_close_requested(stream: &InterfaceCell) -> bool {
    let stream = stream.object::<ReadableStream>();
    match stream.controller() {
        Some(ReadableStreamController::ReadableStreamDefaultController(controller)) => controller.close_requested(),
        Some(ReadableStreamController::ReadableByteStreamController(controller)) => controller.close_requested(),
        None => false,
    }
}

/// Whether the stream's controller is a ReadableByteStreamController.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_is_readable_byte_stream(stream: &InterfaceCell) -> bool {
    let stream = stream.object::<ReadableStream>();
    matches!(
        stream.controller(),
        Some(ReadableStreamController::ReadableByteStreamController(_))
    )
}

/// The desired size to fill the stream's internal queue, written to `desired_size`, or false if it is null because the
/// stream is errored, https://streams.spec.whatwg.org/#readablestream-get-the-desired-size
///
/// # Safety
///
/// `stream` must be a live ReadableStream with a ReadableStreamDefaultController, and `desired_size` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_desired_size(stream: &InterfaceCell, desired_size: *mut f64) -> bool {
    let stream = stream.object::<ReadableStream>();
    let Some(ReadableStreamController::ReadableStreamDefaultController(controller)) = stream.controller() else {
        panic!("only a ReadableStream with a ReadableStreamDefaultController has a desired size to read");
    };
    let Some(size) = readable_stream_default_controller_get_desired_size(controller) else {
        return false;
    };
    // SAFETY: The caller passes a writable f64.
    unsafe { desired_size.write(size) };
    true
}

/// https://streams.spec.whatwg.org/#readablestream-enqueue
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_enqueue(
    vm: &Vm,
    stream: &InterfaceCell,
    chunk: Value,
) -> RawNativeFunctionResult {
    let stream = stream.object::<ReadableStream>();
    let result = ReadableStream::enqueue(stream, vm, chunk);
    completion(&stream.context(vm), result)
}

/// ReadableByteStreamControllerEnqueue on the controller of the stream, a ReadableByteStreamController, with a chunk of
/// the bytes, https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_enqueue_native_bytes(
    vm: &Vm,
    stream: &InterfaceCell,
    bytes: ak::Span<'_, u8>,
) -> RawNativeFunctionResult {
    let stream = stream.object::<ReadableStream>();
    let cx = stream.context(vm);
    let Some(ReadableStreamController::ReadableByteStreamController(controller)) = stream.controller() else {
        panic!("C++ enqueues bytes into a stream with a ReadableByteStreamController");
    };
    let result = readable_byte_stream_controller_enqueue_native_bytes(&cx, controller, bytes.as_slice());
    completion(&cx, result)
}

/// The byte length of the current BYOB request view of the stream, if it has one,
/// https://streams.spec.whatwg.org/#readablestream-current-byob-request-view
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_current_byob_request_view_byte_length(
    vm: &Vm,
    stream: &InterfaceCell,
    byte_length: &mut u64,
) -> bool {
    let stream = stream.object::<ReadableStream>();
    let Some(view) = ReadableStream::current_byob_request_view(stream, vm) else {
        return false;
    };
    *byte_length = BufferSource::from(view).extent().byte_length as u64;
    true
}

/// https://streams.spec.whatwg.org/#readablestream-pull-from-bytes
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_pull_from_bytes(
    vm: &Vm,
    stream: &InterfaceCell,
    bytes: ak::Span<'_, u8>,
) -> RawNativeFunctionResult {
    let stream = stream.object::<ReadableStream>();
    let result = ReadableStream::pull_from_bytes(stream, vm, bytes.as_slice());
    completion(&stream.context(vm), result)
}

/// https://streams.spec.whatwg.org/#readablestream-close
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_close(vm: &Vm, stream: &InterfaceCell) {
    let stream = stream.object::<ReadableStream>();
    ReadableStream::close(stream, vm);
}

/// https://streams.spec.whatwg.org/#readablestream-error
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_error(vm: &Vm, stream: &InterfaceCell, error: Value) {
    let stream = stream.object::<ReadableStream>();
    ReadableStream::error(stream, vm, error);
}

/// ! ReadableStreamCancel(stream, reason), https://streams.spec.whatwg.org/#readable-stream-cancel
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_cancel(vm: &Vm, stream: &InterfaceCell, reason: Value) -> *mut c_void {
    let stream = stream.object::<ReadableStream>();
    promise_into_abi(readable_stream_cancel(&stream.context(vm), stream, reason))
}

/// To tee a ReadableStream stream, https://streams.spec.whatwg.org/#readablestream-tee, which sets the branches.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_tee(
    vm: &Vm,
    stream: &InterfaceCell,
    branch1: &mut *mut c_void,
    branch2: &mut *mut c_void,
) -> RawNativeFunctionResult {
    let stream = stream.object::<ReadableStream>();
    let cx = stream.context(vm);

    // To tee a ReadableStream stream, return ? ReadableStreamTee(stream, true).
    let result = readable_stream_tee(&cx, stream, true).map(|(first, second)| {
        *branch1 = cell_into_abi(first);
        *branch2 = cell_into_abi(second);
    });
    completion(&cx, result)
}

/// To get a reader for a ReadableStream, https://streams.spec.whatwg.org/#readablestream-get-a-reader, which sets the
/// reader.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_get_a_reader(
    vm: &Vm,
    stream: &InterfaceCell,
    reader: &mut *mut c_void,
) -> RawNativeFunctionResult {
    let stream = stream.object::<ReadableStream>();
    let result = ReadableStream::get_a_reader(stream, vm).map(|new_reader| *reader = cell_into_abi(new_reader));
    completion(&stream.context(vm), result)
}

/// The stream piped through the TransformStream, https://streams.spec.whatwg.org/#readablestream-pipe-through
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_piped_through(
    vm: &Vm,
    stream: &InterfaceCell,
    transform: &InterfaceCell,
    prevent_close: bool,
    prevent_abort: bool,
    prevent_cancel: bool,
) -> *mut c_void {
    let (stream, transform) = (stream.object::<ReadableStream>(), transform.object::<TransformStream>());
    let readable = ReadableStream::piped_through(
        stream,
        vm,
        transform,
        prevent_close,
        prevent_abort,
        prevent_cancel,
        None,
    );
    cell_into_abi(readable)
}

/// The reader as a ReadableStreamReader.
fn default_reader(reader: Ref<ReadableStreamDefaultReader>) -> ReadableStreamReader {
    ReadableStreamReader::ReadableStreamDefaultReader(reader)
}

/// To read a chunk from a ReadableStreamDefaultReader, given a read request of C++,
/// https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-a-chunk
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_default_reader_read_a_chunk(
    vm: &Vm,
    reader: &InterfaceCell,
    read_request: &LentCppCell<cpp_class::ReadRequest>,
) {
    let (reader, read_request) = (
        reader.object::<ReadableStreamDefaultReader>(),
        CppReadRequest::from_lent(read_request),
    );
    ReadableStreamDefaultReader::read_a_chunk(vm, reader, read_request);
}

/// To read all bytes from a ReadableStreamDefaultReader, given the successSteps and the failureSteps of C++,
/// https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-all-bytes
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_default_reader_read_all_bytes(
    vm: &Vm,
    reader: &InterfaceCell,
    success_steps: &LentCppCell<cpp_class::BytesSteps>,
    failure_steps: &LentCppCell<cpp_class::ValueSteps>,
) {
    let (reader, success_steps, failure_steps) = (
        reader.object::<ReadableStreamDefaultReader>(),
        CppBytesSteps::from_lent(success_steps),
        CppValueSteps::from_lent(failure_steps),
    );
    ReadableStreamDefaultReader::read_all_bytes(vm, reader, success_steps, failure_steps);
}

/// The steps of the reader's read(), https://streams.spec.whatwg.org/#default-reader-read
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_default_reader_read(vm: &Vm, reader: &InterfaceCell) -> *mut c_void {
    let reader = reader.object::<ReadableStreamDefaultReader>();
    let mut cx = reader.generic().context(vm);
    promise_into_abi(ReadableStreamDefaultReader::read(&reader, &mut cx).must())
}

/// The steps of the reader's cancel(), https://streams.spec.whatwg.org/#generic-reader-cancel
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_default_reader_cancel(
    vm: &Vm,
    reader: &InterfaceCell,
    reason: Value,
) -> *mut c_void {
    let reader = reader.object::<ReadableStreamDefaultReader>();
    promise_into_abi(ReadableStreamGenericReader::cancel(
        vm,
        default_reader(reader),
        Some(reason),
    ))
}

/// The realm of the reader's [[closedPromise]].
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_readable_stream_default_reader_closed_promise_realm(reader: &InterfaceCell) -> *mut c_void {
    let reader = reader.object::<ReadableStreamDefaultReader>();
    let realm: libjs_rust::api::Gc<Realm> = reader.generic().closed_promise_realm();
    realm.as_ptr().cast()
}

/// A new WritableStream in the realm.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_writable_stream_create(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(allocate(vm, |defer_gc| WritableStream::new(defer_gc, realm)))
}

/// Sets the stream up with the writeAlgorithm that C++ gives, https://streams.spec.whatwg.org/#writablestream-set-up
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_writable_stream_set_up(
    vm: &Vm,
    realm: Gc<Realm>,
    stream: &InterfaceCell,
    write_algorithm: &LentCppCell<cpp_class::PromiseAlgorithmWithArgument>,
    high_water_mark: f64,
) {
    let (stream, write_algorithm) = (
        stream.object::<WritableStream>(),
        CppPromiseAlgorithmWithArgument::from_lent(write_algorithm),
    );
    let cx = CallContext::new(vm, realm);
    WritableStream::set_up(
        stream,
        &cx,
        WriteAlgorithm::CppWrapper(write_algorithm),
        high_water_mark,
    );
}

/// A new TransformStream in the realm.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_transform_stream_create(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| TransformStream::new(realm)))
}

/// Sets the stream up with the algorithms that C++ gives, https://streams.spec.whatwg.org/#transformstream-set-up
///
/// # Safety
///
/// The pointers must be the VM, a live realm, a live TransformStream, the live GC::Function of the transformAlgorithm,
/// and null or the live GC::Functions of the flushAlgorithm and the cancelAlgorithm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_transform_stream_set_up(
    vm: &Vm,
    realm: Gc<Realm>,
    stream: &InterfaceCell,
    transform_algorithm: &LentCppCell<cpp_class::PromiseAlgorithmWithArgument>,
    flush_algorithm: *mut c_void,
    cancel_algorithm: *mut c_void,
) {
    // SAFETY: The caller passes the VM, a live realm, a live stream, a live transformAlgorithm and null or live
    //         algorithms.
    let (stream, transform_algorithm, flush_algorithm, cancel_algorithm) = unsafe {
        (
            stream.object::<TransformStream>(),
            CppPromiseAlgorithmWithArgument::from_lent(transform_algorithm),
            optional_cpp_cell(flush_algorithm).map(CppPromiseAlgorithm),
            optional_cpp_cell(cancel_algorithm).map(CppPromiseAlgorithmWithArgument),
        )
    };
    let cx = CallContext::new(vm, realm);
    TransformStream::set_up(
        stream,
        &cx,
        TransformAlgorithm::CppWrapper(transform_algorithm),
        FlushAlgorithm::CppWrapper(flush_algorithm),
        CancelAlgorithm::Wrapper(cancel_algorithm),
    );
}

/// TransformStreamDefaultControllerEnqueue(stream.[[controller]], chunk), as enqueuing the chunk into the stream does,
/// https://streams.spec.whatwg.org/#transformstream-enqueue
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_transform_stream_enqueue(
    vm: &Vm,
    stream: &InterfaceCell,
    chunk: Value,
) -> RawNativeFunctionResult {
    let stream = stream.object::<TransformStream>();
    let cx = CallContext::new(vm, stream.backpressure_change_promise_realm());
    let controller = stream
        .controller()
        .expect("a TransformStream that is set up has a controller");
    let result = transform_stream_default_controller_enqueue(&cx, controller, chunk);
    completion(&cx, result)
}

/// The stream's [[readable]].
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_transform_stream_readable(stream: &InterfaceCell) -> *mut c_void {
    let stream = stream.object::<TransformStream>();
    cell_into_abi(stream.readable_stream())
}

/// The stream's [[writable]].
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_transform_stream_writable(stream: &InterfaceCell) -> *mut c_void {
    let stream = stream.object::<TransformStream>();
    cell_into_abi(stream.writable_stream())
}
