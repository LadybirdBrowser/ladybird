/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TransformStream, https://streams.spec.whatwg.org/#ts-class, and the abstract operations that work with transform
//! streams, https://streams.spec.whatwg.org/#ts-abstract-ops

use core::cell::Cell;

use libjs_rust::api::{Gc, Object, Realm, Throw, Trace, Value, Vm};

use crate::bindings::conversions::FromJs;
use crate::bindings::interface::{Ref, Wrappable, allocate, implementation_of, wrap};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_resolved_promise, promise_realm, react_to_promise, reject_promise,
    resolve_promise, upon_fulfillment,
};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter, Transferable};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::streams::algorithms::{
    AbortAlgorithm, CancelAlgorithm, CloseAlgorithm, FlushAlgorithm, PullAlgorithm, SizeAlgorithm, StartAlgorithm,
    TransformAlgorithm, WriteAlgorithm,
};
use crate::streams::cross_realm_transform::{deserialize_with_transfer, serialize_with_transfer};
use crate::streams::queuing_strategy::{QueuingStrategy, extract_high_water_mark, extract_size_algorithm};
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamState, create_readable_stream, is_readable_stream_locked,
};
use crate::streams::readable_stream_default_controller::{
    ReadableStreamDefaultController, readable_stream_default_controller_close, readable_stream_default_controller_error,
};
use crate::streams::transform_stream_default_controller::{
    TransformStreamDefaultController, set_up_transform_stream_default_controller,
    set_up_transform_stream_default_controller_from_transformer, transform_stream_default_controller_clear_algorithms,
    transform_stream_default_controller_enqueue, transform_stream_default_controller_perform_transform,
};
use crate::streams::transformer::Transformer;
use crate::streams::underlying_source::ReadableStreamController;
use crate::streams::writable_stream::{
    WritableStream, WritableStreamState, create_writable_stream, is_writable_stream_locked,
};
use crate::streams::writable_stream_default_controller::writable_stream_default_controller_error_if_needed;

// https://streams.spec.whatwg.org/#transformstream
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TransformStream {
    /// The realm of the stream, which its promises and the results of its operations are created in.
    realm: Cell<Gc<Realm>>,
    // https://streams.spec.whatwg.org/#transformstream-backpressure
    #[gc(untraced)]
    backpressure: Cell<Option<bool>>,
    // https://streams.spec.whatwg.org/#transformstream-backpressurechangepromise
    backpressure_change_promise: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#transformstream-controller
    controller: Cell<Option<Ref<TransformStreamDefaultController>>>,
    // https://streams.spec.whatwg.org/#transformstream-detached
    detached: Cell<bool>,
    // https://streams.spec.whatwg.org/#transformstream-readable
    readable: Cell<Option<Ref<ReadableStream>>>,
    // https://streams.spec.whatwg.org/#transformstream-writable
    writable: Cell<Option<Ref<WritableStream>>>,
}

impl Wrappable for TransformStream {}

impl TransformStream {
    pub(crate) fn new(realm: Gc<Realm>) -> Self {
        Self {
            realm: Cell::new(realm),
            backpressure: Cell::new(Some(false)),
            backpressure_change_promise: Cell::new(None),
            controller: Cell::new(None),
            detached: Cell::new(false),
            readable: Cell::new(None),
            writable: Cell::new(None),
        }
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm.get()
    }

