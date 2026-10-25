/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableByteStreamController, https://streams.spec.whatwg.org/#rbs-controller-class, and its abstract operations,
//! https://streams.spec.whatwg.org/#rbs-controller-abstract-ops

use std::collections::VecDeque;

use core::cell::Cell;

use libjs_rust::api::{
    ArrayBuffer, ArrayBufferShared, DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType,
    FunctionObject, Gc, GcRefCell, MarkedVec, Must, Throw, Trace, TypedArrayBase, Value, Vm, clone_array_buffer,
    construct,
};

use crate::bindings::conversions::{ArrayBufferView, BufferSource};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, create_resolved_promise, exception_value, react_to_promise};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::streams::abstract_operations::{
    can_copy_data_block_bytes_buffer, can_transfer_array_buffer, transfer_array_buffer,
};
use crate::streams::algorithms::{CancelAlgorithm, PullAlgorithm, StartAlgorithm};
use crate::streams::read_request::{ReadIntoRequest, ReadRequest};
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamReader, ReadableStreamState, complete_rendering_pending_stream,
    is_readable_stream_locked, readable_stream_add_read_into_request, readable_stream_add_read_request,
    readable_stream_close, readable_stream_error, readable_stream_fulfill_read_into_request,
    readable_stream_fulfill_read_request, readable_stream_get_num_read_into_requests,
    readable_stream_get_num_read_requests, readable_stream_has_byob_reader, readable_stream_has_default_reader,
};
use crate::streams::readable_stream_byob_request::ReadableStreamBYOBRequest;
use crate::streams::underlying_source::{ReadableStreamController, UnderlyingSource};

// https://streams.spec.whatwg.org/#pull-into-descriptor-reader-type
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ReaderType {
    Default,
    Byob,
    None,
}

/// A pull-into descriptor, https://streams.spec.whatwg.org/#pull-into-descriptor, which a controller's
/// [[pendingPullIntos]] and the steps that fill it share.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PullIntoDescriptor {
    // https://streams.spec.whatwg.org/#pull-into-descriptor-buffer
    pub(crate) buffer: Cell<Gc<ArrayBuffer>>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-buffer-byte-length
    #[gc(untraced)]
    pub(crate) buffer_byte_length: Cell<u64>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-byte-offset
    #[gc(untraced)]
    pub(crate) byte_offset: Cell<u64>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-byte-length
    #[gc(untraced)]
    pub(crate) byte_length: Cell<u64>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-bytes-filled
    #[gc(untraced)]
    pub(crate) bytes_filled: Cell<u64>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-minimum-fill
    #[gc(untraced)]
    pub(crate) minimum_fill: Cell<u64>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-element-size
    #[gc(untraced)]
    pub(crate) element_size: Cell<u64>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-view-constructor
    pub(crate) view_constructor: Gc<FunctionObject>,
    // https://streams.spec.whatwg.org/#pull-into-descriptor-reader-type
    #[gc(untraced)]
    pub(crate) reader_type: Cell<ReaderType>,
}

static PULL_INTO_DESCRIPTOR_CLASS: EmbedderClass<PullIntoDescriptor> = EmbedderClass::new("PullIntoDescriptor");

impl EmbedderValue for PullIntoDescriptor {
    const CLASS: &'static EmbedderClass<Self> = &PULL_INTO_DESCRIPTOR_CLASS;
}

pub(crate) type PullIntoDescriptorRef = Gc<EmbedderCell<PullIntoDescriptor>>;

/// A readable byte stream queue entry, https://streams.spec.whatwg.org/#readable-byte-stream-queue-entry
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableByteStreamQueueEntry {
    // https://streams.spec.whatwg.org/#readable-byte-stream-queue-entry-buffer
    pub(crate) buffer: Gc<ArrayBuffer>,
    // https://streams.spec.whatwg.org/#readable-byte-stream-queue-entry-byte-offset
    #[gc(untraced)]
    pub(crate) byte_offset: u64,
    // https://streams.spec.whatwg.org/#readable-byte-stream-queue-entry-byte-length
    #[gc(untraced)]
    pub(crate) byte_length: u64,
}

// https://streams.spec.whatwg.org/#readablebytestreamcontroller
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableByteStreamController {
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-autoallocatechunksize
    #[gc(untraced)]
    auto_allocate_chunk_size: Cell<Option<u64>>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-byobrequest
    byob_request: Cell<Option<Ref<ReadableStreamBYOBRequest>>>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-cancelalgorithm
    cancel_algorithm: GcRefCell<Option<CancelAlgorithm>>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-closerequested
    close_requested: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-pullagain
    pull_again: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-pullalgorithm
    pull_algorithm: GcRefCell<Option<PullAlgorithm>>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-pulling
    pulling: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-pendingpullintos
    pending_pull_intos: GcRefCell<VecDeque<PullIntoDescriptorRef>>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-queue
    queue: GcRefCell<VecDeque<ReadableByteStreamQueueEntry>>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-queuetotalsize
    #[gc(untraced)]
    queue_total_size: Cell<f64>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-started
    started: Cell<bool>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-strategyhwm
    #[gc(untraced)]
    strategy_hwm: Cell<f64>,
    // https://streams.spec.whatwg.org/#readablebytestreamcontroller-stream
    stream: Cell<Option<Ref<ReadableStream>>>,
}

impl Wrappable for ReadableByteStreamController {}

