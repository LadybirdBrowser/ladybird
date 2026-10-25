/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WritableStreamDefaultController, https://streams.spec.whatwg.org/#ws-default-controller-class, and its abstract
//! operations, https://streams.spec.whatwg.org/#ws-default-controller-abstract-ops

use core::cell::Cell;

use libjs_rust::api::{DeferGc, GcRefCell, Trace, Value, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Ref, Wrappable, allocate, wrap};
use crate::bindings::promise::{WebIdlPromise, create_resolved_promise, exception_value, react_to_promise};
use crate::bindings::{CallContext, MustNotThrow, WebIdlResult};
use crate::streams::algorithms::{AbortAlgorithm, CloseAlgorithm, SizeAlgorithm, StartAlgorithm, WriteAlgorithm};
use crate::streams::cpp::AbortSignal;
use crate::streams::queue_with_sizes::QueueWithSizes;
use crate::streams::underlying_sink::UnderlyingSink;
use crate::streams::writable_stream::{
    WritableStream, WritableStreamState, writable_stream_close_queued_or_in_flight,
    writable_stream_deal_with_rejection, writable_stream_finish_erroring, writable_stream_finish_in_flight_close,
    writable_stream_finish_in_flight_close_with_error, writable_stream_finish_in_flight_write,
    writable_stream_finish_in_flight_write_with_error, writable_stream_mark_close_request_in_flight,
    writable_stream_mark_first_write_request_in_flight, writable_stream_start_erroring,
    writable_stream_update_backpressure,
};

// https://streams.spec.whatwg.org/#close-sentinel
fn create_close_sentinel() -> Value {
    // The close sentinel is a unique value enqueued into [[queue]], in lieu of a chunk, to signal that the stream is
    // closed. It is only used internally, and is never exposed to web developers.
    Value::EMPTY
}

// https://streams.spec.whatwg.org/#close-sentinel
fn is_close_sentinel(value: Value) -> bool {
    value.is_empty()
}

// https://streams.spec.whatwg.org/#writablestreamdefaultcontroller
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WritableStreamDefaultController {
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-abortalgorithm
    abort_algorithm: GcRefCell<Option<AbortAlgorithm>>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-closealgorithm
    close_algorithm: GcRefCell<Option<CloseAlgorithm>>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-queue
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-queuetotalsize
    queue: QueueWithSizes,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-abortcontroller, of which LibWeb's C++ keeps the
    // signal.
    signal: GcRefCell<Option<AbortSignal>>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-started
    started: Cell<bool>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-strategyhwm
    #[gc(untraced)]
    strategy_hwm: Cell<f64>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-strategysizealgorithm
    strategy_size_algorithm: GcRefCell<Option<SizeAlgorithm>>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-stream
    stream: Cell<Option<Ref<WritableStream>>>,
    // https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-writealgorithm
    write_algorithm: GcRefCell<Option<WriteAlgorithm>>,
}

impl Wrappable for WritableStreamDefaultController {}

impl WritableStreamDefaultController {
    pub(crate) fn new(defer_gc: &DeferGc<'_>) -> Self {
        Self {
            abort_algorithm: GcRefCell::new(None),
            close_algorithm: GcRefCell::new(None),
            queue: QueueWithSizes::new(defer_gc),
            signal: GcRefCell::new(None),
            started: Cell::new(false),
            strategy_hwm: Cell::new(0.0),
            strategy_size_algorithm: GcRefCell::new(None),
            stream: Cell::new(None),
            write_algorithm: GcRefCell::new(None),
        }
    }

    /// The controller's [[stream]], which it has once it is set up.
    pub(crate) fn stream(&self) -> Ref<WritableStream> {
        self.stream.get().expect("a controller that is set up has a stream")
    }

    pub(crate) fn started(&self) -> bool {
        self.started.get()
    }

    /// The controller's [[writeAlgorithm]], which it has until its algorithms are cleared.
    fn write_algorithm(&self) -> Option<WriteAlgorithm> {
        self.write_algorithm.borrow().clone()
    }