    /// The context of the realm of the stream.
    pub(crate) fn context<'vm>(&self, vm: &'vm Vm) -> CallContext<'vm> {
        CallContext::new(vm, self.realm())
    }

    /// The stream's [[readable]], which it has once it is initialized.
    pub(crate) fn readable_stream(&self) -> Ref<ReadableStream> {
        self.readable
            .get()
            .expect("a transform stream that is initialized has a readable stream")
    }

    /// The stream's [[writable]], which it has once it is initialized.
    pub(crate) fn writable_stream(&self) -> Ref<WritableStream> {
        self.writable
            .get()
            .expect("a transform stream that is initialized has a writable stream")
    }

    pub(crate) fn set_readable(&self, readable: Option<Ref<ReadableStream>>) {
        self.readable.set(readable);
    }

    pub(crate) fn set_writable(&self, writable: Option<Ref<WritableStream>>) {
        self.writable.set(writable);
    }

    pub(crate) fn controller(&self) -> Option<Ref<TransformStreamDefaultController>> {
        self.controller.get()
    }

    pub(crate) fn set_controller(&self, controller: Option<Ref<TransformStreamDefaultController>>) {
        self.controller.set(controller);
    }

    pub(crate) fn backpressure(&self) -> Option<bool> {
        self.backpressure.get()
    }

    pub(crate) fn set_backpressure(&self, backpressure: Option<bool>) {
        self.backpressure.set(backpressure);
    }

    pub(crate) fn backpressure_change_promise(&self) -> Option<WebIdlPromise> {
        self.backpressure_change_promise.get()
    }

    pub(crate) fn set_backpressure_change_promise(&self, promise: Option<WebIdlPromise>) {
        self.backpressure_change_promise.set(promise);
    }

    /// The realm of the stream's [[backpressureChangePromise]].
    pub(crate) fn backpressure_change_promise_realm(&self) -> Gc<Realm> {
        promise_realm(
            self.backpressure_change_promise()
                .expect("the stream has a backpressure change promise"),
        )
    }

    // https://streams.spec.whatwg.org/#ts-constructor
    pub(crate) fn construct(
        cx: &mut CallContext,
        transformer: Option<Gc<Object>>,
        writable_strategy: QueuingStrategy,
        readable_strategy: QueuingStrategy,
    ) -> WebIdlResult<Ref<Self>> {
        let this = allocate(cx.vm(), |_| Self::new(cx.realm()));

        // 1. If transformer is missing, set it to null.
        let transformer = transformer.map_or(Value::NULL, Value::from_object);

        // 2. Let transformerDict be transformer, converted to an IDL value of type Transformer.
        let transformer_dict = Transformer::from_js(cx.vm(), transformer)?;

        // 3. If transformerDict["readableType"] exists, throw a RangeError exception.
        if transformer_dict.readable_type.is_some() {
            return Err(Exception::range_error("Invalid use of reserved key 'readableType'"));
        }

        // 4. If transformerDict["writableType"] exists, throw a RangeError exception.
        if transformer_dict.writable_type.is_some() {
            return Err(Exception::range_error("Invalid use of reserved key 'writableType'"));
        }

        // 5. Let readableHighWaterMark be ? ExtractHighWaterMark(readableStrategy, 0).
        let readable_high_water_mark = extract_high_water_mark(&readable_strategy, 0.0)?;

        // 6. Let readableSizeAlgorithm be ! ExtractSizeAlgorithm(readableStrategy).
        let readable_size_algorithm = extract_size_algorithm(&readable_strategy);

        // 7. Let writableHighWaterMark be ? ExtractHighWaterMark(writableStrategy, 1).
        let writable_high_water_mark = extract_high_water_mark(&writable_strategy, 1.0)?;

        // 8. Let writableSizeAlgorithm be ! ExtractSizeAlgorithm(writableStrategy).
        let writable_size_algorithm = extract_size_algorithm(&writable_strategy);

        // 9. Let startPromise be a new promise.
        let start_promise = create_promise(cx.vm(), cx.realm());

        // 10. Perform ! InitializeTransformStream(this, startPromise, writableHighWaterMark, writableSizeAlgorithm,
        //     readableHighWaterMark, readableSizeAlgorithm).
        initialize_transform_stream(
            cx,
            this,
            start_promise,
            writable_high_water_mark,
            writable_size_algorithm,
            readable_high_water_mark,
            readable_size_algorithm,
        );

        // 11. Perform ? SetUpTransformStreamDefaultControllerFromTransformer(this, transformer, transformerDict).
        let start = transformer_dict.start.clone();
        set_up_transform_stream_default_controller_from_transformer(cx, this, transformer, transformer_dict);

        // 12. If transformerDict["start"] exists, then resolve startPromise with the result of invoking
        //     transformerDict["start"] with argument list « this.[[controller]] » and callback this value transformer.
        if let Some(start) = start {
            let controller = this
                .controller()
                .expect("a transform stream that is set up has a controller");
            let result = start.invoke(Some(transformer), &[Value::from_object(wrap(cx, controller))])?;
            resolve_promise(cx.vm(), start_promise, result);
        }
        // 13. Otherwise, resolve startPromise with undefined.
        else {
            resolve_promise(cx.vm(), start_promise, Value::UNDEFINED);
        }

        Ok(this)
    }

    // https://streams.spec.whatwg.org/#ts-readable
    pub(crate) fn readable(&self, _: &mut CallContext) -> WebIdlResult<Ref<ReadableStream>> {
        // 1. Return this.[[readable]].
        Ok(self.readable_stream())
    }

    // https://streams.spec.whatwg.org/#ts-writable
    pub(crate) fn writable(&self, _: &mut CallContext) -> WebIdlResult<Ref<WritableStream>> {
        // 1. Return this.[[writable]].
        Ok(self.writable_stream())
    }

    // https://streams.spec.whatwg.org/#transformstream-set-up
    pub(crate) fn set_up(
        this: Ref<Self>,
        cx: &CallContext,
        transform_algorithm: TransformAlgorithm,
        flush_algorithm: FlushAlgorithm,
        cancel_algorithm: CancelAlgorithm,
    ) {
        // 1. Let writableHighWaterMark be 1.
        let writable_high_water_mark = 1.0;

        // 2. Let writableSizeAlgorithm be an algorithm that returns 1.
        let writable_size_algorithm = SizeAlgorithm::ReturnsOne;

        // 3. Let readableHighWaterMark be 0.
        let readable_high_water_mark = 0.0;

        // 4. Let readableSizeAlgorithm be an algorithm that returns 1.
        let readable_size_algorithm = SizeAlgorithm::ReturnsOne;

        // 5. Let transformAlgorithmWrapper be an algorithm that runs these steps given a value chunk:
        let transform_algorithm_wrapper = transform_algorithm;

        // 6. Let flushAlgorithmWrapper be an algorithm that runs these steps:
        let flush_algorithm_wrapper = flush_algorithm;

        // 7. Let cancelAlgorithmWrapper be an algorithm that runs these steps given a value reason:
        let cancel_algorithm_wrapper = cancel_algorithm;

        // 8. Let startPromise be a promise resolved with undefined.
        let start_promise = create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED);

        // 9. Perform ! InitializeTransformStream(stream, startPromise, writableHighWaterMark, writableSizeAlgorithm,
        //    readableHighWaterMark, readableSizeAlgorithm).
        initialize_transform_stream(
            cx,
            this,
            start_promise,
            writable_high_water_mark,
            writable_size_algorithm,
            readable_high_water_mark,
            readable_size_algorithm,
        );

        // 10. Let controller be a new TransformStreamDefaultController.
        let controller = allocate(cx.vm(), |_| TransformStreamDefaultController::new());

        // 11. Perform ! SetUpTransformStreamDefaultController(stream, controller, transformAlgorithmWrapper,
        //     flushAlgorithmWrapper, cancelAlgorithmWrapper).
        set_up_transform_stream_default_controller(
            this,
            controller,
            transform_algorithm_wrapper,
            flush_algorithm_wrapper,
            cancel_algorithm_wrapper,
        );
    }

    // https://streams.spec.whatwg.org/#transformstream-enqueue
    pub(crate) fn enqueue(this: Ref<Self>, vm: &Vm, chunk: Value) {
        // To enqueue the JavaScript value chunk into a TransformStream stream, perform
        // ! TransformStreamDefaultControllerEnqueue(stream.[[controller]], chunk).
        let controller = this
            .controller()
            .expect("a transform stream that is set up has a controller");
        transform_stream_default_controller_enqueue(
            &CallContext::new(vm, this.backpressure_change_promise_realm()),
            controller,
            chunk,
        )
        .must();
    }
}