impl ReadableByteStreamController {
    pub(crate) fn new(_: &DeferGc<'_>) -> Self {
        Self {
            auto_allocate_chunk_size: Cell::new(None),
            byob_request: Cell::new(None),
            cancel_algorithm: GcRefCell::new(None),
            close_requested: Cell::new(false),
            pull_again: Cell::new(false),
            pull_algorithm: GcRefCell::new(None),
            pulling: Cell::new(false),
            pending_pull_intos: GcRefCell::new(VecDeque::new()),
            queue: GcRefCell::new(VecDeque::new()),
            queue_total_size: Cell::new(0.0),
            started: Cell::new(false),
            strategy_hwm: Cell::new(0.0),
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

    pub(crate) fn pending_pull_intos_is_empty(&self) -> bool {
        self.pending_pull_intos.borrow().is_empty()
    }

    /// The controller's [[pendingPullIntos]][0], if [[pendingPullIntos]] is not empty.
    fn first_pending_pull_into(&self) -> Option<PullIntoDescriptorRef> {
        self.pending_pull_intos.borrow().front().copied()
    }

    // https://streams.spec.whatwg.org/#reset-queue
    fn reset_queue(&self, vm: &Vm) {
        // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.

        // 2. Set container.[[queue]] to a new empty list.
        self.queue.borrow_mut(vm).clear();

        // 3. Set container.[[queueTotalSize]] to 0.
        self.queue_total_size.set(0.0);
    }

    // https://streams.spec.whatwg.org/#rbs-controller-byob-request
    pub(crate) fn byob_request(
        this: &Ref<Self>,
        cx: &mut CallContext,
    ) -> WebIdlResult<Option<Ref<ReadableStreamBYOBRequest>>> {
        // 1. Return ! ReadableByteStreamControllerGetBYOBRequest(this).
        Ok(readable_byte_stream_controller_get_byob_request(cx, *this))
    }

    // https://streams.spec.whatwg.org/#rbs-controller-desired-size
    pub(crate) fn desired_size(this: &Ref<Self>, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        // 1. Return ! ReadableByteStreamControllerGetDesiredSize(this).
        Ok(readable_byte_stream_controller_get_desired_size(*this))
    }

    // https://streams.spec.whatwg.org/#rbs-controller-close
    pub(crate) fn close(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. If this.[[closeRequested]] is true, throw a TypeError exception.
        if this.close_requested() {
            return Err(Exception::type_error("Controller is already closed"));
        }

        // 2. If this.[[stream]].[[state]] is not "readable", throw a TypeError exception.
        let stream = this.stream();
        if stream.state() != ReadableStreamState::Readable {
            let message = if stream.state() == ReadableStreamState::Closed {
                "Cannot close a closed stream"
            } else {
                "Cannot close an errored stream"
            };
            return Err(Exception::type_error(message));
        }

        // 3. Perform ? ReadableByteStreamControllerClose(this).
        readable_byte_stream_controller_close(cx, *this)
    }

    // https://streams.spec.whatwg.org/#rbs-controller-error
    pub(crate) fn error(this: &Ref<Self>, cx: &mut CallContext, e: Option<Value>) -> WebIdlResult<()> {
        // 1. Perform ! ReadableByteStreamControllerError(this, e).
        readable_byte_stream_controller_error(cx, *this, e.unwrap_or(Value::UNDEFINED));
        Ok(())
    }

    // https://streams.spec.whatwg.org/#rbs-controller-enqueue
    pub(crate) fn enqueue(this: &Ref<Self>, cx: &mut CallContext, chunk: ArrayBufferView) -> WebIdlResult<()> {
        let chunk_extent = BufferSource::from(chunk).extent();

        // 1. If chunk.[[ByteLength]] is 0, throw a TypeError exception.
        // 2. If chunk.[[ViewedArrayBuffer]].[[ByteLength]] is 0, throw a TypeError exception.
        if chunk_extent.byte_length == 0 || chunk_extent.buffer.byte_length() == 0 {
            return Err(Exception::type_error("Cannot enqueue chunk with byte length of zero"));
        }

        // 3. If this.[[closeRequested]] is true, throw a TypeError exception.
        if this.close_requested() {
            return Err(Exception::type_error("Close is requested for controller"));
        }

        // 4. If this.[[stream]].[[state]] is not "readable", throw a TypeError exception.
        if !this.stream().is_readable() {
            return Err(Exception::type_error("Stream is not readable"));
        }

        // 5. Return ? ReadableByteStreamControllerEnqueue(this, chunk).
        readable_byte_stream_controller_enqueue(cx, *this, chunk)
    }

    // https://streams.spec.whatwg.org/#rbs-controller-private-cancel
    pub(crate) fn cancel_steps(this: Ref<Self>, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Perform ! ReadableByteStreamControllerClearPendingPullIntos(this).
        readable_byte_stream_controller_clear_pending_pull_intos(cx.vm(), this);

        // 2. Perform ! ResetQueue(this).
        this.reset_queue(cx.vm());

        // 3. Let result be the result of performing this.[[cancelAlgorithm]], passing in reason.
        let cancel_algorithm = this
            .cancel_algorithm
            .borrow()
            .clone()
            .expect("a controller that is canceled has a cancel algorithm");
        let result = cancel_algorithm.run(&this.stream().context(cx.vm()), reason);

        // 4. Perform ! ReadableByteStreamControllerClearAlgorithms(this).
        readable_byte_stream_controller_clear_algorithms(this);

        // 5. Return result.
        result
    }

    // https://streams.spec.whatwg.org/#rbs-controller-private-pull
    pub(crate) fn pull_steps(this: Ref<Self>, cx: &CallContext, read_request: ReadRequest) {
        let vm = cx.vm();

        // 1. Let stream be this.[[stream]].
        let stream = this.stream();

        // 2. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
        assert!(readable_stream_has_default_reader(stream));

        // 3. If this.[[queueTotalSize]] > 0,
        if this.queue_total_size.get() > 0.0 {
            // 1. Assert: ! ReadableStreamGetNumReadRequests(stream) is 0.
            assert!(readable_stream_get_num_read_requests(stream) == 0);

            // 2. Perform ! ReadableByteStreamControllerFillReadRequestFromQueue(this, readRequest).
            readable_byte_stream_controller_fill_read_request_from_queue(cx, this, read_request);

            // 3. Return.
            return;
        }

        // 4. Let autoAllocateChunkSize be this.[[autoAllocateChunkSize]].
        let auto_allocate_chunk_size = this.auto_allocate_chunk_size.get();

        // 5. If autoAllocateChunkSize is not undefined,
        if let Some(auto_allocate_chunk_size) = auto_allocate_chunk_size {
            // 1. Let buffer be Construct(%ArrayBuffer%, « autoAllocateChunkSize »).
            let buffer = ArrayBuffer::create(vm, cx.realm(), auto_allocate_chunk_size as usize, ArrayBufferShared::No);

            // 2. If buffer is an abrupt completion,
            let buffer = match buffer {
                Ok(buffer) => buffer,
                Err(throw) => {
                    // 1. Perform readRequest’s error steps, given buffer.[[Value]].
                    read_request.error_steps(vm, throw.value());

                    // 2. Return.
                    return;
                }
            };

            // 3. Let pullIntoDescriptor be a new pull-into descriptor with
            //
            //    buffer                   buffer.[[Value]]
            //    buffer byte length       autoAllocateChunkSize
            //    byte offset              0
            //    byte length              autoAllocateChunkSize
            //    bytes filled             0
            //    minimum fill             1
            //    element size             1
            //    view constructor         %Uint8Array%
            //    reader type              "default"
            let pull_into_descriptor = PULL_INTO_DESCRIPTOR_CLASS.allocate(vm, |_| PullIntoDescriptor {
                buffer: Cell::new(buffer),
                buffer_byte_length: Cell::new(auto_allocate_chunk_size),
                byte_offset: Cell::new(0),
                byte_length: Cell::new(auto_allocate_chunk_size),
                bytes_filled: Cell::new(0),
                minimum_fill: Cell::new(1),
                element_size: Cell::new(1),
                view_constructor: cx.realm().intrinsics().uint8_array_constructor(vm),
                reader_type: Cell::new(ReaderType::Default),
            });

            // 4. Append pullIntoDescriptor to this.[[pendingPullIntos]].
            this.pending_pull_intos.borrow_mut(vm).push_back(pull_into_descriptor);
        }

        // 6. Perform ! ReadableStreamAddReadRequest(stream, readRequest).
        readable_stream_add_read_request(vm, stream, read_request);

        // 7. Perform ! ReadableByteStreamControllerCallPullIfNeeded(this).
        readable_byte_stream_controller_call_pull_if_needed(cx, this);
    }

    // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontroller-releasesteps
    pub(crate) fn release_steps(vm: &Vm, this: Ref<Self>) {
        // 1. If this.[[pendingPullIntos]] is not empty,
        if let Some(first_pending_pull_into) = this.first_pending_pull_into() {
            // 1. Let firstPendingPullInto be this.[[pendingPullIntos]][0].

            // 2. Set firstPendingPullInto’s reader type to "none".
            first_pending_pull_into.reader_type.set(ReaderType::None);

            // 3. Set this.[[pendingPullIntos]] to the list « firstPendingPullInto ».
            let mut pending_pull_intos = this.pending_pull_intos.borrow_mut(vm);
            pending_pull_intos.clear();
            pending_pull_intos.push_back(first_pending_pull_into);
        }
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-call-pull-if-needed
pub(crate) fn readable_byte_stream_controller_call_pull_if_needed(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
) {
    // 1. Let shouldPull be ! ReadableByteStreamControllerShouldCallPull(controller).
    let should_pull = readable_byte_stream_controller_should_call_pull(controller);

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
        .pull_algorithm
        .borrow()
        .clone()
        .expect("a controller that should pull has a pull algorithm");
    let pull_promise = pull_algorithm.run(&controller.stream().context(cx.vm()));

    react_to_promise(
        cx.vm(),
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

                // 2. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
                readable_byte_stream_controller_call_pull_if_needed(cx, controller);
            }

            Ok(Value::UNDEFINED)
        }),
        // 8. Upon rejection of pullPromise with reason e,
        Some(|cx, controller, error| {
            // 1. Perform ! ReadableByteStreamControllerError(controller, e).
            readable_byte_stream_controller_error(cx, controller, error);

            Ok(Value::UNDEFINED)
        }),
    );
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-algorithms
pub(crate) fn readable_byte_stream_controller_clear_algorithms(controller: Ref<ReadableByteStreamController>) {
    // 1. Set controller.[[pullAlgorithm]] to undefined.
    controller.pull_algorithm.replace(None);

    // 2. Set controller.[[cancelAlgorithm]] to undefined.
    controller.cancel_algorithm.replace(None);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-pending-pull-intos
pub(crate) fn readable_byte_stream_controller_clear_pending_pull_intos(
    vm: &Vm,
    controller: Ref<ReadableByteStreamController>,
) {
    // 1. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
    readable_byte_stream_controller_invalidate_byob_request(controller);

    // 2. Set controller.[[pendingPullIntos]] to a new empty list.
    controller.pending_pull_intos.borrow_mut(vm).clear();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-close
pub(crate) fn readable_byte_stream_controller_close(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
) -> WebIdlResult<()> {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. If controller.[[closeRequested]] is true or stream.[[state]] is not "readable", return.
    if controller.close_requested() || stream.state() != ReadableStreamState::Readable {
        return Ok(());
    }

    complete_rendering_pending_stream(stream);

    // 3. If controller.[[queueTotalSize]] > 0,
    if controller.queue_total_size.get() > 0.0 {
        // 1. Set controller.[[closeRequested]] to true.
        controller.close_requested.set(true);

        // 2. Return.
        return Ok(());
    }

    // 4. If controller.[[pendingPullIntos]] is not empty,
    if let Some(first_pending_pull_into) = controller.first_pending_pull_into() {
        // 1. Let firstPendingPullInto be controller.[[pendingPullIntos]][0].

        // 2. If the remainder after dividing firstPendingPullInto’s bytes filled by firstPendingPullInto’s element size
        //    is not 0,
        if first_pending_pull_into.bytes_filled.get() % first_pending_pull_into.element_size.get() != 0 {
            // 1. Let e be a new TypeError exception.
            let error = exception_value(
                cx.vm(),
                cx.realm(),
                Exception::type_error("Cannot close controller in the middle of processing a write request"),
            );

            // 2. Perform ! ReadableByteStreamControllerError(controller, e).
            readable_byte_stream_controller_error(cx, controller, error);

            // 3. Throw e.
            return Err(Exception::Thrown(Throw::new(error)));
        }
    }

    // 5. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
    readable_byte_stream_controller_clear_algorithms(controller);

    // 6. Perform ! ReadableStreamClose(stream).
    readable_stream_close(cx.vm(), stream);
    Ok(())
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-commit-pull-into-descriptor
pub(crate) fn readable_byte_stream_controller_commit_pull_into_descriptor(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    pull_into_descriptor: PullIntoDescriptorRef,
) {
    // 1. Assert: stream.[[state]] is not "errored".
    assert!(stream.state() != ReadableStreamState::Errored);

    // 2. Assert: pullIntoDescriptor.reader type is not "none".
    assert!(pull_into_descriptor.reader_type.get() != ReaderType::None);

    // 3. Let done be false.
    let mut done = false;

    // 4. If stream.[[state]] is "closed",
    if stream.state() == ReadableStreamState::Closed {
        // 1. Assert: the remainder after dividing pullIntoDescriptor’s bytes filled by pullIntoDescriptor’s element
        //    size is 0.
        assert!(
            pull_into_descriptor
                .bytes_filled
                .get()
                .is_multiple_of(pull_into_descriptor.element_size.get())
        );

        // 2. Set done to true.
        done = true;
    }

    // 5. Let filledView be ! ReadableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor).
    let filled_view = readable_byte_stream_controller_convert_pull_into_descriptor(cx, pull_into_descriptor);

    // 6. If pullIntoDescriptor’s reader type is "default",
    if pull_into_descriptor.reader_type.get() == ReaderType::Default {
        // 1. Perform ! ReadableStreamFulfillReadRequest(stream, filledView, done).
        readable_stream_fulfill_read_request(cx.vm(), stream, filled_view, done);
    }
    // 7. Otherwise,
    else {
        // 1. Assert: pullIntoDescriptor’s reader type is "byob".
        assert!(pull_into_descriptor.reader_type.get() == ReaderType::Byob);

        // 2. Perform ! ReadableStreamFulfillReadIntoRequest(stream, filledView, done).
        readable_stream_fulfill_read_into_request(cx.vm(), stream, filled_view, done);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-convert-pull-into-descriptor
pub(crate) fn readable_byte_stream_controller_convert_pull_into_descriptor(
    cx: &CallContext,
    pull_into_descriptor: PullIntoDescriptorRef,
) -> Value {
    let vm = cx.vm();

    // 1. Let bytesFilled be pullIntoDescriptor’s bytes filled.
    let bytes_filled = pull_into_descriptor.bytes_filled.get();

    // 2. Let elementSize be pullIntoDescriptor’s element size.
    let element_size = pull_into_descriptor.element_size.get();

    // 3. Assert: bytesFilled ≤ pullIntoDescriptor’s byte length.
    assert!(bytes_filled <= pull_into_descriptor.byte_length.get());

    // 4. Assert: the remainder after dividing bytesFilled by elementSize is 0.
    assert!(bytes_filled.is_multiple_of(element_size));

    // 5. Let buffer be ! TransferArrayBuffer(pullIntoDescriptor’s buffer).
    let buffer = transfer_array_buffer(cx, pull_into_descriptor.buffer.get()).must();

    // 6. Return ! Construct(pullIntoDescriptor’s view constructor, « buffer, pullIntoDescriptor’s byte offset,
    //    bytesFilled ÷ elementSize »).
    let view = construct(
        vm,
        pull_into_descriptor.view_constructor,
        &[
            Value::from_object(buffer),
            Value::from_f64(pull_into_descriptor.byte_offset.get() as f64),
            Value::from_f64((bytes_filled / element_size) as f64),
        ],
        None,
    )
    .must();
    Value::from_object(view)
}

fn readable_byte_stream_controller_enqueue_transferred_buffer(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    transferred_buffer: Gc<ArrayBuffer>,
    byte_offset: u64,
    byte_length: u64,
) -> WebIdlResult<()> {
    let vm = cx.vm();
    let stream = controller.stream();

    // 8. If controller.[[pendingPullIntos]] is not empty,
    if let Some(first_pending_pull_into) = controller.first_pending_pull_into() {
        // 1. Let firstPendingPullInto be controller.[[pendingPullIntos]][0].

        // 2. If ! IsDetachedBuffer(firstPendingPullInto’s buffer) is true, throw a TypeError exception.
        if first_pending_pull_into.buffer.get().is_detached() {
            return Err(Exception::type_error("Buffer is detached"));
        }

        // 3. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
        readable_byte_stream_controller_invalidate_byob_request(controller);

        // 4. Set firstPendingPullInto’s buffer to ! TransferArrayBuffer(firstPendingPullInto’s buffer).
        first_pending_pull_into
            .buffer
            .set(transfer_array_buffer(cx, first_pending_pull_into.buffer.get()).must());

        // 5. If firstPendingPullInto’s reader type is "none", perform
        //    ? ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, firstPendingPullInto).
        if first_pending_pull_into.reader_type.get() == ReaderType::None {
            readable_byte_stream_controller_enqueue_detached_pull_into_to_queue(
                cx,
                controller,
                first_pending_pull_into,
            )?;
        }
    }

    // 9. If ! ReadableStreamHasDefaultReader(stream) is true,
    if readable_stream_has_default_reader(stream) {
        // 1. Perform ! ReadableByteStreamControllerProcessReadRequestsUsingQueue(controller).
        readable_byte_stream_controller_process_read_requests_using_queue(cx, controller);

        // 2. If ! ReadableStreamGetNumReadRequests(stream) is 0,
        if readable_stream_get_num_read_requests(stream) == 0 {
            // 1. Assert: controller.[[pendingPullIntos]] is empty.
            assert!(controller.pending_pull_intos_is_empty());

            // 2. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset,
            //    byteLength).
            readable_byte_stream_controller_enqueue_chunk_to_queue(
                vm,
                controller,
                transferred_buffer,
                byte_offset,
                byte_length,
            );
        }
        // 3. Otherwise,
        else {
            // 1. Assert: controller.[[queue]] is empty.
            assert!(controller.queue.borrow().is_empty());

            // 2. If controller.[[pendingPullIntos]] is not empty,
            if let Some(first_pending_pull_into) = controller.first_pending_pull_into() {
                // 1. Assert: controller.[[pendingPullIntos]][0]'s reader type is "default".
                assert!(first_pending_pull_into.reader_type.get() == ReaderType::Default);

                // 2. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
                readable_byte_stream_controller_shift_pending_pull_into(vm, controller);
            }

            // 3. Let transferredView be ! Construct(%Uint8Array%, « transferredBuffer, byteOffset, byteLength »).
            let transferred_view = construct(
                vm,
                cx.realm().intrinsics().uint8_array_constructor(vm),
                &[
                    Value::from_object(transferred_buffer),
                    Value::from_f64(byte_offset as f64),
                    Value::from_f64(byte_length as f64),
                ],
                None,
            )
            .must();

            // 4. Perform ! ReadableStreamFulfillReadRequest(stream, transferredView, false).
            readable_stream_fulfill_read_request(vm, stream, Value::from_object(transferred_view), false);
        }
    }
    // 10. Otherwise, if ! ReadableStreamHasBYOBReader(stream) is true,
    else if readable_stream_has_byob_reader(stream) {
        // 1. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset,
        //    byteLength).
        readable_byte_stream_controller_enqueue_chunk_to_queue(
            vm,
            controller,
            transferred_buffer,
            byte_offset,
            byte_length,
        );

        // 2. Let filledPullIntos be the result of performing
        //    ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
        let filled_pull_intos =
            readable_byte_stream_controller_process_pull_into_descriptors_using_queue(vm, controller);

        // 3. For each filledPullInto of filledPullIntos,
        for index in 0..filled_pull_intos.len() {
            // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(stream, filledPullInto).
            let filled_pull_into = filled_pull_intos.get(index).expect("the index is in bounds");
            readable_byte_stream_controller_commit_pull_into_descriptor(cx, stream, filled_pull_into);
        }
    }
    // 11. Otherwise,
    else {
        // 1. Assert: ! IsReadableStreamLocked(stream) is false.
        assert!(!is_readable_stream_locked(stream));

        // 2. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset,
        //    byteLength).
        readable_byte_stream_controller_enqueue_chunk_to_queue(
            vm,
            controller,
            transferred_buffer,
            byte_offset,
            byte_length,
        );
    }

    // 12. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
    readable_byte_stream_controller_call_pull_if_needed(cx, controller);
    Ok(())
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
pub(crate) fn readable_byte_stream_controller_enqueue(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    chunk: ArrayBufferView,
) -> WebIdlResult<()> {
    let vm = cx.vm();

    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. If controller.[[closeRequested]] is true or stream.[[state]] is not "readable", return.
    if controller.close_requested() || stream.state() != ReadableStreamState::Readable {
        return Ok(());
    }

    // 3. Let buffer be chunk.[[ViewedArrayBuffer]].
    // 4. Let byteOffset be chunk.[[ByteOffset]].
    let chunk_extent = BufferSource::from(chunk).extent();
    let buffer = chunk_extent.buffer;
    let byte_offset = chunk_extent.byte_offset as u64;

    // 6. If ! IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    // FIXME: The streams spec has not been updated for resizable ArrayBuffer objects. We must perform step 6 before
    //        invoking TypedArrayByteLength in step 5. We also must check if the array is out-of-bounds, rather than
    //        just detached.
    if chunk_extent.is_out_of_bounds {
        let name = match chunk {
            ArrayBufferView::TypedArray(_) => "TypedArray",
            ArrayBufferView::DataView(_) => "DataView",
        };
        return Ok(vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&name])?);
    }

    // 5. Let byteLength be chunk.[[ByteLength]].
    let byte_length = chunk_extent.byte_length as u64;

    // 7. Let transferredBuffer be ? TransferArrayBuffer(buffer).
    let transferred_buffer = transfer_array_buffer(cx, buffer)?;

    readable_byte_stream_controller_enqueue_transferred_buffer(
        cx,
        controller,
        transferred_buffer,
        byte_offset,
        byte_length,
    )
}

/// ReadableByteStreamControllerEnqueue with a chunk of the bytes, for C++ that enqueues bytes it received,
/// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
pub(crate) fn readable_byte_stream_controller_enqueue_native_bytes(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    bytes: &[u8],
) -> WebIdlResult<()> {
    let stream = controller.stream();
    if controller.close_requested() || stream.state() != ReadableStreamState::Readable {
        return Ok(());
    }

    let byte_length = bytes.len() as u64;

    // OPTIMIZATION: Native byte producers have no observable chunk object to detach, so enter the enqueue algorithm
    //               after the TransferArrayBuffer step with an already-owned ArrayBuffer.
    let transferred_buffer = ArrayBuffer::create_from_bytes(cx.vm(), cx.realm(), bytes, ArrayBufferShared::No);
    readable_byte_stream_controller_enqueue_transferred_buffer(cx, controller, transferred_buffer, 0, byte_length)
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue-chunk-to-queue
pub(crate) fn readable_byte_stream_controller_enqueue_chunk_to_queue(
    vm: &Vm,
    controller: Ref<ReadableByteStreamController>,
    buffer: Gc<ArrayBuffer>,
    byte_offset: u64,
    byte_length: u64,
) {
    // 1. Append a new readable byte stream queue entry with buffer buffer, byte offset byteOffset, and byte length
    //    byteLength to controller.[[queue]].
    controller.queue.borrow_mut(vm).push_back(ReadableByteStreamQueueEntry {
        buffer,
        byte_offset,
        byte_length,
    });

    // 2. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] + byteLength.
    controller
        .queue_total_size
        .set(controller.queue_total_size.get() + byte_length as f64);
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueueclonedchunktoqueue
pub(crate) fn readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    buffer: Gc<ArrayBuffer>,
    byte_offset: u64,
    byte_length: u64,
) -> WebIdlResult<()> {
    // 1. Let cloneResult be CloneArrayBuffer(buffer, byteOffset, byteLength, %ArrayBuffer%).
    let clone_result = clone_array_buffer(cx.vm(), buffer, byte_offset as usize, byte_length as usize);

    // 2. If cloneResult is an abrupt completion,
    let clone_result = match clone_result {
        Ok(clone_result) => clone_result,
        Err(throw_completion) => {
            // 1. Perform ! ReadableByteStreamControllerError(controller, cloneResult.[[Value]]).
            readable_byte_stream_controller_error(cx, controller, throw_completion.value());

            // 2. Return cloneResult.
            return Err(throw_completion.into());
        }
    };

    // 3. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, cloneResult.[[Value]], 0, byteLength).
    readable_byte_stream_controller_enqueue_chunk_to_queue(cx.vm(), controller, clone_result, 0, byte_length);
    Ok(())
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueuedetachedpullintotoqueue
pub(crate) fn readable_byte_stream_controller_enqueue_detached_pull_into_to_queue(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    pull_into_descriptor: PullIntoDescriptorRef,
) -> WebIdlResult<()> {
    // 1. Assert: pullIntoDescriptor’s reader type is "none".
    assert!(pull_into_descriptor.reader_type.get() == ReaderType::None);

    // 2. If pullIntoDescriptor’s bytes filled > 0, perform
    //    ? ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller, pullIntoDescriptor’s buffer,
    //    pullIntoDescriptor’s byte offset, pullIntoDescriptor’s bytes filled).
    if pull_into_descriptor.bytes_filled.get() > 0 {
        readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(
            cx,
            controller,
            pull_into_descriptor.buffer.get(),
            pull_into_descriptor.byte_offset.get(),
            pull_into_descriptor.bytes_filled.get(),
        )?;
    }

    // 3. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
    readable_byte_stream_controller_shift_pending_pull_into(cx.vm(), controller);
    Ok(())
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-error
pub(crate) fn readable_byte_stream_controller_error(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    error: Value,
) {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. If stream.[[state]] is not "readable", return.
    if stream.state() != ReadableStreamState::Readable {
        return;
    }

    // 3. Perform ! ReadableByteStreamControllerClearPendingPullIntos(controller).
    readable_byte_stream_controller_clear_pending_pull_intos(cx.vm(), controller);

    // 4. Perform ! ResetQueue(controller).
    controller.reset_queue(cx.vm());

    // 5. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
    readable_byte_stream_controller_clear_algorithms(controller);

    // 6. Perform ! ReadableStreamError(stream, e).
    readable_stream_error(cx.vm(), stream, error);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-head-pull-into-descriptor
pub(crate) fn readable_byte_stream_controller_fill_head_pull_into_descriptor(
    controller: Ref<ReadableByteStreamController>,
    size: u64,
    pull_into_descriptor: PullIntoDescriptorRef,
) {
    // 1. Assert: either controller.[[pendingPullIntos]] is empty, or controller.[[pendingPullIntos]][0] is
    //    pullIntoDescriptor.
    assert!(
        controller
            .first_pending_pull_into()
            .is_none_or(|first_pending_pull_into| first_pending_pull_into == pull_into_descriptor)
    );

    // 2. Assert: controller.[[byobRequest]] is null.
    assert!(controller.byob_request.get().is_none());

    // 3. Set pullIntoDescriptor’s bytes filled to bytes filled + size.
    pull_into_descriptor
        .bytes_filled
        .set(pull_into_descriptor.bytes_filled.get() + size);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-pull-into-descriptor-from-queue
pub(crate) fn readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(
    vm: &Vm,
    controller: Ref<ReadableByteStreamController>,
    pull_into_descriptor: PullIntoDescriptorRef,
) -> bool {
    // 1. Let maxBytesToCopy be min(controller.[[queueTotalSize]], pullIntoDescriptor’s byte length −
    //    pullIntoDescriptor’s bytes filled).
    let max_bytes_to_copy = (controller.queue_total_size.get() as u64)
        .min(pull_into_descriptor.byte_length.get() - pull_into_descriptor.bytes_filled.get());

    // 2. Let maxBytesFilled be pullIntoDescriptor’s bytes filled + maxBytesToCopy.
    let max_bytes_filled = pull_into_descriptor.bytes_filled.get() + max_bytes_to_copy;

    // 3. Let totalBytesToCopyRemaining be maxBytesToCopy.
    let mut total_bytes_to_copy_remaining = max_bytes_to_copy;

    // 4. Let ready be false.
    let mut ready = false;

    // 5. Assert: ! IsDetachedBuffer(pullIntoDescriptor’s buffer) is false.
    assert!(!pull_into_descriptor.buffer.get().is_detached());

    // 6. Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s minimum fill.
    assert!(pull_into_descriptor.bytes_filled.get() < pull_into_descriptor.minimum_fill.get());

    // 7. Let remainderBytes be the remainder after dividing maxBytesFilled by pullIntoDescriptor’s element size.
    let remainder_bytes = max_bytes_filled % pull_into_descriptor.element_size.get();

    // 8. Let maxAlignedBytes be maxBytesFilled − remainderBytes.
    let max_aligned_bytes = max_bytes_filled - remainder_bytes;

    // 9. If maxAlignedBytes ≥ pullIntoDescriptor’s minimum fill,
    if max_aligned_bytes >= pull_into_descriptor.minimum_fill.get() {
        // 1. Set totalBytesToCopyRemaining to maxAlignedBytes − pullIntoDescriptor’s bytes filled.
        total_bytes_to_copy_remaining = max_aligned_bytes - pull_into_descriptor.bytes_filled.get();

        // 2. Set ready to true.
        ready = true;

        // NOTE: A descriptor for a read() request that is not yet filled up to its minimum length will stay at the head
        //       of the queue, so the underlying source can keep filling it.
    }

    // 10. Let queue be controller.[[queue]].
    let queue = &controller.queue;

    // 11. While totalBytesToCopyRemaining > 0,
    while total_bytes_to_copy_remaining > 0 {
        // 1. Let headOfQueue be queue[0].
        let head_of_queue = queue
            .borrow()
            .front()
            .cloned()
            .expect("the queue holds the bytes to copy");

        // 2. Let bytesToCopy be min(totalBytesToCopyRemaining, headOfQueue’s byte length).
        let bytes_to_copy = total_bytes_to_copy_remaining.min(head_of_queue.byte_length);

        // 3. Let destStart be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s bytes filled.
        let dest_start = pull_into_descriptor.byte_offset.get() + pull_into_descriptor.bytes_filled.get();

        // 4. Let descriptorBuffer be pullIntoDescriptor’s buffer.
        let descriptor_buffer = pull_into_descriptor.buffer.get();

        // 5. Let queueBuffer be headOfQueue’s buffer.
        let queue_buffer = head_of_queue.buffer;

        // 6. Let queueByteOffset be headOfQueue’s byte offset.
        let queue_byte_offset = head_of_queue.byte_offset;

        // 7. Assert: ! CanCopyDataBlockBytes(descriptorBuffer, destStart, queueBuffer, queueByteOffset, bytesToCopy) is
        //    true.
        assert!(can_copy_data_block_bytes_buffer(
            descriptor_buffer,
            dest_start,
            queue_buffer,
            queue_byte_offset,
            bytes_to_copy
        ));

        // 8. Perform ! CopyDataBlockBytes(descriptorBuffer.[[ArrayBufferData]], destStart,
        //    queueBuffer.[[ArrayBufferData]], queueByteOffset, bytesToCopy).
        head_of_queue.buffer.copy_data_to(
            &descriptor_buffer,
            head_of_queue.byte_offset as usize,
            dest_start as usize,
            bytes_to_copy as usize,
        );

        {
            let mut queue = queue.borrow_mut(vm);

            // 9. If headOfQueue’s byte length is bytesToCopy,
            if head_of_queue.byte_length == bytes_to_copy {
                // 1. Remove queue[0].
                queue.pop_front();
            }
            // 10. Otherwise,
            else {
                let head_of_queue = queue.front_mut().expect("the queue holds the bytes to copy");

                // 1. Set headOfQueue’s byte offset to headOfQueue’s byte offset + bytesToCopy.
                head_of_queue.byte_offset += bytes_to_copy;

                // 2. Set headOfQueue’s byte length to headOfQueue’s byte length − bytesToCopy.
                head_of_queue.byte_length -= bytes_to_copy;
            }
        }

        // 11. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] − bytesToCopy.
        controller
            .queue_total_size
            .set(controller.queue_total_size.get() - bytes_to_copy as f64);

        // 12. Perform ! ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller, bytesToCopy,
        //     pullIntoDescriptor).
        readable_byte_stream_controller_fill_head_pull_into_descriptor(controller, bytes_to_copy, pull_into_descriptor);

        // 13. Set totalBytesToCopyRemaining to totalBytesToCopyRemaining − bytesToCopy.
        total_bytes_to_copy_remaining -= bytes_to_copy;
    }

    // 12. If ready is false,
    if !ready {
        // 1. Assert: controller.[[queueTotalSize]] is 0.
        assert!(controller.queue_total_size.get() == 0.0);

        // 2. Assert: pullIntoDescriptor’s bytes filled > 0.
        assert!(pull_into_descriptor.bytes_filled.get() > 0);

        // 3. Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s minimum fill.
        assert!(pull_into_descriptor.bytes_filled.get() < pull_into_descriptor.minimum_fill.get());
    }

    // 13. Return ready.
    ready
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerfillreadrequestfromqueue
pub(crate) fn readable_byte_stream_controller_fill_read_request_from_queue(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    read_request: ReadRequest,
) {
    let vm = cx.vm();

    // 1. Assert: controller.[[queueTotalSize]] > 0.
    assert!(controller.queue_total_size.get() > 0.0);

    // 2. Let entry be controller.[[queue]][0].
    // 3. Remove entry from controller.[[queue]].
    let entry = controller
        .queue
        .borrow_mut(vm)
        .pop_front()
        .expect("a controller with bytes in its queue has a queue entry");

    // 4. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] − entry’s byte length.
    controller
        .queue_total_size
        .set(controller.queue_total_size.get() - entry.byte_length as f64);

    // 5. Perform ! ReadableByteStreamControllerHandleQueueDrain(controller).
    readable_byte_stream_controller_handle_queue_drain(cx, controller);

    // 6. Let view be ! Construct(%Uint8Array%, « entry’s buffer, entry’s byte offset, entry’s byte length »).
    let view = construct(
        vm,
        cx.realm().intrinsics().uint8_array_constructor(vm),
        &[
            Value::from_object(entry.buffer),
            Value::from_f64(entry.byte_offset as f64),
            Value::from_f64(entry.byte_length as f64),
        ],
        None,
    )
    .must();

    // 7. Perform readRequest’s chunk steps, given view.
    read_request.chunk_steps(vm, Value::from_object(view));
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollergetbyobrequest
pub(crate) fn readable_byte_stream_controller_get_byob_request(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
) -> Option<Ref<ReadableStreamBYOBRequest>> {
    let vm = cx.vm();

    // 1. If controller.[[byobRequest]] is null and controller.[[pendingPullIntos]] is not empty,
    if controller.byob_request.get().is_none()
        && let Some(first_descriptor) = controller.first_pending_pull_into()
    {
        // 1. Let firstDescriptor be controller.[[pendingPullIntos]][0].

        // 2. Let view be ! Construct(%Uint8Array%, « firstDescriptor’s buffer, firstDescriptor’s byte offset +
        //    firstDescriptor’s bytes filled, firstDescriptor’s byte length − firstDescriptor’s bytes filled »).
        let view_object = construct(
            vm,
            cx.realm().intrinsics().uint8_array_constructor(vm),
            &[
                Value::from_object(first_descriptor.buffer.get()),
                Value::from_f64((first_descriptor.byte_offset.get() + first_descriptor.bytes_filled.get()) as f64),
                Value::from_f64((first_descriptor.byte_length.get() - first_descriptor.bytes_filled.get()) as f64),
            ],
            None,
        )
        .must();
        let view = view_object
            .downcast::<TypedArrayBase>()
            .expect("a Uint8Array is a typed array");

        // 3. Let byobRequest be a new ReadableStreamBYOBRequest.
        let byob_request = allocate(vm, |_| ReadableStreamBYOBRequest::new());

        // 4. Set byobRequest.[[controller]] to controller.
        byob_request.set_controller(Some(controller));

        // 5. Set byobRequest.[[view]] to view.
        byob_request.set_view(Some(view));

        // 6. Set controller.[[byobRequest]] to byobRequest.
        controller.byob_request.set(Some(byob_request));
    }

    // 2. Return controller.[[byobRequest]].
    controller.byob_request.get()
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-get-desired-size
pub(crate) fn readable_byte_stream_controller_get_desired_size(
    controller: Ref<ReadableByteStreamController>,
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
    Some(controller.strategy_hwm.get() - controller.queue_total_size.get())
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-handle-queue-drain
pub(crate) fn readable_byte_stream_controller_handle_queue_drain(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
) {
    let stream = controller.stream();

    // 1. Assert: controller.[[stream]].[[state]] is "readable".
    assert!(stream.state() == ReadableStreamState::Readable);

    // 2. If controller.[[queueTotalSize]] is 0 and controller.[[closeRequested]] is true,
    if controller.queue_total_size.get() == 0.0 && controller.close_requested() {
        // 1. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
        readable_byte_stream_controller_clear_algorithms(controller);

        // 2. Perform ! ReadableStreamClose(controller.[[stream]]).
        readable_stream_close(cx.vm(), stream);
    }
    // 3. Otherwise,
    else {
        // 1. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
        readable_byte_stream_controller_call_pull_if_needed(cx, controller);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-invalidate-byob-request
pub(crate) fn readable_byte_stream_controller_invalidate_byob_request(controller: Ref<ReadableByteStreamController>) {
    // 1. If controller.[[byobRequest]] is null, return.
    let Some(byob_request) = controller.byob_request.get() else {
        return;
    };

    // 2. Set controller.[[byobRequest]].[[controller]] to undefined.
    byob_request.set_controller(None);

    // 3. Set controller.[[byobRequest]].[[view]] to null.
    byob_request.set_view(None);

    // 4. Set controller.[[byobRequest]] to null.
    controller.byob_request.set(None);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-process-pull-into-descriptors-using-queue
pub(crate) fn readable_byte_stream_controller_process_pull_into_descriptors_using_queue(
    vm: &Vm,
    controller: Ref<ReadableByteStreamController>,
) -> MarkedVec<'_, PullIntoDescriptorRef> {
    // 1. Assert: controller.[[closeRequested]] is false.
    assert!(!controller.close_requested());

    // 2. Let filledPullIntos be a new empty list.
    let filled_pull_intos = MarkedVec::new(vm);

    // 3. While controller.[[pendingPullIntos]] is not empty,
    while let Some(pull_into_descriptor) = controller.first_pending_pull_into() {
        // 1. If controller.[[queueTotalSize]] is 0, then break.
        if controller.queue_total_size.get() == 0.0 {
            break;
        }

        // 2. Let pullIntoDescriptor be controller.[[pendingPullIntos]][0].

        // 3. If ! ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, pullIntoDescriptor) is true,
        if readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(vm, controller, pull_into_descriptor) {
            // 1. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
            readable_byte_stream_controller_shift_pending_pull_into(vm, controller);

            // 2. Append pullIntoDescriptor to filledPullIntos.
            filled_pull_intos.push(pull_into_descriptor);
        }
    }

    // 4. Return filledPullIntos.
    filled_pull_intos
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerprocessreadrequestsusingqueue
pub(crate) fn readable_byte_stream_controller_process_read_requests_using_queue(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
) {
    // 1. Let reader be controller.[[stream]].[[reader]].
    // 2. Assert: reader implements ReadableStreamDefaultReader.
    let Some(ReadableStreamReader::ReadableStreamDefaultReader(reader)) = controller.stream().reader() else {
        panic!("the stream has a ReadableStreamDefaultReader");
    };

    // 3. While reader.[[readRequests]] is not empty,
    while !reader.read_requests_is_empty() {
        // 1. If controller.[[queueTotalSize]] is 0, return.
        if controller.queue_total_size.get() == 0.0 {
            return;
        }

        // 2. Let readRequest be reader.[[readRequests]][0].
        // 3. Remove readRequest from reader.[[readRequests]].
        let read_request = reader.take_first_read_request(cx.vm());

        // 4. Perform ! ReadableByteStreamControllerFillReadRequestFromQueue(controller, readRequest).
        readable_byte_stream_controller_fill_read_request_from_queue(cx, controller, read_request);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-pull-into
pub(crate) fn readable_byte_stream_controller_pull_into(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    view: ArrayBufferView,
    min: u64,
    read_into_request: ReadIntoRequest,
) {
    let vm = cx.vm();

    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. Let elementSize be 1.
    let mut element_size = 1;

    // 3. Let ctor be %DataView%.
    let mut ctor = cx.realm().intrinsics().data_view_constructor(vm);

    // 4. If view has a [[TypedArrayName]] internal slot (i.e., it is not a DataView),
    if let ArrayBufferView::TypedArray(typed_array) = view {
        // 1. Set elementSize to the element size specified in the typed array constructors table for
        //    view.[[TypedArrayName]].
        element_size = u64::from(typed_array.element_size());

        // 2. Set ctor to the constructor specified in the typed array constructors table for view.[[TypedArrayName]].
        ctor = typed_array.intrinsic_constructor(vm, cx.realm());
    }

    let view_extent = BufferSource::from(view).extent();

    // 5. Let minimumFill be min × elementSize.
    let minimum_fill = min * element_size;

    // 6. Assert: minimumFill ≥ 0 and minimumFill ≤ view.[[ByteLength]].
    assert!(minimum_fill <= view_extent.byte_length as u64);

    // 7. Assert: the remainder after dividing minimumFill by elementSize is 0.
    assert!(minimum_fill.is_multiple_of(element_size));

    // 8. Let byteOffset be view.[[ByteOffset]].
    let byte_offset = view_extent.byte_offset as u64;

    // 9. Let byteLength be view.[[ByteLength]].
    let byte_length = view_extent.byte_length as u64;

    // 10. Let bufferResult be TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
    let buffer_result = transfer_array_buffer(cx, view_extent.buffer);

    // 11. If bufferResult is an abrupt completion,
    let buffer = match buffer_result {
        Err(exception) => {
            // 1. Perform readIntoRequest’s error steps, given bufferResult.[[Value]].
            read_into_request.error_steps(vm, exception_value(vm, cx.realm(), exception));

            // 2. Return.
            return;
        }
        // 12. Let buffer be bufferResult.[[Value]].
        Ok(buffer) => buffer,
    };

    // 13. Let pullIntoDescriptor be a new pull-into descriptor with
    //
    //     buffer                   buffer
    //     buffer byte length       buffer.[[ArrayBufferByteLength]]
    //     byte offset              byteOffset
    //     byte length              byteLength
    //     bytes filled             0
    //     minimum fill             minimumFill
    //     element size             elementSize
    //     view constructor         ctor
    //     reader type              "byob"
    let pull_into_descriptor = PULL_INTO_DESCRIPTOR_CLASS.allocate(vm, |_| PullIntoDescriptor {
        buffer: Cell::new(buffer),
        buffer_byte_length: Cell::new(buffer.byte_length() as u64),
        byte_offset: Cell::new(byte_offset),
        byte_length: Cell::new(byte_length),
        bytes_filled: Cell::new(0),
        minimum_fill: Cell::new(minimum_fill),
        element_size: Cell::new(element_size),
        view_constructor: ctor,
        reader_type: Cell::new(ReaderType::Byob),
    });

    // 14. If controller.[[pendingPullIntos]] is not empty,
    if !controller.pending_pull_intos_is_empty() {
        // 1. Append pullIntoDescriptor to controller.[[pendingPullIntos]].
        controller
            .pending_pull_intos
            .borrow_mut(vm)
            .push_back(pull_into_descriptor);

        // 2. Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
        readable_stream_add_read_into_request(vm, stream, read_into_request);

        // 3. Return.
        return;
    }

    // 15. If stream.[[state]] is "closed",
    if stream.state() == ReadableStreamState::Closed {
        // 1. Let emptyView be ! Construct(ctor, « pullIntoDescriptor’s buffer, pullIntoDescriptor’s byte offset, 0 »).
        let empty_view = construct(
            vm,
            ctor,
            &[
                Value::from_object(pull_into_descriptor.buffer.get()),
                Value::from_f64(pull_into_descriptor.byte_offset.get() as f64),
                Value::from_i32(0),
            ],
            None,
        )
        .must();

        // 2. Perform readIntoRequest’s close steps, given emptyView.
        read_into_request.close_steps(vm, Value::from_object(empty_view));

        // 3. Return.
        return;
    }

    // 16. If controller.[[queueTotalSize]] > 0,
    if controller.queue_total_size.get() > 0.0 {
        // 1. If ! ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, pullIntoDescriptor) is true,
        if readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(vm, controller, pull_into_descriptor) {
            // 1. Let filledView be ! ReadableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor).
            let filled_view = readable_byte_stream_controller_convert_pull_into_descriptor(cx, pull_into_descriptor);

            // 2. Perform ! ReadableByteStreamControllerHandleQueueDrain(controller).
            readable_byte_stream_controller_handle_queue_drain(cx, controller);

            // 3. Perform readIntoRequest’s chunk steps, given filledView.
            read_into_request.chunk_steps(vm, filled_view);

            // 4. Return.
            return;
        }

        // 2. If controller.[[closeRequested]] is true,
        if controller.close_requested() {
            // 1. Let e be a TypeError exception.
            let error = exception_value(vm, cx.realm(), Exception::type_error("Reader has been released"));

            // 2. Perform ! ReadableByteStreamControllerError(controller, e).
            readable_byte_stream_controller_error(cx, controller, error);

            // 3. Perform readIntoRequest’s error steps, given e.
            read_into_request.error_steps(vm, error);

            // 4. Return.
            return;
        }
    }

    // 17. Append pullIntoDescriptor to controller.[[pendingPullIntos]].
    controller
        .pending_pull_intos
        .borrow_mut(vm)
        .push_back(pull_into_descriptor);

    // 18. Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
    readable_stream_add_read_into_request(vm, stream, read_into_request);

    // 19. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
    readable_byte_stream_controller_call_pull_if_needed(cx, controller);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond
pub(crate) fn readable_byte_stream_controller_respond(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    bytes_written: u64,
) -> WebIdlResult<()> {
    // 1. Assert: controller.[[pendingPullIntos]] is not empty.
    // 2. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    let first_descriptor = controller
        .first_pending_pull_into()
        .expect("a controller that is responded to has a pending pull-into");

    // 3. Let state be controller.[[stream]].[[state]].
    let state = controller.stream().state();

    // 4. If state is "closed",
    if state == ReadableStreamState::Closed {
        // 1. If bytesWritten is not 0, throw a TypeError exception.
        if bytes_written != 0 {
            return Err(Exception::type_error("Bytes written is not zero for closed stream"));
        }
    }
    // 5. Otherwise,
    else {
        // 1. Assert: state is "readable".
        assert!(state == ReadableStreamState::Readable);

        // 2. If bytesWritten is 0, throw a TypeError exception.
        if bytes_written == 0 {
            return Err(Exception::type_error(
                "Bytes written is zero for stream which is not closed",
            ));
        }

        // 3. If firstDescriptor’s bytes filled + bytesWritten > firstDescriptor’s byte length, throw a RangeError
        //    exception.
        if first_descriptor.bytes_filled.get() + bytes_written > first_descriptor.byte_length.get() {
            return Err(Exception::range_error(
                "Bytes written is greater than the pull requests byte length",
            ));
        }
    }

    // 6. Set firstDescriptor’s buffer to ! TransferArrayBuffer(firstDescriptor’s buffer).
    first_descriptor
        .buffer
        .set(transfer_array_buffer(cx, first_descriptor.buffer.get()).must());

    // 7. Perform ? ReadableByteStreamControllerRespondInternal(controller, bytesWritten).
    readable_byte_stream_controller_respond_internal(cx, controller, bytes_written)
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-closed-state
pub(crate) fn readable_byte_stream_controller_respond_in_closed_state(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    first_descriptor: PullIntoDescriptorRef,
) {
    // 1. Assert: the remainder after dividing firstDescriptor’s bytes filled by firstDescriptor’s element size is 0.
    assert!(
        first_descriptor
            .bytes_filled
            .get()
            .is_multiple_of(first_descriptor.element_size.get())
    );

    // 2. If firstDescriptor’s reader type is "none", perform
    //    ! ReadableByteStreamControllerShiftPendingPullInto(controller).
    if first_descriptor.reader_type.get() == ReaderType::None {
        readable_byte_stream_controller_shift_pending_pull_into(cx.vm(), controller);
    }

    // 3. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 4. If ! ReadableStreamHasBYOBReader(stream) is true,
    if readable_stream_has_byob_reader(stream) {
        // 1. Let filledPullIntos be a new empty list.
        let filled_pull_intos = MarkedVec::new(cx.vm());

        // 2. While filledPullIntos’s size < ! ReadableStreamGetNumReadIntoRequests(stream),
        while filled_pull_intos.len() < readable_stream_get_num_read_into_requests(stream) {
            // 1. Let pullIntoDescriptor be ! ReadableByteStreamControllerShiftPendingPullInto(controller).
            let pull_into_descriptor = readable_byte_stream_controller_shift_pending_pull_into(cx.vm(), controller);

            // 2. Append pullIntoDescriptor to filledPullIntos.
            filled_pull_intos.push(pull_into_descriptor);
        }

        // 3. For each filledPullInto of filledPullIntos,
        for index in 0..filled_pull_intos.len() {
            // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(stream, filledPullInto).
            let filled_pull_into = filled_pull_intos.get(index).expect("the index is in bounds");
            readable_byte_stream_controller_commit_pull_into_descriptor(cx, stream, filled_pull_into);
        }
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-readable-state
pub(crate) fn readable_byte_stream_controller_respond_in_readable_state(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    bytes_written: u64,
    pull_into_descriptor: PullIntoDescriptorRef,
) -> WebIdlResult<()> {
    // 1. Assert: pullIntoDescriptor’s bytes filled + bytesWritten ≤ pullIntoDescriptor’s byte length.
    assert!(pull_into_descriptor.bytes_filled.get() + bytes_written <= pull_into_descriptor.byte_length.get());

    // 2. Perform ! ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller, bytesWritten,
    //    pullIntoDescriptor).
    readable_byte_stream_controller_fill_head_pull_into_descriptor(controller, bytes_written, pull_into_descriptor);

    // 3. If pullIntoDescriptor’s reader type is "none",
    if pull_into_descriptor.reader_type.get() == ReaderType::None {
        // 1. Perform ? ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, pullIntoDescriptor).
        readable_byte_stream_controller_enqueue_detached_pull_into_to_queue(cx, controller, pull_into_descriptor)?;

        // 2. Let filledPullIntos be the result of performing
        //    ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
        let filled_pulled_intos =
            readable_byte_stream_controller_process_pull_into_descriptors_using_queue(cx.vm(), controller);

        // 3. For each filledPullInto of filledPullIntos,
        for index in 0..filled_pulled_intos.len() {
            // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], filledPullInto).
            let filled_pull_into = filled_pulled_intos.get(index).expect("the index is in bounds");
            readable_byte_stream_controller_commit_pull_into_descriptor(cx, controller.stream(), filled_pull_into);
        }

        // 4. Return.
        return Ok(());
    }

    // 4. If pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s minimum fill, return.
    if pull_into_descriptor.bytes_filled.get() < pull_into_descriptor.minimum_fill.get() {
        return Ok(());
    }

    // NOTE: A descriptor for a read() request that is not yet filled up to its minimum length will stay at the head of
    //       the queue, so the underlying source can keep filling it.

    // 5. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
    readable_byte_stream_controller_shift_pending_pull_into(cx.vm(), controller);

    // 6. Let remainderSize be the remainder after dividing pullIntoDescriptor’s bytes filled by pullIntoDescriptor’s
    //    element size.
    let remainder_size = pull_into_descriptor.bytes_filled.get() % pull_into_descriptor.element_size.get();

    // 7. If remainderSize > 0,
    if remainder_size > 0 {
        // 1. Let end be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s bytes filled.
        let end = pull_into_descriptor.byte_offset.get() + pull_into_descriptor.bytes_filled.get();

        // 2. Perform ? ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller, pullIntoDescriptor’s buffer,
        //    end − remainderSize, remainderSize).
        readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(
            cx,
            controller,
            pull_into_descriptor.buffer.get(),
            end - remainder_size,
            remainder_size,
        )?;
    }

    // 8. Set pullIntoDescriptor’s bytes filled to pullIntoDescriptor’s bytes filled − remainderSize.
    pull_into_descriptor
        .bytes_filled
        .set(pull_into_descriptor.bytes_filled.get() - remainder_size);

    // 9. Let filledPullIntos be the result of performing
    //    ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
    let filled_pulled_intos =
        readable_byte_stream_controller_process_pull_into_descriptors_using_queue(cx.vm(), controller);

    // 10. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], pullIntoDescriptor).
    readable_byte_stream_controller_commit_pull_into_descriptor(cx, controller.stream(), pull_into_descriptor);

    // 11. For each filledPullInto of filledPullIntos,
    for index in 0..filled_pulled_intos.len() {
        // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], filledPullInto).
        let filled_pull_into = filled_pulled_intos.get(index).expect("the index is in bounds");
        readable_byte_stream_controller_commit_pull_into_descriptor(cx, controller.stream(), filled_pull_into);
    }
    Ok(())
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-internal
pub(crate) fn readable_byte_stream_controller_respond_internal(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    bytes_written: u64,
) -> WebIdlResult<()> {
    // 1. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    let first_descriptor = controller
        .first_pending_pull_into()
        .expect("a controller that is responded to has a pending pull-into");

    // 2. Assert: ! CanTransferArrayBuffer(firstDescriptor’s buffer) is true.
    assert!(can_transfer_array_buffer(&first_descriptor.buffer.get()));

    // 3. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
    readable_byte_stream_controller_invalidate_byob_request(controller);

    // 4. Let state be controller.[[stream]].[[state]].
    let state = controller.stream().state();

    // 5. If state is "closed",
    if state == ReadableStreamState::Closed {
        // 1. Assert: bytesWritten is 0.
        assert!(bytes_written == 0);

        // 2. Perform ! ReadableByteStreamControllerRespondInClosedState(controller, firstDescriptor).
        readable_byte_stream_controller_respond_in_closed_state(cx, controller, first_descriptor);
    }
    // 6. Otherwise,
    else {
        // 1. Assert: state is "readable".
        assert!(state == ReadableStreamState::Readable);

        // 2. Assert: bytesWritten > 0.
        assert!(bytes_written > 0);

        // 3. Perform ? ReadableByteStreamControllerRespondInReadableState(controller, bytesWritten, firstDescriptor).
        readable_byte_stream_controller_respond_in_readable_state(cx, controller, bytes_written, first_descriptor)?;
    }

    // 7. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
    readable_byte_stream_controller_call_pull_if_needed(cx, controller);
    Ok(())
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-with-new-view
pub(crate) fn readable_byte_stream_controller_respond_with_new_view(
    cx: &CallContext,
    controller: Ref<ReadableByteStreamController>,
    view: ArrayBufferView,
) -> WebIdlResult<()> {
    let view_extent = BufferSource::from(view).extent();

    // 1. Assert: controller.[[pendingPullIntos]] is not empty.
    assert!(!controller.pending_pull_intos_is_empty());

    // 2. Assert: ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is false.
    assert!(!view_extent.buffer.is_detached());

    // 3. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    let first_descriptor = controller
        .first_pending_pull_into()
        .expect("a controller that is responded to has a pending pull-into");

    // 4. Let state be controller.[[stream]].[[state]].
    let state = controller.stream().state();

    // 5. If state is "closed",
    if state == ReadableStreamState::Closed {
        // 1. If view.[[ByteLength]] is not 0, throw a TypeError exception.
        if view_extent.byte_length != 0 {
            return Err(Exception::type_error("Byte length is not zero for closed stream"));
        }
    }
    // 6. Otherwise,
    else {
        // 1. Assert: state is "readable".
        assert!(state == ReadableStreamState::Readable);

        // 2. If view.[[ByteLength]] is 0, throw a TypeError exception.
        if view_extent.byte_length == 0 {
            return Err(Exception::type_error(
                "Byte length is zero for stream which is not closed",
            ));
        }
    }

    // 7. If firstDescriptor’s byte offset + firstDescriptor’ bytes filled is not view.[[ByteOffset]], throw a
    //    RangeError exception.
    if first_descriptor.byte_offset.get() + first_descriptor.bytes_filled.get() != view_extent.byte_offset as u64 {
        return Err(Exception::range_error(
            "Byte offset is not aligned with the pull request's byte offset",
        ));
    }

    // 8. If firstDescriptor’s buffer byte length is not view.[[ViewedArrayBuffer]].[[ByteLength]], throw a RangeError
    //    exception.
    if first_descriptor.buffer_byte_length.get() != view_extent.buffer.byte_length() as u64 {
        return Err(Exception::range_error(
            "Buffer byte length is not aligned with the pull request's byte length",
        ));
    }

    // 9. If firstDescriptor’s bytes filled + view.[[ByteLength]] > firstDescriptor’s byte length, throw a RangeError
    //    exception.
    if first_descriptor.bytes_filled.get() + view_extent.byte_length as u64 > first_descriptor.byte_length.get() {
        return Err(Exception::range_error(
            "Byte length is greater than the pull request's byte length",
        ));
    }

    // 10. Let viewByteLength be view.[[ByteLength]].
    let view_byte_length = view_extent.byte_length as u64;

    // 11. Set firstDescriptor’s buffer to ? TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
    first_descriptor
        .buffer
        .set(transfer_array_buffer(cx, view_extent.buffer)?);

    // 12. Perform ? ReadableByteStreamControllerRespondInternal(controller, viewByteLength).
    readable_byte_stream_controller_respond_internal(cx, controller, view_byte_length)
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-shift-pending-pull-into
pub(crate) fn readable_byte_stream_controller_shift_pending_pull_into(
    vm: &Vm,
    controller: Ref<ReadableByteStreamController>,
) -> PullIntoDescriptorRef {
    // 1. Assert: controller.[[byobRequest]] is null.
    assert!(controller.byob_request.get().is_none());

    // 2. Let descriptor be controller.[[pendingPullIntos]][0].
    // 3. Remove descriptor from controller.[[pendingPullIntos]].
    // 4. Return descriptor.
    controller
        .pending_pull_intos
        .borrow_mut(vm)
        .pop_front()
        .expect("a controller shifts a pending pull-into it has")
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-should-call-pull
pub(crate) fn readable_byte_stream_controller_should_call_pull(controller: Ref<ReadableByteStreamController>) -> bool {
    // 1. Let stream be controller.[[stream]].
    let stream = controller.stream();

    // 2. If stream.[[state]] is not "readable", return false.
    if stream.state() != ReadableStreamState::Readable {
        return false;
    }

    // 3. If controller.[[closeRequested]] is true, return false.
    if controller.close_requested() {
        return false;
    }

    // 4. If controller.[[started]] is false, return false.
    if !controller.started.get() {
        return false;
    }

    // 5. If ! ReadableStreamHasDefaultReader(stream) is true and ! ReadableStreamGetNumReadRequests(stream) > 0, return
    //    true.
    if readable_stream_has_default_reader(stream) && readable_stream_get_num_read_requests(stream) > 0 {
        return true;
    }

    // 6. If ! ReadableStreamHasBYOBReader(stream) is true and ! ReadableStreamGetNumReadIntoRequests(stream) > 0,
    //    return true.
    if readable_stream_has_byob_reader(stream) && readable_stream_get_num_read_into_requests(stream) > 0 {
        return true;
    }

    // 7. Let desiredSize be ! ReadableByteStreamControllerGetDesiredSize(controller).
    // 8. Assert: desiredSize is not null.
    let desired_size = readable_byte_stream_controller_get_desired_size(controller)
        .expect("a readable stream's controller has a desired size");

    // 9. If desiredSize > 0, return true.
    if desired_size > 0.0 {
        return true;
    }

    // 10. Return false.
    false
}

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
#[allow(clippy::too_many_arguments, reason = "the abstract operation takes these arguments")]
pub(crate) fn set_up_readable_byte_stream_controller(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    controller: Ref<ReadableByteStreamController>,
    start_algorithm: StartAlgorithm,
    pull_algorithm: PullAlgorithm,
    cancel_algorithm: CancelAlgorithm,
    high_water_mark: f64,
    auto_allocate_chunk_size: Option<u64>,
) -> WebIdlResult<()> {
    // 1. Assert: stream.[[controller]] is undefined.
    assert!(stream.controller().is_none());

    // 2. If autoAllocateChunkSize is not undefined,
    if let Some(auto_allocate_chunk_size) = auto_allocate_chunk_size {
        // 1. Assert: ! IsInteger(autoAllocateChunkSize) is true.
        // 2. Assert: autoAllocateChunkSize is positive.
        assert!(auto_allocate_chunk_size > 0);
    }

    // 3. Set controller.[[stream]] to stream.
    controller.stream.set(Some(stream));

    // 4. Set controller.[[pullAgain]] and controller.[[pulling]] to false.
    controller.pull_again.set(false);
    controller.pulling.set(false);

    // 5. Set controller.[[byobRequest]] to null.
    controller.byob_request.set(None);

    // 6. Perform ! ResetQueue(controller).
    controller.reset_queue(cx.vm());

    // 7. Set controller.[[closeRequested]] and controller.[[started]] to false.
    controller.close_requested.set(false);
    controller.started.set(false);

    // 8. Set controller.[[strategyHWM]] to highWaterMark.
    controller.strategy_hwm.set(high_water_mark);

    // 9. Set controller.[[pullAlgorithm]] to pullAlgorithm.
    controller.pull_algorithm.replace(Some(pull_algorithm));

    // 10. Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
    controller.cancel_algorithm.replace(Some(cancel_algorithm));

    // 11. Set controller.[[autoAllocateChunkSize]] to autoAllocateChunkSize.
    controller.auto_allocate_chunk_size.set(auto_allocate_chunk_size);

    // 12. Set controller.[[pendingPullIntos]] to a new empty list.
    controller.pending_pull_intos.borrow_mut(cx.vm()).clear();

    // 13. Set stream.[[controller]] to controller.
    stream.set_controller(Some(ReadableStreamController::ReadableByteStreamController(controller)));

    // 14. Let startResult be the result of performing startAlgorithm.
    let start_result = start_algorithm.run()?;

    // 15. Let startPromise be a promise resolved with startResult.
    let start_promise = create_resolved_promise(cx.vm(), cx.realm(), start_result);

    react_to_promise(
        cx.vm(),
        start_promise,
        controller,
        // 16. Upon fulfillment of startPromise,
        Some(|cx, controller, _| {
            // 1. Set controller.[[started]] to true.
            controller.started.set(true);

            // 2. Assert: controller.[[pulling]] is false.
            assert!(!controller.pulling.get());

            // 3. Assert: controller.[[pullAgain]] is false.
            assert!(!controller.pull_again.get());

            // 4. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
            readable_byte_stream_controller_call_pull_if_needed(cx, controller);

            Ok(Value::UNDEFINED)
        }),
        // 17. Upon rejection of startPromise with reason r,
        Some(|cx, controller, reason| {
            // 1. Perform ! ReadableByteStreamControllerError(controller, r).
            readable_byte_stream_controller_error(cx, controller, reason);

            Ok(Value::UNDEFINED)
        }),
    );
    Ok(())
}

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller-from-underlying-source
pub(crate) fn set_up_readable_byte_stream_controller_from_underlying_source(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    underlying_source: Value,
    underlying_source_dict: UnderlyingSource,
    high_water_mark: f64,
) -> WebIdlResult<()> {
    // 1. Let controller be a new ReadableByteStreamController.
    let controller = allocate(cx.vm(), ReadableByteStreamController::new);

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
            controller: ReadableStreamController::ReadableByteStreamController(controller).to_js(cx),
        };
    }

    // 6. If underlyingSourceDict["pull"] exists, then set pullAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSourceDict["pull"] with argument list « controller » and callback this value
    //    underlyingSource.
    if let Some(pull) = underlying_source_dict.pull {
        pull_algorithm = PullAlgorithm::Callback {
            pull,
            underlying_source,
            controller: ReadableStreamController::ReadableByteStreamController(controller),
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

    // 8. Let autoAllocateChunkSize be underlyingSourceDict["autoAllocateChunkSize"], if it exists, or undefined
    //    otherwise.
    let auto_allocate_chunk_size = underlying_source_dict.auto_allocate_chunk_size;

    // 9. If autoAllocateChunkSize is 0, then throw a TypeError exception.
    if auto_allocate_chunk_size == Some(0) {
        return Err(Exception::type_error("Cannot use an auto allocate chunk size of 0"));
    }

    // 10. Perform ? SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm,
    //     cancelAlgorithm, highWaterMark, autoAllocateChunkSize).
    set_up_readable_byte_stream_controller(
        cx,
        stream,
        controller,
        start_algorithm,
        pull_algorithm,
        cancel_algorithm,
        high_water_mark,
        auto_allocate_chunk_size,
    )
}
