/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStreamDefaultReader, https://streams.spec.whatwg.org/#default-reader-class

use std::collections::VecDeque;

use core::cell::Cell;

use libjs_rust::api::{
    DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Realm, Trace, TypedArrayBase,
    TypedArrayKind, Value, Vm,
};

use crate::bindings::conversions::BufferSource;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_rejected_promise_from_exception, exception_value,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::cpp::{CppBytesSteps, CppReadRequest, CppValueSteps};
use crate::streams::read_request::ReadRequest;
use crate::streams::readable_byte_stream_controller::ReadableByteStreamController;
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamReader, ReadableStreamState, is_readable_stream_locked,
};
use crate::streams::readable_stream_default_controller::ReadableStreamDefaultController;
use crate::streams::readable_stream_generic_reader::{
    ReadableStreamGenericReader, readable_stream_reader_generic_initialize, readable_stream_reader_generic_release,
};
use crate::streams::readable_stream_pipe_to::PipeToRef;
use crate::streams::underlying_source::ReadableStreamController;

/// The dictionary ReadableStreamReadResult of Streams/ReadableStreamDefaultReader.idl, which the bindings convert.
/// NB: The read requests resolve their promises with iterator result objects, as LibWeb's C++ does, so nothing
///     creates this.
pub(crate) struct ReadableStreamReadResult {
    pub(crate) done: Option<bool>,
    pub(crate) value: Option<Value>,
}

// https://streams.spec.whatwg.org/#readablestreamdefaultreader
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableStreamDefaultReader {
    generic: ReadableStreamGenericReader,
    // https://streams.spec.whatwg.org/#readablestreamdefaultreader-readrequests
    read_requests: GcRefCell<VecDeque<ReadRequest>>,
    /// The pipe that reads from the reader's stream, which the reader keeps alive.
    pipe_to: Cell<Option<PipeToRef>>,
}

impl Wrappable for ReadableStreamDefaultReader {}

impl ReadableStreamDefaultReader {
    pub(crate) fn new(_: &DeferGc<'_>) -> Self {
        Self {
            generic: ReadableStreamGenericReader::default(),
            read_requests: GcRefCell::new(VecDeque::new()),
            pipe_to: Cell::new(None),
        }
    }

    pub(crate) fn set_pipe_to(&self, pipe_to: Option<PipeToRef>) {
        self.pipe_to.set(pipe_to);
    }

    pub(crate) fn generic(&self) -> &ReadableStreamGenericReader {
        &self.generic
    }

    pub(crate) fn append_read_request(&self, vm: &Vm, read_request: ReadRequest) {
        self.read_requests.borrow_mut(vm).push_back(read_request);
    }

    /// Takes the reader's [[readRequests]], leaving an empty list.
    pub(crate) fn take_read_requests<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, ReadRequest> {
        let read_requests = MarkedVec::with_capacity(vm, self.read_requests.borrow().len());
        for read_request in self.read_requests.borrow().iter() {
            read_requests.push(read_request.clone());
        }
        self.read_requests.borrow_mut(vm).clear();
        read_requests
    }

    /// Removes the first of the reader's [[readRequests]], which are not empty.
    pub(crate) fn take_first_read_request(&self, vm: &Vm) -> ReadRequest {
        self.read_requests
            .borrow_mut(vm)
            .pop_front()
            .expect("the reader has a read request")
    }

    pub(crate) fn read_requests_len(&self) -> usize {
        self.read_requests.borrow().len()
    }

    pub(crate) fn read_requests_is_empty(&self) -> bool {
        self.read_requests.borrow().is_empty()
    }

    // https://streams.spec.whatwg.org/#default-reader-constructor
    pub(crate) fn construct(cx: &mut CallContext, stream: Ref<ReadableStream>) -> WebIdlResult<Ref<Self>> {
        let reader = allocate(cx.vm(), Self::new);

        // 1. Perform ? SetUpReadableStreamDefaultReader(this, stream).
        set_up_readable_stream_default_reader(cx, reader, stream)?;
        Ok(reader)
    }

    // https://streams.spec.whatwg.org/#generic-reader-closed
    pub(crate) fn closed(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        self.generic.closed()
    }

    // https://streams.spec.whatwg.org/#default-reader-read
    pub(crate) fn read(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let cx = &this.generic.context(cx.vm());

        // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
        if this.generic.stream().is_none() {
            let exception = Exception::type_error("Cannot read from an empty stream");
            return Ok(create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception));
        }

        // 2. Let promise be a new promise.
        let promise = create_promise(cx.vm(), cx.realm());

