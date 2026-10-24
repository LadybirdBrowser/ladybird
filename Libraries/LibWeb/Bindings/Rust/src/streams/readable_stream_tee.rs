/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStreamTee, https://streams.spec.whatwg.org/#readable-stream-tee, and the state that the algorithms and read
//! requests of a tee share.

use core::cell::Cell;

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Value, Vm};

use crate::bindings::conversions::{ArrayBufferView, BufferSource, buffer_source_type_of};
use crate::bindings::interface::{Ref, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_resolved_promise, exception_value, promise_realm, queue_a_microtask,
    resolve_promise, upon_rejection,
};
use crate::bindings::{CallContext, MustNotThrow, WebIdlResult};
use crate::streams::abstract_operations::{clone_as_uint8_array, structured_clone};
use crate::streams::algorithms::{CancelAlgorithm, PullAlgorithm, StartAlgorithm};
use crate::streams::read_request::{ReadIntoRequest, ReadRequest};
use crate::streams::readable_byte_stream_controller::{
    ReadableByteStreamController, readable_byte_stream_controller_close, readable_byte_stream_controller_enqueue,
    readable_byte_stream_controller_error, readable_byte_stream_controller_get_byob_request,
    readable_byte_stream_controller_respond, readable_byte_stream_controller_respond_with_new_view,
};
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamReader, acquire_readable_stream_byob_reader, acquire_readable_stream_default_reader,
    create_array_from_list, create_readable_byte_stream, create_readable_stream, readable_stream_cancel,
};
use crate::streams::readable_stream_byob_reader::{
    readable_stream_byob_reader_read, readable_stream_byob_reader_release,
};
use crate::streams::readable_stream_default_controller::{
    ReadableStreamDefaultController, readable_stream_default_controller_close,
    readable_stream_default_controller_enqueue, readable_stream_default_controller_error,
};
use crate::streams::readable_stream_default_reader::{
    ReadableStreamDefaultReader, readable_stream_default_reader_read, readable_stream_default_reader_release,
};
use crate::streams::underlying_source::ReadableStreamController;

/// The variables of ReadableStreamDefaultTee that its algorithms and its read request share,
/// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaulttee
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DefaultTeeState {
    stream: Ref<ReadableStream>,
    reader: Ref<ReadableStreamDefaultReader>,
    reading: Cell<bool>,
    read_again: Cell<bool>,
    canceled1: Cell<bool>,
    canceled2: Cell<bool>,
    reason1: Cell<Value>,
    reason2: Cell<Value>,
    branch1: Cell<Option<Ref<ReadableStream>>>,
    branch2: Cell<Option<Ref<ReadableStream>>>,
    cancel_promise: WebIdlPromise,
    #[gc(untraced)]
    clone_for_branch2: bool,
}

static DEFAULT_TEE_STATE_CLASS: EmbedderClass<DefaultTeeState> = EmbedderClass::new("ReadableStreamDefaultTee");

impl EmbedderValue for DefaultTeeState {
    const CLASS: &'static EmbedderClass<Self> = &DEFAULT_TEE_STATE_CLASS;
}