    /// The controller's [[closeAlgorithm]], which it has until its algorithms are cleared.
    fn close_algorithm(&self) -> Option<CloseAlgorithm> {
        self.close_algorithm.borrow().clone()
    }

    /// The controller's [[abortAlgorithm]], which it has until its algorithms are cleared.
    fn abort_algorithm(&self) -> Option<AbortAlgorithm> {
        self.abort_algorithm.borrow().clone()
    }

    /// The controller's [[strategySizeAlgorithm]], which it has until its algorithms are cleared.
    fn strategy_size_algorithm(&self) -> Option<SizeAlgorithm> {
        self.strategy_size_algorithm.borrow().clone()
    }

    /// The signal of the controller's [[abortController]].
    pub(crate) fn abort_signal(&self) -> AbortSignal {
        self.signal
            .borrow()
            .clone()
            .expect("a controller that is set up has a signal")
    }

    // https://streams.spec.whatwg.org/#ws-default-controller-signal
    pub(crate) fn signal(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.abort_signal().wrapper(cx.realm()))
    }

    // https://streams.spec.whatwg.org/#ws-default-controller-error
    pub(crate) fn error(this: &Ref<Self>, cx: &mut CallContext, e: Option<Value>) -> WebIdlResult<()> {
        // 1. Let state be this.[[stream]].[[state]].
        let state = this.stream().state();

        // 2. If state is not "writable", return.
        if state != WritableStreamState::Writable {
            return Ok(());
        }

        // 3. Perform ! WritableStreamDefaultControllerError(this, e).
        writable_stream_default_controller_error(cx, *this, e.unwrap_or(Value::UNDEFINED));
        Ok(())
    }

    // https://streams.spec.whatwg.org/#ws-default-controller-private-abort
    pub(crate) fn abort_steps(this: Ref<Self>, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Let result be the result of performing this.[[abortAlgorithm]], passing reason.
        let abort_algorithm = this
            .abort_algorithm()
            .expect("a controller that aborts has an abort algorithm");
        let result = abort_algorithm.run(&this.stream().context(cx.vm()), reason);

        // 2. Perform ! WritableStreamDefaultControllerClearAlgorithms(this).
        writable_stream_default_controller_clear_algorithms(this);

        // 3. Return result.
        result
    }

    // https://streams.spec.whatwg.org/#ws-default-controller-private-error
    pub(crate) fn error_steps(vm: &Vm, this: Ref<Self>) {
        // 1. Perform ! ResetQueue(this).
        this.queue.reset_queue(vm);
    }
}

