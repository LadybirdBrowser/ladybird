/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which the C++ classes of Streams/ReadableStream.h and Streams/TransformStream.h set streams up
//! and read them, for the specifications that LibWeb implements in C++.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{RawNativeFunctionResult, Realm, ThrowCompletionOr, Value, Vm};

use crate::bindings::interface::{Ref, allocate, cell_from_abi, cell_into_abi, realm_arguments};
use crate::bindings::promise::WebIdlPromise;
use crate::bindings::{CallContext, MustNotThrow, WebIdlResult};
use crate::streams::algorithms::{CancelAlgorithm, FlushAlgorithm, TransformAlgorithm};
use crate::streams::cpp::{
    CppBytesSteps, CppCell, CppPromiseAlgorithm, CppPromiseAlgorithmWithArgument, CppReadRequest, CppValueSteps,
};
use crate::streams::readable_byte_stream_controller::readable_byte_stream_controller_enqueue_native_bytes;
use crate::streams::readable_stream::ReadableStreamReader;
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamState, is_readable_stream_locked, readable_stream_cancel,
};
use crate::streams::readable_stream_default_reader::ReadableStreamDefaultReader;
use crate::streams::readable_stream_generic_reader::ReadableStreamGenericReader;
use crate::streams::readable_stream_tee::readable_stream_tee;
use crate::streams::transform_stream::TransformStream;
use crate::streams::transform_stream_default_controller::transform_stream_default_controller_enqueue;
use crate::streams::underlying_source::ReadableStreamController;

/// # Safety
///
/// `vm` must be the live VM.
unsafe fn vm_argument<'vm>(vm: *mut c_void) -> &'vm Vm {
    // SAFETY: The caller passes the live VM.
    unsafe { &*vm.cast::<Vm>() }
}

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
///
/// # Safety
///
/// The pointers must be the VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_create(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(allocate(vm, ReadableStream::new(realm)))
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
    vm: *mut c_void,
    realm: *mut c_void,
    stream: *mut c_void,
    pull_algorithm: *mut c_void,
    cancel_algorithm: *mut c_void,
    high_water_mark: f64,
) {
    // SAFETY: The caller passes the VM, a live realm, a live stream, and null or live algorithms.
    let ((vm, realm), stream, pull_algorithm, cancel_algorithm) = unsafe {
        (
            realm_arguments(vm, realm),
            cell_from_abi::<ReadableStream>(stream),
            optional_cpp_cell(pull_algorithm).map(CppPromiseAlgorithm),
            optional_cpp_cell(cancel_algorithm).map(CppPromiseAlgorithmWithArgument),
        )
    };
    let cx = CallContext::new(vm, realm);
    ReadableStream::set_up_with_byte_reading_support(stream, &cx, pull_algorithm, cancel_algorithm, high_water_mark);
}

/// The [[state]] of the stream: 0 for "readable", 1 for "closed", and 2 for "errored".
///
/// # Safety
///
/// `stream` must be a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_state(stream: *mut c_void) -> u8 {
    // SAFETY: The caller passes a live ReadableStream.
    let stream = unsafe { cell_from_abi::<ReadableStream>(stream) };
    match stream.state() {
        ReadableStreamState::Readable => 0,
        ReadableStreamState::Closed => 1,
        ReadableStreamState::Errored => 2,
    }
}

/// https://streams.spec.whatwg.org/#readablestream-locked
///
/// # Safety
///
/// `stream` must be a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_is_locked(stream: *mut c_void) -> bool {
    // SAFETY: The caller passes a live ReadableStream.
    let stream = unsafe { cell_from_abi::<ReadableStream>(stream) };
    // A ReadableStream stream is locked if ! IsReadableStreamLocked(stream) returns true.
    is_readable_stream_locked(stream)
}

/// https://streams.spec.whatwg.org/#is-readable-stream-disturbed
///
/// # Safety
///
/// `stream` must be a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_is_disturbed(stream: *mut c_void) -> bool {
    // SAFETY: The caller passes a live ReadableStream.
    let stream = unsafe { cell_from_abi::<ReadableStream>(stream) };
    // A ReadableStream stream is disturbed if stream.[[disturbed]] is true.
    stream.disturbed()
}

/// Whether the [[closeRequested]] of the stream's controller is true.
///
/// # Safety
///
/// `stream` must be a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_close_requested(stream: *mut c_void) -> bool {
    // SAFETY: The caller passes a live ReadableStream.
    let stream = unsafe { cell_from_abi::<ReadableStream>(stream) };
    match stream.controller() {
        Some(ReadableStreamController::ReadableStreamDefaultController(controller)) => controller.close_requested(),
        Some(ReadableStreamController::ReadableByteStreamController(controller)) => controller.close_requested(),
        None => false,
    }
}

