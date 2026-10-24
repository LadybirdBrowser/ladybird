/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WritableStream, https://streams.spec.whatwg.org/#ws-class, and the abstract operations that work with writable
//! streams, https://streams.spec.whatwg.org/#ws-abstract-ops

use std::collections::VecDeque;

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, Object, Realm, Trace, Value, Vm};

use crate::bindings::conversions::FromJs;
use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_rejected_promise, create_rejected_promise_from_exception,
    create_resolved_promise, exception_value, mark_promise_as_handled, react_to_promise, reject_promise,
    resolve_promise,
};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter, Transferable};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::abstract_operations::is_non_negative_number;
use crate::streams::algorithms::{AbortAlgorithm, CloseAlgorithm, SizeAlgorithm, StartAlgorithm, WriteAlgorithm};
use crate::streams::cross_realm_transform::{
    create_entangled_ports, deserialize_message_port, serialize_message_port, set_up_cross_realm_transform_readable,
    set_up_cross_realm_transform_writable,
};
use crate::streams::queuing_strategy::{QueuingStrategy, extract_high_water_mark, extract_size_algorithm};
use crate::streams::readable_stream::ReadableStream;
use crate::streams::readable_stream_pipe_to::readable_stream_pipe_to;
use crate::streams::underlying_sink::UnderlyingSink;
use crate::streams::writable_stream_default_controller::{
    WritableStreamDefaultController, set_up_writable_stream_default_controller,
    set_up_writable_stream_default_controller_from_underlying_sink, writable_stream_default_controller_close,
};
use crate::streams::writable_stream_default_writer::{
    WritableStreamDefaultWriter, writable_stream_default_writer_ensure_ready_promise_rejected,
};

// https://streams.spec.whatwg.org/#writablestream-state
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum WritableStreamState {
    Writable,
    Closed,
    Erroring,
    Errored,
}

// https://streams.spec.whatwg.org/#pending-abort-request
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PendingAbortRequest {
    // https://streams.spec.whatwg.org/#pending-abort-request-promise
    pub(crate) promise: WebIdlPromise,
    // https://streams.spec.whatwg.org/#pending-abort-request-reason
    pub(crate) reason: Value,
    // https://streams.spec.whatwg.org/#pending-abort-request-was-already-erroring
    #[gc(untraced)]
    pub(crate) was_already_erroring: bool,
}

// https://streams.spec.whatwg.org/#writablestream
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WritableStream {
    /// The realm of the stream, which its promises and the results of its operations are created in.
    realm: Cell<Gc<Realm>>,
    // https://streams.spec.whatwg.org/#writablestream-backpressure
    backpressure: Cell<bool>,
    // https://streams.spec.whatwg.org/#writablestream-closerequest
    close_request: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#writablestream-controller
    controller: Cell<Option<Ref<WritableStreamDefaultController>>>,
    // https://streams.spec.whatwg.org/#writablestream-detached
    detached: Cell<bool>,
    // https://streams.spec.whatwg.org/#writablestream-inflightwriterequest
    in_flight_write_request: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#writablestream-inflightcloserequest
    in_flight_close_request: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#writablestream-pendingabortrequest
    pending_abort_request: Cell<Option<PendingAbortRequest>>,
    // https://streams.spec.whatwg.org/#writablestream-state
    #[gc(untraced)]
    state: Cell<WritableStreamState>,
    // https://streams.spec.whatwg.org/#writablestream-storederror
    stored_error: Cell<Value>,
    // https://streams.spec.whatwg.org/#writablestream-writer
    writer: Cell<Option<Ref<WritableStreamDefaultWriter>>>,
    // https://streams.spec.whatwg.org/#writablestream-writerequests
    write_requests: GcRefCell<VecDeque<WebIdlPromise>>,
}

impl Wrappable for WritableStream {}

impl WritableStream {
    /// A new WritableStream in the realm, which InitializeWritableStream has initialized.
    pub(crate) fn new(realm: Gc<Realm>) -> Self {
        Self {
            realm: Cell::new(realm),
            backpressure: Cell::new(false),
            close_request: Cell::new(None),
            controller: Cell::new(None),
            detached: Cell::new(false),
            in_flight_write_request: Cell::new(None),
            in_flight_close_request: Cell::new(None),
            pending_abort_request: Cell::new(None),
            state: Cell::new(WritableStreamState::Writable),
            stored_error: Cell::new(Value::UNDEFINED),
            writer: Cell::new(None),
            write_requests: GcRefCell::new(VecDeque::new()),
        }
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm.get()
    }

