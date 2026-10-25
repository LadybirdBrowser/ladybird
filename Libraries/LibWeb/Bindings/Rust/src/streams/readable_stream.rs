/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStream, https://streams.spec.whatwg.org/#rs-class, and the abstract operations that work with readable
//! streams, https://streams.spec.whatwg.org/#rs-abstract-ops

use core::cell::Cell;
use core::ffi::c_void;

use libjs_rust::api::{
    Array, Gc, IteratorHint, IteratorRecord, MarkedVec, Object, Realm, Trace, Value, Vm, call, get_iterator,
    iterator_complete, iterator_next, iterator_value,
};

use crate::bindings::async_iterable::{AsyncIterable, END_OF_ITERATION};
use crate::bindings::conversions::{ArrayBufferView, BufferSource, CppInterface, FromJs};
use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_rejected_promise, create_rejected_promise_from_exception,
    create_resolved_promise, mark_promise_as_handled, reject_promise, resolve_promise, upon_fulfillment,
};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter, Transferable};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::streams::algorithms::{CancelAlgorithm, PullAlgorithm, SizeAlgorithm, StartAlgorithm};
use crate::streams::cpp::{CppPromiseAlgorithm, CppPromiseAlgorithmWithArgument};
use crate::streams::cross_realm_transform::{
    create_entangled_ports, deserialize_message_port, serialize_message_port, set_up_cross_realm_transform_readable,
    set_up_cross_realm_transform_writable,
};
use crate::streams::queuing_strategy::{QueuingStrategy, extract_high_water_mark, extract_size_algorithm};
use crate::streams::read_request::{ReadIntoRequest, ReadRequest};
use crate::streams::readable_byte_stream_controller::{
    ReadableByteStreamController, readable_byte_stream_controller_close, readable_byte_stream_controller_enqueue,
    readable_byte_stream_controller_error, readable_byte_stream_controller_get_byob_request,
    readable_byte_stream_controller_respond, set_up_readable_byte_stream_controller,
    set_up_readable_byte_stream_controller_from_underlying_source,
};
use crate::streams::readable_stream_byob_reader::{ReadableStreamBYOBReader, set_up_readable_stream_byob_reader};
use crate::streams::readable_stream_default_controller::{
    ReadableStreamDefaultController, readable_stream_default_controller_close,
    readable_stream_default_controller_enqueue, readable_stream_default_controller_error,
    set_up_readable_stream_default_controller, set_up_readable_stream_default_controller_from_underlying_source,
};
use crate::streams::readable_stream_default_reader::{
    ReadableStreamDefaultReader, readable_stream_default_reader_read, readable_stream_default_reader_release,
    set_up_readable_stream_default_reader,
};
use crate::streams::readable_stream_generic_reader::{
    ReadableStreamGenericReader, readable_stream_reader_generic_cancel,
};
use crate::streams::readable_stream_pipe_to::readable_stream_pipe_to;
use crate::streams::readable_stream_tee::readable_stream_tee;
use crate::streams::transform_stream::TransformStream;
use crate::streams::underlying_source::{ReadableStreamController, ReadableStreamType, UnderlyingSource};
use crate::streams::writable_stream::{WritableStream, is_writable_stream_locked};

unsafe extern "C" {
    /// Tells Page::RenderingStats that the stream, a ReadableStream, no longer holds rendering back.
    fn web_streams_complete_rendering_pending_stream(stream: *mut c_void);
}

// https://streams.spec.whatwg.org/#readablestream-state
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ReadableStreamState {
    Readable,
    Closed,
    Errored,
}

// https://streams.spec.whatwg.org/#enumdef-readablestreamreadermode
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ReadableStreamReaderMode {
    Byob,
}

/// The dictionary ReadableStreamGetReaderOptions of Streams/ReadableStream.idl, whose members the bindings convert.
pub(crate) struct ReadableStreamGetReaderOptions {
    pub(crate) mode: Option<ReadableStreamReaderMode>,
}

/// The dictionary ReadableWritablePair of Streams/ReadableStream.idl, whose members the bindings convert.
pub(crate) struct ReadableWritablePair {
    pub(crate) readable: Ref<ReadableStream>,
    pub(crate) writable: Ref<WritableStream>,
}

/// The dictionary StreamPipeOptions of Streams/ReadableStream.idl, whose members the bindings convert.
pub(crate) struct StreamPipeOptions {
    pub(crate) prevent_abort: bool,
    pub(crate) prevent_cancel: bool,
    pub(crate) prevent_close: bool,
    pub(crate) signal: Option<CppInterface>,
}

/// The dictionary ReadableStreamIteratorOptions of Streams/ReadableStream.idl, whose members the bindings convert.
pub(crate) struct ReadableStreamIteratorOptions {
    pub(crate) prevent_cancel: bool,
}

/// https://streams.spec.whatwg.org/#typedefdef-readablestreamreader
#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ReadableStreamReader {
    ReadableStreamDefaultReader(Ref<ReadableStreamDefaultReader>),
    ReadableStreamBYOBReader(Ref<ReadableStreamBYOBReader>),
}

impl ReadableStreamReader {
    /// What the reader is as a ReadableStreamGenericReader, https://streams.spec.whatwg.org/#readablestreamgenericreader
    pub(crate) fn generic(&self) -> &ReadableStreamGenericReader {
        match self {
            Self::ReadableStreamDefaultReader(reader) => reader.generic(),
            Self::ReadableStreamBYOBReader(reader) => reader.generic(),
        }
    }
}

// https://streams.spec.whatwg.org/#readablestream
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableStream {
    /// The realm of the stream, which its promises and the results of its operations are created in.
    realm: Cell<Gc<Realm>>,
    // https://streams.spec.whatwg.org/#readablestream-controller
    controller: Cell<Option<ReadableStreamController>>,
    // https://streams.spec.whatwg.org/#readablestream-detached
    detached: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablestream-disturbed
    disturbed: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablestream-reader
    reader: Cell<Option<ReadableStreamReader>>,
    // https://streams.spec.whatwg.org/#readablestream-state
    #[gc(untraced)]
    state: Cell<ReadableStreamState>,
    // https://streams.spec.whatwg.org/#readablestream-storederror
    stored_error: Cell<Value>,
}

impl Wrappable for ReadableStream {
    const FINALIZES: bool = true;

    fn finalize(stream: Ref<Self>) {
        complete_rendering_pending_stream(stream);
    }
}

impl ReadableStream {
    /// A new ReadableStream in the realm, which InitializeReadableStream has initialized.
    pub(crate) fn new(realm: Gc<Realm>) -> Self {
        Self {
            realm: Cell::new(realm),
            controller: Cell::new(None),
            detached: Cell::new(false),
            disturbed: Cell::new(false),
            reader: Cell::new(None),
            state: Cell::new(ReadableStreamState::Readable),
            stored_error: Cell::new(Value::UNDEFINED),
        }
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm.get()
    }

