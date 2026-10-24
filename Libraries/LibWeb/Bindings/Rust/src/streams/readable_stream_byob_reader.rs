/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStreamBYOBReader, https://streams.spec.whatwg.org/#byob-reader-class

use std::collections::VecDeque;

use libjs_rust::api::{GcRefCell, MarkedVec, Trace, Value, Vm};

use crate::bindings::conversions::{ArrayBufferView, BufferSource};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_rejected_promise_from_exception, exception_value,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::read_request::ReadIntoRequest;
use crate::streams::readable_byte_stream_controller::readable_byte_stream_controller_pull_into;
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamReader, ReadableStreamState, is_readable_stream_locked,
};
use crate::streams::readable_stream_generic_reader::{
    ReadableStreamGenericReader, readable_stream_reader_generic_initialize, readable_stream_reader_generic_release,
};
use crate::streams::underlying_source::ReadableStreamController;

/// The dictionary ReadableStreamBYOBReaderReadOptions of Streams/ReadableStreamBYOBReader.idl, whose members the
/// bindings convert.
pub(crate) struct ReadableStreamBYOBReaderReadOptions {
    pub(crate) min: u64,
}

// https://streams.spec.whatwg.org/#readablestreambyobreader
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableStreamBYOBReader {
    generic: ReadableStreamGenericReader,
    // https://streams.spec.whatwg.org/#readablestreambyobreader-readintorequests
    read_into_requests: GcRefCell<VecDeque<ReadIntoRequest>>,
}

impl Wrappable for ReadableStreamBYOBReader {}

impl ReadableStreamBYOBReader {
    pub(crate) fn new() -> Self {
        Self {
            generic: ReadableStreamGenericReader::default(),
            read_into_requests: GcRefCell::new(VecDeque::new()),
        }
    }

    pub(crate) fn generic(&self) -> &ReadableStreamGenericReader {
        &self.generic
    }

    pub(crate) fn append_read_into_request(&self, read_into_request: ReadIntoRequest) {
        self.read_into_requests.borrow_mut().push_back(read_into_request);
    }

    /// Takes the reader's [[readIntoRequests]], leaving an empty list.
    pub(crate) fn take_read_into_requests<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, ReadIntoRequest> {
        let read_into_requests = MarkedVec::with_capacity(vm, self.read_into_requests.borrow().len());
        for read_into_request in self.read_into_requests.borrow().iter() {
            read_into_requests.push(read_into_request.clone());
        }
        self.read_into_requests.borrow_mut().clear();
        read_into_requests
    }

    /// Removes the first of the reader's [[readIntoRequests]], which are not empty.
    pub(crate) fn take_first_read_into_request(&self) -> ReadIntoRequest {
        self.read_into_requests
            .borrow_mut()
            .pop_front()
            .expect("the reader has a read-into request")
    }

    pub(crate) fn read_into_requests_len(&self) -> usize {
        self.read_into_requests.borrow().len()
    }

    pub(crate) fn read_into_requests_is_empty(&self) -> bool {
        self.read_into_requests.borrow().is_empty()
    }

    // https://streams.spec.whatwg.org/#byob-reader-constructor
    pub(crate) fn construct(cx: &mut CallContext, stream: Ref<ReadableStream>) -> WebIdlResult<Ref<Self>> {
        let reader = allocate(cx.vm(), Self::new());

        // 1. Perform ? SetUpReadableStreamBYOBReader(this, stream).
        set_up_readable_stream_byob_reader(cx, reader, stream)?;
        Ok(reader)
    }

    // https://streams.spec.whatwg.org/#generic-reader-closed
    pub(crate) fn closed(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        self.generic.closed()
    }

    // https://streams.spec.whatwg.org/#byob-reader-read
    pub(crate) fn read(
        this: &Ref<Self>,
        cx: &mut CallContext,
        view: ArrayBufferView,
        options: ReadableStreamBYOBReaderReadOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        let cx = &this.generic.context(cx.vm());
        let vm = cx.vm();
        let extent = BufferSource::from(view).extent();

        // 1. If view.[[ByteLength]] is 0, return a promise rejected with a TypeError exception.
        if extent.byte_length == 0 {
            let exception = Exception::type_error("Cannot read in an empty buffer");
            return Ok(create_rejected_promise_from_exception(vm, cx.realm(), exception));
        }

        // 2. If view.[[ViewedArrayBuffer]].[[ByteLength]] is 0, return a promise rejected with a TypeError
        //    exception.
        if extent.buffer.byte_length() == 0 {
            let exception = Exception::type_error("Cannot read in an empty buffer");
            return Ok(create_rejected_promise_from_exception(vm, cx.realm(), exception));
        }

        // 3. If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, return a promise rejected with a TypeError
        //    exception.
        if extent.buffer.is_detached() {
            let exception = Exception::type_error("Cannot read in a detached buffer");
            return Ok(create_rejected_promise_from_exception(vm, cx.realm(), exception));
        }

        // 4. If options["min"] is 0, return a promise rejected with a TypeError exception.
        if options.min == 0 {
            let exception = Exception::type_error("options[\"min\"] cannot have a value of 0.");
            return Ok(create_rejected_promise_from_exception(vm, cx.realm(), exception));
        }

        match view {
            // 5. If view has a [[TypedArrayName]] internal slot,
            ArrayBufferView::TypedArray(_) => {
                // 1. If options["min"] > view.[[ArrayLength]], return a promise rejected with a RangeError exception.
                let array_length = extent.byte_length / extent.element_size;
                if options.min > array_length as u64 {
                    let exception =
                        Exception::range_error("options[\"min\"] cannot be larger than the length of the view.");
                    return Ok(create_rejected_promise_from_exception(vm, cx.realm(), exception));
                }
            }
            // 6. Otherwise (i.e., it is a DataView),
            ArrayBufferView::DataView(_) => {
                // 1. If options["min"] > view.[[ByteLength]], return a promise rejected with a RangeError exception.
                if options.min > extent.byte_length as u64 {
                    let exception =
                        Exception::range_error("options[\"min\"] cannot be larger than the length of the view.");
                    return Ok(create_rejected_promise_from_exception(vm, cx.realm(), exception));
                }
            }
        }

        // 7. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
        if this.generic.stream().is_none() {
            let exception = Exception::type_error("Cannot read from an empty stream");
            return Ok(create_rejected_promise_from_exception(vm, cx.realm(), exception));
        }

        // 8. Let promise be a new promise.
        let promise = create_promise(vm, cx.realm());

        // 9. Let readIntoRequest be a new read-into request with the following items:
        //    chunk steps, given chunk
        //        1. Resolve promise with «[ "value" → chunk, "done" → false ]».
        //    close steps, given chunk
        //        1. Resolve promise with «[ "value" → chunk, "done" → true ]».
        //    error steps, given e
        //        1. Reject promise with e.
        let read_into_request = ReadIntoRequest::BYOBReaderRead(promise);

        // 10. Perform ! ReadableStreamBYOBReaderRead(this, view, options["min"], readIntoRequest).
        readable_stream_byob_reader_read(vm, *this, view, options.min, read_into_request);

        // 11. Return promise.
        Ok(promise)
    }