    /// The context of the realm of the stream.
    pub(crate) fn context<'vm>(&self, vm: &'vm Vm) -> CallContext<'vm> {
        CallContext::new(vm, self.realm())
    }

    pub(crate) fn state(&self) -> WritableStreamState {
        self.state.get()
    }

    pub(crate) fn stored_error(&self) -> Value {
        self.stored_error.get()
    }

    pub(crate) fn writer(&self) -> Option<Ref<WritableStreamDefaultWriter>> {
        self.writer.get()
    }

    pub(crate) fn controller(&self) -> Option<Ref<WritableStreamDefaultController>> {
        self.controller.get()
    }

    pub(crate) fn set_controller(&self, controller: Option<Ref<WritableStreamDefaultController>>) {
        self.controller.set(controller);
    }

    pub(crate) fn set_writer(&self, writer: Option<Ref<WritableStreamDefaultWriter>>) {
        self.writer.set(writer);
    }

    pub(crate) fn in_flight_write_request(&self) -> Option<WebIdlPromise> {
        self.in_flight_write_request.get()
    }

    /// Takes the stream's [[writeRequests]], leaving an empty list.
    fn take_write_requests<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, WebIdlPromise> {
        let write_requests = MarkedVec::with_capacity(vm, self.write_requests.borrow().len());
        for write_request in self.write_requests.borrow().iter() {
            write_requests.push(*write_request);
        }
        self.write_requests.borrow_mut().clear();
        write_requests
    }

    // https://streams.spec.whatwg.org/#ws-constructor
    pub(crate) fn construct(
        cx: &mut CallContext,
        underlying_sink: Option<Gc<Object>>,
        strategy: QueuingStrategy,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. If underlyingSink is missing, set it to null.
        let underlying_sink = underlying_sink.map_or(Value::NULL, Value::from_object);

        // 2. Let underlyingSinkDict be underlyingSink, converted to an IDL value of type UnderlyingSink.
        let underlying_sink_dict = UnderlyingSink::from_js(cx.vm(), underlying_sink)?;

        // 3. If underlyingSinkDict["type"] exists, throw a RangeError exception.
        if underlying_sink_dict.r#type.is_some() {
            return Err(Exception::range_error("Invalid use of reserved key 'type'"));
        }

        // 4. Perform ! InitializeWritableStream(this).
        let this = allocate(cx.vm(), Self::new(cx.realm()));

        // 5. Let sizeAlgorithm be ! ExtractSizeAlgorithm(strategy).
        let size_algorithm = extract_size_algorithm(&strategy);

        // 6. Let highWaterMark be ? ExtractHighWaterMark(strategy, 1).
        let high_water_mark = extract_high_water_mark(&strategy, 1.0)?;

        // 7. Perform ? SetUpWritableStreamDefaultControllerFromUnderlyingSink(this, underlyingSink, underlyingSinkDict,
        //    highWaterMark, sizeAlgorithm).
        set_up_writable_stream_default_controller_from_underlying_sink(
            cx,
            this,
            underlying_sink,
            underlying_sink_dict,
            high_water_mark,
            size_algorithm,
        )?;
        Ok(this)
    }

    // https://streams.spec.whatwg.org/#ws-locked
    pub(crate) fn locked(this: &Ref<Self>, _: &mut CallContext) -> WebIdlResult<bool> {
        // 1. Return ! IsWritableStreamLocked(this).
        Ok(is_writable_stream_locked(*this))
    }

    // https://streams.spec.whatwg.org/#ws-abort
    pub(crate) fn abort(this: &Ref<Self>, cx: &mut CallContext, reason: Option<Value>) -> WebIdlResult<WebIdlPromise> {
        // 1. If ! IsWritableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
        if is_writable_stream_locked(*this) {
            let exception = Exception::type_error("Cannot abort a locked stream");
            return Ok(create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception));
        }

        // 2. Return ! WritableStreamAbort(this, reason).
        Ok(writable_stream_abort(cx, *this, reason.unwrap_or(Value::UNDEFINED)))
    }

    // https://streams.spec.whatwg.org/#ws-close
    pub(crate) fn close(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // 1. If ! IsWritableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
        if is_writable_stream_locked(*this) {
            let exception = Exception::type_error("Cannot close a locked stream");
            return Ok(create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception));
        }

        // 2. If ! WritableStreamCloseQueuedOrInFlight(this) is true, return a promise rejected with a TypeError
        //    exception.
        if writable_stream_close_queued_or_in_flight(*this) {
            let exception = Exception::type_error("Cannot close a stream that is already closed or errored");
            return Ok(create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception));
        }

        // 3. Return ! WritableStreamClose(this).
        Ok(writable_stream_close(cx, *this))
    }

    // https://streams.spec.whatwg.org/#ws-get-writer
    pub(crate) fn get_writer(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<Ref<WritableStreamDefaultWriter>> {
        // 1. Return ? AcquireWritableStreamDefaultWriter(this).
        acquire_writable_stream_default_writer(cx, *this)
    }
}

impl Transferable for WritableStream {
    fn detached(&self) -> &Cell<bool> {
        &self.detached
    }