    pub(crate) fn set_realm(&self, realm: Gc<Realm>) {
        self.realm.set(realm);
    }

    pub(crate) fn controller(&self) -> Option<ReadableStreamController> {
        self.controller.get()
    }

    pub(crate) fn set_controller(&self, controller: Option<ReadableStreamController>) {
        self.controller.set(controller);
    }

    pub(crate) fn disturbed(&self) -> bool {
        self.disturbed.get()
    }

    pub(crate) fn set_disturbed(&self, disturbed: bool) {
        self.disturbed.set(disturbed);
    }

    pub(crate) fn reader(&self) -> Option<ReadableStreamReader> {
        self.reader.get()
    }

    pub(crate) fn set_reader(&self, reader: Option<ReadableStreamReader>) {
        self.reader.set(reader);
    }

    pub(crate) fn state(&self) -> ReadableStreamState {
        self.state.get()
    }

    pub(crate) fn set_state(&self, state: ReadableStreamState) {
        self.state.set(state);
    }

    pub(crate) fn stored_error(&self) -> Value {
        self.stored_error.get()
    }

    pub(crate) fn set_stored_error(&self, error: Value) {
        self.stored_error.set(error);
    }

    /// The context of the realm of the stream.
    pub(crate) fn context<'vm>(&self, vm: &'vm Vm) -> CallContext<'vm> {
        CallContext::new(vm, self.realm())
    }

    // https://streams.spec.whatwg.org/#rs-constructor
    pub(crate) fn construct(
        cx: &mut CallContext,
        underlying_source: Option<Gc<Object>>,
        strategy: QueuingStrategy,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. If underlyingSource is missing, set it to null.
        let underlying_source = underlying_source.map_or(Value::NULL, Value::from_object);

        // 2. Let underlyingSourceDict be underlyingSource, converted to an IDL value of type UnderlyingSource.
        let underlying_source_dict = UnderlyingSource::from_js(cx.vm(), underlying_source)?;

        // 3. Perform ! InitializeReadableStream(this).
        let this = allocate(cx.vm(), Self::new(cx.realm()));

        // 4. If underlyingSourceDict["type"] is "bytes":
        if underlying_source_dict.r#type == Some(ReadableStreamType::Bytes) {
            // 1. If strategy["size"] exists, throw a RangeError exception.
            if strategy.size.is_some() {
                return Err(Exception::range_error("Size strategy not allowed for byte stream"));
            }

            // 2. Let highWaterMark be ? ExtractHighWaterMark(strategy, 0).
            let high_water_mark = extract_high_water_mark(&strategy, 0.0)?;

            // 3. Perform ? SetUpReadableByteStreamControllerFromUnderlyingSource(this, underlyingSource,
            //    underlyingSourceDict, highWaterMark).
            set_up_readable_byte_stream_controller_from_underlying_source(
                cx,
                this,
                underlying_source,
                underlying_source_dict,
                high_water_mark,
            )?;
        }
        // 5. Otherwise,
        else {
            // 1. Assert: underlyingSourceDict["type"] does not exist.
            assert!(underlying_source_dict.r#type.is_none());

            // 2. Let sizeAlgorithm be ! ExtractSizeAlgorithm(strategy).
            let size_algorithm = extract_size_algorithm(&strategy);

            // 3. Let highWaterMark be ? ExtractHighWaterMark(strategy, 1).
            let high_water_mark = extract_high_water_mark(&strategy, 1.0)?;

            // 4. Perform ? SetUpReadableStreamDefaultControllerFromUnderlyingSource(this, underlyingSource,
            //    underlyingSourceDict, highWaterMark, sizeAlgorithm).
            set_up_readable_stream_default_controller_from_underlying_source(
                cx,
                this,
                underlying_source,
                underlying_source_dict,
                high_water_mark,
                size_algorithm,
            )?;
        }
        Ok(this)
    }

    // https://streams.spec.whatwg.org/#rs-from
    pub(crate) fn from(cx: &mut CallContext, async_iterable: Value) -> WebIdlResult<Ref<Self>> {
        // 1. Return ? ReadableStreamFromIterable(asyncIterable).
        readable_stream_from_iterable(cx, async_iterable)
    }

    // https://streams.spec.whatwg.org/#rs-locked
    pub(crate) fn locked(this: &Ref<Self>, _: &mut CallContext) -> WebIdlResult<bool> {
        // 1. Return ! IsReadableStreamLocked(this).
        Ok(is_readable_stream_locked(*this))
    }

    // https://streams.spec.whatwg.org/#rs-cancel
    pub(crate) fn cancel(this: &Ref<Self>, cx: &mut CallContext, reason: Option<Value>) -> WebIdlResult<WebIdlPromise> {
        let cx = &this.context(cx.vm());

        // 1. If ! IsReadableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
        if is_readable_stream_locked(*this) {
            let exception = Exception::type_error("Cannot cancel a locked stream");
            return Ok(create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception));
        }

        // 2. Return ! ReadableStreamCancel(this, reason).
        Ok(readable_stream_cancel(cx, *this, reason.unwrap_or(Value::UNDEFINED)))
    }

    // https://streams.spec.whatwg.org/#rs-get-reader
    pub(crate) fn get_reader(
        this: &Ref<Self>,
        cx: &mut CallContext,
        options: ReadableStreamGetReaderOptions,
    ) -> WebIdlResult<ReadableStreamReader> {
        let cx = &this.context(cx.vm());

        // 1. If options["mode"] does not exist, return ? AcquireReadableStreamDefaultReader(this).
        let Some(mode) = options.mode else {
            return Ok(ReadableStreamReader::ReadableStreamDefaultReader(
                acquire_readable_stream_default_reader(cx, *this)?,
            ));
        };

        // 2. Assert: options["mode"] is "byob".
        assert!(mode == ReadableStreamReaderMode::Byob);

        // 3. Return ? AcquireReadableStreamBYOBReader(this).
        Ok(ReadableStreamReader::ReadableStreamBYOBReader(
            acquire_readable_stream_byob_reader(cx, *this)?,
        ))
    }

    // https://streams.spec.whatwg.org/#rs-pipe-through
    pub(crate) fn pipe_through(
        this: &Ref<Self>,
        cx: &mut CallContext,
        transform: ReadableWritablePair,
        options: StreamPipeOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let cx = &this.context(cx.vm());

        // 1. If ! IsReadableStreamLocked(this) is true, throw a TypeError exception.
        if is_readable_stream_locked(*this) {
            return Err(Exception::type_error(
                "Failed to execute 'pipeThrough' on 'ReadableStream': Cannot pipe a locked stream",
            ));
        }

        // 2. If ! IsWritableStreamLocked(transform["writable"]) is true, throw a TypeError exception.
        if is_writable_stream_locked(transform.writable) {
            return Err(Exception::type_error(
                "Failed to execute 'pipeThrough' on 'ReadableStream': parameter 1's 'writable' is locked",
            ));
        }

        // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
        let signal = options.signal;

        // 4. Let promise be ! ReadableStreamPipeTo(this, transform["writable"], options["preventClose"],
        //    options["preventAbort"], options["preventCancel"], signal).
        let promise = readable_stream_pipe_to(
            cx,
            *this,
            transform.writable,
            options.prevent_close,
            options.prevent_abort,
            options.prevent_cancel,
            signal,
        );

        // 5. Set promise.[[PromiseIsHandled]] to true.
        mark_promise_as_handled(cx.vm(), promise);

        // 6. Return transform["readable"].
        Ok(transform.readable)
    }

    // https://streams.spec.whatwg.org/#rs-pipe-to
    pub(crate) fn pipe_to(
        this: &Ref<Self>,
        cx: &mut CallContext,
        destination: Ref<WritableStream>,
        options: StreamPipeOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        let cx = &this.context(cx.vm());

        // 1. If ! IsReadableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
        if is_readable_stream_locked(*this) {
            let exception =
                Exception::type_error("Failed to execute 'pipeTo' on 'ReadableStream': Cannot pipe a locked stream");
            return Ok(create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception));
        }

        // 2. If ! IsWritableStreamLocked(destination) is true, return a promise rejected with a TypeError exception.
        if is_writable_stream_locked(destination) {
            let exception = Exception::type_error(
                "Failed to execute 'pipeTo' on 'ReadableStream':  Cannot pipe to a locked stream",
            );
            return Ok(create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception));
        }

        // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
        let signal = options.signal;

        // 4. Return ! ReadableStreamPipeTo(this, destination, options["preventClose"], options["preventAbort"],
        //    options["preventCancel"], signal).
        Ok(readable_stream_pipe_to(
            cx,
            *this,
            destination,
            options.prevent_close,
            options.prevent_abort,
            options.prevent_cancel,
            signal,
        ))
    }

    // https://streams.spec.whatwg.org/#rs-tee
    pub(crate) fn tee<'vm>(this: &Ref<Self>, cx: &mut CallContext<'vm>) -> WebIdlResult<MarkedVec<'vm, Ref<Self>>> {
        let vm = cx.vm();
        let cx = &this.context(vm);

        // 1. Return ? ReadableStreamTee(this, false).
        let (branch1, branch2) = readable_stream_tee(cx, *this, false)?;
        let branches = MarkedVec::with_capacity(vm, 2);
        branches.push(branch1);
        branches.push(branch2);
        Ok(branches)
    }

    // https://streams.spec.whatwg.org/#ref-for-asynchronous-iterator-initialization-steps
    pub(crate) fn asynchronous_iterator_initialization_steps(
        this: &Ref<Self>,
        cx: &mut CallContext,
        options: ReadableStreamIteratorOptions,
    ) -> WebIdlResult<ReadableStreamAsyncIteratorState> {
        let realm_cx = &this.context(cx.vm());

        // 1. Let reader be ? AcquireReadableStreamDefaultReader(stream).
        // 2. Set iterator’s reader to reader.
        let reader = acquire_readable_stream_default_reader(realm_cx, *this)?;

        // 3. Let preventCancel be args[0]["preventCancel"].
        // 4. Set iterator’s prevent cancel to preventCancel.
        Ok(ReadableStreamAsyncIteratorState {
            reader,
            prevent_cancel: options.prevent_cancel,
        })
    }
}

