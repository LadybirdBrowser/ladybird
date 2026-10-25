/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStreamDefaultController, https://streams.spec.whatwg.org/#rs-default-controller-class, and its abstract
//! operations, https://streams.spec.whatwg.org/#rs-default-controller-abstract-ops

use core::cell::Cell;

use libjs_rust::api::{GcRefCell, Throw, Trace, Value, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, create_resolved_promise, exception_value, react_to_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::algorithms::{CancelAlgorithm, PullAlgorithm, SizeAlgorithm, StartAlgorithm};
use crate::streams::queue_with_sizes::QueueWithSizes;
use crate::streams::read_request::ReadRequest;
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamState, complete_rendering_pending_stream, is_readable_stream_locked,
    readable_stream_add_read_request, readable_stream_close, readable_stream_error,
    readable_stream_fulfill_read_request, readable_stream_get_num_read_requests,
};
use crate::streams::underlying_source::{ReadableStreamController, UnderlyingSource};

// https://streams.spec.whatwg.org/#readablestreamdefaultcontroller
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableStreamDefaultController {
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-cancelalgorithm
    cancel_algorithm: GcRefCell<Option<CancelAlgorithm>>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-closerequested
    close_requested: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-pullagain
    pull_again: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-pullalgorithm
    pull_algorithm: GcRefCell<Option<PullAlgorithm>>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-pulling
    pulling: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-queue
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-queuetotalsize
    queue: QueueWithSizes,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-started
    started: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-strategyhwm
    #[gc(untraced)]
    strategy_hwm: Cell<f64>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-strategysizealgorithm
    strategy_size_algorithm: GcRefCell<Option<SizeAlgorithm>>,
    // https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-stream
    stream: Cell<Option<Ref<ReadableStream>>>,
}

impl Wrappable for ReadableStreamDefaultController {}

impl ReadableStreamDefaultController {
    pub(crate) fn new() -> Self {
        Self {
            cancel_algorithm: GcRefCell::new(None),
            close_requested: Cell::new(false),
            pull_again: Cell::new(false),
            pull_algorithm: GcRefCell::new(None),
            pulling: Cell::new(false),
            queue: QueueWithSizes::default(),
            started: Cell::new(false),
            strategy_hwm: Cell::new(0.0),
            strategy_size_algorithm: GcRefCell::new(None),
            stream: Cell::new(None),
        }
    }

    pub(crate) fn close_requested(&self) -> bool {
        self.close_requested.get()
    }

    /// The controller's [[stream]], which it has once it is set up.
    pub(crate) fn stream(&self) -> Ref<ReadableStream> {
        self.stream.get().expect("a controller that is set up has a stream")
    }

    /// The controller's [[pullAlgorithm]], which it has until its algorithms are cleared.
    pub(crate) fn pull_algorithm(&self) -> Option<PullAlgorithm> {
        self.pull_algorithm.borrow().clone()
    }

    /// The controller's [[cancelAlgorithm]], which it has until its algorithms are cleared.
    pub(crate) fn cancel_algorithm(&self) -> Option<CancelAlgorithm> {
        self.cancel_algorithm.borrow().clone()
    }

    /// The controller's [[strategySizeAlgorithm]], which it has until its algorithms are cleared.
    pub(crate) fn strategy_size_algorithm(&self) -> Option<SizeAlgorithm> {
        self.strategy_size_algorithm.borrow().clone()
    }

    // https://streams.spec.whatwg.org/#rs-default-controller-desired-size
    pub(crate) fn desired_size(this: &Ref<Self>, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        // 1. Return ! ReadableStreamDefaultControllerGetDesiredSize(this).
        Ok(readable_stream_default_controller_get_desired_size(*this))
    }

    // https://streams.spec.whatwg.org/#rs-default-controller-close
    pub(crate) fn close(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is false, throw a TypeError exception.
        if !readable_stream_default_controller_can_close_or_enqueue(*this) {
            return Err(Exception::type_error("Stream is not closable"));
        }

        // 2. Perform ! ReadableStreamDefaultControllerClose(this).
        readable_stream_default_controller_close(cx, *this);
        Ok(())
    }