// https://streams.spec.whatwg.org/#set-up-writable-stream-default-controller
#[allow(clippy::too_many_arguments, reason = "the abstract operation takes these arguments")]
pub(crate) fn set_up_writable_stream_default_controller(
    cx: &CallContext,
    stream: Ref<WritableStream>,
    controller: Ref<WritableStreamDefaultController>,
    start_algorithm: StartAlgorithm,
    write_algorithm: WriteAlgorithm,
    close_algorithm: CloseAlgorithm,
    abort_algorithm: AbortAlgorithm,
    high_water_mark: f64,
    size_algorithm: SizeAlgorithm,
) -> WebIdlResult<()> {
    let vm = cx.vm();

    // 1. Assert: stream implements WritableStream.

    // 2. Assert: stream.[[controller]] is undefined.
    assert!(stream.controller().is_none());

    // 3. Set controller.[[stream]] to stream.
    controller.stream.set(Some(stream));

    // 4. Set stream.[[controller]] to controller.
    stream.set_controller(Some(controller));

    // 5. Perform ! ResetQueue(controller).
    controller.queue.reset_queue(vm);

    // 6. Set controller.[[abortController]] to a new AbortController.
    let signal = AbortSignal::create();
    *controller.signal.borrow_mut(vm) = Some(signal);

    // 7. Set controller.[[started]] to false.
    controller.started.set(false);

    // 8. Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm.
    *controller.strategy_size_algorithm.borrow_mut(vm) = Some(size_algorithm);

    // 9. Set controller.[[strategyHWM]] to highWaterMark.
    controller.strategy_hwm.set(high_water_mark);

    // 10. Set controller.[[writeAlgorithm]] to writeAlgorithm.
    *controller.write_algorithm.borrow_mut(vm) = Some(write_algorithm);

    // 11. Set controller.[[closeAlgorithm]] to closeAlgorithm.
    *controller.close_algorithm.borrow_mut(vm) = Some(close_algorithm);

    // 12. Set controller.[[abortAlgorithm]] to abortAlgorithm.
    *controller.abort_algorithm.borrow_mut(vm) = Some(abort_algorithm);

    // 13. Let backpressure be ! WritableStreamDefaultControllerGetBackpressure(controller).
    let backpressure = writable_stream_default_controller_get_backpressure(controller);

    // 14. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
    writable_stream_update_backpressure(cx, stream, backpressure);

    // 15. Let startResult be the result of performing startAlgorithm. (This may throw an exception.)
    let start_result = start_algorithm.run()?;

    // 16. Let startPromise be a promise resolved with startResult.
    let start_promise = create_resolved_promise(vm, cx.realm(), start_result);

    react_to_promise(
        vm,
        start_promise,
        (controller, stream),
        // 17. Upon fulfillment of startPromise,
        Some(|cx, (controller, stream), _| {
            let cx = &stream.context(cx.vm());

            // 1. Assert: stream.[[state]] is "writable" or "erroring".
            assert!(matches!(
                stream.state(),
                WritableStreamState::Writable | WritableStreamState::Erroring
            ));

            // 2. Set controller.[[started]] to true.
            controller.started.set(true);

            // 3. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
            writable_stream_default_controller_advance_queue_if_needed(cx, controller);

            Ok(Value::UNDEFINED)
        }),
        // 18. Upon rejection of startPromise with reason r,
        Some(|cx, (controller, stream), reason| {
            let cx = &stream.context(cx.vm());

            // 1. Assert: stream.[[state]] is "writable" or "erroring".
            assert!(matches!(
                stream.state(),
                WritableStreamState::Writable | WritableStreamState::Erroring
            ));

            // 2. Set controller.[[started]] to true.
            controller.started.set(true);

            // 3. Perform ! WritableStreamDealWithRejection(stream, r).
            writable_stream_deal_with_rejection(cx, stream, reason);

            Ok(Value::UNDEFINED)
        }),
    );
    Ok(())
}