/// The state of an asynchronous iterator of a ReadableStream, https://streams.spec.whatwg.org/#rs-asynciterator
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableStreamAsyncIteratorState {
    reader: Ref<ReadableStreamDefaultReader>,
    #[gc(untraced)]
    prevent_cancel: bool,
}

impl AsyncIterable for ReadableStream {
    type IteratorState = ReadableStreamAsyncIteratorState;

    // https://streams.spec.whatwg.org/#ref-for-dfn-get-the-next-iteration-result
    fn get_the_next_iteration_result(cx: &CallContext, iterator: &ReadableStreamAsyncIteratorState) -> WebIdlPromise {
        // 1. Let reader be iterator’s reader.
        let reader = iterator.reader;

        // 2. Assert: reader.[[stream]] is not undefined.
        assert!(reader.generic().stream().is_some());

        // 3. Let promise be a new promise.
        let promise = create_promise(cx.vm(), cx.realm());

        // 4. Let readRequest be a new read request with the following items:
        let read_request = ReadRequest::AsyncIterator { reader, promise };

        // 5. Perform ! ReadableStreamDefaultReaderRead(this, readRequest).
        readable_stream_default_reader_read(cx.vm(), reader, read_request);

        // 6. Return promise.
        promise
    }

    // https://streams.spec.whatwg.org/#ref-for-asynchronous-iterator-return
    fn asynchronous_iterator_return(
        cx: &CallContext,
        iterator: &ReadableStreamAsyncIteratorState,
        arg: Value,
    ) -> WebIdlPromise {
        // 1. Let reader be iterator’s reader.
        let reader = iterator.reader;

        // 2. Assert: reader.[[stream]] is not undefined.
        assert!(reader.generic().stream().is_some());

        // 3. Assert: reader.[[readRequests]] is empty, as the async iterator machinery guarantees that any previous
        //    calls to next() have settled before this is called.
        assert!(reader.read_requests_is_empty());

        // 4. If iterator’s prevent cancel is false:
        if !iterator.prevent_cancel {
            // 1. Let result be ! ReadableStreamReaderGenericCancel(reader, arg).
            let result = readable_stream_reader_generic_cancel(
                cx.vm(),
                ReadableStreamReader::ReadableStreamDefaultReader(reader),
                arg,
            );

            // 2. Perform ! ReadableStreamDefaultReaderRelease(reader).
            readable_stream_default_reader_release(cx.vm(), reader);

            // 3. Return result.
            return result;
        }

        // 5. Perform ! ReadableStreamDefaultReaderRelease(reader).
        readable_stream_default_reader_release(cx.vm(), reader);

        // 6. Return a promise resolved with undefined.
        create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
    }
}

/// The steps of the read request of getting the next iteration result of a ReadableStream's asynchronous iterator,
/// https://streams.spec.whatwg.org/#ref-for-dfn-get-the-next-iteration-result
pub(crate) mod async_iterator_read_request_steps {
    use super::*;

    // chunk steps, given chunk
    pub(crate) fn chunk_steps(vm: &Vm, _: Ref<ReadableStreamDefaultReader>, promise: WebIdlPromise, chunk: Value) {
        // 1. Resolve promise with chunk.
        resolve_promise(vm, promise, chunk);
    }

    // close steps
    pub(crate) fn close_steps(vm: &Vm, reader: Ref<ReadableStreamDefaultReader>, promise: WebIdlPromise) {
        // 1. Perform ! ReadableStreamDefaultReaderRelease(reader).
        readable_stream_default_reader_release(vm, reader);

        // 2. Resolve promise with end of iteration.
        resolve_promise(vm, promise, END_OF_ITERATION);
    }