impl Transferable for TransformStream {
    fn detached(&self) -> &Cell<bool> {
        &self.detached
    }

    // https://streams.spec.whatwg.org/#ref-for-transfer-steps②
    fn transfer_steps(
        this: &Ref<Self>,
        cx: &mut CallContext,
        data_holder: &mut TransferDataWriter,
    ) -> WebIdlResult<()> {
        // 1. Let readable be value.[[readable]].
        let readable = this.readable_stream();

        // 2. Let writable be value.[[writable]].
        let writable = this.writable_stream();

        // 3. If ! IsReadableStreamLocked(readable) is true, throw a "DataCloneError" DOMException.
        if is_readable_stream_locked(readable) {
            return Err(Exception::Dom(
                "DataCloneError",
                ak::Utf16String::from_ascii(b"Cannot transfer locked ReadableStream"),
            ));
        }

        // 4. If ! IsWritableStreamLocked(writable) is true, throw a "DataCloneError" DOMException.
        if is_writable_stream_locked(writable) {
            return Err(Exception::Dom(
                "DataCloneError",
                ak::Utf16String::from_ascii(b"Cannot transfer locked WritableStream"),
            ));
        }

        // 5. Set dataHolder.[[readable]] to ! StructuredSerializeWithTransfer(readable, « readable »).
        serialize_with_transfer(data_holder, Value::from_object(wrap(cx, readable)));

        // 6. Set dataHolder.[[writable]] to ! StructuredSerializeWithTransfer(writable, « writable »).
        serialize_with_transfer(data_holder, Value::from_object(wrap(cx, writable)));

        Ok(())
    }