    // https://streams.spec.whatwg.org/#ref-for-transfer-steps①
    fn transfer_steps(
        this: &Ref<Self>,
        cx: &mut CallContext,
        data_holder: &mut TransferDataWriter,
    ) -> WebIdlResult<()> {
        let this = *this;
        let cx: &CallContext = cx;
        with_temporary_execution_context_and_callbacks(cx.realm(), || {
            // 1. If ! IsWritableStreamLocked(value) is true, throw a "DataCloneError" DOMException.
            if is_writable_stream_locked(this) {
                return Err(Exception::Dom(
                    "DataCloneError",
                    Utf16String::from_ascii(b"Cannot transfer locked WritableStream"),
                ));
            }

            // 2. Let port1 be a new MessagePort in the current Realm.
            // 3. Let port2 be a new MessagePort in the current Realm.
            // 4. Entangle port1 and port2.
            let (port1, port2) = create_entangled_ports(cx);

            // 5. Let readable be a new ReadableStream in the current Realm.
            let readable = allocate(cx.vm(), ReadableStream::new(cx.realm()));

            // 6. Perform ! SetUpCrossRealmTransformReadable(readable, port1).
            set_up_cross_realm_transform_readable(cx, readable, port1);

            // 7. Let promise be ! ReadableStreamPipeTo(readable, value, false, false, false).
            let promise = readable_stream_pipe_to(cx, readable, this, false, false, false, None);

            // 8. Set promise.[[PromiseIsHandled]] to true.
            mark_promise_as_handled(cx.vm(), promise);

            // 9. Set dataHolder.[[port]] to ! StructuredSerializeWithTransfer(port2, « port2 »).
            serialize_message_port(cx, data_holder, &port2);
            Ok(())
        })
    }

    // https://streams.spec.whatwg.org/#ref-for-transfer-receiving-steps①
    fn transfer_receiving_steps(cx: &mut CallContext, data_holder: &mut TransferDataReader) -> WebIdlResult<Ref<Self>> {
        let cx: &CallContext = cx;
        with_temporary_execution_context_and_callbacks(cx.realm(), || {
            let value = allocate(cx.vm(), Self::new(cx.realm()));

            // 1. Let deserializedRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[port]], the current
            //    Realm).
            // 2. Let port be deserializedRecord.[[Deserialized]].
            let port = deserialize_message_port(cx, data_holder);

            // 3. Perform ! SetUpCrossRealmTransformWritable(value, port).
            set_up_cross_realm_transform_writable(cx, value, port);
            Ok(value)
        })
    }
}

// https://streams.spec.whatwg.org/#acquire-writable-stream-default-writer
pub(crate) fn acquire_writable_stream_default_writer(
    cx: &CallContext,
    stream: Ref<WritableStream>,
) -> WebIdlResult<Ref<WritableStreamDefaultWriter>> {
    // 1. Let writer be a new WritableStreamDefaultWriter.
    let writer = allocate(cx.vm(), WritableStreamDefaultWriter::new());

    // 2. Perform ? SetUpWritableStreamDefaultWriter(writer, stream).
    set_up_writable_stream_default_writer(cx, writer, stream)?;

    // 3. Return writer.
    Ok(writer)
}

// https://streams.spec.whatwg.org/#create-writable-stream
#[allow(clippy::too_many_arguments, reason = "the abstract operation takes these arguments")]
pub(crate) fn create_writable_stream(
    cx: &CallContext,
    start_algorithm: StartAlgorithm,
    write_algorithm: WriteAlgorithm,
    close_algorithm: CloseAlgorithm,
    abort_algorithm: AbortAlgorithm,
    high_water_mark: f64,
    size_algorithm: SizeAlgorithm,
) -> WebIdlResult<Ref<WritableStream>> {
    // 1. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
    assert!(is_non_negative_number(Value::from_f64(high_water_mark)));

    // 2. Let stream be a new WritableStream.
    let stream = allocate(cx.vm(), WritableStream::new(cx.realm()));

    // 3. Perform ! InitializeWritableStream(stream).
    initialize_writable_stream(stream);

    // 4. Let controller be a new WritableStreamDefaultController.
    let controller = allocate(cx.vm(), WritableStreamDefaultController::new());

    // 5. Perform ? SetUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm,
    //    closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm).
    set_up_writable_stream_default_controller(
        cx,
        stream,
        controller,
        start_algorithm,
        write_algorithm,
        close_algorithm,
        abort_algorithm,
        high_water_mark,
        size_algorithm,
    )?;

    // 6. Return stream.
    Ok(stream)
}

// https://streams.spec.whatwg.org/#initialize-writable-stream
pub(crate) fn initialize_writable_stream(stream: Ref<WritableStream>) {
    // 1. Set stream.[[state]] to "writable".
    stream.state.set(WritableStreamState::Writable);

    // 2. Set stream.[[storedError]], stream.[[writer]], stream.[[controller]], stream.[[inFlightWriteRequest]],
    //    stream.[[closeRequest]], stream.[[inFlightCloseRequest]], and stream.[[pendingAbortRequest]] to undefined.
    stream.stored_error.set(Value::UNDEFINED);
    stream.writer.set(None);
    stream.controller.set(None);
    stream.in_flight_write_request.set(None);
    stream.close_request.set(None);
    stream.in_flight_close_request.set(None);
    stream.pending_abort_request.set(None);

    // 3. Set stream.[[writeRequests]] to a new empty list.
    stream.write_requests.borrow_mut().clear();

    // 4. Set stream.[[backpressure]] to false.
    stream.backpressure.set(false);
}