impl DefaultTeeState {
    /// The context of the realm of cancelPromise, which the read request runs in.
    fn context<'vm>(&self, vm: &'vm Vm) -> CallContext<'vm> {
        CallContext::new(vm, promise_realm(self.cancel_promise))
    }

    /// branch1.[[controller]] and branch2.[[controller]].
    fn controllers(
        &self,
    ) -> (
        Ref<ReadableStreamDefaultController>,
        Ref<ReadableStreamDefaultController>,
    ) {
        let branch1 = self.branch1.get().expect("the tee has created its branches");
        let branch2 = self.branch2.get().expect("the tee has created its branches");
        (default_controller_of(branch1), default_controller_of(branch2))
    }

    /// The pullAlgorithm of the tee.
    pub(crate) fn pull_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext) -> WebIdlPromise {
        // 1. If reading is true,
        if this.reading.get() {
            // 1. Set readAgain to true.
            this.read_again.set(true);

            // 2. Return a promise resolved with undefined.
            return create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED);
        }

        // 2. Set reading to true.
        this.reading.set(true);

        // 3. Let readRequest be a read request with the following items:
        let read_request = ReadRequest::DefaultTee(this);

        // 4. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        readable_stream_default_reader_read(cx.vm(), this.reader, read_request);

        // 5. Return a promise resolved with undefined.
        create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
    }

    /// The cancel1Algorithm of the tee.
    pub(crate) fn cancel1_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Set canceled1 to true.
        this.canceled1.set(true);

        // 2. Set reason1 to reason.
        this.reason1.set(reason);

        // 3. If canceled2 is true,
        if this.canceled2.get() {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            let composite_reason = create_array_from_list(cx, &[this.reason1.get(), this.reason2.get()]);

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            let cancel_result = readable_stream_cancel(cx, this.stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            resolve_promise(
                cx.vm(),
                this.cancel_promise,
                Value::from_object(cancel_result.promise()),
            );
        }

        // 4. Return cancelPromise.
        this.cancel_promise
    }

    /// The cancel2Algorithm of the tee.
    pub(crate) fn cancel2_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Set canceled2 to true.
        this.canceled2.set(true);

        // 2. Set reason2 to reason.
        this.reason2.set(reason);

        // 3. If canceled1 is true,
        if this.canceled1.get() {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            let composite_reason = create_array_from_list(cx, &[this.reason1.get(), this.reason2.get()]);

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            let cancel_result = readable_stream_cancel(cx, this.stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            resolve_promise(
                cx.vm(),
                this.cancel_promise,
                Value::from_object(cancel_result.promise()),
            );
        }

        // 4. Return cancelPromise.
        this.cancel_promise
    }

    // chunk steps, given chunk
    pub(crate) fn read_request_chunk_steps(this: Gc<EmbedderCell<Self>>, vm: &Vm, chunk: Value) {
        // 1. Queue a microtask to perform the following steps:
        queue_a_microtask(
            vm,
            promise_realm(this.cancel_promise),
            (this, chunk),
            |vm, (this, chunk)| {
                with_temporary_execution_context_and_callbacks(promise_realm(this.cancel_promise), || {
                    let cx = &this.context(vm);
                    let (controller1, controller2) = this.controllers();

                    // 1. Set readAgain to false.
                    this.read_again.set(false);

                    // 2. Let chunk1 and chunk2 be chunk.
                    let chunk1 = chunk;
                    let mut chunk2 = chunk;

                    // 3. If canceled2 is false and cloneForBranch2 is true,
                    if !this.canceled2.get() && this.clone_for_branch2 {
                        // 1. Let cloneResult be StructuredClone(chunk2).
                        let clone_result = structured_clone(cx, chunk2);

                        match clone_result {
                            // 2. If cloneResult is an abrupt completion,
                            Err(exception) => {
                                let error = exception_value(vm, cx.realm(), exception);

                                // 1. Perform ! ReadableStreamDefaultControllerError(branch1.[[controller]],
                                //    cloneResult.[[Value]]).
                                readable_stream_default_controller_error(cx, controller1, error);

                                // 2. Perform ! ReadableStreamDefaultControllerError(branch2.[[controller]],
                                //    cloneResult.[[Value]]).
                                readable_stream_default_controller_error(cx, controller2, error);

                                // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                                let cancel_result = readable_stream_cancel(cx, this.stream, error);

                                // Note: We need to manually convert the result to an ECMAScript value here, by extracting
                                //       its [[Promise]] slot.
                                resolve_promise(vm, this.cancel_promise, Value::from_object(cancel_result.promise()));

                                // 4. Return.
                                return;
                            }
                            // 3. Otherwise, set chunk2 to cloneResult.[[Value]].
                            Ok(value) => chunk2 = value,
                        }
                    }

                    // 4. If canceled1 is false, perform ! ReadableStreamDefaultControllerEnqueue(branch1.[[controller]],
                    //    chunk1).
                    if !this.canceled1.get() {
                        readable_stream_default_controller_enqueue(cx, controller1, chunk1).must();
                    }

                    // 5. If canceled2 is false, perform ! ReadableStreamDefaultControllerEnqueue(branch2.[[controller]],
                    //    chunk2).
                    if !this.canceled2.get() {
                        readable_stream_default_controller_enqueue(cx, controller2, chunk2).must();
                    }

                    // 6. Set reading to false.
                    this.reading.set(false);

                    // 7. If readAgain is true, perform pullAlgorithm.
                    if this.read_again.get() {
                        Self::pull_algorithm(this, cx);
                    }
                });
            },
        );

        // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
        //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
        //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
    }

    // close steps
    pub(crate) fn read_request_close_steps(this: Gc<EmbedderCell<Self>>, vm: &Vm) {
        let cx = &this.context(vm);
        let (controller1, controller2) = this.controllers();

        // 1. Set reading to false.
        this.reading.set(false);

        // 2. If canceled1 is false, perform ! ReadableStreamDefaultControllerClose(branch1.[[controller]]).
        if !this.canceled1.get() {
            readable_stream_default_controller_close(cx, controller1);
        }

        // 3. If canceled2 is false, perform ! ReadableStreamDefaultControllerClose(branch2.[[controller]]).
        if !this.canceled2.get() {
            readable_stream_default_controller_close(cx, controller2);
        }

        // 4. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
        if !this.canceled1.get() || !this.canceled2.get() {
            resolve_promise(vm, this.cancel_promise, Value::UNDEFINED);
        }
    }

    // error steps
    pub(crate) fn read_request_error_steps(this: Gc<EmbedderCell<Self>>, _: &Vm, _: Value) {
        // 1. Set reading to false.
        this.reading.set(false);
    }
}

/// The variables of ReadableByteStreamTee that its algorithms and its read requests share,
/// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ByteTeeState {
    stream: Ref<ReadableStream>,
    reader: Cell<ReadableStreamReader>,
    reading: Cell<bool>,
    read_again_for_branch1: Cell<bool>,
    read_again_for_branch2: Cell<bool>,
    canceled1: Cell<bool>,
    canceled2: Cell<bool>,
    reason1: Cell<Value>,
    reason2: Cell<Value>,
    branch1: Cell<Option<Ref<ReadableStream>>>,
    branch2: Cell<Option<Ref<ReadableStream>>>,
    cancel_promise: WebIdlPromise,
}

