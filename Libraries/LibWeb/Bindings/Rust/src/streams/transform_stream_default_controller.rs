/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TransformStreamDefaultController, https://streams.spec.whatwg.org/#ts-default-controller-class, and its abstract
//! operations, https://streams.spec.whatwg.org/#ts-default-controller-abstract-ops

use core::cell::Cell;

use libjs_rust::api::{GcRefCell, Throw, Trace, Value};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, exception_value, upon_rejection};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::algorithms::{CancelAlgorithm, FlushAlgorithm, TransformAlgorithm};
use crate::streams::readable_stream_default_controller::{
    readable_stream_default_controller_can_close_or_enqueue, readable_stream_default_controller_close,
    readable_stream_default_controller_enqueue, readable_stream_default_controller_get_desired_size,
    readable_stream_default_controller_has_backpressure,
};
use crate::streams::transform_stream::{
    TransformStream, readable_default_controller, transform_stream_error,
    transform_stream_error_writable_and_unblock_write, transform_stream_set_backpressure,
};
use crate::streams::transformer::Transformer;

// https://streams.spec.whatwg.org/#transformstreamdefaultcontroller
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TransformStreamDefaultController {
    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-cancelalgorithm
    cancel_algorithm: GcRefCell<Option<CancelAlgorithm>>,
    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-finishpromise
    finish_promise: Cell<Option<WebIdlPromise>>,
    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-flushalgorithm
    flush_algorithm: GcRefCell<Option<FlushAlgorithm>>,
    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-transformalgorithm
    transform_algorithm: GcRefCell<Option<TransformAlgorithm>>,
    // https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-stream
    stream: Cell<Option<Ref<TransformStream>>>,
}

impl Wrappable for TransformStreamDefaultController {}

impl TransformStreamDefaultController {
    pub(crate) fn new() -> Self {
        Self {
            cancel_algorithm: GcRefCell::new(None),
            finish_promise: Cell::new(None),
            flush_algorithm: GcRefCell::new(None),
            transform_algorithm: GcRefCell::new(None),
            stream: Cell::new(None),
        }
    }

    /// The controller's [[stream]], which it has once it is set up.
    pub(crate) fn stream(&self) -> Ref<TransformStream> {
        self.stream.get().expect("a controller that is set up has a stream")
    }

    pub(crate) fn set_stream(&self, stream: Ref<TransformStream>) {
        self.stream.set(Some(stream));
    }

    pub(crate) fn finish_promise(&self) -> Option<WebIdlPromise> {
        self.finish_promise.get()
    }

    pub(crate) fn set_finish_promise(&self, promise: Option<WebIdlPromise>) {
        self.finish_promise.set(promise);
    }

    pub(crate) fn cancel_algorithm(&self) -> Option<CancelAlgorithm> {
        self.cancel_algorithm.borrow().clone()
    }

    pub(crate) fn set_cancel_algorithm(&self, algorithm: Option<CancelAlgorithm>) {
        self.cancel_algorithm.replace(algorithm);
    }

    pub(crate) fn flush_algorithm(&self) -> Option<FlushAlgorithm> {
        self.flush_algorithm.borrow().clone()
    }

    pub(crate) fn set_flush_algorithm(&self, algorithm: Option<FlushAlgorithm>) {
        self.flush_algorithm.replace(algorithm);
    }

    pub(crate) fn transform_algorithm(&self) -> Option<TransformAlgorithm> {
        self.transform_algorithm.borrow().clone()
    }

    pub(crate) fn set_transform_algorithm(&self, algorithm: Option<TransformAlgorithm>) {
        self.transform_algorithm.replace(algorithm);
    }

    // https://streams.spec.whatwg.org/#ts-default-controller-desired-size
    pub(crate) fn desired_size(this: &Ref<Self>, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        // 1. Let readableController be this.[[stream]].[[readable]].[[controller]].
        let readable_controller = readable_default_controller(this.stream().readable_stream());

        // 2. Return ! ReadableStreamDefaultControllerGetDesiredSize(readableController).
        Ok(readable_stream_default_controller_get_desired_size(readable_controller))
    }

    // https://streams.spec.whatwg.org/#ts-default-controller-enqueue
    pub(crate) fn enqueue(this: &Ref<Self>, cx: &mut CallContext, chunk: Option<Value>) -> WebIdlResult<()> {
        let cx = &CallContext::new(cx.vm(), this.stream().backpressure_change_promise_realm());

        // 1. Perform ? TransformStreamDefaultControllerEnqueue(this, chunk).
        transform_stream_default_controller_enqueue(cx, *this, chunk.unwrap_or(Value::UNDEFINED))
    }

