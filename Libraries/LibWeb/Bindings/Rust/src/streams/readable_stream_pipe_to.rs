/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStreamPipeTo, https://streams.spec.whatwg.org/#readable-stream-pipe-to, whose reading of chunks from its
//! source and writing them to its destination in parallel happens in the state of the pipe.

use std::collections::VecDeque;

use core::cell::Cell;

use libjs_rust::api::{
    EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, NativeFunction, PropertyKey, Trace, Value, Vm,
};

use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Ref, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{
    ReactionSteps, WebIdlPromise, create_promise, create_resolved_promise, exception_value,
    get_promise_for_wait_for_all, is_promise_fulfilled, mark_promise_as_handled, promise_realm, queue_a_microtask,
    react_to_promise, reject_promise, resolve_promise,
};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::streams::cpp::AbortSignal;
use crate::streams::read_request::ReadRequest;
use crate::streams::readable_stream::{
    ReadableStream, ReadableStreamState, acquire_readable_stream_default_reader, is_readable_stream_locked,
    readable_stream_cancel,
};
use crate::streams::readable_stream_default_reader::{
    ReadableStreamDefaultReader, readable_stream_default_reader_read, readable_stream_default_reader_release,
};
use crate::streams::writable_stream::{
    WritableStream, WritableStreamState, acquire_writable_stream_default_writer, is_writable_stream_locked,
    writable_stream_abort, writable_stream_close_queued_or_in_flight,
};
use crate::streams::writable_stream_default_writer::{
    WritableStreamDefaultWriter, writable_stream_default_writer_close_with_error_propagation,
    writable_stream_default_writer_release, writable_stream_default_writer_write,
};

/// The action that the pipe shuts down with, https://streams.spec.whatwg.org/#rs-pipeTo-shutdown-with-action
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
enum ShutdownAction {
    /// ! WritableStreamAbort(dest, source.[[storedError]])
    AbortDestination,
    /// ! ReadableStreamCancel(source, dest.[[storedError]])
    CancelSourceWithDestinationError,
    /// ! ReadableStreamCancel(source, destClosed)
    CancelSource(Value),
    /// ! WritableStreamDefaultWriterCloseWithErrorPropagation(writer)
    CloseDestination,
    /// Getting a promise to wait for all of the actions of the abortAlgorithm, given the signal's abort reason.
    AbortAlgorithm(Value),
}

/// What shutting down the pipe does once every chunk that has been read has been written.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
enum ShutdownSteps {
    /// The steps of shutting down with an action, passing along originalError if it was given.
    WithAction {
        action: ShutdownAction,
        original_error: Option<Value>,
    },
    /// The steps of shutting down, passing along error if it was given.
    WithoutAction { error: Option<Value> },
}

/// The state of a pipe, https://streams.spec.whatwg.org/#ref-for-in-parallel
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PipeTo {
    promise: WebIdlPromise,
    source: Ref<ReadableStream>,
    destination: Ref<WritableStream>,
    reader: Ref<ReadableStreamDefaultReader>,
    writer: Ref<WritableStreamDefaultWriter>,
    signal: GcRefCell<Option<AbortSignal>>,
    #[gc(untraced)]
    signal_algorithm_id: Cell<Option<u64>>,
    last_write_promise: Cell<Option<WebIdlPromise>>,
    unwritten_chunks: GcRefCell<VecDeque<Value>>,
    #[gc(untraced)]
    prevent_close: bool,
    #[gc(untraced)]
    prevent_abort: bool,
    #[gc(untraced)]
    prevent_cancel: bool,
    // https://streams.spec.whatwg.org/#readable-stream-pipe-to: shuttingDown
    shutting_down: Cell<bool>,
}

static PIPE_TO_CLASS: EmbedderClass<PipeTo> = EmbedderClass::new("ReadableStreamPipeTo");

impl EmbedderValue for PipeTo {
    const CLASS: &'static EmbedderClass<Self> = &PIPE_TO_CLASS;
}