static BYTE_TEE_STATE_CLASS: EmbedderClass<ByteTeeState> = EmbedderClass::new("ReadableByteStreamTee");

impl EmbedderValue for ByteTeeState {
    const CLASS: &'static EmbedderClass<Self> = &BYTE_TEE_STATE_CLASS;
}

impl ByteTeeState {
    /// The context of the realm of cancelPromise, which the read requests run in.
    fn context<'vm>(&self, vm: &'vm Vm) -> CallContext<'vm> {
        CallContext::new(vm, promise_realm(self.cancel_promise))
    }

    fn branch1(&self) -> Ref<ReadableStream> {
        self.branch1.get().expect("the tee has created its branches")
    }

    fn branch2(&self) -> Ref<ReadableStream> {
        self.branch2.get().expect("the tee has created its branches")
    }

    /// branch1.[[controller]] and branch2.[[controller]].
    fn controllers(&self) -> (Ref<ReadableByteStreamController>, Ref<ReadableByteStreamController>) {
        (byte_controller_of(self.branch1()), byte_controller_of(self.branch2()))
    }

    // forwardReaderError, given thisReader
    fn forward_reader_error(this: Gc<EmbedderCell<Self>>, cx: &CallContext, this_reader: ReadableStreamReader) {
        // 1. Upon rejection of thisReader.[[closedPromise]] with reason r,
        let closed_promise = this_reader
            .generic()
            .closed_promise()
            .expect("a reader of a stream has a closed promise");

        upon_rejection(
            cx.vm(),
            closed_promise,
            (this, this_reader),
            |cx, (this, this_reader), reason| {
                let (controller1, controller2) = this.controllers();

                // 1. If thisReader is not reader, return.
                if this_reader != this.reader.get() {
                    return Ok(Value::UNDEFINED);
                }

                // 2. Perform ! ReadableByteStreamControllerError(branch1.[[controller]], r).
                readable_byte_stream_controller_error(cx, controller1, reason);

                // 3. Perform ! ReadableByteStreamControllerError(branch2.[[controller]], r).
                readable_byte_stream_controller_error(cx, controller2, reason);

                // 4. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
                if !this.canceled1.get() || !this.canceled2.get() {
                    resolve_promise(cx.vm(), this.cancel_promise, Value::UNDEFINED);
                }

                Ok(Value::UNDEFINED)
            },
        );
    }

    // pullWithDefaultReader
    fn pull_with_default_reader(this: Gc<EmbedderCell<Self>>, cx: &CallContext) {
        // 1. If reader implements ReadableStreamBYOBReader,
        if let ReadableStreamReader::ReadableStreamBYOBReader(byob_reader) = this.reader.get() {
            // 1. Assert: reader.[[readIntoRequests]] is empty.
            assert!(byob_reader.read_into_requests_is_empty());

            // 2. Perform ! ReadableStreamBYOBReaderRelease(reader).
            readable_stream_byob_reader_release(cx.vm(), byob_reader);

            // 3. Set reader to ! AcquireReadableStreamDefaultReader(stream).
            let reader = acquire_readable_stream_default_reader(cx, this.stream).must();
            this.reader
                .set(ReadableStreamReader::ReadableStreamDefaultReader(reader));

            // 4. Perform forwardReaderError, given reader.
            Self::forward_reader_error(this, cx, this.reader.get());
        }

        // 2. Let readRequest be a read request with the following items:
        let read_request = ReadRequest::ByteTee(this);

        // 3. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        let ReadableStreamReader::ReadableStreamDefaultReader(reader) = this.reader.get() else {
            unreachable!("the reader is a ReadableStreamDefaultReader");
        };
        readable_stream_default_reader_read(cx.vm(), reader, read_request);
    }

    // pullWithBYOBReader, given view and forBranch2
    fn pull_with_byob_reader(this: Gc<EmbedderCell<Self>>, cx: &CallContext, view: ArrayBufferView, for_branch2: bool) {
        // 1. If reader implements ReadableStreamDefaultReader,
        if let ReadableStreamReader::ReadableStreamDefaultReader(default_reader) = this.reader.get() {
            // 2. Assert: reader.[[readRequests]] is empty.
            assert!(default_reader.read_requests_is_empty());

            // 3. Perform ! ReadableStreamDefaultReaderRelease(reader).
            readable_stream_default_reader_release(cx.vm(), default_reader);

            // 4. Set reader to ! AcquireReadableStreamBYOBReader(stream).
            let reader = acquire_readable_stream_byob_reader(cx, this.stream).must();
            this.reader.set(ReadableStreamReader::ReadableStreamBYOBReader(reader));

            // 5. Perform forwardReaderError, given reader.
            Self::forward_reader_error(this, cx, this.reader.get());
        }

        // 2. Let byobBranch be branch2 if forBranch2 is true, and branch1 otherwise.
        let byob_branch = if for_branch2 { this.branch2() } else { this.branch1() };

        // 3. Let otherBranch be branch2 if forBranch2 is false, and branch1 otherwise.
        let other_branch = if !for_branch2 { this.branch2() } else { this.branch1() };

        // 4. Let readIntoRequest be a read-into request with the following items:
        let read_into_request = ReadIntoRequest::ByteTee(BYTE_TEE_BYOB_READ_REQUEST_CLASS.allocate(
            cx.vm(),
            ByteTeeBYOBReadRequest {
                state: this,
                byob_branch,
                other_branch,
                for_branch2,
            },
        ));

        // 5. Perform ! ReadableStreamBYOBReaderRead(reader, view, 1, readIntoRequest).
        let ReadableStreamReader::ReadableStreamBYOBReader(reader) = this.reader.get() else {
            unreachable!("the reader is a ReadableStreamBYOBReader");
        };
        readable_stream_byob_reader_read(cx.vm(), reader, view, 1, read_into_request);
    }

    /// The pull1Algorithm of the tee.
    pub(crate) fn pull1_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext) -> WebIdlPromise {
        // 1. If reading is true,
        if this.reading.get() {
            // 1. Set readAgainForBranch1 to true.
            this.read_again_for_branch1.set(true);

            // 2. Return a promise resolved with undefined.
            return create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED);
        }

        // 2. Set reading to true.
        this.reading.set(true);

        // 3. Let byobRequest be ! ReadableByteStreamControllerGetBYOBRequest(branch1.[[controller]]).
        let byob_request = readable_byte_stream_controller_get_byob_request(cx, byte_controller_of(this.branch1()));

        match byob_request {
            // 4. If byobRequest is null, perform pullWithDefaultReader.
            None => Self::pull_with_default_reader(this, cx),
            // 5. Otherwise, perform pullWithBYOBReader, given byobRequest.[[view]] and false.
            Some(byob_request) => {
                let view = byob_request
                    .current_view()
                    .expect("a BYOB request of a controller has a view");
                Self::pull_with_byob_reader(this, cx, ArrayBufferView::TypedArray(view), false);
            }
        }

        // 6. Return a promise resolved with undefined.
        create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
    }

    /// The pull2Algorithm of the tee.
    pub(crate) fn pull2_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext) -> WebIdlPromise {
        // 1. If reading is true,
        if this.reading.get() {
            // 1. Set readAgainForBranch2 to true.
            this.read_again_for_branch2.set(true);

            // 2. Return a promise resolved with undefined.
            return create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED);
        }

        // 2. Set reading to true.
        this.reading.set(true);

        // 3. Let byobRequest be ! ReadableByteStreamControllerGetBYOBRequest(branch2.[[controller]]).
        let byob_request = readable_byte_stream_controller_get_byob_request(cx, byte_controller_of(this.branch2()));

        match byob_request {
            // 4. If byobRequest is null, perform pullWithDefaultReader.
            None => Self::pull_with_default_reader(this, cx),
            // 5. Otherwise, perform pullWithBYOBReader, given byobRequest.[[view]] and true.
            Some(byob_request) => {
                let view = byob_request
                    .current_view()
                    .expect("a BYOB request of a controller has a view");
                Self::pull_with_byob_reader(this, cx, ArrayBufferView::TypedArray(view), true);
            }
        }

        // 6. Return a promise resolved with undefined.
        create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
    }

    /// The cancel1Algorithm of the tee.
    pub(crate) fn cancel1_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Set canceled1 to true.
        this.canceled1.set(true);

        // 2. Set reason1 to reason.
        this.reason1.set(reason);

        // 3. If canceled2 is true,
        if this.canceled2.get() {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            let composite_reason = create_array_from_list(cx, &[this.reason1.get(), this.reason2.get()]);

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            let cancel_result = readable_stream_cancel(cx, this.stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            resolve_promise(
                cx.vm(),
                this.cancel_promise,
                Value::from_object(cancel_result.promise()),
            );
        }

        // 4. Return cancelPromise.
        this.cancel_promise
    }

    /// The cancel2Algorithm of the tee.
    pub(crate) fn cancel2_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Set canceled2 to true.
        this.canceled2.set(true);

        // 2. Set reason2 to reason.
        this.reason2.set(reason);

        // 3. If canceled1 is true,
        if this.canceled1.get() {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            let composite_reason = create_array_from_list(cx, &[this.reason1.get(), this.reason2.get()]);

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            let cancel_result = readable_stream_cancel(cx, this.stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            resolve_promise(
                cx.vm(),
                this.cancel_promise,
                Value::from_object(cancel_result.promise()),
            );
        }

        // 4. Return cancelPromise.
        this.cancel_promise
    }

    // chunk steps, given chunk, of the read request of pullWithDefaultReader
    pub(crate) fn default_read_request_chunk_steps(this: Gc<EmbedderCell<Self>>, vm: &Vm, chunk: Value) {
        // 1. Queue a microtask to perform the following steps:
        queue_a_microtask(
            vm,
            promise_realm(this.cancel_promise),
            (this, chunk),
            |vm, (this, chunk)| {
                with_temporary_execution_context_and_callbacks(promise_realm(this.cancel_promise), || {
                    let cx = &this.context(vm);
                    let (controller1, controller2) = this.controllers();

                    // 1. Set readAgainForBranch1 to false.
                    this.read_again_for_branch1.set(false);

                    // 2. Set readAgainForBranch2 to false.
                    this.read_again_for_branch2.set(false);

                    // 3. Let chunk1 and chunk2 be chunk.
                    let chunk1 = chunk;
                    let mut chunk2 = chunk;

                    // 4. If canceled1 is false and canceled2 is false,
                    if !this.canceled1.get() && !this.canceled2.get() {
                        // 1. Let cloneResult be CloneAsUint8Array(chunk).
                        let clone_result = clone_as_uint8_array(cx, array_buffer_view_of(chunk));

                        match clone_result {
                            // 2. If cloneResult is an abrupt completion,
                            Err(exception) => {
                                let error = exception_value(vm, cx.realm(), exception);

                                // 1. Perform ! ReadableByteStreamControllerError(branch1.[[controller]],
                                //    cloneResult.[[Value]]).
                                readable_byte_stream_controller_error(cx, controller1, error);

                                // 2. Perform ! ReadableByteStreamControllerError(branch2.[[controller]],
                                //    cloneResult.[[Value]]).
                                readable_byte_stream_controller_error(cx, controller2, error);

                                // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                                let cancel_result = readable_stream_cancel(cx, this.stream, error);
                                resolve_promise(vm, this.cancel_promise, Value::from_object(cancel_result.promise()));

                                // 4. Return.
                                return;
                            }
                            // 3. Otherwise, set chunk2 to cloneResult.[[Value]].
                            Ok(value) => chunk2 = value,
                        }
                    }

                    // 5. If canceled1 is false, perform ! ReadableByteStreamControllerEnqueue(branch1.[[controller]],
                    //    chunk1).
                    if !this.canceled1.get() {
                        readable_byte_stream_controller_enqueue(cx, controller1, array_buffer_view_of(chunk1)).must();
                    }

                    // 6. If canceled2 is false, perform ! ReadableByteStreamControllerEnqueue(branch2.[[controller]],
                    //    chunk2).
                    if !this.canceled2.get() {
                        readable_byte_stream_controller_enqueue(cx, controller2, array_buffer_view_of(chunk2)).must();
                    }

                    // 7. Set reading to false.
                    this.reading.set(false);

                    // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
                    if this.read_again_for_branch1.get() {
                        Self::pull1_algorithm(this, cx);
                    }
                    // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
                    else if this.read_again_for_branch2.get() {
                        Self::pull2_algorithm(this, cx);
                    }
                });
            },
        );

        // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
        //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
        //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
    }

    // close steps, of the read request of pullWithDefaultReader
    pub(crate) fn default_read_request_close_steps(this: Gc<EmbedderCell<Self>>, vm: &Vm) {
        let cx = &this.context(vm);
        let (controller1, controller2) = this.controllers();

        // 1. Set reading to false.
        this.reading.set(false);

        // 2. If canceled1 is false, perform ! ReadableByteStreamControllerClose(branch1.[[controller]]).
        if !this.canceled1.get() {
            readable_byte_stream_controller_close(cx, controller1).must();
        }

        // 3. If canceled2 is false, perform ! ReadableByteStreamControllerClose(branch2.[[controller]]).
        if !this.canceled2.get() {
            readable_byte_stream_controller_close(cx, controller2).must();
        }

        // 4. If branch1.[[controller]].[[pendingPullIntos]] is not empty, perform
        //    ! ReadableByteStreamControllerRespond(branch1.[[controller]], 0).
        if !controller1.pending_pull_intos_is_empty() {
            readable_byte_stream_controller_respond(cx, controller1, 0).must();
        }

        // 5. If branch2.[[controller]].[[pendingPullIntos]] is not empty, perform
        //    ! ReadableByteStreamControllerRespond(branch2.[[controller]], 0).
        if !controller2.pending_pull_intos_is_empty() {
            readable_byte_stream_controller_respond(cx, controller2, 0).must();
        }

        // 6. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
        if !this.canceled1.get() || !this.canceled2.get() {
            resolve_promise(vm, this.cancel_promise, Value::UNDEFINED);
        }
    }

    // error steps, of the read request of pullWithDefaultReader
    pub(crate) fn default_read_request_error_steps(this: Gc<EmbedderCell<Self>>, _: &Vm, _: Value) {
        // 1. Set reading to false.
        this.reading.set(false);
    }
}