    // https://streams.spec.whatwg.org/#ts-default-controller-error
    pub(crate) fn error(this: &Ref<Self>, cx: &mut CallContext, reason: Option<Value>) -> WebIdlResult<()> {
        let cx = &this.stream().context(cx.vm());

        // 1. Perform ? TransformStreamDefaultControllerError(this, e).
        transform_stream_default_controller_error(cx, *this, reason.unwrap_or(Value::UNDEFINED));
        Ok(())
    }

    // https://streams.spec.whatwg.org/#ts-default-controller-terminate
    pub(crate) fn terminate(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        let cx = &this.stream().context(cx.vm());

        // 1. Perform ? TransformStreamDefaultControllerTerminate(this).
        transform_stream_default_controller_terminate(cx, *this);
        Ok(())
    }
}

// https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller
pub(crate) fn set_up_transform_stream_default_controller(
    stream: Ref<TransformStream>,
    controller: Ref<TransformStreamDefaultController>,
    transform_algorithm: TransformAlgorithm,
    flush_algorithm: FlushAlgorithm,
    cancel_algorithm: CancelAlgorithm,
) {
    // 1. Assert: stream implements TransformStream.
    // 2. Assert: stream.[[controller]] is undefined.
    assert!(stream.controller().is_none());

    // 3. Set controller.[[stream]] to stream.
    controller.set_stream(stream);

    // 4. Set stream.[[controller]] to controller.
    stream.set_controller(Some(controller));

    // 5. Set controller.[[transformAlgorithm]] to transformAlgorithm.
    controller.set_transform_algorithm(Some(transform_algorithm));

    // 6. Set controller.[[flushAlgorithm]] to flushAlgorithm.
    controller.set_flush_algorithm(Some(flush_algorithm));

    // 7. Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
    controller.set_cancel_algorithm(Some(cancel_algorithm));
}