pub(crate) type PipeToRef = Gc<EmbedderCell<PipeTo>>;

impl PipeTo {
    /// The context of the realm of promise.
    fn context<'vm>(&self, vm: &'vm Vm) -> CallContext<'vm> {
        CallContext::new(vm, promise_realm(self.promise))
    }

    /// The reaction to the reader's and the writer's closed promises and to the rejection of the writer's ready
    /// promise, which checks whether the pipe has to shut down.
    fn on_shutdown(cx: &CallContext, this: PipeToRef, _: Value) -> WebIdlResult<Value> {
        Self::check_for_error_and_close_states(this, cx.vm());
        Ok(Value::UNDEFINED)
    }

    fn process(this: PipeToRef, vm: &Vm) {
        if Self::check_for_error_and_close_states(this, vm) {
            return;
        }

        let ready_promise = this.writer.ready_promise();

        if let Some(ready_promise) = ready_promise
            && is_promise_fulfilled(ready_promise)
        {
            Self::read_chunk(this, vm);
            return;
        }

        if let Some(ready_promise) = ready_promise {
            react_to_promise(
                vm,
                ready_promise,
                this,
                Some(|cx, this, _| {
                    Self::read_chunk(this, cx.vm());
                    Ok(Value::UNDEFINED)
                }),
                Some(Self::on_shutdown),
            );
        }
    }

    // abortAlgorithm
    fn abort_algorithm(this: PipeToRef, vm: &Vm, signal: &AbortSignal) {
        with_temporary_execution_context_and_callbacks(promise_realm(this.promise), || {
            // 1. Let error be signal’s abort reason.
            let error = signal.reason();

            // 5. Shutdown with an action consisting of getting a promise to wait for all of the actions in actions,
            //    and with error.
            Self::shutdown_with_action(this, vm, ShutdownAction::AbortAlgorithm(error), Some(error));
        });
    }

    /// Performs the action that the pipe shuts down with.
    fn perform_action(this: PipeToRef, cx: &CallContext, action: ShutdownAction) -> WebIdlPromise {
        let vm = cx.vm();
        match action {
            ShutdownAction::AbortDestination => writable_stream_abort(cx, this.destination, this.source.stored_error()),
            ShutdownAction::CancelSourceWithDestinationError => {
                readable_stream_cancel(cx, this.source, this.destination.stored_error())
            }
            ShutdownAction::CancelSource(error) => readable_stream_cancel(cx, this.source, error),
            ShutdownAction::CloseDestination => {
                writable_stream_default_writer_close_with_error_propagation(vm, this.writer)
            }
            ShutdownAction::AbortAlgorithm(error) => {
                // 2. Let actions be an empty ordered set.
                let actions = MarkedVec::new(vm);

                // 3. If preventAbort is false, append the following action to actions:
                if !this.prevent_abort {
                    // 1. If dest.[[state]] is "writable", return ! WritableStreamAbort(dest, error).
                    if this.destination.state() == WritableStreamState::Writable {
                        actions.push(writable_stream_abort(cx, this.destination, error));
                    }
                    // 2. Otherwise, return a promise resolved with undefined.
                    else {
                        actions.push(create_resolved_promise(vm, cx.realm(), Value::UNDEFINED));
                    }
                }

                // 4. If preventCancel is false, append the following action action to actions:
                if !this.prevent_cancel {
                    // 1. If source.[[state]] is "readable", return ! ReadableStreamCancel(source, error).
                    if this.source.state() == ReadableStreamState::Readable {
                        actions.push(readable_stream_cancel(cx, this.source, error));
                    }
                    // 2. Otherwise, return a promise resolved with undefined.
                    else {
                        actions.push(create_resolved_promise(vm, cx.realm(), Value::UNDEFINED));
                    }
                }

                get_promise_for_wait_for_all(vm, cx.realm(), &actions.to_vec())
            }
        }
    }

    // https://streams.spec.whatwg.org/#rs-pipeTo-shutdown-with-action
    fn shutdown_with_action(this: PipeToRef, vm: &Vm, action: ShutdownAction, original_error: Option<Value>) {
        // 1. If shuttingDown is true, abort these substeps.
        if this.shutting_down.get() {
            return;
        }

        // 2. Set shuttingDown to true.
        this.shutting_down.set(true);

        let steps = ShutdownSteps::WithAction { action, original_error };

        // 3. If dest.[[state]] is "writable" and ! WritableStreamCloseQueuedOrInFlight(dest) is false,
        if this.destination.state() == WritableStreamState::Writable
            && !writable_stream_close_queued_or_in_flight(this.destination)
        {
            // 1. If any chunks have been read but not yet written, write them to dest.
            Self::write_unwritten_chunks(this, vm);

            // 2. Wait until every chunk that has been read has been written (i.e. the corresponding promises have
            //    settled).
            Self::wait_for_pending_writes_to_complete(this, vm, steps);
        } else {
            Self::on_pending_writes_complete(this, vm, steps);
        }
    }

    // https://streams.spec.whatwg.org/#rs-pipeTo-shutdown
    fn shutdown(this: PipeToRef, vm: &Vm, error: Option<Value>) {
        // 1. If shuttingDown is true, abort these substeps.
        if this.shutting_down.get() {
            return;
        }

        // 2. Set shuttingDown to true.
        this.shutting_down.set(true);

        let steps = ShutdownSteps::WithoutAction { error };

        // 3. If dest.[[state]] is "writable" and ! WritableStreamCloseQueuedOrInFlight(dest) is false,
        if this.destination.state() == WritableStreamState::Writable
            && !writable_stream_close_queued_or_in_flight(this.destination)
        {
            // 1. If any chunks have been read but not yet written, write them to dest.
            Self::write_unwritten_chunks(this, vm);

            // 2. Wait until every chunk that has been read has been written (i.e. the corresponding promises have
            //    settled).
            Self::wait_for_pending_writes_to_complete(this, vm, steps);
        } else {
            Self::on_pending_writes_complete(this, vm, steps);
        }
    }

    /// The steps of shutting down that follow waiting until every chunk that has been read has been written.
    fn on_pending_writes_complete(this: PipeToRef, vm: &Vm, steps: ShutdownSteps) {
        with_temporary_execution_context_and_callbacks(promise_realm(this.promise), || match steps {
            ShutdownSteps::WithAction { action, original_error } => {
                // 4. Let p be the result of performing action.
                let promise = Self::perform_action(this, &this.context(vm), action);

                react_to_promise(
                    vm,
                    promise,
                    (this, original_error),
                    // 5. Upon fulfillment of p, finalize, passing along originalError if it was given.
                    Some(|cx, (this, original_error), _| {
                        Self::finish(this, cx.vm(), original_error);
                        Ok(Value::UNDEFINED)
                    }),
                    // 6. Upon rejection of p with reason newError, finalize with newError.
                    Some(|cx, (this, _), new_error| {
                        Self::finish(this, cx.vm(), Some(new_error));
                        Ok(Value::UNDEFINED)
                    }),
                );
            }
            ShutdownSteps::WithoutAction { error } => {
                // 4. Finalize, passing along error if it was given.
                Self::finish(this, vm, error);
            }
        });
    }

    fn read_chunk(this: PipeToRef, vm: &Vm) {
        // Shutdown must stop activity: if shuttingDown becomes true, the user agent must not initiate further reads
        // from reader, and must only perform writes of already-read chunks, as described below. In particular, the
        // user agent must check the below conditions before performing any reads or writes, since they might lead to
        // immediate shutdown.
        if Self::check_for_error_and_close_states(this, vm) {
            return;
        }

        let read_request = ReadRequest::PipeTo(this);
        readable_stream_default_reader_read(vm, this.reader, read_request);
    }

    // chunk steps, given chunk, of the read request of the pipe
    pub(crate) fn read_request_chunk_steps(this: PipeToRef, vm: &Vm, chunk: Value) {
        this.unwritten_chunks.borrow_mut().push_back(chunk);

        if Self::check_for_error_and_close_states(this, vm) {
            return;
        }

        queue_a_microtask(vm, promise_realm(this.promise), this, |vm, this| {
            with_temporary_execution_context_and_callbacks(promise_realm(this.promise), || {
                Self::write_chunk(this, vm);
                Self::process(this, vm);
            });
        });
    }

    // close steps, of the read request of the pipe
    pub(crate) fn read_request_close_steps(this: PipeToRef, vm: &Vm) {
        if !Self::check_for_error_and_close_states(this, vm) {
            Self::finish(this, vm, None);
        }
    }

    // error steps, of the read request of the pipe
    pub(crate) fn read_request_error_steps(this: PipeToRef, vm: &Vm, _: Value) {
        Self::check_for_error_and_close_states(this, vm);
    }

    fn write_chunk(this: PipeToRef, vm: &Vm) {
        // Shutdown must stop activity: if shuttingDown becomes true, the user agent must not initiate further reads
        // from reader, and must only perform writes of already-read chunks, as described below. In particular, the
        // user agent must check the below conditions before performing any reads or writes, since they might lead to
        // immediate shutdown.
        if !this.shutting_down.get() && Self::check_for_error_and_close_states(this, vm) {
            return;
        }

        // NB: Shutdown writes every unwritten chunk, which can run before the write that a read queued.
        let Some(chunk) = this.unwritten_chunks.borrow_mut().pop_front() else {
            return;
        };
        let promise = writable_stream_default_writer_write(vm, this.writer, chunk);
        mark_promise_as_handled(vm, promise);

        this.last_write_promise.set(Some(promise));
    }

    fn write_unwritten_chunks(this: PipeToRef, vm: &Vm) {
        while !this.unwritten_chunks.borrow().is_empty() {
            Self::write_chunk(this, vm);
        }
    }

    fn wait_for_pending_writes_to_complete(this: PipeToRef, vm: &Vm, steps: ShutdownSteps) {
        let Some(last_write_promise) = this.last_write_promise.take() else {
            queue_a_microtask(vm, promise_realm(this.promise), (this, steps), |vm, (this, steps)| {
                Self::on_pending_writes_complete(this, vm, steps);
            });
            return;
        };

        let run_complete_steps: ReactionSteps<(PipeToRef, ShutdownSteps)> = |cx, (this, steps), _| {
            Self::on_pending_writes_complete(this, cx.vm(), steps);
            Ok(Value::UNDEFINED)
        };
        react_to_promise(
            vm,
            last_write_promise,
            (this, steps),
            Some(run_complete_steps),
            Some(run_complete_steps),
        );
    }

    // https://streams.spec.whatwg.org/#rs-pipeTo-finalize
    fn finish(this: PipeToRef, vm: &Vm, error: Option<Value>) {
        // 1. Perform ! WritableStreamDefaultWriterRelease(writer).
        writable_stream_default_writer_release(vm, this.writer);

        // 2. If reader implements ReadableStreamBYOBReader, perform ! ReadableStreamBYOBReaderRelease(reader).
        // 3. Otherwise, perform ! ReadableStreamDefaultReaderRelease(reader).
        readable_stream_default_reader_release(vm, this.reader);

        // 4. If signal is not undefined, remove abortAlgorithm from signal.
        let signal = this.signal.borrow().clone();
        if let Some(signal) = signal {
            signal.remove_algorithm(
                this.signal_algorithm_id
                    .get()
                    .expect("an abort algorithm was added to the signal"),
            );
        }

        match error {
            // 5. If error was given, reject promise with error.
            Some(error) => reject_promise(vm, this.promise, error),
            // 6. Otherwise, resolve promise with undefined.
            None => resolve_promise(vm, this.promise, Value::UNDEFINED),
        }

        this.reader.set_pipe_to(None);
    }

    fn check_for_error_and_close_states(this: PipeToRef, vm: &Vm) -> bool {
        // Error and close states must be propagated: the following conditions must be applied in order.
        this.shutting_down.get()
            || Self::check_for_forward_errors(this, vm)
            || Self::check_for_backward_errors(this, vm)
            || Self::check_for_forward_close(this, vm)
            || Self::check_for_backward_close(this, vm)
    }

    fn check_for_forward_errors(this: PipeToRef, vm: &Vm) -> bool {
        // 1. Errors must be propagated forward: if source.[[state]] is or becomes "errored", then
        if this.source.state() == ReadableStreamState::Errored {
            // 1. If preventAbort is false, shutdown with an action of ! WritableStreamAbort(dest,
            //    source.[[storedError]]) and with source.[[storedError]].
            if !this.prevent_abort {
                Self::shutdown_with_action(
                    this,
                    vm,
                    ShutdownAction::AbortDestination,
                    Some(this.source.stored_error()),
                );
            }
            // 2. Otherwise, shutdown with source.[[storedError]].
            else {
                Self::shutdown(this, vm, Some(this.source.stored_error()));
            }
        }

        this.shutting_down.get()
    }

    fn check_for_backward_errors(this: PipeToRef, vm: &Vm) -> bool {
        // 2. Errors must be propagated backward: if dest.[[state]] is or becomes "errored", then
        if this.destination.state() == WritableStreamState::Errored {
            // 1. If preventCancel is false, shutdown with an action of ! ReadableStreamCancel(source,
            //    dest.[[storedError]]) and with dest.[[storedError]].
            if !this.prevent_cancel {
                Self::shutdown_with_action(
                    this,
                    vm,
                    ShutdownAction::CancelSourceWithDestinationError,
                    Some(this.destination.stored_error()),
                );
            }
            // 2. Otherwise, shutdown with dest.[[storedError]].
            else {
                Self::shutdown(this, vm, Some(this.destination.stored_error()));
            }
        }

        this.shutting_down.get()
    }

    fn check_for_forward_close(this: PipeToRef, vm: &Vm) -> bool {
        // 3. Closing must be propagated forward: if source.[[state]] is or becomes "closed", then
        if this.source.state() == ReadableStreamState::Closed {
            // 1. If preventClose is false, shutdown with an action of
            //    ! WritableStreamDefaultWriterCloseWithErrorPropagation(writer).
            if !this.prevent_close {
                Self::shutdown_with_action(this, vm, ShutdownAction::CloseDestination, None);
            }
            // 2. Otherwise, shutdown.
            else {
                Self::shutdown(this, vm, None);
            }
        }

        this.shutting_down.get()
    }

    fn check_for_backward_close(this: PipeToRef, vm: &Vm) -> bool {
        // 4. Closing must be propagated backward: if ! WritableStreamCloseQueuedOrInFlight(dest) is true or
        //    dest.[[state]] is "closed", then
        if writable_stream_close_queued_or_in_flight(this.destination)
            || this.destination.state() == WritableStreamState::Closed
        {
            // 1. Assert: no chunks have been read or written.

            // 2. Let destClosed be a new TypeError.
            let destination_closed = exception_value(
                vm,
                promise_realm(this.promise),
                Exception::type_error("Destination stream was closed during piping operation"),
            );

            // 3. If preventCancel is false, shutdown with an action of ! ReadableStreamCancel(source, destClosed)
            //    and with destClosed.
            if !this.prevent_cancel {
                Self::shutdown_with_action(
                    this,
                    vm,
                    ShutdownAction::CancelSource(destination_closed),
                    Some(destination_closed),
                );
            }
            // 4. Otherwise, shutdown with destClosed.
            else {
                Self::shutdown(this, vm, Some(destination_closed));
            }
        }

        this.shutting_down.get()
    }
}