/// https://streams.spec.whatwg.org/#readablestream-enqueue
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_enqueue(
    vm: *mut c_void,
    stream: *mut c_void,
    chunk: u64,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live ReadableStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStream>(stream)) };
    let result = ReadableStream::enqueue(stream, vm, Value(chunk));
    completion(&stream.context(vm), result)
}

/// ReadableByteStreamControllerEnqueue on the controller of the stream, a ReadableByteStreamController, with a chunk of
/// the bytes, https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStream, and the bytes must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_enqueue_native_bytes(
    vm: *mut c_void,
    stream: *mut c_void,
    bytes: ak::Span<'_, u8>,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live ReadableStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStream>(stream)) };
    let cx = stream.context(vm);
    let Some(ReadableStreamController::ReadableByteStreamController(controller)) = stream.controller() else {
        panic!("C++ enqueues bytes into a stream with a ReadableByteStreamController");
    };
    let result = readable_byte_stream_controller_enqueue_native_bytes(&cx, controller, bytes.as_slice());
    completion(&cx, result)
}

/// https://streams.spec.whatwg.org/#readablestream-close
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_close(vm: *mut c_void, stream: *mut c_void) {
    // SAFETY: The caller passes the VM and a live ReadableStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStream>(stream)) };
    ReadableStream::close(stream, vm);
}

/// https://streams.spec.whatwg.org/#readablestream-error
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_error(vm: *mut c_void, stream: *mut c_void, error: u64) {
    // SAFETY: The caller passes the VM and a live ReadableStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStream>(stream)) };
    ReadableStream::error(stream, vm, Value(error));
}

/// ! ReadableStreamCancel(stream, reason), https://streams.spec.whatwg.org/#readable-stream-cancel
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_cancel(
    vm: *mut c_void,
    stream: *mut c_void,
    reason: u64,
) -> *mut c_void {
    // SAFETY: The caller passes the VM and a live ReadableStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStream>(stream)) };
    promise_into_abi(readable_stream_cancel(&stream.context(vm), stream, Value(reason)))
}

/// To tee a ReadableStream stream, https://streams.spec.whatwg.org/#readablestream-tee, which sets the branches.
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_tee(
    vm: *mut c_void,
    stream: *mut c_void,
    branch1: &mut *mut c_void,
    branch2: &mut *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live ReadableStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStream>(stream)) };
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
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_get_a_reader(
    vm: *mut c_void,
    stream: *mut c_void,
    reader: &mut *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live ReadableStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStream>(stream)) };
    let result = ReadableStream::get_a_reader(stream, vm).map(|new_reader| *reader = cell_into_abi(new_reader));
    completion(&stream.context(vm), result)
}

/// The stream piped through the TransformStream, https://streams.spec.whatwg.org/#readablestream-pipe-through
///
/// # Safety
///
/// The pointers must be the VM, a live ReadableStream and a live TransformStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_piped_through(
    vm: *mut c_void,
    stream: *mut c_void,
    transform: *mut c_void,
    prevent_close: bool,
    prevent_abort: bool,
    prevent_cancel: bool,
) -> *mut c_void {
    // SAFETY: The caller passes the VM, a live ReadableStream and a live TransformStream.
    let (vm, stream, transform) = unsafe {
        (
            vm_argument(vm),
            cell_from_abi::<ReadableStream>(stream),
            cell_from_abi::<TransformStream>(transform),
        )
    };
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
///
/// # Safety
///
/// The pointers must be the VM, a live ReadableStreamDefaultReader and a live Streams::ReadRequest.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_default_reader_read_a_chunk(
    vm: *mut c_void,
    reader: *mut c_void,
    read_request: *mut c_void,
) {
    // SAFETY: The caller passes the VM, a live reader and a live read request.
    let (vm, reader, read_request) = unsafe {
        (
            vm_argument(vm),
            cell_from_abi::<ReadableStreamDefaultReader>(reader),
            CppReadRequest(CppCell::from_raw(read_request)),
        )
    };
    ReadableStreamDefaultReader::read_a_chunk(vm, reader, read_request);
}