// https://streams.spec.whatwg.org/#is-writable-stream-locked
pub(crate) fn is_writable_stream_locked(stream: Ref<WritableStream>) -> bool {
    // 1. If stream.[[writer]] is undefined, return false.
    // 2. Return true.
    stream.writer().is_some()
}

// https://streams.spec.whatwg.org/#set-up-writable-stream-default-writer
pub(crate) fn set_up_writable_stream_default_writer(
    cx: &CallContext,
    writer: Ref<WritableStreamDefaultWriter>,
    stream: Ref<WritableStream>,
) -> WebIdlResult<()> {
    let vm = cx.vm();
    let realm = cx.realm();

    // 1. If ! IsWritableStreamLocked(stream) is true, throw a TypeError exception.
    if is_writable_stream_locked(stream) {
        return Err(Exception::type_error("Stream is locked"));
    }

    // 2. Set writer.[[stream]] to stream.
    writer.set_stream(Some(stream));

    // 3. Set stream.[[writer]] to writer.
    stream.set_writer(Some(writer));

    // 4. Let state be stream.[[state]].
    let state = stream.state();

    match state {
        // 5. If state is "writable",
        WritableStreamState::Writable => {
            // 1. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and stream.[[backpressure]] is true, set
            //    writer.[[readyPromise]] to a new promise.
            if !writable_stream_close_queued_or_in_flight(stream) && stream.backpressure.get() {
                writer.set_ready_promise(create_promise(vm, realm));
            }
            // 2. Otherwise, set writer.[[readyPromise]] to a promise resolved with undefined.
            else {
                writer.set_ready_promise(create_resolved_promise(vm, realm, Value::UNDEFINED));
            }

            // 3. Set writer.[[closedPromise]] to a new promise.
            writer.set_closed_promise(create_promise(vm, realm));
        }
        // 6. Otherwise, if state is "erroring",
        WritableStreamState::Erroring => {
            // 1. Set writer.[[readyPromise]] to a promise rejected with stream.[[storedError]].
            let ready_promise = create_rejected_promise(vm, realm, stream.stored_error());
            writer.set_ready_promise(ready_promise);

            // 2. Set writer.[[readyPromise]].[[PromiseIsHandled]] to true.
            mark_promise_as_handled(vm, ready_promise);

            // 3. Set writer.[[closedPromise]] to a new promise.
            writer.set_closed_promise(create_promise(vm, realm));
        }
        // 7. Otherwise, if state is "closed",
        WritableStreamState::Closed => {
            // 1. Set writer.[[readyPromise]] to a promise resolved with undefined.
            writer.set_ready_promise(create_resolved_promise(vm, realm, Value::UNDEFINED));

            // 2. Set writer.[[closedPromise]] to a promise resolved with undefined.
            writer.set_closed_promise(create_resolved_promise(vm, realm, Value::UNDEFINED));
        }
        // 8. Otherwise,
        WritableStreamState::Errored => {
            // 1. Assert: state is "errored".

            // 2. Let storedError be stream.[[storedError]].
            let stored_error = stream.stored_error();

            // 3. Set writer.[[readyPromise]] to a promise rejected with storedError.
            let ready_promise = create_rejected_promise(vm, realm, stored_error);
            writer.set_ready_promise(ready_promise);

            // 4. Set writer.[[readyPromise]].[[PromiseIsHandled]] to true.
            mark_promise_as_handled(vm, ready_promise);

            // 5. Set writer.[[closedPromise]] to a promise rejected with storedError.
            let closed_promise = create_rejected_promise(vm, realm, stored_error);
            writer.set_closed_promise(closed_promise);

            // 6. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
            mark_promise_as_handled(vm, closed_promise);
        }
    }
    Ok(())
}