/// The read-into request of ReadableByteStreamTee's pullWithBYOBReader.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ByteTeeBYOBReadRequest {
    state: Gc<EmbedderCell<ByteTeeState>>,
    byob_branch: Ref<ReadableStream>,
    other_branch: Ref<ReadableStream>,
    #[gc(untraced)]
    for_branch2: bool,
}

static BYTE_TEE_BYOB_READ_REQUEST_CLASS: EmbedderClass<ByteTeeBYOBReadRequest> =
    EmbedderClass::new("ReadableByteStreamTeeBYOBReadRequest");

impl EmbedderValue for ByteTeeBYOBReadRequest {
    const CLASS: &'static EmbedderClass<Self> = &BYTE_TEE_BYOB_READ_REQUEST_CLASS;
}

impl ByteTeeBYOBReadRequest {
    // chunk steps, given chunk
    pub(crate) fn chunk_steps(&self, vm: &Vm, chunk: Value) {
        let captures = (self.state, self.byob_branch, self.other_branch, self.for_branch2, chunk);

        // 1. Queue a microtask to perform the following steps:
        queue_a_microtask(
            vm,
            promise_realm(self.state.cancel_promise),
            captures,
            |vm, (state, byob_branch, other_branch, for_branch2, chunk)| {
                with_temporary_execution_context_and_callbacks(promise_realm(state.cancel_promise), || {
                    let cx = &state.context(vm);
                    let chunk = array_buffer_view_of(chunk);
                    let byob_controller = byte_controller_of(byob_branch);
                    let other_controller = byte_controller_of(other_branch);

                    // 1. Set readAgainForBranch1 to false.
                    state.read_again_for_branch1.set(false);

                    // 2. Set readAgainForBranch2 to false.
                    state.read_again_for_branch2.set(false);

                    // 3. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1 otherwise.
                    let byob_canceled = if for_branch2 {
                        state.canceled2.get()
                    } else {
                        state.canceled1.get()
                    };

                    // 4. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1 otherwise.
                    let other_canceled = if !for_branch2 {
                        state.canceled2.get()
                    } else {
                        state.canceled1.get()
                    };

                    // 5. If otherCanceled is false,
                    if !other_canceled {
                        // 1. Let cloneResult be CloneAsUint8Array(chunk).
                        let clone_result = clone_as_uint8_array(cx, chunk);

                        // 2. If cloneResult is an abrupt completion,
                        let cloned_chunk = match clone_result {
                            Err(exception) => {
                                let error = exception_value(vm, cx.realm(), exception);

                                // 1. Perform ! ReadableByteStreamControllerError(byobBranch.[[controller]],
                                //    cloneResult.[[Value]]).
                                readable_byte_stream_controller_error(cx, byob_controller, error);

                                // 2. Perform ! ReadableByteStreamControllerError(otherBranch.[[controller]],
                                //    cloneResult.[[Value]]).
                                readable_byte_stream_controller_error(cx, other_controller, error);

                                // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                                let cancel_result = readable_stream_cancel(cx, state.stream, error);
                                resolve_promise(vm, state.cancel_promise, Value::from_object(cancel_result.promise()));

                                // 4. Return.
                                return;
                            }
                            // 3. Otherwise, let clonedChunk be cloneResult.[[Value]].
                            Ok(cloned_chunk) => cloned_chunk,
                        };

                        // 4. If byobCanceled is false, perform
                        //    ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
                        if !byob_canceled {
                            readable_byte_stream_controller_respond_with_new_view(cx, byob_controller, chunk).must();
                        }

                        // 5. Perform ! ReadableByteStreamControllerEnqueue(otherBranch.[[controller]], clonedChunk).
                        readable_byte_stream_controller_enqueue(
                            cx,
                            other_controller,
                            array_buffer_view_of(cloned_chunk),
                        )
                        .must();
                    }
                    // 6. Otherwise, if byobCanceled is false, perform
                    //    ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
                    else if !byob_canceled {
                        readable_byte_stream_controller_respond_with_new_view(cx, byob_controller, chunk).must();
                    }

                    // 7. Set reading to false.
                    state.reading.set(false);

                    // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
                    if state.read_again_for_branch1.get() {
                        ByteTeeState::pull1_algorithm(state, cx);
                    }
                    // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
                    else if state.read_again_for_branch2.get() {
                        ByteTeeState::pull2_algorithm(state, cx);
                    }
                });
            },
        );

        // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
        //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
        //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
    }