    // https://streams.spec.whatwg.org/#ref-for-transfer-receiving-steps②
    fn transfer_receiving_steps(cx: &mut CallContext, data_holder: &mut TransferDataReader) -> WebIdlResult<Ref<Self>> {
        let value = allocate(cx.vm(), |_| Self::new(cx.realm()));

        // 1. Let readableRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[readable]], the current Realm).
        let readable_record = deserialize_with_transfer(data_holder);

        // 2. Let writableRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[writable]], the current Realm).
        let writable_record = deserialize_with_transfer(data_holder);

        // 3. Set value.[[readable]] to readableRecord.[[Deserialized]].
        assert!(readable_record.is_object());
        value.set_readable(Some(implementation_of::<ReadableStream>(readable_record.as_object())));

        // 4. Set value.[[writable]] to writableRecord.[[Deserialized]].
        assert!(writable_record.is_object());
        value.set_writable(Some(implementation_of::<WritableStream>(writable_record.as_object())));

        // 5. Set value.[[backpressure]], value.[[backpressureChangePromise]], and value.[[controller]] to undefined.
        value.set_backpressure(None);
        value.set_backpressure_change_promise(None);
        value.set_controller(None);

        Ok(value)
    }
}

/// The ReadableStreamDefaultController of the [[readable]] of a TransformStream.
pub(crate) fn readable_default_controller(readable: Ref<ReadableStream>) -> Ref<ReadableStreamDefaultController> {
    let Some(ReadableStreamController::ReadableStreamDefaultController(controller)) = readable.controller() else {
        panic!("the readable stream of a transform stream has a ReadableStreamDefaultController");
    };
    controller
}