        // 3. Let readRequest be a new read request with the following items:
        //    chunk steps, given chunk
        //        1. Resolve promise with «[ "value" → chunk, "done" → false ]».
        //    close steps
        //        1. Resolve promise with «[ "value" → undefined, "done" → true ]».
        //    error steps, given e
        //        1. Reject promise with e.
        let read_request = ReadRequest::DefaultReaderRead(promise);

        // 4. Perform ! ReadableStreamDefaultReaderRead(this, readRequest).
        readable_stream_default_reader_read(cx.vm(), *this, read_request);

        // 5. Return promise.
        Ok(promise)
    }

    // https://streams.spec.whatwg.org/#default-reader-release-lock
    pub(crate) fn release_lock(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. If this.[[stream]] is undefined, return.
        if this.generic.stream().is_none() {
            return Ok(());
        }

        // 2. Perform ! ReadableStreamDefaultReaderRelease(this).
        readable_stream_default_reader_release(cx.vm(), *this);
        Ok(())
    }

    // https://streams.spec.whatwg.org/#generic-reader-cancel
    pub(crate) fn cancel(this: &Ref<Self>, cx: &mut CallContext, reason: Option<Value>) -> WebIdlResult<WebIdlPromise> {
        let reader = ReadableStreamReader::ReadableStreamDefaultReader(*this);
        Ok(ReadableStreamGenericReader::cancel(cx.vm(), reader, reason))
    }

    /// To read a chunk from a ReadableStreamDefaultReader reader, given a read request readRequest,
    /// https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-a-chunk
    pub(crate) fn read_a_chunk(vm: &Vm, reader: Ref<Self>, read_request: CppReadRequest) {
        // To read a chunk from a ReadableStreamDefaultReader reader, given a read request readRequest,
        // perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        readable_stream_default_reader_read(vm, reader, ReadRequest::Cpp(read_request));
    }

    // https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-all-bytes
    pub(crate) fn read_all_bytes(
        vm: &Vm,
        reader: Ref<Self>,
        success_steps: CppBytesSteps,
        failure_steps: CppValueSteps,
    ) {
        let realm = reader.generic.closed_promise_realm();

        // To read all bytes from a ReadableStreamDefaultReader reader, given successSteps, which is an algorithm
        // accepting a byte sequence, and failureSteps, which is an algorithm accepting a JavaScript value: read-loop
        // given reader, a new byte sequence, successSteps, and failureSteps.

        // https://streams.spec.whatwg.org/#read-loop
        // 1. Let readRequest be a new read request with the following items:
        //    NOTE: The items are the steps of ReadLoop.
        let read_request = READ_LOOP_CLASS.allocate(vm, |_| ReadLoop {
            realm,
            reader,
            bytes: core::cell::RefCell::new(Vec::new()),
            success_steps,
            failure_steps,
        });

        // 2. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        readable_stream_default_reader_read(vm, reader, ReadRequest::ReadLoop(read_request));
    }
}

/// The state of a read-loop, https://streams.spec.whatwg.org/#read-loop, whose read request reads the next chunk with
/// the same state.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadLoop {
    realm: Gc<Realm>,
    reader: Ref<ReadableStreamDefaultReader>,
    #[gc(untraced)]
    bytes: core::cell::RefCell<Vec<u8>>,
    success_steps: CppBytesSteps,
    failure_steps: CppValueSteps,
}

static READ_LOOP_CLASS: EmbedderClass<ReadLoop> = EmbedderClass::new("ReadLoop");

impl EmbedderValue for ReadLoop {
    const CLASS: &'static EmbedderClass<Self> = &READ_LOOP_CLASS;
}

impl ReadLoop {
    // chunk steps, given chunk
    pub(crate) fn chunk_steps(this: Gc<EmbedderCell<Self>>, vm: &Vm, chunk: Value) {
        // 1. If chunk is not a Uint8Array object, call failureSteps with a TypeError and abort these steps.
        let typed_array = if chunk.is_object() {
            chunk.as_object().downcast::<TypedArrayBase>()
        } else {
            None
        };
        let Some(array) = typed_array.filter(|typed_array| typed_array.kind() == TypedArrayKind::Uint8Array) else {
            let error = exception_value(vm, this.realm, Exception::type_error("Chunk data is not Uint8Array"));
            this.failure_steps.call(error);
            return;
        };

        // 2. Append the bytes represented by chunk to bytes.
        // NB: Since a chunk is whatever the stream enqueued, and enqueue() takes an 'any', this Uint8Array may well be
        //     a view onto an SAB with no conversion having had the chance to reject one. So, use with_bytes() to read
        //     it through — rather than handing over a pointer into memory that another agent can be writing.
        // SAFETY: Appending the bytes to bytes neither runs script nor allocates a cell.
        unsafe { BufferSource::TypedArray(array).with_bytes(|bytes| this.bytes.borrow_mut().extend_from_slice(bytes)) };

        // FIXME: As the spec suggests, implement this non-recursively - instead of directly. It is not too big of a
        //        deal currently as we enqueue the entire blob buffer in one go, meaning that we only recurse a single
        //        time. Once we begin queuing up more than one chunk at a time, we may run into stack overflow problems.
        //
        // 3. Read-loop given reader, bytes, successSteps, and failureSteps.
        readable_stream_default_reader_read(vm, this.reader, ReadRequest::ReadLoop(this));
    }