// https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller-from-transformer
pub(crate) fn set_up_transform_stream_default_controller_from_transformer(
    cx: &CallContext,
    stream: Ref<TransformStream>,
    transformer: Value,
    transformer_dict: Transformer,
) {
    // 1. Let controller be a new TransformStreamDefaultController.
    let controller = allocate(cx.vm(), TransformStreamDefaultController::new());

    // 2. Let transformAlgorithm be the following steps, taking a chunk argument:
    let mut transform_algorithm = TransformAlgorithm::Enqueue(controller);

    // 3. Let flushAlgorithm be an algorithm which returns a promise resolved with undefined.
    let mut flush_algorithm = FlushAlgorithm::ReturnsResolvedUndefined;

    // 4. Let cancelAlgorithm be an algorithm which returns a promise resolved with undefined.
    let mut cancel_algorithm = CancelAlgorithm::ReturnsResolvedUndefined;

    // 5. If transformerDict["transform"] exists, set transformAlgorithm to an algorithm which takes an argument chunk
    //    and returns the result of invoking transformerDict["transform"] with argument list « chunk, controller » and
    //    callback this value transformer.
    if let Some(transform) = transformer_dict.transform {
        transform_algorithm = TransformAlgorithm::Callback {
            transform,
            transformer,
            controller,
        };
    }

    // 6. If transformerDict["flush"] exists, set flushAlgorithm to an algorithm which returns the result of invoking
    //    transformerDict["flush"] with argument list « controller » and callback this value transformer.
    if let Some(flush) = transformer_dict.flush {
        flush_algorithm = FlushAlgorithm::Callback {
            flush,
            transformer,
            controller,
        };
    }

    // 7. If transformerDict["cancel"] exists, set cancelAlgorithm to an algorithm which takes an argument reason and
    //    returns the result of invoking transformerDict["cancel"] with argument list « reason » and callback this value
    //    transformer.
    if let Some(cancel) = transformer_dict.cancel {
        cancel_algorithm = CancelAlgorithm::Callback {
            cancel,
            this_value: transformer,
        };
    }

    // 8. Perform ! SetUpTransformStreamDefaultController(stream, controller, transformAlgorithm, flushAlgorithm,
    //    cancelAlgorithm).
    set_up_transform_stream_default_controller(
        stream,
        controller,
        transform_algorithm,
        flush_algorithm,
        cancel_algorithm,
    );
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-clear-algorithms
pub(crate) fn transform_stream_default_controller_clear_algorithms(controller: Ref<TransformStreamDefaultController>) {
    // NOTE: This is observable using weak references. See tc39/proposal-weakrefs#31 for more detail.
    // 1. Set controller.[[transformAlgorithm]] to undefined.
    controller.set_transform_algorithm(None);

    // 2. Set controller.[[flushAlgorithm]] to undefined.
    controller.set_flush_algorithm(None);

    // 3. Set controller.[[cancelAlgorithm]] to undefined.
    controller.set_cancel_algorithm(None);
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-enqueue
pub(crate) fn transform_stream_default_controller_enqueue(
    cx: &CallContext,
    controller: Ref<TransformStreamDefaultController>,
    chunk: Value,
) -> WebIdlResult<()> {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. Let readableController be stream.[[readable]].[[controller]].
    let readable_controller = readable_default_controller(stream.readable_stream());

    // 3. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(readableController) is false, throw a TypeError
    //    exception.
    if !readable_stream_default_controller_can_close_or_enqueue(readable_controller) {
        return Err(Exception::type_error(
            "ReadableController is either closed or not readable.",
        ));
    }

    // 4. Let enqueueResult be ReadableStreamDefaultControllerEnqueue(readableController, chunk).
    let enqueue_result = readable_stream_default_controller_enqueue(cx, readable_controller, chunk);

    // 5. If enqueueResult is an abrupt completion,
    if let Err(exception) = enqueue_result {
        let error = exception_value(cx.vm(), cx.realm(), exception);

        // 1. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, enqueueResult.[[Value]]).
        transform_stream_error_writable_and_unblock_write(cx, stream, error);

        // 2. Throw stream.[[readable]].[[storedError]].
        return Err(Exception::Thrown(Throw::new(stream.readable_stream().stored_error())));
    }

    // 6. Let backpressure be ! ReadableStreamDefaultControllerHasBackpressure(readableController).
    let backpressure = readable_stream_default_controller_has_backpressure(readable_controller);

    // 7. If backpressure is not stream.[[backpressure]],
    if Some(backpressure) != stream.backpressure() {
        // 1. Assert: backpressure is true.
        assert!(backpressure);

        // 2. Perform ! TransformStreamSetBackpressure(stream, true).
        transform_stream_set_backpressure(
            &CallContext::new(cx.vm(), stream.backpressure_change_promise_realm()),
            stream,
            true,
        );
    }

    Ok(())
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-error
pub(crate) fn transform_stream_default_controller_error(
    cx: &CallContext,
    controller: Ref<TransformStreamDefaultController>,
    error: Value,
) {
    // 1. Perform ! TransformStreamError(controller.[[stream]], e).
    transform_stream_error(cx, controller.stream(), error);
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-perform-transform
pub(crate) fn transform_stream_default_controller_perform_transform(
    cx: &CallContext,
    controller: Ref<TransformStreamDefaultController>,
    chunk: Value,
) -> WebIdlPromise {
    let vm = cx.vm();

    // 1. Let transformPromise be the result of performing controller.[[transformAlgorithm]], passing chunk.
    let transform_algorithm = controller
        .transform_algorithm()
        .expect("the controller's algorithms are not cleared");
    let transform_promise = transform_algorithm.run(&controller.stream().context(vm), chunk);

    // 2. Return the result of reacting to transformPromise with the following rejection steps given the argument r:
    upon_rejection(vm, transform_promise, controller, |cx, controller, reason| {
        let stream = controller.stream();

        // 1. Perform ! TransformStreamError(controller.[[stream]], r).
        transform_stream_error(&stream.context(cx.vm()), stream, reason);

        // 2. Throw r.
        Err(Exception::Thrown(Throw::new(reason)))
    })
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-terminate
pub(crate) fn transform_stream_default_controller_terminate(
    cx: &CallContext,
    controller: Ref<TransformStreamDefaultController>,
) {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();
    let realm = stream.backpressure_change_promise_realm();

    // 2. Let readableController be stream.[[readable]].[[controller]].
    let readable_controller = readable_default_controller(stream.readable_stream());

    // 3. Perform ! ReadableStreamDefaultControllerClose(readableController).
    readable_stream_default_controller_close(cx, readable_controller);

    // 4. Let error be a TypeError exception indicating that the stream has been terminated.
    let error = exception_value(cx.vm(), realm, Exception::type_error("Stream has been terminated."));

    // 5. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, error).
    transform_stream_error_writable_and_unblock_write(cx, stream, error);
}