// https://streams.spec.whatwg.org/#initialize-transform-stream
pub(crate) fn initialize_transform_stream(
    cx: &CallContext,
    stream: Ref<TransformStream>,
    start_promise: WebIdlPromise,
    writable_high_water_mark: f64,
    writable_size_algorithm: SizeAlgorithm,
    readable_high_water_mark: f64,
    readable_size_algorithm: SizeAlgorithm,
) {
    // 1. Let startAlgorithm be an algorithm that returns startPromise.
    let start_algorithm = || StartAlgorithm::ReturnsPromise(start_promise);

    // 2. Let writeAlgorithm be the following steps, taking a chunk argument:
    //    1. Return ! TransformStreamDefaultSinkWriteAlgorithm(stream, chunk).
    let write_algorithm = WriteAlgorithm::TransformStream(stream);

    // 3. Let abortAlgorithm be the following steps, taking a reason argument:
    //    1. Return ! TransformStreamDefaultSinkAbortAlgorithm(stream, reason).
    let abort_algorithm = AbortAlgorithm::TransformStream(stream);

    // 4. Let closeAlgorithm be the following steps:
    //    1. Return ! TransformStreamDefaultSinkCloseAlgorithm(stream).
    let close_algorithm = CloseAlgorithm::TransformStream(stream);

    // 5. Set stream.[[writable]] to ! CreateWritableStream(startAlgorithm, writeAlgorithm, closeAlgorithm,
    //    abortAlgorithm, writableHighWaterMark, writableSizeAlgorithm).
    let writable = create_writable_stream(
        cx,
        start_algorithm(),
        write_algorithm,
        close_algorithm,
        abort_algorithm,
        writable_high_water_mark,
        writable_size_algorithm,
    )
    .must();
    stream.set_writable(Some(writable));

    // 6. Let pullAlgorithm be the following steps:
    //    1. Return ! TransformStreamDefaultSourcePullAlgorithm(stream).
    let pull_algorithm = PullAlgorithm::TransformStream(stream);

    // 7. Let cancelAlgorithm be the following steps, taking a reason argument:
    //    1. Return ! TransformStreamDefaultSourceCancelAlgorithm(stream, reason).
    let cancel_algorithm = CancelAlgorithm::TransformStream(stream);

    // 8. Set stream.[[readable]] to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm,
    //    readableHighWaterMark, readableSizeAlgorithm).
    let readable = create_readable_stream(
        cx,
        start_algorithm(),
        pull_algorithm,
        cancel_algorithm,
        Some(readable_high_water_mark),
        Some(readable_size_algorithm),
    )
    .must();
    stream.set_readable(Some(readable));

    // 9. Set stream.[[backpressure]] and stream.[[backpressureChangePromise]] to undefined.
    stream.set_backpressure(None);
    stream.set_backpressure_change_promise(None);

    // 10. Perform ! TransformStreamSetBackpressure(stream, true).
    transform_stream_set_backpressure(cx, stream, true);

    // 11. Set stream.[[controller]] to undefined.
    stream.set_controller(None);
}

// https://streams.spec.whatwg.org/#transform-stream-error
pub(crate) fn transform_stream_error(cx: &CallContext, stream: Ref<TransformStream>, error: Value) {
    let readable_controller = readable_default_controller(stream.readable_stream());

    // 1. Perform ! ReadableStreamDefaultControllerError(stream.[[readable]].[[controller]], e).
    readable_stream_default_controller_error(cx, readable_controller, error);

    // 2. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, e).
    transform_stream_error_writable_and_unblock_write(cx, stream, error);
}

// https://streams.spec.whatwg.org/#transform-stream-error-writable-and-unblock-write
pub(crate) fn transform_stream_error_writable_and_unblock_write(
    cx: &CallContext,
    stream: Ref<TransformStream>,
    error: Value,
) {
    // 1. Perform ! TransformStreamDefaultControllerClearAlgorithms(stream.[[controller]]).
    transform_stream_default_controller_clear_algorithms(
        stream
            .controller()
            .expect("a transform stream that is set up has a controller"),
    );

    // 2. Perform ! WritableStreamDefaultControllerErrorIfNeeded(stream.[[writable]].[[controller]], e).
    let writable_controller = stream
        .writable_stream()
        .controller()
        .expect("a writable stream has a controller");
    writable_stream_default_controller_error_if_needed(cx, writable_controller, error);

    // 3. Perform ! TransformStreamUnblockWrite(stream).
    transform_stream_unblock_write(cx, stream);
}

// https://streams.spec.whatwg.org/#transform-stream-set-backpressure
pub(crate) fn transform_stream_set_backpressure(cx: &CallContext, stream: Ref<TransformStream>, backpressure: bool) {
    // 1. Assert: stream.[[backpressure]] is not backpressure.
    assert!(stream.backpressure() != Some(backpressure));

    // 2. If stream.[[backpressureChangePromise]] is not undefined, resolve stream.[[backpressureChangePromise]] with
    //    undefined.
    if let Some(backpressure_change_promise) = stream.backpressure_change_promise() {
        resolve_promise(cx.vm(), backpressure_change_promise, Value::UNDEFINED);
    }

    // 3. Set stream.[[backpressureChangePromise]] to a new promise.
    stream.set_backpressure_change_promise(Some(create_promise(cx.vm(), cx.realm())));

    // 4. Set stream.[[backpressure]] to backpressure.
    stream.set_backpressure(Some(backpressure));
}