    // close steps
    pub(crate) fn close_steps(&self) {
        // 1. Call successSteps with bytes.
        let bytes = core::mem::take(&mut *self.bytes.borrow_mut());
        self.success_steps.call(&bytes);
    }

    // error steps, given e
    pub(crate) fn error_steps(&self, error: Value) {
        // 1. Call failureSteps with e.
        self.failure_steps.call(error);
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreadererrorreadrequests
pub(crate) fn readable_stream_default_reader_error_read_requests(
    vm: &Vm,
    reader: Ref<ReadableStreamDefaultReader>,
    error: Value,
) {
    // 1. Let readRequests be reader.[[readRequests]].
    // 2. Set reader.[[readRequests]] to a new empty list.
    let read_requests = reader.take_read_requests(vm);

    // 3. For each readRequest of readRequests,
    for index in 0..read_requests.len() {
        // 1. Perform readRequest’s error steps, given e.
        read_requests
            .get(index)
            .expect("the index is in bounds")
            .error_steps(vm, error);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-default-reader-read
pub(crate) fn readable_stream_default_reader_read(
    vm: &Vm,
    reader: Ref<ReadableStreamDefaultReader>,
    read_request: ReadRequest,
) {
    // 1. Let stream be reader.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = reader.generic.stream().expect("a reader that reads has a stream");

    // 3. Set stream.[[disturbed]] to true.
    stream.set_disturbed(true);

    match stream.state() {
        // 4. If stream.[[state]] is "closed", perform readRequest’s close steps.
        ReadableStreamState::Closed => read_request.close_steps(vm),
        // 5. Otherwise, if stream.[[state]] is "errored", perform readRequest’s error steps given
        //    stream.[[storedError]].
        ReadableStreamState::Errored => read_request.error_steps(vm, stream.stored_error()),
        // 6. Otherwise,
        ReadableStreamState::Readable => {
            // 1. Assert: stream.[[state]] is "readable".
            // 2. Perform ! stream.[[controller]].[[PullSteps]](readRequest).
            match stream.controller().expect("a stream that is set up has a controller") {
                ReadableStreamController::ReadableStreamDefaultController(controller) => {
                    ReadableStreamDefaultController::pull_steps(controller, vm, read_request);
                }
                ReadableStreamController::ReadableByteStreamController(controller) => {
                    ReadableByteStreamController::pull_steps(controller, &reader.generic.context(vm), read_request);
                }
            }
        }
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreaderrelease
pub(crate) fn readable_stream_default_reader_release(vm: &Vm, reader: Ref<ReadableStreamDefaultReader>) {
    let realm = reader.generic.closed_promise_realm();

    // 1. Perform ! ReadableStreamReaderGenericRelease(reader).
    readable_stream_reader_generic_release(vm, ReadableStreamReader::ReadableStreamDefaultReader(reader));

    // 2. Let e be a new TypeError exception.
    let exception = exception_value(vm, realm, Exception::type_error("Reader has been released"));

    // 3. Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e).
    readable_stream_default_reader_error_read_requests(vm, reader, exception);
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-reader
pub(crate) fn set_up_readable_stream_default_reader(
    cx: &CallContext,
    reader: Ref<ReadableStreamDefaultReader>,
    stream: Ref<ReadableStream>,
) -> WebIdlResult<()> {
    // 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError exception.
    if is_readable_stream_locked(stream) {
        return Err(Exception::type_error("Cannot create stream reader for a locked stream"));
    }

    // 2. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
    readable_stream_reader_generic_initialize(cx, ReadableStreamReader::ReadableStreamDefaultReader(reader), stream);

    // 3. Set reader.[[readRequests]] to a new empty list.
    reader.read_requests.borrow_mut(cx.vm()).clear();
    Ok(())
}