    // close steps, given chunk
    pub(crate) fn close_steps(&self, vm: &Vm, chunk: Value) {
        let state = self.state;
        let cx = &state.context(vm);
        let byob_controller = byte_controller_of(self.byob_branch);
        let other_controller = byte_controller_of(self.other_branch);

        // 1. Set reading to false.
        state.reading.set(false);

        // 2. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1 otherwise.
        let byob_canceled = if self.for_branch2 {
            state.canceled2.get()
        } else {
            state.canceled1.get()
        };

        // 3. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1 otherwise.
        let other_canceled = if !self.for_branch2 {
            state.canceled2.get()
        } else {
            state.canceled1.get()
        };

        // 4. If byobCanceled is false, perform ! ReadableByteStreamControllerClose(byobBranch.[[controller]]).
        if !byob_canceled {
            readable_byte_stream_controller_close(cx, byob_controller).must();
        }

        // 5. If otherCanceled is false, perform ! ReadableByteStreamControllerClose(otherBranch.[[controller]]).
        if !other_canceled {
            readable_byte_stream_controller_close(cx, other_controller).must();
        }

        // 6. If chunk is not undefined,
        if !chunk.is_undefined() {
            // 1. Assert: chunk.[[ByteLength]] is 0.

            // 2. If byobCanceled is false, perform
            //    ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
            if !byob_canceled {
                readable_byte_stream_controller_respond_with_new_view(cx, byob_controller, array_buffer_view_of(chunk))
                    .must();
            }

            // 3. If otherCanceled is false and otherBranch.[[controller]].[[pendingPullIntos]] is not empty,
            //    perform ! ReadableByteStreamControllerRespond(otherBranch.[[controller]], 0).
            if !other_canceled && !other_controller.pending_pull_intos_is_empty() {
                readable_byte_stream_controller_respond(cx, other_controller, 0).must();
            }
        }

        // 7. If byobCanceled is false or otherCanceled is false, resolve cancelPromise with undefined.
        if !byob_canceled || !other_canceled {
            resolve_promise(vm, state.cancel_promise, Value::UNDEFINED);
        }
    }