// https://streams.spec.whatwg.org/#transform-stream-unblock-write
pub(crate) fn transform_stream_unblock_write(cx: &CallContext, stream: Ref<TransformStream>) {
    // 1. If stream.[[backpressure]] is true, perform ! TransformStreamSetBackpressure(stream, false).
    if stream.backpressure() == Some(true) {
        transform_stream_set_backpressure(
            &CallContext::new(cx.vm(), stream.backpressure_change_promise_realm()),
            stream,
            false,
        );
    }
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-write-algorithm
pub(crate) fn transform_stream_default_sink_write_algorithm(
    cx: &CallContext,
    stream: Ref<TransformStream>,
    chunk: Value,
) -> WebIdlPromise {
    // 1. Assert: stream.[[writable]].[[state]] is "writable".
    assert!(stream.writable_stream().state() == WritableStreamState::Writable);

    // 2. Let controller be stream.[[controller]].
    let controller = stream
        .controller()
        .expect("a transform stream that is set up has a controller");

    // 3. If stream.[[backpressure]] is true,
    if stream.backpressure() == Some(true) {
        // 1. Let backpressureChangePromise be stream.[[backpressureChangePromise]].
        let backpressure_change_promise = stream.backpressure_change_promise();

        // 2. Assert: backpressureChangePromise is not undefined.
        let backpressure_change_promise =
            backpressure_change_promise.expect("a stream with backpressure has a backpressure change promise");

        // 3. Return the result of reacting to backpressureChangePromise with the following fulfillment steps:
        return upon_fulfillment(
            cx.vm(),
            backpressure_change_promise,
            (stream, controller, chunk),
            |cx, (stream, controller, chunk), _| {
                // 1. Let writable be stream.[[writable]].
                let writable = stream.writable_stream();

                // 2. Let state be writable.[[state]].
                let state = writable.state();

                // 3. If state is "erroring", throw writable.[[storedError]].
                if state == WritableStreamState::Erroring {
                    return Err(Exception::Thrown(Throw::new(writable.stored_error())));
                }

                // 4. Assert: state is "writable".
                assert!(state == WritableStreamState::Writable);

                // 5. Return ! TransformStreamDefaultControllerPerformTransform(controller, chunk).
                let promise = transform_stream_default_controller_perform_transform(cx, controller, chunk);
                Ok(Value::from_object(promise.promise()))
            },
        );
    }

    // 4. Return ! TransformStreamDefaultControllerPerformTransform(controller, chunk).
    transform_stream_default_controller_perform_transform(cx, controller, chunk)
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-abort-algorithm
pub(crate) fn transform_stream_default_sink_abort_algorithm(
    cx: &CallContext,
    stream: Ref<TransformStream>,
    reason: Value,
) -> WebIdlPromise {
    let vm = cx.vm();
    let realm = stream.backpressure_change_promise_realm();

    // 1. Let controller be stream.[[controller]].
    let controller = stream
        .controller()
        .expect("a transform stream that is set up has a controller");

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if let Some(finish_promise) = controller.finish_promise() {
        return finish_promise;
    }

    // 3. Let readable be stream.[[readable]].
    let readable = stream.readable_stream();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller.set_finish_promise(Some(create_promise(vm, realm)));

    // 5. Let cancelPromise be the result of performing controller.[[cancelAlgorithm]], passing reason.
    let cancel_algorithm = controller
        .cancel_algorithm()
        .expect("the controller's algorithms are not cleared");
    let cancel_promise = cancel_algorithm.run(&stream.context(vm), reason);

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(controller);

    // 7. React to cancelPromise:
    react_to_promise(
        vm,
        cancel_promise,
        (readable, controller, reason),
        // 1. If cancelPromise was fulfilled, then:
        Some(|cx, (readable, controller, reason), _| {
            let vm = cx.vm();
            let finish_promise = controller
                .finish_promise()
                .expect("the controller has a finish promise");

            // 1. If readable.[[state]] is "errored", reject controller.[[finishPromise]] with readable.[[storedError]].
            if readable.state() == ReadableStreamState::Errored {
                reject_promise(vm, finish_promise, readable.stored_error());
            }
            // 2. Otherwise:
            else {
                // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], reason).
                readable_stream_default_controller_error(
                    &readable.context(vm),
                    readable_default_controller(readable),
                    reason,
                );

                // 2. Resolve controller.[[finishPromise]] with undefined.
                resolve_promise(vm, finish_promise, Value::UNDEFINED);
            }

            Ok(Value::UNDEFINED)
        }),
        // 2. If cancelPromise was rejected with reason r, then:
        Some(|cx, (readable, controller, _), reason| {
            let vm = cx.vm();

            // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], r).
            readable_stream_default_controller_error(
                &readable.context(vm),
                readable_default_controller(readable),
                reason,
            );

            // 2. Reject controller.[[finishPromise]] with r.
            let finish_promise = controller
                .finish_promise()
                .expect("the controller has a finish promise");
            reject_promise(vm, finish_promise, reason);

            Ok(Value::UNDEFINED)
        }),
    );

    // 8. Return controller.[[finishPromise]].
    controller
        .finish_promise()
        .expect("the controller has a finish promise")
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-close-algorithm
pub(crate) fn transform_stream_default_sink_close_algorithm(
    cx: &CallContext,
    stream: Ref<TransformStream>,
) -> WebIdlPromise {
    let vm = cx.vm();
    let realm = stream.backpressure_change_promise_realm();

    // 1. Let controller be stream.[[controller]].
    let controller = stream
        .controller()
        .expect("a transform stream that is set up has a controller");

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if let Some(finish_promise) = controller.finish_promise() {
        return finish_promise;
    }

    // 3. Let readable be stream.[[readable]].
    let readable = stream.readable_stream();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller.set_finish_promise(Some(create_promise(vm, realm)));

    // 5. Let flushPromise be the result of performing controller.[[flushAlgorithm]].
    let flush_algorithm = controller
        .flush_algorithm()
        .expect("the controller's algorithms are not cleared");
    let flush_promise = flush_algorithm.run(&stream.context(vm));

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(controller);

    // 7. React to flushPromise:
    react_to_promise(
        vm,
        flush_promise,
        (controller, readable),
        // 1. If flushPromise was fulfilled, then:
        Some(|cx, (controller, readable), _| {
            let vm = cx.vm();
            let finish_promise = controller
                .finish_promise()
                .expect("the controller has a finish promise");

            // 1. If readable.[[state]] is "errored", reject controller.[[finishPromise]] with readable.[[storedError]].
            if readable.state() == ReadableStreamState::Errored {
                reject_promise(vm, finish_promise, readable.stored_error());
            }
            // 2. Otherwise:
            else {
                // 1. Perform ! ReadableStreamDefaultControllerClose(readable.[[controller]]).
                readable_stream_default_controller_close(&readable.context(vm), readable_default_controller(readable));

                // 2. Resolve controller.[[finishPromise]] with undefined.
                resolve_promise(vm, finish_promise, Value::UNDEFINED);
            }

            Ok(Value::UNDEFINED)
        }),
        // 2. If flushPromise was rejected with reason r, then:
        Some(|cx, (controller, readable), reason| {
            let vm = cx.vm();

            // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], r).
            readable_stream_default_controller_error(
                &readable.context(vm),
                readable_default_controller(readable),
                reason,
            );

            // 2. Reject controller.[[finishPromise]] with r.
            let finish_promise = controller
                .finish_promise()
                .expect("the controller has a finish promise");
            reject_promise(vm, finish_promise, reason);

            Ok(Value::UNDEFINED)
        }),
    );

    // 8. Return controller.[[finishPromise]].
    controller
        .finish_promise()
        .expect("the controller has a finish promise")
}

// https://streams.spec.whatwg.org/#transform-stream-default-source-cancel
pub(crate) fn transform_stream_default_source_cancel_algorithm(
    cx: &CallContext,
    stream: Ref<TransformStream>,
    reason: Value,
) -> WebIdlPromise {
    let vm = cx.vm();
    let realm = stream.backpressure_change_promise_realm();

    // 1. Let controller be stream.[[controller]].
    let controller = stream
        .controller()
        .expect("a transform stream that is set up has a controller");

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if let Some(finish_promise) = controller.finish_promise() {
        return finish_promise;
    }

    // 3. Let writable be stream.[[writable]].
    let writable = stream.writable_stream();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller.set_finish_promise(Some(create_promise(vm, realm)));

    // 5. Let cancelPromise be the result of performing controller.[[cancelAlgorithm]], passing reason.
    let cancel_algorithm = controller
        .cancel_algorithm()
        .expect("the controller's algorithms are not cleared");
    let cancel_promise = cancel_algorithm.run(&stream.context(vm), reason);

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(controller);

    // 7. React to cancelPromise:
    react_to_promise(
        vm,
        cancel_promise,
        (stream, writable, controller, reason),
        // 1. If cancelPromise was fulfilled, then:
        Some(|cx, (stream, writable, controller, reason), _| {
            let vm = cx.vm();
            let finish_promise = controller
                .finish_promise()
                .expect("the controller has a finish promise");

            // 1. If writable.[[state]] is "errored", reject controller.[[finishPromise]] with writable.[[storedError]].
            if writable.state() == WritableStreamState::Errored {
                reject_promise(vm, finish_promise, writable.stored_error());
            }
            // 2. Otherwise:
            else {
                // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(writable.[[controller]], reason).
                let writable_controller = writable.controller().expect("a writable stream has a controller");
                writable_stream_default_controller_error_if_needed(&writable.context(vm), writable_controller, reason);

                // 2. Perform ! TransformStreamUnblockWrite(stream).
                transform_stream_unblock_write(cx, stream);

                // 3. Resolve controller.[[finishPromise]] with undefined.
                resolve_promise(vm, finish_promise, Value::UNDEFINED);
            }

            Ok(Value::UNDEFINED)
        }),
        // 2. If cancelPromise was rejected with reason r, then:
        Some(|cx, (stream, writable, controller, _), reason| {
            let vm = cx.vm();

            // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(writable.[[controller]], r).
            let writable_controller = writable.controller().expect("a writable stream has a controller");
            writable_stream_default_controller_error_if_needed(&writable.context(vm), writable_controller, reason);

            // 2. Perform ! TransformStreamUnblockWrite(stream).
            transform_stream_unblock_write(cx, stream);

            // 3. Reject controller.[[finishPromise]] with r.
            let finish_promise = controller
                .finish_promise()
                .expect("the controller has a finish promise");
            reject_promise(vm, finish_promise, reason);

            Ok(Value::UNDEFINED)
        }),
    );

    // 8. Return controller.[[finishPromise]].
    controller
        .finish_promise()
        .expect("the controller has a finish promise")
}

// https://streams.spec.whatwg.org/#transform-stream-default-source-pull
pub(crate) fn transform_stream_default_source_pull_algorithm(
    cx: &CallContext,
    stream: Ref<TransformStream>,
) -> WebIdlPromise {
    // 1. Assert: stream.[[backpressure]] is true.
    assert!(stream.backpressure() == Some(true));

    // 2. Assert: stream.[[backpressureChangePromise]] is not undefined.
    assert!(stream.backpressure_change_promise().is_some());

    // 3. Perform ! TransformStreamSetBackpressure(stream, false).
    transform_stream_set_backpressure(
        &CallContext::new(cx.vm(), stream.backpressure_change_promise_realm()),
        stream,
        false,
    );

    // 4. Return stream.[[backpressureChangePromise]].
    stream
        .backpressure_change_promise()
        .expect("the stream has a backpressure change promise")
}