    // https://streams.spec.whatwg.org/#rs-default-controller-enqueue
    pub(crate) fn enqueue(this: &Ref<Self>, cx: &mut CallContext, chunk: Option<Value>) -> WebIdlResult<()> {
        // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is false, throw a TypeError exception.
        if !readable_stream_default_controller_can_close_or_enqueue(*this) {
            return Err(Exception::type_error("Cannot enqueue chunk to stream"));
        }

        // 2. Perform ? ReadableStreamDefaultControllerEnqueue(this, chunk).
        readable_stream_default_controller_enqueue(cx, *this, chunk.unwrap_or(Value::UNDEFINED))
    }

    // https://streams.spec.whatwg.org/#rs-default-controller-error
    pub(crate) fn error(this: &Ref<Self>, cx: &mut CallContext, e: Option<Value>) -> WebIdlResult<()> {
        // 1. Perform ! ReadableStreamDefaultControllerError(this, e).
        readable_stream_default_controller_error(cx, *this, e.unwrap_or(Value::UNDEFINED));
        Ok(())
    }

    // https://streams.spec.whatwg.org/#rs-default-controller-private-cancel
    pub(crate) fn cancel_steps(this: Ref<Self>, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Perform ! ResetQueue(this).
        this.queue.reset_queue(cx.vm());

        // 2. Let result be the result of performing this.[[cancelAlgorithm]], passing reason.
        let cancel_algorithm = this
            .cancel_algorithm()
            .expect("a controller that cancels has a cancel algorithm");
        let result = cancel_algorithm.run(&this.stream().context(cx.vm()), reason);

        // 3. Perform ! ReadableStreamDefaultControllerClearAlgorithms(this).
        readable_stream_default_controller_clear_algorithms(this);

        // 4. Return result.
        result
    }

    // https://streams.spec.whatwg.org/#rs-default-controller-private-pull
    pub(crate) fn pull_steps(this: Ref<Self>, vm: &Vm, read_request: ReadRequest) {
        // 1. Let stream be this.[[stream]].
        let stream = this.stream();
        let cx = &stream.context(vm);

        // 2. If this.[[queue]] is not empty,
        if !this.queue.is_empty() {
            // 1. Let chunk be ! DequeueValue(this).
            let chunk = this.queue.dequeue_value(vm);

            // 2. If this.[[closeRequested]] is true and this.[[queue]] is empty,
            if this.close_requested() && this.queue.is_empty() {
                // 1. Perform ! ReadableStreamDefaultControllerClearAlgorithms(this).
                readable_stream_default_controller_clear_algorithms(this);

                // 2. Perform ! ReadableStreamClose(stream).
                readable_stream_close(vm, stream);
            }
            // 3. Otherwise, perform ! ReadableStreamDefaultControllerCallPullIfNeeded(this).
            else {
                readable_stream_default_controller_call_pull_if_needed(cx, this);
            }

            // 4. Perform readRequest’s chunk steps, given chunk.
            read_request.chunk_steps(vm, chunk);
        }
        // 3. Otherwise,
        else {
            // 1. Perform ! ReadableStreamAddReadRequest(stream, readRequest).
            readable_stream_add_read_request(vm, stream, read_request);

            // 2. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(this).
            readable_stream_default_controller_call_pull_if_needed(cx, this);
        }
    }