    // error steps
    pub(crate) fn error_steps(&self, _: &Vm, _: Value) {
        // 1. Set reading to false.
        self.state.reading.set(false);
    }
}

/// The [[controller]] of a branch of ReadableStreamDefaultTee.
fn default_controller_of(branch: Ref<ReadableStream>) -> Ref<ReadableStreamDefaultController> {
    let Some(ReadableStreamController::ReadableStreamDefaultController(controller)) = branch.controller() else {
        panic!("a branch of a default tee has a ReadableStreamDefaultController");
    };
    controller
}

/// The [[controller]] of a branch of ReadableByteStreamTee.
fn byte_controller_of(branch: Ref<ReadableStream>) -> Ref<ReadableByteStreamController> {
    let Some(ReadableStreamController::ReadableByteStreamController(controller)) = branch.controller() else {
        panic!("a branch of a byte tee has a ReadableByteStreamController");
    };
    controller
}

/// A chunk of a byte stream, which is an ArrayBufferView, as one.
fn array_buffer_view_of(chunk: Value) -> ArrayBufferView {
    match buffer_source_type_of(chunk) {
        Some(BufferSource::TypedArray(typed_array)) => ArrayBufferView::TypedArray(typed_array),
        Some(BufferSource::DataView(view)) => ArrayBufferView::DataView(view),
        _ => panic!("a chunk of a byte stream is an ArrayBufferView"),
    }
}