// https://streams.spec.whatwg.org/#readable-stream-pipe-to
pub(crate) fn readable_stream_pipe_to(
    cx: &CallContext,
    source: Ref<ReadableStream>,
    destination: Ref<WritableStream>,
    prevent_close: bool,
    prevent_abort: bool,
    prevent_cancel: bool,
    signal: Option<CppInterface>,
) -> WebIdlPromise {
    let vm = cx.vm();

    // 1. Assert: source implements ReadableStream.
    // 2. Assert: dest implements WritableStream.
    // 3. Assert: preventClose, preventAbort, and preventCancel are all booleans.

    // 4. If signal was not given, let signal be undefined.
    // 5. Assert: either signal is undefined, or signal implements AbortSignal.
    let signal = signal.map(AbortSignal::from_wrapper);

    // 6. Assert: ! IsReadableStreamLocked(source) is false.
    assert!(!is_readable_stream_locked(source));

    // 7. Assert: ! IsWritableStreamLocked(dest) is false.
    assert!(!is_writable_stream_locked(destination));

    // 8. If source.[[controller]] implements ReadableByteStreamController, let reader be either
    //    ! AcquireReadableStreamBYOBReader(source) or ! AcquireReadableStreamDefaultReader(source), at the user
    //    agent’s discretion.
    // 9. Otherwise, let reader be ! AcquireReadableStreamDefaultReader(source).
    let reader = acquire_readable_stream_default_reader(cx, source).must();

    // 10. Let writer be ! AcquireWritableStreamDefaultWriter(dest).
    let writer = acquire_writable_stream_default_writer(cx, destination).must();

    // 11. Set source.[[disturbed]] to true.
    source.set_disturbed(true);

    // 12. Let shuttingDown be false.
    // NOTE: This is internal to the state of the pipe.

    // 13. Let promise be a new promise.
    let promise = create_promise(vm, cx.realm());

    let this = PIPE_TO_CLASS.allocate(
        vm,
        PipeTo {
            promise,
            source,
            destination,
            reader,
            writer,
            signal: GcRefCell::new(None),
            signal_algorithm_id: Cell::new(None),
            last_write_promise: Cell::new(None),
            unwritten_chunks: GcRefCell::new(VecDeque::new()),
            prevent_close,
            prevent_abort,
            prevent_cancel,
            shutting_down: Cell::new(false),
        },
    );

    reader.set_pipe_to(Some(this));

    if let Some(reader_closed_promise) = reader.generic().closed_promise() {
        react_to_promise(
            vm,
            reader_closed_promise,
            this,
            Some(PipeTo::on_shutdown),
            Some(PipeTo::on_shutdown),
        );
    }
    if let Some(writer_closed_promise) = writer.closed_promise() {
        react_to_promise(
            vm,
            writer_closed_promise,
            this,
            Some(PipeTo::on_shutdown),
            Some(PipeTo::on_shutdown),
        );
    }

    // 14. If signal is not undefined,
    if let Some(signal) = signal {
        // 1. Let abortAlgorithm be the following steps:

        // 2. If signal is aborted, perform abortAlgorithm and return promise.
        if signal.aborted() {
            PipeTo::abort_algorithm(this, vm, &signal);
            return promise;
        }

        // 3. Add abortAlgorithm to signal.
        let abort_algorithm = NativeFunction::create(
            vm,
            (this, signal.clone()),
            |vm: &Vm, (this, signal): &(PipeToRef, AbortSignal)| {
                PipeTo::abort_algorithm(*this, vm, signal);
                Ok(Value::UNDEFINED)
            },
            0,
            &PropertyKey::from_utf8(""),
            Some(cx.realm()),
            None,
            None,
        );
        let signal_id = signal
            .add_algorithm(abort_algorithm)
            .expect("a signal that is not aborted takes algorithms");
        *this.signal.borrow_mut() = Some(signal);
        this.signal_algorithm_id.set(Some(signal_id));
    }

    // 15. In parallel (but not really; see #905), using reader and writer, read all chunks from source and write them
    //     to dest. Due to the locking provided by the reader and writer, the exact manner in which this happens is not
    //     observable to author code, and so there is flexibility in how this is done. The following constraints apply
    //     regardless of the exact algorithm used:
    PipeTo::process(this, vm);

    // 16. Return promise.
    promise
}