// https://streams.spec.whatwg.org/#writable-stream-abort
pub(crate) fn writable_stream_abort(cx: &CallContext, stream: Ref<WritableStream>, reason: Value) -> WebIdlPromise {
    let vm = cx.vm();
    let realm = cx.realm();
    let mut reason = reason;

    // 1. If stream.[[state]] is "closed" or "errored", return a promise resolved with undefined.
    if matches!(
        stream.state(),
        WritableStreamState::Closed | WritableStreamState::Errored
    ) {
        return create_resolved_promise(vm, realm, Value::UNDEFINED);
    }

    // 2. Signal abort on stream.[[controller]].[[abortController]] with reason.
    let mut signal_abort_reason = reason;
    if signal_abort_reason.is_undefined() {
        signal_abort_reason = exception_value(
            vm,
            realm,
            Exception::Dom("AbortError", Utf16String::from_ascii(b"Aborted without reason")),
        );
    }
    let controller = stream.controller().expect("a stream that is set up has a controller");
    controller.abort_signal().signal_abort(signal_abort_reason, realm);

    // 3. Let state be stream.[[state]].
    let state = stream.state();

    // 4. If state is "closed" or "errored", return a promise resolved with undefined.
    if state == WritableStreamState::Closed || state == WritableStreamState::Errored {
        return create_resolved_promise(vm, realm, Value::UNDEFINED);
    }

    // 5. If stream.[[pendingAbortRequest]] is not undefined, return stream.[[pendingAbortRequest]]'s promise.
    if let Some(pending_abort_request) = stream.pending_abort_request.get() {
        return pending_abort_request.promise;
    }

    // 6. Assert: state is "writable" or "erroring".
    assert!(state == WritableStreamState::Writable || state == WritableStreamState::Erroring);

    // 7. Let wasAlreadyErroring be false.
    let mut was_already_erroring = false;

    // 8. If state is "erroring",
    if state == WritableStreamState::Erroring {
        // 1. Set wasAlreadyErroring to true.
        was_already_erroring = true;

        // 2. Set reason to undefined.
        reason = Value::UNDEFINED;
    }

    // 9. Let promise be a new promise.
    let promise = create_promise(vm, realm);

    // 10. Set stream.[[pendingAbortRequest]] to a new pending abort request whose promise is promise, reason is reason,
    //     and was already erroring is wasAlreadyErroring.
    stream.pending_abort_request.set(Some(PendingAbortRequest {
        promise,
        reason,
        was_already_erroring,
    }));

    // 11. If wasAlreadyErroring is false, perform ! WritableStreamStartErroring(stream, reason).
    if !was_already_erroring {
        writable_stream_start_erroring(cx, stream, reason);
    }

    // 12. Return promise.
    promise
}

// https://streams.spec.whatwg.org/#writable-stream-close
pub(crate) fn writable_stream_close(cx: &CallContext, stream: Ref<WritableStream>) -> WebIdlPromise {
    let vm = cx.vm();
    let realm = cx.realm();

    // 1. Let state be stream.[[state]].
    let state = stream.state();

    // 2. If state is "closed" or "errored", return a promise rejected with a TypeError exception.
    if state == WritableStreamState::Closed || state == WritableStreamState::Errored {
        let message = if state == WritableStreamState::Closed {
            "Cannot close a closed stream"
        } else {
            "Cannot close an errored stream"
        };
        let exception = Exception::type_error(message);

        return create_rejected_promise_from_exception(vm, realm, exception);
    }

    // 3. Assert: state is "writable" or "erroring".
    assert!(state == WritableStreamState::Writable || state == WritableStreamState::Erroring);

    // 4. Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
    assert!(!writable_stream_close_queued_or_in_flight(stream));

    // 5. Let promise be a new promise.
    let promise = create_promise(vm, realm);

    // 6. Set stream.[[closeRequest]] to promise.
    stream.close_request.set(Some(promise));

    // 7. Let writer be stream.[[writer]].
    let writer = stream.writer();

    // 8. If writer is not undefined, and stream.[[backpressure]] is true, and state is "writable", resolve
    //    writer.[[readyPromise]] with undefined.
    if let Some(writer) = writer
        && stream.backpressure.get()
        && state == WritableStreamState::Writable
    {
        let ready_promise = writer
            .ready_promise()
            .expect("a writer that is set up has a ready promise");
        resolve_promise(vm, ready_promise, Value::UNDEFINED);
    }

    // 9. Perform ! WritableStreamDefaultControllerClose(stream.[[controller]]).
    let controller = stream.controller().expect("a stream that is set up has a controller");
    writable_stream_default_controller_close(cx, controller);

    // 10. Return promise.
    promise
}

// https://streams.spec.whatwg.org/#writable-stream-add-write-request
pub(crate) fn writable_stream_add_write_request(cx: &CallContext, stream: Ref<WritableStream>) -> WebIdlPromise {
    // 1. Assert: ! IsWritableStreamLocked(stream) is true.
    assert!(is_writable_stream_locked(stream));

    // 2. Assert: stream.[[state]] is "writable".
    assert!(stream.state() == WritableStreamState::Writable);

    // 3. Let promise be a new promise.
    let promise = create_promise(cx.vm(), cx.realm());

    // 4. Append promise to stream.[[writeRequests]].
    stream.write_requests.borrow_mut().push_back(promise);

    // 5. Return promise.
    promise
}

// https://streams.spec.whatwg.org/#writable-stream-close-queued-or-in-flight
pub(crate) fn writable_stream_close_queued_or_in_flight(stream: Ref<WritableStream>) -> bool {
    // 1. If stream.[[closeRequest]] is undefined and stream.[[inFlightCloseRequest]] is undefined, return false.
    // 2. Return true.
    stream.close_request.get().is_some() || stream.in_flight_close_request.get().is_some()
}