    // error steps, given e
    pub(crate) fn error_steps(vm: &Vm, reader: Ref<ReadableStreamDefaultReader>, promise: WebIdlPromise, error: Value) {
        // 1. Perform ! ReadableStreamDefaultReaderRelease(reader).
        readable_stream_default_reader_release(vm, reader);

        // 2. Reject promise with e.
        reject_promise(vm, promise, error);
    }
}

impl Transferable for ReadableStream {
    fn detached(&self) -> &Cell<bool> {
        &self.detached
    }

    // https://streams.spec.whatwg.org/#ref-for-transfer-steps
    fn transfer_steps(
        this: &Ref<Self>,
        cx: &mut CallContext,
        data_holder: &mut TransferDataWriter,
    ) -> WebIdlResult<()> {
        let this = *this;
        let cx = &*cx;
        with_temporary_execution_context_and_callbacks(cx.realm(), || {
            // 1. If ! IsReadableStreamLocked(value) is true, throw a "DataCloneError" DOMException.
            if is_readable_stream_locked(this) {
                return Err(Exception::Dom(
                    "DataCloneError",
                    ak::Utf16String::from_ascii(b"Cannot transfer locked ReadableStream"),
                ));
            }

            // 2. Let port1 be a new MessagePort in the current Realm.
            // 3. Let port2 be a new MessagePort in the current Realm.
            // 4. Entangle port1 and port2.
            let (port1, port2) = create_entangled_ports(cx);

            // 5. Let writable be a new WritableStream in the current Realm.
            let writable = allocate(cx.vm(), WritableStream::new(cx.realm()));

            // 6. Perform ! SetUpCrossRealmTransformWritable(writable, port1).
            set_up_cross_realm_transform_writable(cx, writable, port1);

            // 7. Let promise be ! ReadableStreamPipeTo(value, writable, false, false, false).
            let promise = readable_stream_pipe_to(cx, this, writable, false, false, false, None);

            // 8. Set promise.[[PromiseIsHandled]] to true.
            mark_promise_as_handled(cx.vm(), promise);

            // 9. Set dataHolder.[[port]] to ! StructuredSerializeWithTransfer(port2, « port2 »).
            serialize_message_port(cx, data_holder, &port2);
            Ok(())
        })
    }

    // https://streams.spec.whatwg.org/#ref-for-transfer-receiving-steps
    fn transfer_receiving_steps(cx: &mut CallContext, data_holder: &mut TransferDataReader) -> WebIdlResult<Ref<Self>> {
        let cx = &*cx;
        with_temporary_execution_context_and_callbacks(cx.realm(), || {
            let value = allocate(cx.vm(), Self::new(cx.realm()));

            // 1. Let deserializedRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[port]], the current Realm).
            // 2. Let port be deserializedRecord.[[Deserialized]].
            let port = deserialize_message_port(cx, data_holder);

            // 3. Perform ! SetUpCrossRealmTransformReadable(value, port).
            set_up_cross_realm_transform_readable(cx, value, port);
            Ok(value)
        })
    }
}

/// What specifications that set up and read ReadableStreams use, https://streams.spec.whatwg.org/#other-specs-rs
impl ReadableStream {
    // https://streams.spec.whatwg.org/#readablestream-set-up-with-byte-reading-support
    pub(crate) fn set_up_with_byte_reading_support(
        stream: Ref<Self>,
        cx: &CallContext,
        pull_algorithm: Option<CppPromiseAlgorithm>,
        cancel_algorithm: Option<CppPromiseAlgorithmWithArgument>,
        high_water_mark: f64,
    ) {
        stream.set_realm(cx.realm());

        // 1. Let startAlgorithm be an algorithm that returns undefined.
        let start_algorithm = StartAlgorithm::ReturnsUndefined;

        // 2. Let pullAlgorithmWrapper be an algorithm that runs these steps:
        let pull_algorithm_wrapper = PullAlgorithm::WithByteReadingSupport(pull_algorithm);

        // 3. Let cancelAlgorithmWrapper be an algorithm that runs these steps:
        let cancel_algorithm_wrapper = CancelAlgorithm::Wrapper(cancel_algorithm);

        // 4. Perform ! InitializeReadableStream(stream).
        // 5. Let controller be a new ReadableByteStreamController.
        let controller = allocate(cx.vm(), ReadableByteStreamController::new());

        // 6. Perform ! SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithmWrapper,
        //    cancelAlgorithmWrapper, highWaterMark, undefined).
        set_up_readable_byte_stream_controller(
            cx,
            stream,
            controller,
            start_algorithm,
            pull_algorithm_wrapper,
            cancel_algorithm_wrapper,
            high_water_mark,
            None,
        )
        .must();
    }

    // https://streams.spec.whatwg.org/#readablestream-close
    pub(crate) fn close(this: Ref<Self>, vm: &Vm) {
        let cx = &this.context(vm);
        match this.controller().expect("a stream that is set up has a controller") {
            // 1. If stream.[[controller]] implements ReadableByteStreamController,
            ReadableStreamController::ReadableByteStreamController(controller) => {
                // 1. Perform ! ReadableByteStreamControllerClose(stream.[[controller]]).
                // AD-HOC: This throws, having errored the stream, when a pending pull-into holds part of an element.
                //         See: https://github.com/whatwg/streams/issues/1387
                if readable_byte_stream_controller_close(cx, controller).is_err() {
                    return;
                }

                // 2. If stream.[[controller]].[[pendingPullIntos]] is not empty, perform
                //    ! ReadableByteStreamControllerRespond(stream.[[controller]], 0).
                if !controller.pending_pull_intos_is_empty() {
                    readable_byte_stream_controller_respond(cx, controller, 0).must();
                }
            }
            // 2. Otherwise, perform ! ReadableStreamDefaultControllerClose(stream.[[controller]]).
            ReadableStreamController::ReadableStreamDefaultController(controller) => {
                readable_stream_default_controller_close(cx, controller);
            }
        }
    }

    // https://streams.spec.whatwg.org/#readablestream-error
    pub(crate) fn error(this: Ref<Self>, vm: &Vm, error: Value) {
        let cx = &this.context(vm);
        match this.controller().expect("a stream that is set up has a controller") {
            // 1. If stream.[[controller]] implements ReadableByteStreamController, then perform
            //    ! ReadableByteStreamControllerError(stream.[[controller]], e).
            ReadableStreamController::ReadableByteStreamController(controller) => {
                readable_byte_stream_controller_error(cx, controller, error);
            }
            // 2. Otherwise, perform ! ReadableStreamDefaultControllerError(stream.[[controller]], e).
            ReadableStreamController::ReadableStreamDefaultController(controller) => {
                readable_stream_default_controller_error(cx, controller, error);
            }
        }
    }

    // https://streams.spec.whatwg.org/#readablestream-readable
    pub(crate) fn is_readable(&self) -> bool {
        // A ReadableStream stream is readable if stream.[[state]] is "readable".
        self.state() == ReadableStreamState::Readable
    }