/// To read all bytes from a ReadableStreamDefaultReader, given the successSteps and the failureSteps of C++,
/// https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-all-bytes
///
/// # Safety
///
/// The pointers must be the VM, a live ReadableStreamDefaultReader, a live GC::Function<void(ByteBuffer)> and a live
/// GC::Function<void(JS::Value)>.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_default_reader_read_all_bytes(
    vm: *mut c_void,
    reader: *mut c_void,
    success_steps: *mut c_void,
    failure_steps: *mut c_void,
) {
    // SAFETY: The caller passes the VM, a live reader and live steps.
    let (vm, reader, success_steps, failure_steps) = unsafe {
        (
            vm_argument(vm),
            cell_from_abi::<ReadableStreamDefaultReader>(reader),
            CppBytesSteps(CppCell::from_raw(success_steps)),
            CppValueSteps(CppCell::from_raw(failure_steps)),
        )
    };
    ReadableStreamDefaultReader::read_all_bytes(vm, reader, success_steps, failure_steps);
}

/// The steps of the reader's read(), https://streams.spec.whatwg.org/#default-reader-read
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStreamDefaultReader.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_default_reader_read(
    vm: *mut c_void,
    reader: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the VM and a live reader.
    let (vm, reader) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStreamDefaultReader>(reader)) };
    let mut cx = reader.generic().context(vm);
    promise_into_abi(ReadableStreamDefaultReader::read(&reader, &mut cx).must())
}

/// The steps of the reader's cancel(), https://streams.spec.whatwg.org/#generic-reader-cancel
///
/// # Safety
///
/// The pointers must be the VM and a live ReadableStreamDefaultReader.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_default_reader_cancel(
    vm: *mut c_void,
    reader: *mut c_void,
    reason: u64,
) -> *mut c_void {
    // SAFETY: The caller passes the VM and a live reader.
    let (vm, reader) = unsafe { (vm_argument(vm), cell_from_abi::<ReadableStreamDefaultReader>(reader)) };
    promise_into_abi(ReadableStreamGenericReader::cancel(
        vm,
        default_reader(reader),
        Some(Value(reason)),
    ))
}

/// The realm of the reader's [[closedPromise]].
///
/// # Safety
///
/// `reader` must be a live ReadableStreamDefaultReader.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_readable_stream_default_reader_closed_promise_realm(
    reader: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes a live reader.
    let reader = unsafe { cell_from_abi::<ReadableStreamDefaultReader>(reader) };
    let realm: libjs_rust::api::Gc<Realm> = reader.generic().closed_promise_realm();
    realm.as_ptr().cast()
}

/// A new TransformStream in the realm.
///
/// # Safety
///
/// The pointers must be the VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_transform_stream_create(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(allocate(vm, TransformStream::new(realm)))
}

/// Sets the stream up with the algorithms that C++ gives, https://streams.spec.whatwg.org/#transformstream-set-up
///
/// # Safety
///
/// The pointers must be the VM, a live realm, a live TransformStream, the live GC::Function of the transformAlgorithm,
/// and null or the live GC::Functions of the flushAlgorithm and the cancelAlgorithm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_transform_stream_set_up(
    vm: *mut c_void,
    realm: *mut c_void,
    stream: *mut c_void,
    transform_algorithm: *mut c_void,
    flush_algorithm: *mut c_void,
    cancel_algorithm: *mut c_void,
) {
    // SAFETY: The caller passes the VM, a live realm, a live stream, a live transformAlgorithm and null or live
    //         algorithms.
    let ((vm, realm), stream, transform_algorithm, flush_algorithm, cancel_algorithm) = unsafe {
        (
            realm_arguments(vm, realm),
            cell_from_abi::<TransformStream>(stream),
            CppPromiseAlgorithmWithArgument(CppCell::from_raw(transform_algorithm)),
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
///
/// # Safety
///
/// The pointers must be the VM and a live TransformStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_transform_stream_enqueue(
    vm: *mut c_void,
    stream: *mut c_void,
    chunk: u64,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live TransformStream.
    let (vm, stream) = unsafe { (vm_argument(vm), cell_from_abi::<TransformStream>(stream)) };
    let cx = CallContext::new(vm, stream.backpressure_change_promise_realm());
    let controller = stream
        .controller()
        .expect("a TransformStream that is set up has a controller");
    let result = transform_stream_default_controller_enqueue(&cx, controller, Value(chunk));
    completion(&cx, result)
}

/// The stream's [[readable]].
///
/// # Safety
///
/// `stream` must be a live TransformStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_transform_stream_readable(stream: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live TransformStream.
    let stream = unsafe { cell_from_abi::<TransformStream>(stream) };
    cell_into_abi(stream.readable_stream())
}

/// The stream's [[writable]].
///
/// # Safety
///
/// `stream` must be a live TransformStream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_transform_stream_writable(stream: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live TransformStream.
    let stream = unsafe { cell_from_abi::<TransformStream>(stream) };
    cell_into_abi(stream.writable_stream())
}