// https://streams.spec.whatwg.org/#set-up-writable-stream-default-controller-from-underlying-sink
pub(crate) fn set_up_writable_stream_default_controller_from_underlying_sink(
    cx: &CallContext,
    stream: Ref<WritableStream>,
    underlying_sink: Value,
    underlying_sink_dict: UnderlyingSink,
    high_water_mark: f64,
    size_algorithm: SizeAlgorithm,
) -> WebIdlResult<()> {
    // 1. Let controller be a new WritableStreamDefaultController.
    let controller = allocate(cx.vm(), WritableStreamDefaultController::new);

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    let mut start_algorithm = StartAlgorithm::ReturnsUndefined;

    // 3. Let writeAlgorithm be an algorithm that returns a promise resolved with undefined.
    let mut write_algorithm = WriteAlgorithm::ReturnsResolvedUndefined;

    // 4. Let closeAlgorithm be an algorithm that returns a promise resolved with undefined.
    let mut close_algorithm = CloseAlgorithm::ReturnsResolvedUndefined;

    // 5. Let abortAlgorithm be an algorithm that returns a promise resolved with undefined.
    let mut abort_algorithm = AbortAlgorithm::ReturnsResolvedUndefined;

    // 6. If underlyingSinkDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSinkDict["start"] with argument list « controller », exception behavior "rethrow", and
    //    callback this value underlyingSink.
    if let Some(start) = underlying_sink_dict.start {
        start_algorithm = StartAlgorithm::Callback {
            start,
            this_value: underlying_sink,
            controller: Value::from_object(wrap(cx, controller)),
        };
    }

    // 7. If underlyingSinkDict["write"] exists, then set writeAlgorithm to an algorithm which takes an argument chunk
    //    and returns the result of invoking underlyingSinkDict["write"] with argument list « chunk, controller » and
    //    callback this value underlyingSink.
    if let Some(write) = underlying_sink_dict.write {
        write_algorithm = WriteAlgorithm::Callback {
            write,
            underlying_sink,
            controller,
        };
    }

    // 8. If underlyingSinkDict["close"] exists, then set closeAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSinkDict["close"] with argument list «» and callback this value underlyingSink.
    if let Some(close) = underlying_sink_dict.close {
        close_algorithm = CloseAlgorithm::Callback { close, underlying_sink };
    }

    // 9. If underlyingSinkDict["abort"] exists, then set abortAlgorithm to an algorithm which takes an argument reason
    //    and returns the result of invoking underlyingSinkDict["abort"] with argument list « reason » and callback this
    //    value underlyingSink.
    if let Some(abort) = underlying_sink_dict.abort {
        abort_algorithm = AbortAlgorithm::Callback { abort, underlying_sink };
    }

    // 10. Perform ? SetUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm,
    //     closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm).
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
    )
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-advance-queue-if-needed
pub(crate) fn writable_stream_default_controller_advance_queue_if_needed(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
) {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. If controller.[[started]] is false, return.
    if !controller.started() {
        return;
    }

    // 3. If stream.[[inFlightWriteRequest]] is not undefined, return.
    if stream.in_flight_write_request().is_some() {
        return;
    }

    // 4. Let state be stream.[[state]].
    let state = stream.state();

    // 5. Assert: state is not "closed" or "errored".
    assert!(state != WritableStreamState::Closed && state != WritableStreamState::Errored);

    // 6. If state is "erroring",
    if state == WritableStreamState::Erroring {
        // 1. Perform ! WritableStreamFinishErroring(stream).
        writable_stream_finish_erroring(cx, stream);

        // 2. Return.
        return;
    }

    // 7. If controller.[[queue]] is empty, return.
    if controller.queue.is_empty() {
        return;
    }

    // 8. Let value be ! PeekQueueValue(controller).
    let value = controller.queue.peek_queue_value();

    // 9. If value is the close sentinel, perform ! WritableStreamDefaultControllerProcessClose(controller).
    if is_close_sentinel(value) {
        writable_stream_default_controller_process_close(cx, controller);
    }
    // 10. Otherwise, perform ! WritableStreamDefaultControllerProcessWrite(controller, value).
    else {
        writable_stream_default_controller_process_write(cx, controller, value);
    }
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-clear-algorithms
pub(crate) fn writable_stream_default_controller_clear_algorithms(controller: Ref<WritableStreamDefaultController>) {
    // 1. Set controller.[[writeAlgorithm]] to undefined.
    controller.write_algorithm.replace(None);

    // 2. Set controller.[[closeAlgorithm]] to undefined.
    controller.close_algorithm.replace(None);

    // 3. Set controller.[[abortAlgorithm]] to undefined.
    controller.abort_algorithm.replace(None);

    // 4. Set controller.[[strategySizeAlgorithm]] to undefined.
    controller.strategy_size_algorithm.replace(None);
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-close
pub(crate) fn writable_stream_default_controller_close(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
) {
    // 1. Perform ! EnqueueValueWithSize(controller, close sentinel, 0).
    controller
        .queue
        .enqueue_value_with_size(cx.vm(), create_close_sentinel(), Value::from_f64(0.0))
        .must();

    // 2. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
    writable_stream_default_controller_advance_queue_if_needed(cx, controller);
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-error
pub(crate) fn writable_stream_default_controller_error(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
    error: Value,
) {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. Assert: stream.[[state]] is "writable".
    assert!(stream.state() == WritableStreamState::Writable);

    // 3. Perform ! WritableStreamDefaultControllerClearAlgorithms(controller).
    writable_stream_default_controller_clear_algorithms(controller);

    // 4. Perform ! WritableStreamStartErroring(stream, error).
    writable_stream_start_erroring(cx, stream, error);
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-error-if-needed
pub(crate) fn writable_stream_default_controller_error_if_needed(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
    error: Value,
) {
    // 1. If controller.[[stream]].[[state]] is "writable", perform ! WritableStreamDefaultControllerError(controller,
    //    error).
    if controller.stream().state() == WritableStreamState::Writable {
        writable_stream_default_controller_error(cx, controller, error);
    }
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-get-backpressure
pub(crate) fn writable_stream_default_controller_get_backpressure(
    controller: Ref<WritableStreamDefaultController>,
) -> bool {
    // 1. Let desiredSize be ! WritableStreamDefaultControllerGetDesiredSize(controller).
    let desired_size = writable_stream_default_controller_get_desired_size(controller);

    // 2. Return true if desiredSize ≤ 0, or false otherwise.
    desired_size <= 0.0
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-get-chunk-size
pub(crate) fn writable_stream_default_controller_get_chunk_size(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
    chunk: Value,
) -> Value {
    // 1. If controller.[[strategySizeAlgorithm]] is undefined, then:
    let Some(strategy_size_algorithm) = controller.strategy_size_algorithm() else {
        // 1. Assert: controller.[[stream]].[[state]] is not "writable".
        assert!(controller.stream().state() != WritableStreamState::Writable);

        // 2. Return 1.
        return Value::from_f64(1.0);
    };

    // 2. Let returnValue be the result of performing controller.[[strategySizeAlgorithm]], passing in chunk, and
    //    interpreting the result as a completion record.
    let return_value = strategy_size_algorithm.run(&controller.stream().context(cx.vm()), chunk);

    match return_value {
        // 3. If returnValue is an abrupt completion,
        Err(throw) => {
            // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, returnValue.[[Value]]).
            writable_stream_default_controller_error_if_needed(cx, controller, throw.value());

            // 2. Return 1.
            Value::from_f64(1.0)
        }
        // 4. Return returnValue.[[Value]].
        Ok(value) => value,
    }
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-get-desired-size
pub(crate) fn writable_stream_default_controller_get_desired_size(
    controller: Ref<WritableStreamDefaultController>,
) -> f64 {
    // 1. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
    controller.strategy_hwm.get() - controller.queue.total_size()
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-process-close
pub(crate) fn writable_stream_default_controller_process_close(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
) {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. Perform ! WritableStreamMarkCloseRequestInFlight(stream).
    writable_stream_mark_close_request_in_flight(stream);

    // 3. Perform ! DequeueValue(controller).
    controller.queue.dequeue_value(cx.vm());

    // 4. Assert: controller.[[queue]] is empty.
    assert!(controller.queue.is_empty());

    // 5. Let sinkClosePromise be the result of performing controller.[[closeAlgorithm]].
    let close_algorithm = controller
        .close_algorithm()
        .expect("a controller that closes has a close algorithm");
    let sink_close_promise = close_algorithm.run(&stream.context(cx.vm()));

    // 6. Perform ! WritableStreamDefaultControllerClearAlgorithms(controller).
    writable_stream_default_controller_clear_algorithms(controller);

    react_to_promise(
        cx.vm(),
        sink_close_promise,
        stream,
        // 7. Upon fulfillment of sinkClosePromise,
        Some(|cx, stream, _| {
            // 1. Perform ! WritableStreamFinishInFlightClose(stream).
            writable_stream_finish_in_flight_close(cx, stream);

            Ok(Value::UNDEFINED)
        }),
        // 8. Upon rejection of sinkClosePromise with reason reason,
        Some(|cx, stream, reason| {
            // 1. Perform ! WritableStreamFinishInFlightCloseWithError(stream, reason).
            writable_stream_finish_in_flight_close_with_error(cx, stream, reason);

            Ok(Value::UNDEFINED)
        }),
    );
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-process-write
pub(crate) fn writable_stream_default_controller_process_write(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
    chunk: Value,
) {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. Perform ! WritableStreamMarkFirstWriteRequestInFlight(stream).
    writable_stream_mark_first_write_request_in_flight(cx.vm(), stream);

    // 3. Let sinkWritePromise be the result of performing controller.[[writeAlgorithm]], passing in chunk.
    let write_algorithm = controller
        .write_algorithm()
        .expect("a controller that writes has a write algorithm");
    let sink_write_promise = write_algorithm.run(&stream.context(cx.vm()), chunk);

    react_to_promise(
        cx.vm(),
        sink_write_promise,
        (controller, stream),
        // 4. Upon fulfillment of sinkWritePromise,
        Some(|cx, (controller, stream), _| {
            // 1. Perform ! WritableStreamFinishInFlightWrite(stream).
            writable_stream_finish_in_flight_write(cx, stream);

            // 2. Let state be stream.[[state]].
            let state = stream.state();

            // 3. Assert: state is "writable" or "erroring".
            assert!(state == WritableStreamState::Writable || state == WritableStreamState::Erroring);

            // 4. Perform ! DequeueValue(controller).
            controller.queue.dequeue_value(cx.vm());

            // 5. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and state is "writable",
            if !writable_stream_close_queued_or_in_flight(stream) && state == WritableStreamState::Writable {
                // 1. Let backpressure be ! WritableStreamDefaultControllerGetBackpressure(controller).
                let backpressure = writable_stream_default_controller_get_backpressure(controller);

                // 2. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
                writable_stream_update_backpressure(cx, stream, backpressure);
            }

            // 6. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
            writable_stream_default_controller_advance_queue_if_needed(cx, controller);

            Ok(Value::UNDEFINED)
        }),
        // 5. Upon rejection of sinkWritePromise with reason,
        Some(|cx, (controller, stream), reason| {
            // 1. If stream.[[state]] is "writable", perform
            //    ! WritableStreamDefaultControllerClearAlgorithms(controller).
            if stream.state() == WritableStreamState::Writable {
                writable_stream_default_controller_clear_algorithms(controller);
            }

            // 2. Perform ! WritableStreamFinishInFlightWriteWithError(stream, reason).
            writable_stream_finish_in_flight_write_with_error(cx, stream, reason);

            Ok(Value::UNDEFINED)
        }),
    );
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-write
pub(crate) fn writable_stream_default_controller_write(
    cx: &CallContext,
    controller: Ref<WritableStreamDefaultController>,
    chunk: Value,
    chunk_size: Value,
) {
    // 1. Let enqueueResult be EnqueueValueWithSize(controller, chunk, chunkSize).
    let enqueue_result = controller.queue.enqueue_value_with_size(cx.vm(), chunk, chunk_size);

    // 2. If enqueueResult is an abrupt completion,
    if let Err(exception) = enqueue_result {
        let error = exception_value(cx.vm(), cx.realm(), exception);

        // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, enqueueResult.[[Value]]).
        writable_stream_default_controller_error_if_needed(cx, controller, error);

        // 2. Return.
        return;
    }

    // 3. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 4. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and stream.[[state]] is "writable",
    if !writable_stream_close_queued_or_in_flight(stream) && stream.state() == WritableStreamState::Writable {
        // 1. Let backpressure be ! WritableStreamDefaultControllerGetBackpressure(controller).
        let backpressure = writable_stream_default_controller_get_backpressure(controller);

        // 2. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
        writable_stream_update_backpressure(cx, stream, backpressure);
    }

    // 5. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
    writable_stream_default_controller_advance_queue_if_needed(cx, controller);
}