    // https://streams.spec.whatwg.org/#readablestream-get-a-reader
    pub(crate) fn get_a_reader(this: Ref<Self>, vm: &Vm) -> WebIdlResult<Ref<ReadableStreamDefaultReader>> {
        // To get a reader for a ReadableStream stream, return ? AcquireReadableStreamDefaultReader(stream). The result
        // will be a ReadableStreamDefaultReader.
        acquire_readable_stream_default_reader(&this.context(vm), this)
    }

    // https://streams.spec.whatwg.org/#readablestream-current-byob-request-view
    pub(crate) fn current_byob_request_view(this: Ref<Self>, vm: &Vm) -> Option<ArrayBufferView> {
        // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
        let Some(ReadableStreamController::ReadableByteStreamController(controller)) = this.controller() else {
            panic!("the stream has a ReadableByteStreamController");
        };

        // 2. Let byobRequest be ! ReadableByteStreamControllerGetBYOBRequest(stream.[[controller]]).
        // 3. If byobRequest is null, then return null.
        let byob_request = readable_byte_stream_controller_get_byob_request(&this.context(vm), controller)?;

        // 4. Return byobRequest.[[view]].
        Some(ArrayBufferView::TypedArray(
            byob_request
                .current_view()
                .expect("a BYOB request of a controller has a view"),
        ))
    }

    // https://streams.spec.whatwg.org/#readablestream-enqueue
    pub(crate) fn enqueue(this: Ref<Self>, vm: &Vm, chunk: Value) -> WebIdlResult<()> {
        let cx = &this.context(vm);
        match this.controller().expect("a stream that is set up has a controller") {
            // 1. If stream.[[controller]] implements ReadableStreamDefaultController,
            ReadableStreamController::ReadableStreamDefaultController(controller) => {
                // 1. Perform ! ReadableStreamDefaultControllerEnqueue(stream.[[controller]], chunk).
                readable_stream_default_controller_enqueue(cx, controller, chunk).must();
            }
            // 2. Otherwise,
            ReadableStreamController::ReadableByteStreamController(controller) => {
                // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
                // 2. Assert: chunk is an ArrayBufferView.
                let chunk_view = match crate::bindings::conversions::buffer_source_type_of(chunk) {
                    Some(BufferSource::TypedArray(typed_array)) => ArrayBufferView::TypedArray(typed_array),
                    Some(BufferSource::DataView(view)) => ArrayBufferView::DataView(view),
                    _ => panic!("a chunk of a byte stream is an ArrayBufferView"),
                };
                let chunk_extent = BufferSource::from(chunk_view).extent();

                // 3. Let byobView be the current BYOB request view for stream.
                let byob_view = Self::current_byob_request_view(this, vm);

                // 4. If byobView is non-null, and chunk.[[ViewedArrayBuffer]] is byobView.[[ViewedArrayBuffer]], then:
                let byob_extent = byob_view.map(|byob_view| BufferSource::from(byob_view).extent());
                if let Some(byob_extent) = byob_extent
                    && chunk_extent.buffer == byob_extent.buffer
                {
                    // 1. Assert: chunk.[[ByteOffset]] is byobView.[[ByteOffset]].
                    assert!(chunk_extent.byte_offset == byob_extent.byte_offset);

                    // 2. Assert: chunk.[[ByteLength]] ≤ byobView.[[ByteLength]].
                    assert!(chunk_extent.byte_length <= byob_extent.byte_length);

                    // 3. Perform ? ReadableByteStreamControllerRespond(stream.[[controller]], chunk.[[ByteLength]]).
                    readable_byte_stream_controller_respond(cx, controller, chunk_extent.byte_length as u64)?;
                }
                // 5. Otherwise, perform ? ReadableByteStreamControllerEnqueue(stream.[[controller]], chunk).
                else {
                    readable_byte_stream_controller_enqueue(cx, controller, chunk_view)?;
                }
            }
        }
        Ok(())
    }

    // https://streams.spec.whatwg.org/#readablestream-pipe-through
    pub(crate) fn piped_through(
        this: Ref<Self>,
        vm: &Vm,
        transform: Ref<TransformStream>,
        prevent_close: bool,
        prevent_abort: bool,
        prevent_cancel: bool,
        signal: Option<CppInterface>,
    ) -> Ref<Self> {
        let cx = &this.context(vm);

        // 1. Assert: ! IsReadableStreamLocked(readable) is false.
        assert!(!is_readable_stream_locked(this));

        // 2. Assert: ! IsWritableStreamLocked(transform.[[writable]]) is false.
        assert!(!is_writable_stream_locked(transform.writable_stream()));

        // 3. Let signalArg be signal if signal was given, or undefined otherwise.
        // 4. Let promise be ! ReadableStreamPipeTo(readable, transform.[[writable]], preventClose, preventAbort,
        //    preventCancel, signalArg).
        let promise = readable_stream_pipe_to(
            cx,
            this,
            transform.writable_stream(),
            prevent_close,
            prevent_abort,
            prevent_cancel,
            signal,
        );

        // 5. Set promise.[[PromiseIsHandled]] to true.
        mark_promise_as_handled(vm, promise);

        // 6. Return transform.[[readable]].
        transform.readable_stream()
    }
}

/// Tells Page::RenderingStats that the stream no longer holds rendering back, once it has closed or errored or its
/// controller has been asked to close.
pub(crate) fn complete_rendering_pending_stream(stream: Ref<ReadableStream>) {
    // SAFETY: The stream is a live ReadableStream.
    unsafe { web_streams_complete_rendering_pending_stream(stream.as_ptr().cast()) };
}

// https://streams.spec.whatwg.org/#acquire-readable-stream-byob-reader
pub(crate) fn acquire_readable_stream_byob_reader(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
) -> WebIdlResult<Ref<ReadableStreamBYOBReader>> {
    // 1. Let reader be a new ReadableStreamBYOBReader.
    let reader = allocate(cx.vm(), ReadableStreamBYOBReader::new());

    // 2. Perform ? SetUpReadableStreamBYOBReader(reader, stream).
    set_up_readable_stream_byob_reader(cx, reader, stream)?;

    // 3. Return reader.
    Ok(reader)
}

// https://streams.spec.whatwg.org/#acquire-readable-stream-reader
pub(crate) fn acquire_readable_stream_default_reader(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
) -> WebIdlResult<Ref<ReadableStreamDefaultReader>> {
    // 1. Let reader be a new ReadableStreamDefaultReader.
    let reader = allocate(cx.vm(), ReadableStreamDefaultReader::new());

    // 2. Perform ? SetUpReadableStreamDefaultReader(reader, stream).
    set_up_readable_stream_default_reader(cx, reader, stream)?;

    // 3. Return reader.
    Ok(reader)
}

