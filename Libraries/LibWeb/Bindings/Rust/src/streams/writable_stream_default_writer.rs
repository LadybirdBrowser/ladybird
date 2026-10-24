/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WritableStreamDefaultWriter, https://streams.spec.whatwg.org/#default-writer-class, and the abstract operations of
//! writers, https://streams.spec.whatwg.org/#ws-writer-abstract-ops

use core::cell::Cell;

use libjs_rust::api::{Gc, Promise, PromiseState, Realm, Trace, Value, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{
    WebIdlPromise, create_rejected_promise, create_rejected_promise_from_exception, create_resolved_promise,
    exception_value, mark_promise_as_handled, promise_realm, reject_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::writable_stream::{
    WritableStream, WritableStreamState, set_up_writable_stream_default_writer, writable_stream_abort,
    writable_stream_add_write_request, writable_stream_close, writable_stream_close_queued_or_in_flight,
};
use crate::streams::writable_stream_default_controller::{
    writable_stream_default_controller_get_chunk_size, writable_stream_default_controller_get_desired_size,
    writable_stream_default_controller_write,
};

// https://streams.spec.whatwg.org/#writablestreamdefaultwriter
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WritableStreamDefaultWriter {
    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-closedpromise
    closed_promise: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-readypromise
    ready_promise: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultwriter-stream
    stream: Cell<Option<Ref<WritableStream>>>,
}

impl Wrappable for WritableStreamDefaultWriter {}

impl WritableStreamDefaultWriter {
    pub(crate) fn new() -> Self {
        Self {
            closed_promise: Cell::new(None),
            ready_promise: Cell::new(None),
            stream: Cell::new(None),
        }
    }

    pub(crate) fn closed_promise(&self) -> Option<WebIdlPromise> {
        self.closed_promise.get()
    }

    pub(crate) fn ready_promise(&self) -> Option<WebIdlPromise> {
        self.ready_promise.get()
    }

    pub(crate) fn stream(&self) -> Option<Ref<WritableStream>> {
        self.stream.get()
    }

    pub(crate) fn set_closed_promise(&self, promise: WebIdlPromise) {
        self.closed_promise.set(Some(promise));
    }

    pub(crate) fn set_ready_promise(&self, promise: WebIdlPromise) {
        self.ready_promise.set(Some(promise));
    }

    pub(crate) fn set_stream(&self, stream: Option<Ref<WritableStream>>) {
        self.stream.set(stream);
    }

    /// The realm of the writer's [[closedPromise]], which most promises and exceptions of the writer are created in,
    /// as LibWeb's C++ creates them.
    pub(crate) fn closed_promise_realm(&self) -> Gc<Realm> {
        promise_realm(
            self.closed_promise()
                .expect("a writer that is set up has a closed promise"),
        )
    }

    /// The realm of the writer's [[readyPromise]], which the promises of the writer's write() are created in, as
    /// LibWeb's C++ creates them.
    pub(crate) fn ready_promise_realm(&self) -> Gc<Realm> {
        promise_realm(
            self.ready_promise()
                .expect("a writer that is set up has a ready promise"),
        )
    }

    // https://streams.spec.whatwg.org/#default-writer-constructor
    pub(crate) fn construct(cx: &mut CallContext, stream: Ref<WritableStream>) -> WebIdlResult<Ref<Self>> {
        let this = allocate(cx.vm(), Self::new());

        // 1. Perform ? SetUpWritableStreamDefaultWriter(this, stream).
        set_up_writable_stream_default_writer(cx, this, stream)?;
        Ok(this)
    }

    // https://streams.spec.whatwg.org/#default-writer-closed
    pub(crate) fn closed(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // 1. Return this.[[closedPromise]].
        Ok(self
            .closed_promise()
            .expect("a writer that is set up has a closed promise"))
    }

    // https://streams.spec.whatwg.org/#default-writer-desired-size
    pub(crate) fn desired_size(this: &Ref<Self>, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        // 1. If this.[[stream]] is undefined, throw a TypeError exception.
        if this.stream().is_none() {
            return Err(Exception::type_error(
                "Cannot get desired size of writer that has no locked stream",
            ));
        }

        // 2. Return ! WritableStreamDefaultWriterGetDesiredSize(this).
        Ok(writable_stream_default_writer_get_desired_size(*this))
    }

    // https://streams.spec.whatwg.org/#default-writer-ready
    pub(crate) fn ready(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // 1. Return this.[[readyPromise]].
        Ok(self
            .ready_promise()
            .expect("a writer that is set up has a ready promise"))
    }

    // https://streams.spec.whatwg.org/#default-writer-abort
    pub(crate) fn abort(this: &Ref<Self>, cx: &mut CallContext, reason: Option<Value>) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = this.closed_promise_realm();

        // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
        if this.stream().is_none() {
            let exception = Exception::type_error("Cannot abort a writer that has no locked stream");
            return Ok(create_rejected_promise_from_exception(vm, realm, exception));
        }

        // 2. Return ! WritableStreamDefaultWriterAbort(this, reason).
        Ok(writable_stream_default_writer_abort(
            vm,
            *this,
            reason.unwrap_or(Value::UNDEFINED),
        ))
    }

    // https://streams.spec.whatwg.org/#default-writer-close
    pub(crate) fn close(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = this.closed_promise_realm();

        // 1. Let stream be this.[[stream]].
        // 2. If stream is undefined, return a promise rejected with a TypeError exception.
        let Some(stream) = this.stream() else {
            let exception = Exception::type_error("Cannot close a writer that has no locked stream");
            return Ok(create_rejected_promise_from_exception(vm, realm, exception));
        };

        // 3. If ! WritableStreamCloseQueuedOrInFlight(stream) is true, return a promise rejected with a TypeError
        //    exception.
        if writable_stream_close_queued_or_in_flight(stream) {
            let exception = Exception::type_error("Cannot close a stream that is already closed or errored");
            return Ok(create_rejected_promise_from_exception(vm, realm, exception));
        }

        // 4. Return ! WritableStreamDefaultWriterClose(this).
        Ok(writable_stream_default_writer_close(vm, *this))
    }

    // https://streams.spec.whatwg.org/#default-writer-release-lock
    pub(crate) fn release_lock(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. Let stream be this.[[stream]].
        // 2. If stream is undefined, return.
        let Some(stream) = this.stream() else {
            return Ok(());
        };

        // 3. Assert: stream.[[writer]] is not undefined.
        assert!(stream.writer().is_some());

        // 4. Perform ! WritableStreamDefaultWriterRelease(this).
        writable_stream_default_writer_release(cx.vm(), *this);
        Ok(())
    }

    // https://streams.spec.whatwg.org/#default-writer-write
    pub(crate) fn write(this: &Ref<Self>, cx: &mut CallContext, chunk: Option<Value>) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = this.ready_promise_realm();

        // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
        if this.stream().is_none() {
            let exception = Exception::type_error("Cannot write to a writer that has no locked stream");
            return Ok(create_rejected_promise_from_exception(vm, realm, exception));
        }

        // 2. Return ! WritableStreamDefaultWriterWrite(this, chunk).
        Ok(writable_stream_default_writer_write(
            vm,
            *this,
            chunk.unwrap_or(Value::UNDEFINED),
        ))
    }
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-abort
pub(crate) fn writable_stream_default_writer_abort(
    vm: &Vm,
    writer: Ref<WritableStreamDefaultWriter>,
    reason: Value,
) -> WebIdlPromise {
    // 1. Let stream be writer.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = writer.stream().expect("a writer that aborts its stream has a stream");

    // 3. Return ! WritableStreamAbort(stream, reason).
    writable_stream_abort(&CallContext::new(vm, writer.closed_promise_realm()), stream, reason)
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-close
pub(crate) fn writable_stream_default_writer_close(vm: &Vm, writer: Ref<WritableStreamDefaultWriter>) -> WebIdlPromise {
    // 1. Let stream be writer.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = writer.stream().expect("a writer that closes its stream has a stream");

    // 3. Return ! WritableStreamClose(stream).
    writable_stream_close(&CallContext::new(vm, writer.closed_promise_realm()), stream)
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-close-with-error-propagation
pub(crate) fn writable_stream_default_writer_close_with_error_propagation(
    vm: &Vm,
    writer: Ref<WritableStreamDefaultWriter>,
) -> WebIdlPromise {
    let realm = writer.closed_promise_realm();

    // 1. Let stream be writer.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = writer.stream().expect("a writer that closes its stream has a stream");

    // 3. Let state be stream.[[state]].
    let state = stream.state();

    // 4. If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state is "closed", return a promise resolved with
    //    undefined.
    if writable_stream_close_queued_or_in_flight(stream) || state == WritableStreamState::Closed {
        return create_resolved_promise(vm, realm, Value::UNDEFINED);
    }

    // 5. If state is "errored", return a promise rejected with stream.[[storedError]].
    if state == WritableStreamState::Errored {
        return create_rejected_promise(vm, realm, stream.stored_error());
    }

    // 6. Assert: state is "writable" or "erroring".
    assert!(state == WritableStreamState::Writable || state == WritableStreamState::Erroring);

    // 7. Return ! WritableStreamDefaultWriterClose(writer).
    writable_stream_default_writer_close(vm, writer)
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-ensure-closed-promise-rejected
pub(crate) fn writable_stream_default_writer_ensure_closed_promise_rejected(
    vm: &Vm,
    writer: Ref<WritableStreamDefaultWriter>,
    error: Value,
) {
    let realm = writer.closed_promise_realm();
    let closed_promise = writer
        .closed_promise()
        .expect("a writer that is set up has a closed promise");

    // 1. If writer.[[closedPromise]].[[PromiseState]] is "pending", reject writer.[[closedPromise]] with error.
    let state = closed_promise
        .promise()
        .downcast::<Promise>()
        .expect("a writer's promises are Promises")
        .state();
    if state == PromiseState::Pending {
        reject_promise(vm, closed_promise, error);
    }
    // 2. Otherwise, set writer.[[closedPromise]] to a promise rejected with error.
    else {
        writer.set_closed_promise(create_rejected_promise(vm, realm, error));
    }

    // 3. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
    mark_promise_as_handled(
        vm,
        writer
            .closed_promise()
            .expect("a writer that is set up has a closed promise"),
    );
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-ensure-ready-promise-rejected
pub(crate) fn writable_stream_default_writer_ensure_ready_promise_rejected(
    vm: &Vm,
    writer: Ref<WritableStreamDefaultWriter>,
    error: Value,
) {
    let realm = writer.ready_promise_realm();
    let ready_promise = writer
        .ready_promise()
        .expect("a writer that is set up has a ready promise");

    // 1. If writer.[[readyPromise]].[[PromiseState]] is "pending", reject writer.[[readyPromise]] with error.
    let state = ready_promise
        .promise()
        .downcast::<Promise>()
        .expect("a writer's promises are Promises")
        .state();
    if state == PromiseState::Pending {
        reject_promise(vm, ready_promise, error);
    }
    // 2. Otherwise, set writer.[[readyPromise]] to a promise rejected with error.
    else {
        writer.set_ready_promise(create_rejected_promise(vm, realm, error));
    }

    // 3. Set writer.[[readyPromise]].[[PromiseIsHandled]] to true.
    mark_promise_as_handled(
        vm,
        writer
            .ready_promise()
            .expect("a writer that is set up has a ready promise"),
    );
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-get-desired-size
pub(crate) fn writable_stream_default_writer_get_desired_size(writer: Ref<WritableStreamDefaultWriter>) -> Option<f64> {
    // 1. Let stream be writer.[[stream]].
    let stream = writer
        .stream()
        .expect("a writer whose desired size is gotten has a stream");

    // 2. Let state be stream.[[state]].
    let state = stream.state();

    // 3. If state is "errored" or "erroring", return null.
    if state == WritableStreamState::Errored || state == WritableStreamState::Erroring {
        return None;
    }

    // 4. If state is "closed", return 0.
    if state == WritableStreamState::Closed {
        return Some(0.0);
    }

    // 5. Return ! WritableStreamDefaultControllerGetDesiredSize(stream.[[controller]]).
    Some(writable_stream_default_controller_get_desired_size(
        stream.controller().expect("a stream that is set up has a controller"),
    ))
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-release
pub(crate) fn writable_stream_default_writer_release(vm: &Vm, writer: Ref<WritableStreamDefaultWriter>) {
    let realm = writer.closed_promise_realm();

    // 1. Let stream be writer.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = writer.stream().expect("a writer that is released has a stream");

    // 3. Assert: stream.[[writer]] is writer.
    assert!(stream.writer() == Some(writer));

    // 4. Let releasedError be a new TypeError.
    let released_error = exception_value(
        vm,
        realm,
        Exception::type_error("Writer's stream lock has been released"),
    );

    // 5. Perform ! WritableStreamDefaultWriterEnsureReadyPromiseRejected(writer, releasedError).
    writable_stream_default_writer_ensure_ready_promise_rejected(vm, writer, released_error);

    // 6. Perform ! WritableStreamDefaultWriterEnsureClosedPromiseRejected(writer, releasedError).
    writable_stream_default_writer_ensure_closed_promise_rejected(vm, writer, released_error);

    // 7. Set stream.[[writer]] to undefined.
    stream.set_writer(None);

    // 8. Set writer.[[stream]] to undefined.
    writer.set_stream(None);
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-write
pub(crate) fn writable_stream_default_writer_write(
    vm: &Vm,
    writer: Ref<WritableStreamDefaultWriter>,
    chunk: Value,
) -> WebIdlPromise {
    let realm = writer.ready_promise_realm();
    let cx = &CallContext::new(vm, realm);

    // 1. Let stream be writer.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = writer.stream().expect("a writer that writes has a stream");

    // 3. Let controller be stream.[[controller]].
    let controller = stream.controller().expect("a stream that is set up has a controller");

    // 4. Let chunkSize be ! WritableStreamDefaultControllerGetChunkSize(controller, chunk).
    let chunk_size = writable_stream_default_controller_get_chunk_size(cx, controller, chunk);

    // 5. If stream is not equal to writer.[[stream]], return a promise rejected with a TypeError exception.
    if writer.stream() != Some(stream) {
        let exception = Exception::type_error("Writer's locked stream changed during write");
        return create_rejected_promise_from_exception(vm, realm, exception);
    }

    // 6. Let state be stream.[[state]].
    let state = stream.state();

    // 7. If state is "errored", return a promise rejected with stream.[[storedError]].
    if state == WritableStreamState::Errored {
        return create_rejected_promise(vm, realm, stream.stored_error());
    }

    // 8. If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state is "closed", return a promise rejected with
    //    a TypeError exception indicating that the stream is closing or closed.
    if writable_stream_close_queued_or_in_flight(stream) || state == WritableStreamState::Closed {
        let exception = Exception::type_error("Cannot write to a writer whose stream is closing or already closed");
        return create_rejected_promise_from_exception(vm, realm, exception);
    }

    // 9. If state is "erroring", return a promise rejected with stream.[[storedError]].
    if state == WritableStreamState::Erroring {
        return create_rejected_promise(vm, realm, stream.stored_error());
    }

    // 10. Assert: state is "writable".
    assert!(state == WritableStreamState::Writable);

    // 11. Let promise be ! WritableStreamAddWriteRequest(stream).
    let promise = writable_stream_add_write_request(cx, stream);

    // 12. Perform ! WritableStreamDefaultControllerWrite(controller, chunk, chunkSize).
    writable_stream_default_controller_write(cx, controller, chunk, chunk_size);

    // 13. Return promise.
    promise
}