    // https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultcontroller-releasesteps
    pub(crate) fn release_steps(_: Ref<Self>) {
        // 1. Return.
    }
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-call-pull-if-needed
pub(crate) fn readable_stream_default_controller_call_pull_if_needed(
    cx: &CallContext,
    controller: Ref<ReadableStreamDefaultController>,
) {
    let vm = cx.vm();

    // 1. Let shouldPull be ! ReadableStreamDefaultControllerShouldCallPull(controller).
    let should_pull = readable_stream_default_controller_should_call_pull(controller);

    // 2. If shouldPull is false, return.
    if !should_pull {
        return;
    }

    // 3. If controller.[[pulling]] is true,
    if controller.pulling.get() {
        // 1. Set controller.[[pullAgain]] to true.
        controller.pull_again.set(true);

        // 2. Return.
        return;
    }

    // 4. Assert: controller.[[pullAgain]] is false.
    assert!(!controller.pull_again.get());

    // 5. Set controller.[[pulling]] to true.
    controller.pulling.set(true);

    // 6. Let pullPromise be the result of performing controller.[[pullAlgorithm]].
    let pull_algorithm = controller
        .pull_algorithm()
        .expect("a controller that pulls has a pull algorithm");
    let pull_promise = pull_algorithm.run(&controller.stream().context(vm));

    react_to_promise(
        vm,
        pull_promise,
        controller,
        // 7. Upon fulfillment of pullPromise,
        Some(|cx, controller, _| {
            // 1. Set controller.[[pulling]] to false.
            controller.pulling.set(false);

            // 2. If controller.[[pullAgain]] is true,
            if controller.pull_again.get() {
                // 1. Set controller.[[pullAgain]] to false.
                controller.pull_again.set(false);

                // 2. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
                readable_stream_default_controller_call_pull_if_needed(cx, controller);
            }
            Ok(Value::UNDEFINED)
        }),
        // 8. Upon rejection of pullPromise with reason e,
        Some(|cx, controller, error| {
            // 1. Perform ! ReadableStreamDefaultControllerError(controller, e).
            readable_stream_default_controller_error(cx, controller, error);
            Ok(Value::UNDEFINED)
        }),
    );
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-should-call-pull
pub(crate) fn readable_stream_default_controller_should_call_pull(
    controller: Ref<ReadableStreamDefaultController>,
) -> bool {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller) is false, return false.
    if !readable_stream_default_controller_can_close_or_enqueue(controller) {
        return false;
    }

    // 3. If controller.[[started]] is false, return false.
    if !controller.started.get() {
        return false;
    }

    // 4. If ! IsReadableStreamLocked(stream) is true and ! ReadableStreamGetNumReadRequests(stream) > 0, return true.
    if is_readable_stream_locked(stream) && readable_stream_get_num_read_requests(stream) > 0 {
        return true;
    }

    // 5. Let desiredSize be ! ReadableStreamDefaultControllerGetDesiredSize(controller).
    // 6. Assert: desiredSize is not null.
    let desired_size =
        readable_stream_default_controller_get_desired_size(controller).expect("the desired size is not null");

    // 7. If desiredSize > 0, return true.
    if desired_size > 0.0 {
        return true;
    }

    // 8. Return false.
    false
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-clear-algorithms
pub(crate) fn readable_stream_default_controller_clear_algorithms(controller: Ref<ReadableStreamDefaultController>) {
    // 1. Set controller.[[pullAlgorithm]] to undefined.
    controller.pull_algorithm.replace(None);

    // 2. Set controller.[[cancelAlgorithm]] to undefined.
    controller.cancel_algorithm.replace(None);

    // 3. Set controller.[[strategySizeAlgorithm]] to undefined.
    controller.strategy_size_algorithm.replace(None);
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-close
pub(crate) fn readable_stream_default_controller_close(
    cx: &CallContext,
    controller: Ref<ReadableStreamDefaultController>,
) {
    // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller) is false, return.
    if !readable_stream_default_controller_can_close_or_enqueue(controller) {
        return;
    }

    // 2. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 3. Set controller.[[closeRequested]] to true.
    controller.close_requested.set(true);
    complete_rendering_pending_stream(stream);

    // 4. If controller.[[queue]] is empty,
    if controller.queue.is_empty() {
        // 1. Perform ! ReadableStreamDefaultControllerClearAlgorithms(controller).
        readable_stream_default_controller_clear_algorithms(controller);

        // 2. Perform ! ReadableStreamClose(stream).
        readable_stream_close(cx.vm(), stream);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-enqueue
pub(crate) fn readable_stream_default_controller_enqueue(
    cx: &CallContext,
    controller: Ref<ReadableStreamDefaultController>,
    chunk: Value,
) -> WebIdlResult<()> {
    let vm = cx.vm();

    // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller) is false, return.
    if !readable_stream_default_controller_can_close_or_enqueue(controller) {
        return Ok(());
    }

    // 2. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 3. If ! IsReadableStreamLocked(stream) is true and ! ReadableStreamGetNumReadRequests(stream) > 0, perform
    //    ! ReadableStreamFulfillReadRequest(stream, chunk, false).
    if is_readable_stream_locked(stream) && readable_stream_get_num_read_requests(stream) > 0 {
        readable_stream_fulfill_read_request(vm, stream, chunk, false);
    }
    // 4. Otherwise,
    else {
        // 1. Let result be the result of performing controller.[[strategySizeAlgorithm]], passing in chunk, and
        //    interpreting the result as a completion record.
        let strategy_size_algorithm = controller
            .strategy_size_algorithm()
            .expect("a controller that enqueues has a strategy size algorithm");
        let result = strategy_size_algorithm.run(&stream.context(vm), chunk);

        // 2. If result is an abrupt completion,
        let chunk_size = match result {
            Err(throw) => {
                // 1. Perform ! ReadableStreamDefaultControllerError(controller, result.[[Value]]).
                readable_stream_default_controller_error(cx, controller, throw.value());

                // 2. Return result.
                return Err(Exception::Thrown(throw));
            }
            // 3. Let chunkSize be result.[[Value]].
            Ok(chunk_size) => chunk_size,
        };

        // 4. Let enqueueResult be EnqueueValueWithSize(controller, chunk, chunkSize).
        let enqueue_result = controller.queue.enqueue_value_with_size(vm, chunk, chunk_size);

        // 5. If enqueueResult is an abrupt completion,
        if let Err(exception) = enqueue_result {
            let error = exception_value(vm, cx.realm(), exception);

            // 1. Perform ! ReadableStreamDefaultControllerError(controller, enqueueResult.[[Value]]).
            readable_stream_default_controller_error(cx, controller, error);

            // 2. Return enqueueResult.
            return Err(Exception::Thrown(Throw::new(error)));
        }
    }

    // 5. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
    readable_stream_default_controller_call_pull_if_needed(cx, controller);
    Ok(())
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-error
pub(crate) fn readable_stream_default_controller_error(
    cx: &CallContext,
    controller: Ref<ReadableStreamDefaultController>,
    error: Value,
) {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. If stream.[[state]] is not "readable", return.
    if stream.state() != ReadableStreamState::Readable {
        return;
    }

    // 3. Perform ! ResetQueue(controller).
    controller.queue.reset_queue(cx.vm());

    // 4. Perform ! ReadableStreamDefaultControllerClearAlgorithms(controller).
    readable_stream_default_controller_clear_algorithms(controller);

    // 5. Perform ! ReadableStreamError(stream, e).
    readable_stream_error(cx.vm(), stream, error);
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-get-desired-size
pub(crate) fn readable_stream_default_controller_get_desired_size(
    controller: Ref<ReadableStreamDefaultController>,
) -> Option<f64> {
    // 1. Let state be controller.[[stream]].[[state]].
    let state = controller.stream().state();

    // 2. If state is "errored", return null.
    if state == ReadableStreamState::Errored {
        return None;
    }

    // 3. If state is "closed", return 0.
    if state == ReadableStreamState::Closed {
        return Some(0.0);
    }

    // 4. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
    Some(controller.strategy_hwm.get() - controller.queue.total_size())
}

// https://streams.spec.whatwg.org/#rs-default-controller-has-backpressure
pub(crate) fn readable_stream_default_controller_has_backpressure(
    controller: Ref<ReadableStreamDefaultController>,
) -> bool {
    // 1. If ! ReadableStreamDefaultControllerShouldCallPull(controller) is true, return false.
    if readable_stream_default_controller_should_call_pull(controller) {
        return false;
    }

    // 2. Otherwise, return true.
    true
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-can-close-or-enqueue
pub(crate) fn readable_stream_default_controller_can_close_or_enqueue(
    controller: Ref<ReadableStreamDefaultController>,
) -> bool {
    // 1. Let state be controller.[[stream]].[[state]].
    let state = controller.stream().state();

    // 2. If controller.[[closeRequested]] is false and state is "readable", return true.
    if !controller.close_requested() && state == ReadableStreamState::Readable {
        return true;
    }

    // 3. Otherwise, return false.
    false
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller
#[allow(clippy::too_many_arguments, reason = "the abstract operation takes these arguments")]
pub(crate) fn set_up_readable_stream_default_controller(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    controller: Ref<ReadableStreamDefaultController>,
    start_algorithm: StartAlgorithm,
    pull_algorithm: PullAlgorithm,
    cancel_algorithm: CancelAlgorithm,
    high_water_mark: f64,
    size_algorithm: SizeAlgorithm,
) -> WebIdlResult<()> {
    let vm = cx.vm();

    // 1. Assert: stream.[[controller]] is undefined.
    assert!(stream.controller().is_none());

    // 2. Set controller.[[stream]] to stream.
    controller.stream.set(Some(stream));

    // 3. Perform ! ResetQueue(controller).
    controller.queue.reset_queue(vm);

    // 4. Set controller.[[started]], controller.[[closeRequested]], controller.[[pullAgain]], and
    //    controller.[[pulling]] to false.
    controller.started.set(false);
    controller.close_requested.set(false);
    controller.pull_again.set(false);
    controller.pulling.set(false);

    // 5. Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm and controller.[[strategyHWM]] to highWaterMark.
    *controller.strategy_size_algorithm.borrow_mut(vm) = Some(size_algorithm);
    controller.strategy_hwm.set(high_water_mark);

    // 6. Set controller.[[pullAlgorithm]] to pullAlgorithm.
    *controller.pull_algorithm.borrow_mut(vm) = Some(pull_algorithm);

    // 7. Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
    *controller.cancel_algorithm.borrow_mut(vm) = Some(cancel_algorithm);

    // 8. Set stream.[[controller]] to controller.
    stream.set_controller(Some(ReadableStreamController::ReadableStreamDefaultController(
        controller,
    )));

    // 9. Let startResult be the result of performing startAlgorithm. (This might throw an exception.)
    let start_result = start_algorithm.run()?;

    // 10. Let startPromise be a promise resolved with startResult.
    let start_promise = create_resolved_promise(vm, cx.realm(), start_result);

    react_to_promise(
        vm,
        start_promise,
        controller,
        // 11. Upon fulfillment of startPromise,
        Some(|cx, controller, _| {
            // 1. Set controller.[[started]] to true.
            controller.started.set(true);

            // 2. Assert: controller.[[pulling]] is false.
            assert!(!controller.pulling.get());

            // 3. Assert: controller.[[pullAgain]] is false.
            assert!(!controller.pull_again.get());

            // 4. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
            readable_stream_default_controller_call_pull_if_needed(cx, controller);
            Ok(Value::UNDEFINED)
        }),
        // 12. Upon rejection of startPromise with reason r,
        Some(|cx, controller, reason| {
            // 1. Perform ! ReadableStreamDefaultControllerError(controller, r).
            readable_stream_default_controller_error(cx, controller, reason);
            Ok(Value::UNDEFINED)
        }),
    );
    Ok(())
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller-from-underlying-source
pub(crate) fn set_up_readable_stream_default_controller_from_underlying_source(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    underlying_source: Value,
    underlying_source_dict: UnderlyingSource,
    high_water_mark: f64,
    size_algorithm: SizeAlgorithm,
) -> WebIdlResult<()> {
    // 1. Let controller be a new ReadableStreamDefaultController.
    let controller = allocate(cx.vm(), ReadableStreamDefaultController::new());

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    let mut start_algorithm = StartAlgorithm::ReturnsUndefined;

    // 3. Let pullAlgorithm be an algorithm that returns a promise resolved with undefined.
    let mut pull_algorithm = PullAlgorithm::ReturnsResolvedUndefined;

    // 4. Let cancelAlgorithm be an algorithm that returns a promise resolved with undefined.
    let mut cancel_algorithm = CancelAlgorithm::ReturnsResolvedUndefined;

    // 5. If underlyingSourceDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSourceDict["start"] with argument list « controller » and callback this value
    //    underlyingSource.
    if let Some(start) = underlying_source_dict.start {
        start_algorithm = StartAlgorithm::Callback {
            start,
            this_value: underlying_source,
            controller: ReadableStreamController::ReadableStreamDefaultController(controller).to_js(cx),
        };
    }

    // 6. If underlyingSourceDict["pull"] exists, then set pullAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSourceDict["pull"] with argument list « controller » and callback this value
    //    underlyingSource.
    if let Some(pull) = underlying_source_dict.pull {
        pull_algorithm = PullAlgorithm::Callback {
            pull,
            underlying_source,
            controller: ReadableStreamController::ReadableStreamDefaultController(controller),
        };
    }

    // 7. If underlyingSourceDict["cancel"] exists, then set cancelAlgorithm to an algorithm which takes an argument
    //    reason and returns the result of invoking underlyingSourceDict["cancel"] with argument list « reason » and
    //    callback this value underlyingSource.
    if let Some(cancel) = underlying_source_dict.cancel {
        cancel_algorithm = CancelAlgorithm::Callback {
            cancel,
            this_value: underlying_source,
        };
    }

    // 8. Perform ? SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm,
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
}