// https://streams.spec.whatwg.org/#create-readable-stream
pub(crate) fn create_readable_stream(
    cx: &CallContext,
    start_algorithm: StartAlgorithm,
    pull_algorithm: PullAlgorithm,
    cancel_algorithm: CancelAlgorithm,
    high_water_mark: Option<f64>,
    size_algorithm: Option<SizeAlgorithm>,
) -> WebIdlResult<Ref<ReadableStream>> {
    // 4. Let stream be a new ReadableStream.
    // 5. Perform ! InitializeReadableStream(stream).
    let stream = allocate(cx.vm(), ReadableStream::new(cx.realm()));
    create_readable_stream_with_existing_stream(
        cx,
        stream,
        start_algorithm,
        pull_algorithm,
        cancel_algorithm,
        high_water_mark,
        size_algorithm,
    )?;
    Ok(stream)
}

/// CreateReadableStream for a stream that the caller created, for algorithms that refer to the stream before it is
/// set up, https://streams.spec.whatwg.org/#create-readable-stream
pub(crate) fn create_readable_stream_with_existing_stream(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    start_algorithm: StartAlgorithm,
    pull_algorithm: PullAlgorithm,
    cancel_algorithm: CancelAlgorithm,
    high_water_mark: Option<f64>,
    size_algorithm: Option<SizeAlgorithm>,
) -> WebIdlResult<()> {
    // 1. If highWaterMark was not passed, set it to 1.
    let high_water_mark = high_water_mark.unwrap_or(1.0);

    // 2. If sizeAlgorithm was not passed, set it to an algorithm that returns 1.
    let size_algorithm = size_algorithm.unwrap_or(SizeAlgorithm::ReturnsOne);

    // 3. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
    assert!(high_water_mark >= 0.0);

    // 4. Let stream be a new ReadableStream.
    // 5. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(stream);

    // 6. Let controller be a new ReadableStreamDefaultController.
    let controller = allocate(cx.vm(), ReadableStreamDefaultController::new());

    // 7. Perform ? SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm,
    //    cancelAlgorithm, highWaterMark, sizeAlgorithm).
    set_up_readable_stream_default_controller(
        cx,
        stream,
        controller,
        start_algorithm,
        pull_algorithm,
        cancel_algorithm,
        high_water_mark,
        size_algorithm,
    )

    // 8. Return stream.
}

// https://streams.spec.whatwg.org/#abstract-opdef-createreadablebytestream
pub(crate) fn create_readable_byte_stream(
    cx: &CallContext,
    start_algorithm: StartAlgorithm,
    pull_algorithm: PullAlgorithm,
    cancel_algorithm: CancelAlgorithm,
) -> WebIdlResult<Ref<ReadableStream>> {
    // 1. Let stream be a new ReadableStream.
    // 2. Perform ! InitializeReadableStream(stream).
    let stream = allocate(cx.vm(), ReadableStream::new(cx.realm()));

    // 3. Let controller be a new ReadableByteStreamController.
    let controller = allocate(cx.vm(), ReadableByteStreamController::new());

    // 4. Perform ? SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm,
    //    cancelAlgorithm, 0, undefined).
    set_up_readable_byte_stream_controller(
        cx,
        stream,
        controller,
        start_algorithm,
        pull_algorithm,
        cancel_algorithm,
        0.0,
        None,
    )?;

    // 5. Return stream.
    Ok(stream)
}

// https://streams.spec.whatwg.org/#initialize-readable-stream
pub(crate) fn initialize_readable_stream(stream: Ref<ReadableStream>) {
    // 1. Set stream.[[state]] to "readable".
    stream.set_state(ReadableStreamState::Readable);

    // 2. Set stream.[[reader]] and stream.[[storedError]] to undefined.
    stream.set_reader(None);
    stream.set_stored_error(Value::UNDEFINED);

    // 3. Set stream.[[disturbed]] to false.
    stream.set_disturbed(false);
}

// https://streams.spec.whatwg.org/#is-readable-stream-locked
pub(crate) fn is_readable_stream_locked(stream: Ref<ReadableStream>) -> bool {
    // 1. If stream.[[reader]] is undefined, return false.
    // 2. Return true.
    stream.reader().is_some()
}

// https://streams.spec.whatwg.org/#readable-stream-from-iterable
pub(crate) fn readable_stream_from_iterable(
    cx: &CallContext,
    async_iterable: Value,
) -> WebIdlResult<Ref<ReadableStream>> {
    // 1. Let stream be undefined.
    // NB: The algorithms below refer to the stream, so it is created here.
    let stream = allocate(cx.vm(), ReadableStream::new(cx.realm()));

    // 2. Let iteratorRecord be ? GetIterator(asyncIterable, async).
    let iterator_record = get_iterator(cx.vm(), async_iterable, IteratorHint::Async)?;

    // 3. Let startAlgorithm be an algorithm that returns undefined.
    let start_algorithm = StartAlgorithm::ReturnsUndefined;

    // 4. Let pullAlgorithm be the following steps:
    let pull_algorithm = PullAlgorithm::FromIterable {
        stream,
        iterator_record,
    };

    // 5. Let cancelAlgorithm be the following steps, given reason:
    let cancel_algorithm = CancelAlgorithm::FromIterable { iterator_record };

    // 6. Set stream to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, 0).
    create_readable_stream_with_existing_stream(
        cx,
        stream,
        start_algorithm,
        pull_algorithm,
        cancel_algorithm,
        Some(0.0),
        None,
    )
    .must();

    // 7. Return stream.
    Ok(stream)
}

/// The pullAlgorithm of ReadableStreamFromIterable, https://streams.spec.whatwg.org/#readable-stream-from-iterable
pub(crate) fn readable_stream_from_iterable_pull_algorithm(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    iterator_record: Gc<IteratorRecord>,
) -> WebIdlPromise {
    let vm = cx.vm();

    // 1. Let nextResult be IteratorNext(iteratorRecord).
    let next_result = iterator_next(vm, &iterator_record, None);

    // 2. If nextResult is an abrupt completion, return a promise rejected with nextResult.[[Value]].
    let next_result = match next_result {
        Ok(next_result) => next_result,
        Err(throw) => return create_rejected_promise(vm, cx.realm(), throw.value()),
    };

    // 3. Let nextPromise be a promise resolved with nextResult.[[Value]].
    let next_promise = create_resolved_promise(vm, cx.realm(), Value::from_object(next_result));

    // 4. Return the result of reacting to nextPromise with the following fulfillment steps, given iterResult:
    upon_fulfillment(vm, next_promise, stream, |cx, stream, iter_result| {
        let vm = cx.vm();

        // 1. If iterResult is not an Object, throw a TypeError.
        if !iter_result.is_object() {
            return Err(Exception::type_error("iterResult is not an Object"));
        }

        // 2. Let done be ? IteratorComplete(iterResult).
        let done = iterator_complete(vm, iter_result.as_object())?;

        let Some(ReadableStreamController::ReadableStreamDefaultController(controller)) = stream.controller() else {
            panic!("a stream from an iterable has a ReadableStreamDefaultController");
        };

        // 3. If done is true:
        if done {
            // 1. Perform ! ReadableStreamDefaultControllerClose(stream.[[controller]]).
            readable_stream_default_controller_close(&stream.context(vm), controller);
        }
        // 4. Otherwise:
        else {
            // 1. Let value be ? IteratorValue(iterResult).
            let value = iterator_value(vm, iter_result.as_object())?;

            // 2. Perform ! ReadableStreamDefaultControllerEnqueue(stream.[[controller]], value).
            readable_stream_default_controller_enqueue(&stream.context(vm), controller, value).must();
        }
        Ok(Value::UNDEFINED)
    })
}