// https://streams.spec.whatwg.org/#writable-stream-deal-with-rejection
pub(crate) fn writable_stream_deal_with_rejection(cx: &CallContext, stream: Ref<WritableStream>, error: Value) {
    // 1. Let state be stream.[[state]].
    let state = stream.state();

    // 2. If state is "writable",
    if state == WritableStreamState::Writable {
        // 1. Perform ! WritableStreamStartErroring(stream, error).
        writable_stream_start_erroring(cx, stream, error);

        // 2. Return.
        return;
    }

    // 3. Assert: state is "erroring".
    assert!(state == WritableStreamState::Erroring);

    // 4. Perform ! WritableStreamFinishErroring(stream).
    writable_stream_finish_erroring(cx, stream);
}

// https://streams.spec.whatwg.org/#writable-stream-finish-erroring
pub(crate) fn writable_stream_finish_erroring(cx: &CallContext, stream: Ref<WritableStream>) {
    let vm = cx.vm();

    // 1. Assert: stream.[[state]] is "erroring".
    assert!(stream.state() == WritableStreamState::Erroring);

    // 2. Assert: ! WritableStreamHasOperationMarkedInFlight(stream) is false.
    assert!(!writable_stream_has_operation_marked_in_flight(stream));

    // 3. Set stream.[[state]] to "errored".
    stream.state.set(WritableStreamState::Errored);

    // 4. Perform ! stream.[[controller]].[[ErrorSteps]]().
    let controller = stream.controller().expect("a stream that is set up has a controller");
    WritableStreamDefaultController::error_steps(controller);

    // 5. Let storedError be stream.[[storedError]].
    let stored_error = stream.stored_error();

    // 6. For each writeRequest of stream.[[writeRequests]]:
    // 7. Set stream.[[writeRequests]] to an empty list.
    let write_requests = stream.take_write_requests(vm);
    for index in 0..write_requests.len() {
        // 1. Reject writeRequest with storedError.
        reject_promise(
            vm,
            write_requests.get(index).expect("the index is in bounds"),
            stored_error,
        );
    }

    // 8. If stream.[[pendingAbortRequest]] is undefined,
    if stream.pending_abort_request.get().is_none() {
        // 1. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
        writable_stream_reject_close_and_closed_promise_if_needed(cx, stream);

        // 2. Return.
        return;
    }

    // 9. Let abortRequest be stream.[[pendingAbortRequest]].
    // 10. Set stream.[[pendingAbortRequest]] to undefined.
    let abort_request = stream
        .pending_abort_request
        .take()
        .expect("the stream has a pending abort request");

    // 11. If abortRequest’s was already erroring is true,
    if abort_request.was_already_erroring {
        // 1. Reject abortRequest’s promise with storedError.
        reject_promise(vm, abort_request.promise, stored_error);

        // 2. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
        writable_stream_reject_close_and_closed_promise_if_needed(cx, stream);

        // 3. Return.
        return;
    }

    // 12. Let promise be ! stream.[[controller]].[[AbortSteps]](abortRequest’s reason).
    let promise = WritableStreamDefaultController::abort_steps(controller, cx, abort_request.reason);

    react_to_promise(
        vm,
        promise,
        (stream, abort_request.promise),
        // 13. Upon fulfillment of promise,
        Some(|cx, (stream, abort_promise), _| {
            // 1. Resolve abortRequest’s promise with undefined.
            resolve_promise(cx.vm(), abort_promise, Value::UNDEFINED);

            // 2. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
            writable_stream_reject_close_and_closed_promise_if_needed(cx, stream);

            Ok(Value::UNDEFINED)
        }),
        // 14. Upon rejection of promise with reason reason,
        Some(|cx, (stream, abort_promise), reason| {
            // 1. Reject abortRequest’s promise with reason.
            reject_promise(cx.vm(), abort_promise, reason);

            // 2. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
            writable_stream_reject_close_and_closed_promise_if_needed(cx, stream);

            Ok(Value::UNDEFINED)
        }),
    );
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close
pub(crate) fn writable_stream_finish_in_flight_close(cx: &CallContext, stream: Ref<WritableStream>) {
    let vm = cx.vm();

    // 1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
    // 2. Resolve stream.[[inFlightCloseRequest]] with undefined.
    // 3. Set stream.[[inFlightCloseRequest]] to undefined.
    let in_flight_close_request = stream
        .in_flight_close_request
        .take()
        .expect("a stream finishes an in-flight close request");
    resolve_promise(vm, in_flight_close_request, Value::UNDEFINED);

    // 4. Let state be stream.[[state]].
    let state = stream.state();

    // 5. Assert: stream.[[state]] is "writable" or "erroring".
    assert!(state == WritableStreamState::Writable || state == WritableStreamState::Erroring);

    // 6. If state is "erroring",
    if state == WritableStreamState::Erroring {
        // 1. Set stream.[[storedError]] to undefined.
        stream.stored_error.set(Value::UNDEFINED);

        // 2. If stream.[[pendingAbortRequest]] is not undefined,
        // 1. Resolve stream.[[pendingAbortRequest]]'s promise with undefined.
        // 2. Set stream.[[pendingAbortRequest]] to undefined.
        if let Some(pending_abort_request) = stream.pending_abort_request.take() {
            resolve_promise(vm, pending_abort_request.promise, Value::UNDEFINED);
        }
    }

    // 7. Set stream.[[state]] to "closed".
    stream.state.set(WritableStreamState::Closed);

    // 8. Let writer be stream.[[writer]].
    // 9. If writer is not undefined, resolve writer.[[closedPromise]] with undefined.
    if let Some(writer) = stream.writer() {
        let closed_promise = writer
            .closed_promise()
            .expect("a writer that is set up has a closed promise");
        resolve_promise(vm, closed_promise, Value::UNDEFINED);
    }

    // 10. Assert: stream.[[pendingAbortRequest]] is undefined.
    assert!(stream.pending_abort_request.get().is_none());

    // 11. Assert: stream.[[storedError]] is undefined.
    assert!(stream.stored_error().is_undefined());
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close-with-error
pub(crate) fn writable_stream_finish_in_flight_close_with_error(
    cx: &CallContext,
    stream: Ref<WritableStream>,
    error: Value,
) {
    let vm = cx.vm();

    // 1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
    // 2. Reject stream.[[inFlightCloseRequest]] with error.
    // 3. Set stream.[[inFlightCloseRequest]] to undefined.
    let in_flight_close_request = stream
        .in_flight_close_request
        .take()
        .expect("a stream finishes an in-flight close request");
    reject_promise(vm, in_flight_close_request, error);

    // 4. Assert: stream.[[state]] is "writable" or "erroring".
    assert!(matches!(
        stream.state(),
        WritableStreamState::Writable | WritableStreamState::Erroring
    ));

    // 5. If stream.[[pendingAbortRequest]] is not undefined,
    // 1. Reject stream.[[pendingAbortRequest]]'s promise with error.
    // 2. Set stream.[[pendingAbortRequest]] to undefined.
    if let Some(pending_abort_request) = stream.pending_abort_request.take() {
        reject_promise(vm, pending_abort_request.promise, error);
    }

    // 6. Perform ! WritableStreamDealWithRejection(stream, error).
    writable_stream_deal_with_rejection(cx, stream, error);
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write
pub(crate) fn writable_stream_finish_in_flight_write(cx: &CallContext, stream: Ref<WritableStream>) {
    // 1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
    // 2. Resolve stream.[[inFlightWriteRequest]] with undefined.
    // 3. Set stream.[[inFlightWriteRequest]] to undefined.
    let in_flight_write_request = stream
        .in_flight_write_request
        .take()
        .expect("a stream finishes an in-flight write request");
    resolve_promise(cx.vm(), in_flight_write_request, Value::UNDEFINED);
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write-with-error
pub(crate) fn writable_stream_finish_in_flight_write_with_error(
    cx: &CallContext,
    stream: Ref<WritableStream>,
    error: Value,
) {
    // 1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
    // 2. Reject stream.[[inFlightWriteRequest]] with error.
    // 3. Set stream.[[inFlightWriteRequest]] to undefined.
    let in_flight_write_request = stream
        .in_flight_write_request
        .take()
        .expect("a stream finishes an in-flight write request");
    reject_promise(cx.vm(), in_flight_write_request, error);

    // 4. Assert: stream.[[state]] is "writable" or "erroring".
    assert!(matches!(
        stream.state(),
        WritableStreamState::Writable | WritableStreamState::Erroring
    ));

    // 5. Perform ! WritableStreamDealWithRejection(stream, error).
    writable_stream_deal_with_rejection(cx, stream, error);
}

// https://streams.spec.whatwg.org/#writable-stream-has-operation-marked-in-flight
pub(crate) fn writable_stream_has_operation_marked_in_flight(stream: Ref<WritableStream>) -> bool {
    // 1. If stream.[[inFlightWriteRequest]] is undefined and stream.[[inFlightCloseRequest]] is undefined, return
    //    false.
    // 2. Return true.
    stream.in_flight_write_request.get().is_some() || stream.in_flight_close_request.get().is_some()
}

// https://streams.spec.whatwg.org/#writable-stream-mark-close-request-in-flight
pub(crate) fn writable_stream_mark_close_request_in_flight(stream: Ref<WritableStream>) {
    // 1. Assert: stream.[[inFlightCloseRequest]] is undefined.
    assert!(stream.in_flight_close_request.get().is_none());

    // 2. Assert: stream.[[closeRequest]] is not undefined.
    // 3. Set stream.[[inFlightCloseRequest]] to stream.[[closeRequest]].
    // 4. Set stream.[[closeRequest]] to undefined.
    let close_request = stream
        .close_request
        .take()
        .expect("a stream has a close request to mark in flight");
    stream.in_flight_close_request.set(Some(close_request));
}

// https://streams.spec.whatwg.org/#writable-stream-mark-first-write-request-in-flight
pub(crate) fn writable_stream_mark_first_write_request_in_flight(stream: Ref<WritableStream>) {
    // 1. Assert: stream.[[inFlightWriteRequest]] is undefined.
    assert!(stream.in_flight_write_request.get().is_none());

    // 2. Assert: stream.[[writeRequests]] is not empty.
    // 3. Let writeRequest be stream.[[writeRequests]][0].
    // 4. Remove writeRequest from stream.[[writeRequests]].
    let write_request = stream
        .write_requests
        .borrow_mut()
        .pop_front()
        .expect("a stream has a write request to mark in flight");

    // 5. Set stream.[[inFlightWriteRequest]] to writeRequest.
    stream.in_flight_write_request.set(Some(write_request));
}

// https://streams.spec.whatwg.org/#writable-stream-reject-close-and-closed-promise-if-needed
pub(crate) fn writable_stream_reject_close_and_closed_promise_if_needed(cx: &CallContext, stream: Ref<WritableStream>) {
    let vm = cx.vm();

    // 1. Assert: stream.[[state]] is "errored".
    assert!(stream.state() == WritableStreamState::Errored);

    // 2. If stream.[[closeRequest]] is not undefined,
    if let Some(close_request) = stream.close_request.get() {
        // 1. Assert: stream.[[inFlightCloseRequest]] is undefined.
        assert!(stream.in_flight_close_request.get().is_none());

        // 2. Reject stream.[[closeRequest]] with stream.[[storedError]].
        reject_promise(vm, close_request, stream.stored_error());

        // 3. Set stream.[[closeRequest]] to undefined.
        stream.close_request.set(None);
    }

    // 3. Let writer be stream.[[writer]].
    // 4. If writer is not undefined,
    if let Some(writer) = stream.writer() {
        let closed_promise = writer
            .closed_promise()
            .expect("a writer that is set up has a closed promise");

        // 1. Reject writer.[[closedPromise]] with stream.[[storedError]].
        reject_promise(vm, closed_promise, stream.stored_error());

        // 2. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
        mark_promise_as_handled(vm, closed_promise);
    }
}

// https://streams.spec.whatwg.org/#writable-stream-start-erroring
pub(crate) fn writable_stream_start_erroring(cx: &CallContext, stream: Ref<WritableStream>, reason: Value) {
    // 1. Assert: stream.[[storedError]] is undefined.
    assert!(stream.stored_error().is_undefined());

    // 2. Assert: stream.[[state]] is "writable".
    assert!(stream.state() == WritableStreamState::Writable);

    // 3. Let controller be stream.[[controller]].
    // 4. Assert: controller is not undefined.
    let controller = stream
        .controller()
        .expect("a stream that starts erroring has a controller");

    // 5. Set stream.[[state]] to "erroring".
    stream.state.set(WritableStreamState::Erroring);

    // 6. Set stream.[[storedError]] to reason.
    stream.stored_error.set(reason);

    // 7. Let writer be stream.[[writer]].
    // 8. If writer is not undefined, perform ! WritableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason).
    if let Some(writer) = stream.writer() {
        writable_stream_default_writer_ensure_ready_promise_rejected(cx.vm(), writer, reason);
    }

    // 9. If ! WritableStreamHasOperationMarkedInFlight(stream) is false and controller.[[started]] is true,
    //    perform ! WritableStreamFinishErroring(stream).
    if !writable_stream_has_operation_marked_in_flight(stream) && controller.started() {
        writable_stream_finish_erroring(cx, stream);
    }
}

// https://streams.spec.whatwg.org/#writable-stream-update-backpressure
pub(crate) fn writable_stream_update_backpressure(cx: &CallContext, stream: Ref<WritableStream>, backpressure: bool) {
    let vm = cx.vm();

    // 1. Assert: stream.[[state]] is "writable".
    assert!(stream.state() == WritableStreamState::Writable);

    // 2. Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
    assert!(!writable_stream_close_queued_or_in_flight(stream));

    // 3. Let writer be stream.[[writer]].
    // 4. If writer is not undefined and backpressure is not stream.[[backpressure]],
    if let Some(writer) = stream.writer()
        && backpressure != stream.backpressure.get()
    {
        let realm = writer.ready_promise_realm();

        // 1. If backpressure is true, set writer.[[readyPromise]] to a new promise.
        if backpressure {
            writer.set_ready_promise(create_promise(vm, realm));
        }
        // 2. Otherwise,
        else {
            // 1. Assert: backpressure is false.

            // 2. Resolve writer.[[readyPromise]] with undefined.
            let ready_promise = writer
                .ready_promise()
                .expect("a writer that is set up has a ready promise");
            resolve_promise(vm, ready_promise, Value::UNDEFINED);
        }
    }

    // 5. Set stream.[[backpressure]] to backpressure.
    stream.backpressure.set(backpressure);
}
