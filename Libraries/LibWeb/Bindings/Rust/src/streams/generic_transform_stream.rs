/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The TransformStream of an interface that includes GenericTransformStream,
//! https://streams.spec.whatwg.org/#generictransformstream, which the interface sets up with steps of its own.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{EmbedderCell, EmbedderValue, ForeignCellSlot, Gc, Trace, Value};

use crate::bindings::interface::{Ref, allocate, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{WebIdlPromise, create_rejected_promise, create_resolved_promise, exception_value};
use crate::bindings::{CallContext, WebIdlResult};
use crate::streams::algorithms::{CancelAlgorithm, FlushAlgorithm, TransformAlgorithm};
use crate::streams::transform_stream::TransformStream;

/// The transformAlgorithm and the flushAlgorithm that an interface sets its TransformStream up with,
/// https://streams.spec.whatwg.org/#transformstream-set-up, which live in a cell of their own. An exception that they
/// throw rejects the promise of the algorithm.
pub(crate) trait TransformSteps: EmbedderValue {
    fn transform(&self, cx: &CallContext, transform: Ref<TransformStream>, chunk: Value) -> WebIdlResult<()>;

    fn flush(&self, cx: &CallContext, transform: Ref<TransformStream>) -> WebIdlResult<()>;
}

type TransformEntry = unsafe fn(&CallContext, NonNull<c_void>, Ref<TransformStream>, Value) -> WebIdlResult<()>;
type FlushEntry = unsafe fn(&CallContext, NonNull<c_void>, Ref<TransformStream>) -> WebIdlResult<()>;

/// The steps of an implementation of TransformSteps, with the stream that they were set up for. The steps run in the
/// realm of the stream, as the callbacks of a transformer run in their own realm, so what they enqueue is created there.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RustTransformSteps {
    steps: ForeignCellSlot,
    stream: Ref<TransformStream>,
    #[gc(untraced)]
    transform: TransformEntry,
    #[gc(untraced)]
    flush: FlushEntry,
}

impl Clone for RustTransformSteps {
    fn clone(&self) -> Self {
        let steps = ForeignCellSlot::empty();
        // SAFETY: The slot of `self` keeps the cell alive.
        unsafe { steps.set(Some(NonNull::new(self.steps.as_ptr()).expect("the steps are a cell"))) };
        Self {
            steps,
            stream: self.stream,
            transform: self.transform,
            flush: self.flush,
        }
    }
}

impl RustTransformSteps {
    fn new<T: TransformSteps>(steps: Gc<EmbedderCell<T>>, stream: Ref<TransformStream>) -> Self {
        let slot = ForeignCellSlot::empty();
        // SAFETY: The steps are a live cell.
        unsafe { slot.set(Some(steps.as_non_null().cast())) };
        Self {
            steps: slot,
            stream,
            transform: transform_entry::<T>,
            flush: flush_entry::<T>,
        }
    }

    fn steps(&self) -> NonNull<c_void> {
        NonNull::new(self.steps.as_ptr()).expect("the steps are a cell")
    }

    /// The transformAlgorithmWrapper of setting up the stream, given the chunk.
    pub(crate) fn transform(&self, cx: &CallContext, chunk: Value) -> WebIdlPromise {
        // 1. Let result be the result of running transformAlgorithm given chunk. If this throws an exception e, return
        //    a promise rejected with e.
        let result = with_temporary_execution_context_and_callbacks(cx.realm(), || {
            // SAFETY: The entry is that of the class of the steps.
            unsafe { (self.transform)(cx, self.steps(), self.stream, chunk) }
        });
        promise_for(cx, result)
    }

    /// The flushAlgorithmWrapper of setting up the stream.
    pub(crate) fn flush(&self, cx: &CallContext) -> WebIdlPromise {
        // 1. Let result be the result of running flushAlgorithm, if flushAlgorithm was given, or null otherwise. If
        //    this throws an exception e, return a promise rejected with e.
        let result = with_temporary_execution_context_and_callbacks(cx.realm(), || {
            // SAFETY: The entry is that of the class of the steps.
            unsafe { (self.flush)(cx, self.steps(), self.stream) }
        });
        promise_for(cx, result)
    }
}

/// A promise rejected with the exception that steps threw, or else, as their result is not a promise, a promise
/// resolved with undefined.
fn promise_for(cx: &CallContext, result: WebIdlResult<()>) -> WebIdlPromise {
    match result {
        Err(exception) => {
            let reason = exception_value(cx.vm(), cx.realm(), exception);
            create_rejected_promise(cx.vm(), cx.realm(), reason)
        }
        // 2. If result is a Promise, then return result.
        // 3. Return a promise resolved with undefined.
        Ok(()) => create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED),
    }
}

/// # Safety
///
/// `steps` must be a live cell of the class of `T`.
unsafe fn transform_entry<T: TransformSteps>(
    cx: &CallContext,
    steps: NonNull<c_void>,
    stream: Ref<TransformStream>,
    chunk: Value,
) -> WebIdlResult<()> {
    // SAFETY: The caller passes a live cell of the class of T.
    let steps = unsafe { T::CLASS.downcast(steps) }.expect("the steps are of the class they were set up with");
    let _type_error_realm = cx.vm().type_error_realm_scope(cx.realm());
    steps.transform(cx, stream, chunk)
}

/// # Safety
///
/// `steps` must be a live cell of the class of `T`.
unsafe fn flush_entry<T: TransformSteps>(
    cx: &CallContext,
    steps: NonNull<c_void>,
    stream: Ref<TransformStream>,
) -> WebIdlResult<()> {
    // SAFETY: The caller passes a live cell of the class of T.
    let steps = unsafe { T::CLASS.downcast(steps) }.expect("the steps are of the class they were set up with");
    let _type_error_realm = cx.vm().type_error_realm_scope(cx.realm());
    steps.flush(cx, stream)
}

/// A new TransformStream, set up with the steps, https://streams.spec.whatwg.org/#transformstream-set-up
pub(crate) fn create_and_set_up<T: TransformSteps>(cx: &CallContext, steps: T) -> Ref<TransformStream> {
    let steps = T::CLASS.allocate(cx.vm(), |_| steps);
    let stream = allocate(cx.vm(), |_| TransformStream::new(cx.realm()));
    let algorithms = RustTransformSteps::new(steps, stream);
    TransformStream::set_up(
        stream,
        cx,
        TransformAlgorithm::RustWrapper(algorithms.clone()),
        FlushAlgorithm::RustWrapper(algorithms),
        CancelAlgorithm::Wrapper(None),
    );
    stream
}