/// The cancelAlgorithm of ReadableStreamFromIterable, https://streams.spec.whatwg.org/#readable-stream-from-iterable
pub(crate) fn readable_stream_from_iterable_cancel_algorithm(
    cx: &CallContext,
    iterator_record: Gc<IteratorRecord>,
    reason: Value,
) -> WebIdlPromise {
    let vm = cx.vm();

    // 1. Let iterator be iteratorRecord.[[Iterator]].
    let iterator = iterator_record.iterator();

    // 2. Let returnMethod be GetMethod(iterator, "return").
    let return_method = Value::from_object(iterator).get_method(vm, &vm.names.return_);

    // 3. If returnMethod is an abrupt completion, return a promise rejected with returnMethod.[[Value]].
    let return_method = match return_method {
        Ok(return_method) => return_method,
        Err(throw) => return create_rejected_promise(vm, cx.realm(), throw.value()),
    };

    // 4. If returnMethod.[[Value]] is undefined, return a promise resolved with undefined.
    let Some(return_method) = return_method else {
        return create_resolved_promise(vm, cx.realm(), Value::UNDEFINED);
    };

    // 5. Let returnResult be Call(returnMethod.[[Value]], iterator, « reason »).
    let return_result = call(
        vm,
        Value::from_object(return_method),
        Value::from_object(iterator),
        &[reason],
    );

    // 6. If returnResult is an abrupt completion, return a promise rejected with returnResult.[[Value]].
    let return_result = match return_result {
        Ok(return_result) => return_result,
        Err(throw) => return create_rejected_promise(vm, cx.realm(), throw.value()),
    };

    // 7. Let returnPromise be a promise resolved with returnResult.[[Value]].
    let return_promise = create_resolved_promise(vm, cx.realm(), return_result);

    // 8. Return the result of reacting to returnPromise with the following fulfillment steps, given iterResult:
    upon_fulfillment(vm, return_promise, (), |_, (), iter_result| {
        // 1. If iterResult is not an Object, throw a TypeError.
        if !iter_result.is_object() {
            return Err(Exception::type_error("iterResult is not an Object"));
        }

        // 2. Return undefined.
        Ok(Value::UNDEFINED)
    })
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-into-request
pub(crate) fn readable_stream_add_read_into_request(stream: Ref<ReadableStream>, read_into_request: ReadIntoRequest) {
    // 1. Assert: stream.[[reader]] implements ReadableStreamBYOBReader.
    let Some(ReadableStreamReader::ReadableStreamBYOBReader(reader)) = stream.reader() else {
        panic!("the stream is locked to a ReadableStreamBYOBReader");
    };

    // 2. Assert: stream.[[state]] is "readable" or "closed".
    assert!(matches!(
        stream.state(),
        ReadableStreamState::Readable | ReadableStreamState::Closed
    ));

    // 3. Append readRequest to stream.[[reader]].[[readIntoRequests]].
    reader.append_read_into_request(read_into_request);
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-request
pub(crate) fn readable_stream_add_read_request(stream: Ref<ReadableStream>, read_request: ReadRequest) {
    // 1. Assert: stream.[[reader]] implements ReadableStreamDefaultReader.
    let Some(ReadableStreamReader::ReadableStreamDefaultReader(reader)) = stream.reader() else {
        panic!("the stream is locked to a ReadableStreamDefaultReader");
    };

    // 2. Assert: stream.[[state]] is "readable".
    assert!(stream.state() == ReadableStreamState::Readable);

    // 3. Append readRequest to stream.[[reader]].[[readRequests]].
    reader.append_read_request(read_request);
}

// https://streams.spec.whatwg.org/#readable-stream-cancel
pub(crate) fn readable_stream_cancel(cx: &CallContext, stream: Ref<ReadableStream>, reason: Value) -> WebIdlPromise {
    let vm = cx.vm();

    // 1. Set stream.[[disturbed]] to true.
    stream.set_disturbed(true);

    // 2. If stream.[[state]] is "closed", return a promise resolved with undefined.
    if stream.state() == ReadableStreamState::Closed {
        return create_resolved_promise(vm, cx.realm(), Value::UNDEFINED);
    }

    // 3. If stream.[[state]] is "errored", return a promise rejected with stream.[[storedError]].
    if stream.state() == ReadableStreamState::Errored {
        return create_rejected_promise(vm, cx.realm(), stream.stored_error());
    }

    // 4. Perform ! ReadableStreamClose(stream).
    readable_stream_close(vm, stream);

    // 5. Let reader be stream.[[reader]].
    // 6. If reader is not undefined and reader implements ReadableStreamBYOBReader,
    if let Some(ReadableStreamReader::ReadableStreamBYOBReader(reader)) = stream.reader() {
        // 1. Let readIntoRequests be reader.[[readIntoRequests]].
        // 2. Set reader.[[readIntoRequests]] to an empty list.
        let read_into_requests = reader.take_read_into_requests(vm);

        // 3. For each readIntoRequest of readIntoRequests,
        for index in 0..read_into_requests.len() {
            // 1. Perform readIntoRequest’s close steps, given undefined.
            read_into_requests
                .get(index)
                .expect("the index is in bounds")
                .close_steps(vm, Value::UNDEFINED);
        }
    }

    // 7. Let sourceCancelPromise be ! stream.[[controller]].[[CancelSteps]](reason).
    let source_cancel_promise = match stream.controller().expect("a stream that is set up has a controller") {
        ReadableStreamController::ReadableStreamDefaultController(controller) => {
            ReadableStreamDefaultController::cancel_steps(controller, cx, reason)
        }
        ReadableStreamController::ReadableByteStreamController(controller) => {
            ReadableByteStreamController::cancel_steps(controller, cx, reason)
        }
    };

    // 8. Return the result of reacting to sourceCancelPromise with a fulfillment step that returns undefined.
    upon_fulfillment(vm, source_cancel_promise, (), |_, (), _| Ok(Value::UNDEFINED))
}

// https://streams.spec.whatwg.org/#readable-stream-close
pub(crate) fn readable_stream_close(vm: &Vm, stream: Ref<ReadableStream>) {
    // 1. Assert: stream.[[state]] is "readable".
    assert!(stream.state() == ReadableStreamState::Readable);

    // 2. Set stream.[[state]] to "closed".
    stream.set_state(ReadableStreamState::Closed);
    complete_rendering_pending_stream(stream);

    // 3. Let reader be stream.[[reader]].
    // 4. If reader is undefined, return.
    let Some(reader) = stream.reader() else {
        return;
    };

    // 5. Resolve reader.[[closedPromise]] with undefined.
    let closed_promise = reader
        .generic()
        .closed_promise()
        .expect("a reader of a stream has a closed promise");
    resolve_promise(vm, closed_promise, Value::UNDEFINED);

    // 6. If reader implements ReadableStreamDefaultReader,
    if let ReadableStreamReader::ReadableStreamDefaultReader(reader) = reader {
        // 1. Let readRequests be reader.[[readRequests]].
        // 2. Set reader.[[readRequests]] to an empty list.
        let read_requests = reader.take_read_requests(vm);

        // 3. For each readRequest of readRequests,
        for index in 0..read_requests.len() {
            // 1. Perform readRequest’s close steps.
            read_requests
                .get(index)
                .expect("the index is in bounds")
                .close_steps(vm);
        }
    }
}

// https://streams.spec.whatwg.org/#readable-stream-error
pub(crate) fn readable_stream_error(vm: &Vm, stream: Ref<ReadableStream>, error: Value) {
    // 1. Assert: stream.[[state]] is "readable".
    assert!(stream.state() == ReadableStreamState::Readable);

    // 2. Set stream.[[state]] to "errored".
    stream.set_state(ReadableStreamState::Errored);
    complete_rendering_pending_stream(stream);

    // 3. Set stream.[[storedError]] to e.
    stream.set_stored_error(error);

    // 4. Let reader be stream.[[reader]].
    // 5. If reader is undefined, return.
    let Some(reader) = stream.reader() else {
        return;
    };

    // 6. Reject reader.[[closedPromise]] with e.
    let closed_promise = reader
        .generic()
        .closed_promise()
        .expect("a reader of a stream has a closed promise");
    reject_promise(vm, closed_promise, error);

    // 7. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
    mark_promise_as_handled(vm, closed_promise);

    match reader {
        // 8. If reader implements ReadableStreamDefaultReader,
        ReadableStreamReader::ReadableStreamDefaultReader(reader) => {
            // 1. Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e).
            crate::streams::readable_stream_default_reader::readable_stream_default_reader_error_read_requests(
                vm, reader, error,
            );
        }
        // 9. Otherwise,
        ReadableStreamReader::ReadableStreamBYOBReader(reader) => {
            // 1. Assert: reader implements ReadableStreamBYOBReader.
            // 2. Perform ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
            crate::streams::readable_stream_byob_reader::readable_stream_byob_reader_error_read_into_requests(
                vm, reader, error,
            );
        }
    }
}

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-into-request
pub(crate) fn readable_stream_fulfill_read_into_request(
    vm: &Vm,
    stream: Ref<ReadableStream>,
    chunk: Value,
    done: bool,
) {
    // 1. Assert: ! ReadableStreamHasBYOBReader(stream) is true.
    // 2. Let reader be stream.[[reader]].
    let Some(ReadableStreamReader::ReadableStreamBYOBReader(reader)) = stream.reader() else {
        panic!("the stream has a ReadableStreamBYOBReader");
    };

    // 3. Assert: reader.[[readIntoRequests]] is not empty.
    // 4. Let readIntoRequest be reader.[[readIntoRequests]][0].
    // 5. Remove readIntoRequest from reader.[[readIntoRequests]].
    let read_into_request = reader.take_first_read_into_request();

    // 6. If done is true, perform readIntoRequest’s close steps, given chunk.
    if done {
        read_into_request.close_steps(vm, chunk);
    }
    // 7. Otherwise, perform readIntoRequest’s chunk steps, given chunk.
    else {
        read_into_request.chunk_steps(vm, chunk);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-request
pub(crate) fn readable_stream_fulfill_read_request(vm: &Vm, stream: Ref<ReadableStream>, chunk: Value, done: bool) {
    // 1. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
    // 2. Let reader be stream.[[reader]].
    let Some(ReadableStreamReader::ReadableStreamDefaultReader(reader)) = stream.reader() else {
        panic!("the stream has a ReadableStreamDefaultReader");
    };

    // 3. Assert: reader.[[readRequests]] is not empty.
    // 4. Let readRequest be reader.[[readRequests]][0].
    // 5. Remove readRequest from reader.[[readRequests]].
    let read_request = reader.take_first_read_request();

    // 6. If done is true, perform readRequest’s close steps.
    if done {
        read_request.close_steps(vm);
    }
    // 7. Otherwise, perform readRequest’s chunk steps, given chunk.
    else {
        read_request.chunk_steps(vm, chunk);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-into-requests
pub(crate) fn readable_stream_get_num_read_into_requests(stream: Ref<ReadableStream>) -> usize {
    // 1. Assert: ! ReadableStreamHasBYOBReader(stream) is true.
    let Some(ReadableStreamReader::ReadableStreamBYOBReader(reader)) = stream.reader() else {
        panic!("the stream has a ReadableStreamBYOBReader");
    };

    // 2. Return stream.[[reader]].[[readIntoRequests]]'s size.
    reader.read_into_requests_len()
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-requests
pub(crate) fn readable_stream_get_num_read_requests(stream: Ref<ReadableStream>) -> usize {
    // 1. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
    let Some(ReadableStreamReader::ReadableStreamDefaultReader(reader)) = stream.reader() else {
        panic!("the stream has a ReadableStreamDefaultReader");
    };

    // 2. Return stream.[[reader]].[[readRequests]]'s size.
    reader.read_requests_len()
}

// https://streams.spec.whatwg.org/#readable-stream-has-byob-reader
pub(crate) fn readable_stream_has_byob_reader(stream: Ref<ReadableStream>) -> bool {
    // 1. Let reader be stream.[[reader]].
    // 2. If reader is undefined, return false.
    // 3. If reader implements ReadableStreamBYOBReader, return true.
    // 4. Return false.
    matches!(stream.reader(), Some(ReadableStreamReader::ReadableStreamBYOBReader(_)))
}

// https://streams.spec.whatwg.org/#readable-stream-has-default-reader
pub(crate) fn readable_stream_has_default_reader(stream: Ref<ReadableStream>) -> bool {
    // 1. Let reader be stream.[[reader]].
    // 2. If reader is undefined, return false.
    // 3. If reader implements ReadableStreamDefaultReader, return true.
    // 4. Return false.
    matches!(
        stream.reader(),
        Some(ReadableStreamReader::ReadableStreamDefaultReader(_))
    )
}

/// The composite reason of the cancel algorithms of a tee, ! CreateArrayFromList(« reason1, reason2 »).
pub(crate) fn create_array_from_list(cx: &CallContext, values: &[Value]) -> Value {
    Value::from_object(Array::create_from(cx.vm(), cx.realm(), values).upcast::<Object>())
}