    // https://streams.spec.whatwg.org/#byob-reader-release-lock
    pub(crate) fn release_lock(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. If this.[[stream]] is undefined, return.
        if this.generic.stream().is_none() {
            return Ok(());
        }

        // 2. Perform ! ReadableStreamBYOBReaderRelease(this).
        readable_stream_byob_reader_release(cx.vm(), *this);
        Ok(())
    }

    // https://streams.spec.whatwg.org/#generic-reader-cancel
    pub(crate) fn cancel(this: &Ref<Self>, cx: &mut CallContext, reason: Option<Value>) -> WebIdlResult<WebIdlPromise> {
        let reader = ReadableStreamReader::ReadableStreamBYOBReader(*this);
        Ok(ReadableStreamGenericReader::cancel(cx.vm(), reader, reason))
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreadererrorreadintorequests
pub(crate) fn readable_stream_byob_reader_error_read_into_requests(
    vm: &Vm,
    reader: Ref<ReadableStreamBYOBReader>,
    error: Value,
) {
    // 1. Let readIntoRequests be reader.[[readIntoRequests]].
    // 2. Set reader.[[readIntoRequests]] to a new empty list.
    let read_into_requests = reader.take_read_into_requests(vm);

    // 3. For each readIntoRequest of readIntoRequests,
    for index in 0..read_into_requests.len() {
        // 1. Perform readIntoRequest’s error steps, given e.
        read_into_requests
            .get(index)
            .expect("the index is in bounds")
            .error_steps(vm, error);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
pub(crate) fn readable_stream_byob_reader_read(
    vm: &Vm,
    reader: Ref<ReadableStreamBYOBReader>,
    view: ArrayBufferView,
    min: u64,
    read_into_request: ReadIntoRequest,
) {
    let cx = &reader.generic.context(vm);

    // 1. Let stream be reader.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = reader.generic.stream().expect("a reader that reads has a stream");

    // 3. Set stream.[[disturbed]] to true.
    stream.set_disturbed(true);

    // 4. If stream.[[state]] is "errored", perform readIntoRequest’s error steps given stream.[[storedError]].
    if stream.state() == ReadableStreamState::Errored {
        read_into_request.error_steps(vm, stream.stored_error());
    }
    // 5. Otherwise, perform ! ReadableByteStreamControllerPullInto(stream.[[controller]], view, min, readIntoRequest).
    else {
        let Some(ReadableStreamController::ReadableByteStreamController(controller)) = stream.controller() else {
            panic!("a stream with a BYOB reader has a ReadableByteStreamController");
        };
        readable_byte_stream_controller_pull_into(cx, controller, view, min, read_into_request);
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreaderrelease
pub(crate) fn readable_stream_byob_reader_release(vm: &Vm, reader: Ref<ReadableStreamBYOBReader>) {
    let realm = reader.generic.closed_promise_realm();

    // 1. Perform ! ReadableStreamReaderGenericRelease(reader).
    readable_stream_reader_generic_release(vm, ReadableStreamReader::ReadableStreamBYOBReader(reader));

    // 2. Let e be a new TypeError exception.
    let exception = exception_value(vm, realm, Exception::type_error("Reader has been released"));

    // 3. Perform ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
    readable_stream_byob_reader_error_read_into_requests(vm, reader, exception);
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
pub(crate) fn set_up_readable_stream_byob_reader(
    cx: &CallContext,
    reader: Ref<ReadableStreamBYOBReader>,
    stream: Ref<ReadableStream>,
) -> WebIdlResult<()> {
    // 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError exception.
    if is_readable_stream_locked(stream) {
        return Err(Exception::type_error("Cannot create stream reader for a locked stream"));
    }

    // 2. If stream.[[controller]] does not implement ReadableByteStreamController, throw a TypeError exception.
    if !matches!(
        stream.controller(),
        Some(ReadableStreamController::ReadableByteStreamController(_))
    ) {
        return Err(Exception::type_error(
            "BYOB reader cannot set up reader from non-byte stream",
        ));
    }

    // 3. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
    readable_stream_reader_generic_initialize(cx, ReadableStreamReader::ReadableStreamBYOBReader(reader), stream);

    // 4. Set reader.[[readIntoRequests]] to a new empty list.
    reader.read_into_requests.borrow_mut().clear();
    Ok(())
}