// https://streams.spec.whatwg.org/#readable-stream-tee
pub(crate) fn readable_stream_tee(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    clone_for_branch2: bool,
) -> WebIdlResult<(Ref<ReadableStream>, Ref<ReadableStream>)> {
    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: cloneForBranch2 is a boolean.

    // 3. If stream.[[controller]] implements ReadableByteStreamController, return ? ReadableByteStreamTee(stream).
    if let Some(ReadableStreamController::ReadableByteStreamController(_)) = stream.controller() {
        return readable_byte_stream_tee(cx, stream);
    }

    // 4. Return ? ReadableStreamDefaultTee(stream, cloneForBranch2).
    readable_stream_default_tee(cx, stream, clone_for_branch2)
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaulttee
pub(crate) fn readable_stream_default_tee(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    clone_for_branch2: bool,
) -> WebIdlResult<(Ref<ReadableStream>, Ref<ReadableStream>)> {
    let vm = cx.vm();

    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: cloneForBranch2 is a boolean.

    // 3. Let reader be ? AcquireReadableStreamDefaultReader(stream).
    let reader = acquire_readable_stream_default_reader(cx, stream)?;

    // 4. Let reading be false.
    // 5. Let readAgain be false.
    // 6. Let canceled1 be false.
    // 7. Let canceled2 be false.
    // 8. Let reason1 be undefined.
    // 9. Let reason2 be undefined.
    // 10. Let branch1 be undefined.
    // 11. Let branch2 be undefined.
    // 12. Let cancelPromise be a new promise.
    let cancel_promise = create_promise(vm, cx.realm());
    let state = DEFAULT_TEE_STATE_CLASS.allocate(
        vm,
        DefaultTeeState {
            stream,
            reader,
            reading: Cell::new(false),
            read_again: Cell::new(false),
            canceled1: Cell::new(false),
            canceled2: Cell::new(false),
            reason1: Cell::new(Value::UNDEFINED),
            reason2: Cell::new(Value::UNDEFINED),
            branch1: Cell::new(None),
            branch2: Cell::new(None),
            cancel_promise,
            clone_for_branch2,
        },
    );

    // 13. Let pullAlgorithm be the following steps:
    // 14. Let cancel1Algorithm be the following steps, taking a reason argument:
    // 15. Let cancel2Algorithm be the following steps, taking a reason argument:
    // 16. Let startAlgorithm be an algorithm that returns undefined.

    // 17. Set branch1 to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancel1Algorithm).
    let branch1 = create_readable_stream(
        cx,
        StartAlgorithm::ReturnsUndefined,
        PullAlgorithm::DefaultTee(state),
        CancelAlgorithm::DefaultTee {
            state,
            for_branch2: false,
        },
        None,
        None,
    )
    .must();
    state.branch1.set(Some(branch1));

    // 18. Set branch2 to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancel2Algorithm).
    let branch2 = create_readable_stream(
        cx,
        StartAlgorithm::ReturnsUndefined,
        PullAlgorithm::DefaultTee(state),
        CancelAlgorithm::DefaultTee {
            state,
            for_branch2: true,
        },
        None,
        None,
    )
    .must();
    state.branch2.set(Some(branch2));

    // 19. Upon rejection of reader.[[closedPromise]] with reason r,
    let closed_promise = reader
        .generic()
        .closed_promise()
        .expect("a reader of a stream has a closed promise");
    upon_rejection(vm, closed_promise, state, |cx, state, reason| {
        let (controller1, controller2) = state.controllers();

        // 1. Perform ! ReadableStreamDefaultControllerError(branch1.[[controller]], r).
        readable_stream_default_controller_error(cx, controller1, reason);

        // 2. Perform ! ReadableStreamDefaultControllerError(branch2.[[controller]], r).
        readable_stream_default_controller_error(cx, controller2, reason);

        // 3. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
        if !state.canceled1.get() || !state.canceled2.get() {
            resolve_promise(cx.vm(), state.cancel_promise, Value::UNDEFINED);
        }

        Ok(Value::UNDEFINED)
    });

    // 20. Return « branch1, branch2 ».
    Ok((branch1, branch2))
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
pub(crate) fn readable_byte_stream_tee(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
) -> WebIdlResult<(Ref<ReadableStream>, Ref<ReadableStream>)> {
    let vm = cx.vm();

    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: stream.[[controller]] implements ReadableByteStreamController.
    assert!(matches!(
        stream.controller(),
        Some(ReadableStreamController::ReadableByteStreamController(_))
    ));

    // 3. Let reader be ? AcquireReadableStreamDefaultReader(stream).
    let reader = acquire_readable_stream_default_reader(cx, stream)?;

    // 4. Let reading be false.
    // 5. Let readAgainForBranch1 be false.
    // 6. Let readAgainForBranch2 be false.
    // 7. Let canceled1 be false.
    // 8. Let canceled2 be false.
    // 9. Let reason1 be undefined.
    // 10. Let reason2 be undefined.
    // 11. Let branch1 be undefined.
    // 12. Let branch2 be undefined.
    // 13. Let cancelPromise be a new promise.
    let cancel_promise = create_promise(vm, cx.realm());
    let state = BYTE_TEE_STATE_CLASS.allocate(
        vm,
        ByteTeeState {
            stream,
            reader: Cell::new(ReadableStreamReader::ReadableStreamDefaultReader(reader)),
            reading: Cell::new(false),
            read_again_for_branch1: Cell::new(false),
            read_again_for_branch2: Cell::new(false),
            canceled1: Cell::new(false),
            canceled2: Cell::new(false),
            reason1: Cell::new(Value::UNDEFINED),
            reason2: Cell::new(Value::UNDEFINED),
            branch1: Cell::new(None),
            branch2: Cell::new(None),
            cancel_promise,
        },
    );

    // 14. Let forwardReaderError be the following steps, taking a thisReader argument:
    // 15. Let pullWithDefaultReader be the following steps:
    // 16. Let pullWithBYOBReader be the following steps, given view and forBranch2:
    // 17. Let pull1Algorithm be the following steps:
    // 18. Let pull2Algorithm be the following steps:
    // 19. Let cancel1Algorithm be the following steps, taking a reason argument:
    // 20. Let cancel2Algorithm be the following steps, taking a reason argument:
    // 21. Let startAlgorithm be an algorithm that returns undefined.

    // 22. Set branch1 to ! CreateReadableByteStream(startAlgorithm, pull1Algorithm, cancel1Algorithm).
    let branch1 = create_readable_byte_stream(
        cx,
        StartAlgorithm::ReturnsUndefined,
        PullAlgorithm::ByteTee {
            state,
            for_branch2: false,
        },
        CancelAlgorithm::ByteTee {
            state,
            for_branch2: false,
        },
    )
    .must();
    state.branch1.set(Some(branch1));

    // 23. Set branch2 to ! CreateReadableByteStream(startAlgorithm, pull2Algorithm, cancel2Algorithm).
    let branch2 = create_readable_byte_stream(
        cx,
        StartAlgorithm::ReturnsUndefined,
        PullAlgorithm::ByteTee {
            state,
            for_branch2: true,
        },
        CancelAlgorithm::ByteTee {
            state,
            for_branch2: true,
        },
    )
    .must();
    state.branch2.set(Some(branch2));

    // 24. Perform forwardReaderError, given reader.
    ByteTeeState::forward_reader_error(state, cx, ReadableStreamReader::ReadableStreamDefaultReader(reader));

    // 25. Return « branch1, branch2 ».
    Ok((branch1, branch2))
}
