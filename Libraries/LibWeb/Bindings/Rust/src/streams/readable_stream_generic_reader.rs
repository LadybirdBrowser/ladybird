/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStreamGenericReader, https://streams.spec.whatwg.org/#generic-reader-mixin, and the abstract operations of
//! readers, https://streams.spec.whatwg.org/#rs-reader-abstract-ops

use core::cell::Cell;

use libjs_rust::api::{Gc, Realm, Trace, Value, Vm};

use crate::bindings::interface::Ref;
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_rejected_promise, create_rejected_promise_from_exception,
    create_resolved_promise, exception_value, mark_promise_as_handled, promise_realm, reject_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::readable_byte_stream_controller::ReadableByteStreamController;
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamReader, ReadableStreamState, readable_stream_cancel,
};
use crate::streams::readable_stream_default_controller::ReadableStreamDefaultController;
use crate::streams::underlying_source::ReadableStreamController;

/// What a ReadableStreamDefaultReader and a ReadableStreamBYOBReader both are,
/// https://streams.spec.whatwg.org/#readablestreamgenericreader
#[derive(Default, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableStreamGenericReader {
    // https://streams.spec.whatwg.org/#readablestreamgenericreader-closedpromise
    closed_promise: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#readablestreamgenericreader-stream
    stream: Cell<Option<Ref<ReadableStream>>>,
}

impl ReadableStreamGenericReader {
    pub(crate) fn closed_promise(&self) -> Option<WebIdlPromise> {
        self.closed_promise.get()
    }

    pub(crate) fn set_closed_promise(&self, promise: WebIdlPromise) {
        self.closed_promise.set(Some(promise));
    }

    pub(crate) fn stream(&self) -> Option<Ref<ReadableStream>> {
        self.stream.get()
    }

    pub(crate) fn set_stream(&self, stream: Option<Ref<ReadableStream>>) {
        self.stream.set(stream);
    }

    /// The realm of the reader's [[closedPromise]], which the promises and the exceptions of the reader are created
    /// in, as LibWeb's C++ creates them.
    pub(crate) fn closed_promise_realm(&self) -> Gc<Realm> {
        promise_realm(
            self.closed_promise()
                .expect("a reader that is set up has a closed promise"),
        )
    }

    /// The context of the realm of the reader's [[closedPromise]].
    pub(crate) fn context<'vm>(&self, vm: &'vm Vm) -> CallContext<'vm> {
        CallContext::new(vm, self.closed_promise_realm())
    }

    // https://streams.spec.whatwg.org/#generic-reader-closed
    pub(crate) fn closed(&self) -> WebIdlResult<WebIdlPromise> {
        // 1. Return this.[[closedPromise]].
        Ok(self
            .closed_promise()
            .expect("a reader that is set up has a closed promise"))
    }

    // https://streams.spec.whatwg.org/#generic-reader-cancel
    pub(crate) fn cancel(vm: &Vm, reader: ReadableStreamReader, reason: Option<Value>) -> WebIdlPromise {
        // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
        if reader.generic().stream().is_none() {
            let exception = Exception::type_error("No stream present to cancel");
            let realm = reader.generic().closed_promise_realm();
            return create_rejected_promise_from_exception(vm, realm, exception);
        }

        // 2. Return ! ReadableStreamReaderGenericCancel(this, reason).
        readable_stream_reader_generic_cancel(vm, reader, reason.unwrap_or(Value::UNDEFINED))
    }
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-cancel
pub(crate) fn readable_stream_reader_generic_cancel(
    vm: &Vm,
    reader: ReadableStreamReader,
    reason: Value,
) -> WebIdlPromise {
    // 1. Let stream be reader.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = reader.generic().stream().expect("a reader that cancels has a stream");

    // 3. Return ! ReadableStreamCancel(stream, reason).
    readable_stream_cancel(&reader.generic().context(vm), stream, reason)
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-initialize
pub(crate) fn readable_stream_reader_generic_initialize(
    cx: &CallContext,
    reader: ReadableStreamReader,
    stream: Ref<ReadableStream>,
) {
    let vm = cx.vm();
    let generic = reader.generic();

    // 1. Set reader.[[stream]] to stream.
    generic.set_stream(Some(stream));

    // 2. Set stream.[[reader]] to reader.
    stream.set_reader(Some(reader));

    match stream.state() {
        // 3. If stream.[[state]] is "readable",
        ReadableStreamState::Readable => {
            // 1. Set reader.[[closedPromise]] to a new promise.
            generic.set_closed_promise(create_promise(vm, cx.realm()));
        }
        // 4. Otherwise, if stream.[[state]] is "closed",
        ReadableStreamState::Closed => {
            // 1. Set reader.[[closedPromise]] to a promise resolved with undefined.
            generic.set_closed_promise(create_resolved_promise(vm, cx.realm(), Value::UNDEFINED));
        }
        // 5. Otherwise,
        ReadableStreamState::Errored => {
            // 1. Assert: stream.[[state]] is "errored".
            // 2. Set reader.[[closedPromise]] to a promise rejected with stream.[[storedError]].
            let closed_promise = create_rejected_promise(vm, cx.realm(), stream.stored_error());
            generic.set_closed_promise(closed_promise);

            // 3. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
            mark_promise_as_handled(vm, closed_promise);
        }
    }
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-release
pub(crate) fn readable_stream_reader_generic_release(vm: &Vm, reader: ReadableStreamReader) {
    let generic = reader.generic();

    // 1. Let stream be reader.[[stream]].
    // 2. Assert: stream is not undefined.
    let stream = generic.stream().expect("a reader that is released has a stream");

    // 3. Assert: stream.[[reader]] is reader.
    assert!(stream.reader() == Some(reader));

    let realm = generic.closed_promise_realm();
    let exception = exception_value(vm, realm, Exception::type_error("Reader has been released"));

    // 4. If stream.[[state]] is "readable", reject reader.[[closedPromise]] with a TypeError exception.
    if stream.state() == ReadableStreamState::Readable {
        reject_promise(
            vm,
            generic
                .closed_promise()
                .expect("a reader of a stream has a closed promise"),
            exception,
        );
    }
    // 5. Otherwise, set reader.[[closedPromise]] to a promise rejected with a TypeError exception.
    else {
        generic.set_closed_promise(create_rejected_promise(vm, realm, exception));
    }

    // 6. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
    mark_promise_as_handled(
        vm,
        generic
            .closed_promise()
            .expect("a reader of a stream has a closed promise"),
    );

    // 7. Perform ! stream.[[controller]].[[ReleaseSteps]]().
    match stream.controller().expect("a stream that is set up has a controller") {
        ReadableStreamController::ReadableStreamDefaultController(controller) => {
            ReadableStreamDefaultController::release_steps(controller);
        }
        ReadableStreamController::ReadableByteStreamController(controller) => {
            ReadableByteStreamController::release_steps(vm, controller);
        }
    }

    // 8. Set stream.[[reader]] to undefined.
    stream.set_reader(None);

    // 9. Set reader.[[stream]] to undefined.
    generic.set_stream(None);
}
