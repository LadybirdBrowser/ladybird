/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The algorithms that streams and their controllers are set up with: one variant for each way the Streams Standard,
//! or a specification that sets a stream up, defines one, holding what its steps refer to. The steps of a variant are
//! next to the abstract operation that defines them.

use libjs_rust::api::{EmbedderCell, Gc, IteratorRecord, ThrowCompletionOr, Trace, Value};

use crate::bindings::callback::{CallbackFunction, PromiseCallbackFunction};
use crate::bindings::conversions::to_unrestricted_double;
use crate::bindings::interface::{Ref, wrap};
use crate::bindings::promise::{WebIdlPromise, create_rejected_promise, create_resolved_promise};
use crate::bindings::{CallContext, WebIdlResult};
use crate::streams::cpp::{CppPromiseAlgorithm, CppPromiseAlgorithmWithArgument};
use crate::streams::cross_realm_transform::{CrossRealmTransformPort, CrossRealmWritableState};
use crate::streams::generic_transform_stream::RustTransformSteps;
use crate::streams::readable_stream::{
    ReadableStream, readable_stream_from_iterable_cancel_algorithm, readable_stream_from_iterable_pull_algorithm,
};
use crate::streams::readable_stream_tee::{ByteTeeState, DefaultTeeState};
use crate::streams::transform_stream::{
    TransformStream, transform_stream_default_sink_abort_algorithm, transform_stream_default_sink_close_algorithm,
    transform_stream_default_sink_write_algorithm, transform_stream_default_source_cancel_algorithm,
    transform_stream_default_source_pull_algorithm,
};
use crate::streams::transform_stream_default_controller::{
    TransformStreamDefaultController, transform_stream_default_controller_enqueue,
};
use crate::streams::underlying_source::ReadableStreamController;
use crate::streams::writable_stream_default_controller::WritableStreamDefaultController;

/// A promise resolved with undefined in the realm of the context.
fn resolved_with_undefined(cx: &CallContext) -> WebIdlPromise {
    create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
}

/// The startAlgorithm of a stream, which its set-up runs once.
pub(crate) enum StartAlgorithm {
    /// An algorithm that returns undefined.
    ReturnsUndefined,
    /// An algorithm that returns the result of invoking `start` with argument list « controller » and callback this
    /// value `this_value`.
    Callback {
        start: CallbackFunction,
        this_value: Value,
        controller: Value,
    },
    /// An algorithm that returns the promise.
    ReturnsPromise(WebIdlPromise),
}

impl StartAlgorithm {
    pub(crate) fn run(self) -> WebIdlResult<Value> {
        match self {
            Self::ReturnsUndefined => Ok(Value::UNDEFINED),
            Self::Callback {
                start,
                this_value,
                controller,
            } => Ok(start.invoke(Some(this_value), &[controller])?),
            Self::ReturnsPromise(promise) => Ok(Value::from_object(promise.promise())),
        }
    }
}

/// The pullAlgorithm of a readable stream's controller, https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-pullalgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum PullAlgorithm {
    /// An algorithm that returns a promise resolved with undefined.
    ReturnsResolvedUndefined,
    /// An algorithm that returns the result of invoking `pull` with argument list « controller » and callback this
    /// value `underlying_source`.
    Callback {
        pull: PromiseCallbackFunction,
        underlying_source: Value,
        controller: ReadableStreamController,
    },
    /// The pullAlgorithm of ReadableStreamFromIterable.
    FromIterable {
        stream: Ref<ReadableStream>,
        iterator_record: Gc<IteratorRecord>,
    },
    /// The pullAlgorithm of ReadableStreamDefaultTee.
    DefaultTee(Gc<EmbedderCell<DefaultTeeState>>),
    /// The pull1Algorithm or the pull2Algorithm of ReadableByteStreamTee.
    ByteTee {
        state: Gc<EmbedderCell<ByteTeeState>>,
        for_branch2: bool,
    },
    /// The pullAlgorithm of InitializeTransformStream.
    TransformStream(Ref<TransformStream>),
    /// The pullAlgorithm of SetUpCrossRealmTransformReadable.
    CrossRealm(CrossRealmTransformPort),
    /// The pullAlgorithmWrapper of setting up a ReadableStream with byte reading support, given the pullAlgorithm that
    /// C++ set the stream up with, if any.
    WithByteReadingSupport(Option<CppPromiseAlgorithm>),
}

impl PullAlgorithm {
    /// Performs the algorithm, with the context of the realm of the stream.
    pub(crate) fn run(&self, cx: &CallContext) -> WebIdlPromise {
        match self {
            Self::ReturnsResolvedUndefined => resolved_with_undefined(cx),
            Self::Callback {
                pull,
                underlying_source,
                controller,
            } => pull.invoke(Some(*underlying_source), &[controller.to_js(cx)]),
            Self::FromIterable {
                stream,
                iterator_record,
            } => readable_stream_from_iterable_pull_algorithm(cx, *stream, *iterator_record),
            Self::DefaultTee(state) => DefaultTeeState::pull_algorithm(*state, cx),
            Self::ByteTee { state, for_branch2 } => {
                if *for_branch2 {
                    ByteTeeState::pull2_algorithm(*state, cx)
                } else {
                    ByteTeeState::pull1_algorithm(*state, cx)
                }
            }
            Self::TransformStream(stream) => transform_stream_default_source_pull_algorithm(cx, *stream),
            Self::CrossRealm(port) => port.pull_algorithm(cx),
            Self::WithByteReadingSupport(pull_algorithm) => {
                // 1. Let result be the result of running pullAlgorithm, if pullAlgorithm was given, or null otherwise.
                //    If this throws an exception e, return a promise rejected with e.
                // 2. If result is a Promise, then return result.
                if let Some(pull_algorithm) = pull_algorithm {
                    return pull_algorithm.call();
                }

                // 3. Return a promise resolved with undefined.
                resolved_with_undefined(cx)
            }
        }
    }
}

/// The cancelAlgorithm of a readable stream's controller or of a TransformStreamDefaultController,
/// https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-cancelalgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CancelAlgorithm {
    /// An algorithm that returns a promise resolved with undefined.
    ReturnsResolvedUndefined,
    /// An algorithm that takes an argument reason and returns the result of invoking `cancel` with argument list
    /// « reason » and callback this value `this_value`.
    Callback {
        cancel: PromiseCallbackFunction,
        this_value: Value,
    },
    /// The cancelAlgorithm of ReadableStreamFromIterable.
    FromIterable { iterator_record: Gc<IteratorRecord> },
    /// The cancel1Algorithm or the cancel2Algorithm of ReadableStreamDefaultTee.
    DefaultTee {
        state: Gc<EmbedderCell<DefaultTeeState>>,
        for_branch2: bool,
    },
    /// The cancel1Algorithm or the cancel2Algorithm of ReadableByteStreamTee.
    ByteTee {
        state: Gc<EmbedderCell<ByteTeeState>>,
        for_branch2: bool,
    },
    /// The cancelAlgorithm of InitializeTransformStream.
    TransformStream(Ref<TransformStream>),
    /// The cancelAlgorithm of SetUpCrossRealmTransformReadable.
    CrossRealm(CrossRealmTransformPort),
    /// The cancelAlgorithmWrapper of setting up a ReadableStream with byte reading support or a TransformStream, given
    /// the cancelAlgorithm that C++ set the stream up with, if any.
    Wrapper(Option<CppPromiseAlgorithmWithArgument>),
}

impl CancelAlgorithm {
    /// Performs the algorithm given the reason, with the context of the realm of the stream.
    pub(crate) fn run(&self, cx: &CallContext, reason: Value) -> WebIdlPromise {
        match self {
            Self::ReturnsResolvedUndefined => resolved_with_undefined(cx),
            Self::Callback { cancel, this_value } => cancel.invoke(Some(*this_value), &[reason]),
            Self::FromIterable { iterator_record } => {
                readable_stream_from_iterable_cancel_algorithm(cx, *iterator_record, reason)
            }
            Self::DefaultTee { state, for_branch2 } => {
                if *for_branch2 {
                    DefaultTeeState::cancel2_algorithm(*state, cx, reason)
                } else {
                    DefaultTeeState::cancel1_algorithm(*state, cx, reason)
                }
            }
            Self::ByteTee { state, for_branch2 } => {
                if *for_branch2 {
                    ByteTeeState::cancel2_algorithm(*state, cx, reason)
                } else {
                    ByteTeeState::cancel1_algorithm(*state, cx, reason)
                }
            }
            Self::TransformStream(stream) => transform_stream_default_source_cancel_algorithm(cx, *stream, reason),
            Self::CrossRealm(port) => port.cancel_algorithm(cx, reason),
            Self::Wrapper(cancel_algorithm) => {
                // 1. Let result be the result of running cancelAlgorithm given reason, if cancelAlgorithm was given,
                //    or null otherwise. If this throws an exception e, return a promise rejected with e.
                // 2. If result is a Promise, then return result.
                if let Some(cancel_algorithm) = cancel_algorithm {
                    return cancel_algorithm.call(reason);
                }

                // 3. Return a promise resolved with undefined.
                resolved_with_undefined(cx)
            }
        }
    }
}

/// The strategySizeAlgorithm of a controller, https://streams.spec.whatwg.org/#readablestreamdefaultcontroller-strategysizealgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum SizeAlgorithm {
    /// An algorithm that returns 1.
    ReturnsOne,
    /// An algorithm that returns the result of invoking strategy["size"] with argument list « chunk ».
    Strategy(CallbackFunction),
}

impl SizeAlgorithm {
    pub(crate) fn run(&self, cx: &CallContext, chunk: Value) -> ThrowCompletionOr<Value> {
        match self {
            Self::ReturnsOne => Ok(Value::from_i32(1)),
            Self::Strategy(size) => {
                let size = size.invoke(None, &[chunk])?;
                Ok(Value::from_f64(to_unrestricted_double(cx.vm(), size)?))
            }
        }
    }
}

/// The writeAlgorithm of a WritableStreamDefaultController, https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-writealgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum WriteAlgorithm {
    /// An algorithm that returns a promise resolved with undefined.
    ReturnsResolvedUndefined,
    /// An algorithm that takes an argument chunk and returns the result of invoking `write` with argument list
    /// « chunk, controller » and callback this value `underlying_sink`.
    Callback {
        write: PromiseCallbackFunction,
        underlying_sink: Value,
        controller: Ref<WritableStreamDefaultController>,
    },
    /// The writeAlgorithm of InitializeTransformStream.
    TransformStream(Ref<TransformStream>),
    /// The writeAlgorithm of SetUpCrossRealmTransformWritable.
    CrossRealm(Gc<EmbedderCell<CrossRealmWritableState>>),
}

impl WriteAlgorithm {
    /// Performs the algorithm given the chunk, with the context of the realm of the stream.
    pub(crate) fn run(&self, cx: &CallContext, chunk: Value) -> WebIdlPromise {
        match self {
            Self::ReturnsResolvedUndefined => resolved_with_undefined(cx),
            Self::Callback {
                write,
                underlying_sink,
                controller,
            } => {
                let controller = Value::from_object(wrap(cx, *controller));
                write.invoke(Some(*underlying_sink), &[chunk, controller])
            }
            Self::TransformStream(stream) => transform_stream_default_sink_write_algorithm(cx, *stream, chunk),
            Self::CrossRealm(state) => CrossRealmWritableState::write_algorithm(*state, cx, chunk),
        }
    }
}

/// The closeAlgorithm of a WritableStreamDefaultController, https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-closealgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CloseAlgorithm {
    /// An algorithm that returns a promise resolved with undefined.
    ReturnsResolvedUndefined,
    /// An algorithm that returns the result of invoking `close` with argument list « » and callback this value
    /// `underlying_sink`.
    Callback {
        close: PromiseCallbackFunction,
        underlying_sink: Value,
    },
    /// The closeAlgorithm of InitializeTransformStream.
    TransformStream(Ref<TransformStream>),
    /// The closeAlgorithm of SetUpCrossRealmTransformWritable.
    CrossRealm(CrossRealmTransformPort),
}

impl CloseAlgorithm {
    /// Performs the algorithm, with the context of the realm of the stream.
    pub(crate) fn run(&self, cx: &CallContext) -> WebIdlPromise {
        match self {
            Self::ReturnsResolvedUndefined => resolved_with_undefined(cx),
            Self::Callback { close, underlying_sink } => close.invoke(Some(*underlying_sink), &[]),
            Self::TransformStream(stream) => transform_stream_default_sink_close_algorithm(cx, *stream),
            Self::CrossRealm(port) => port.close_algorithm(cx),
        }
    }
}

/// The abortAlgorithm of a WritableStreamDefaultController, https://streams.spec.whatwg.org/#writablestreamdefaultcontroller-abortalgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AbortAlgorithm {
    /// An algorithm that returns a promise resolved with undefined.
    ReturnsResolvedUndefined,
    /// An algorithm that takes an argument reason and returns the result of invoking `abort` with argument list
    /// « reason » and callback this value `underlying_sink`.
    Callback {
        abort: PromiseCallbackFunction,
        underlying_sink: Value,
    },
    /// The abortAlgorithm of InitializeTransformStream.
    TransformStream(Ref<TransformStream>),
    /// The abortAlgorithm of SetUpCrossRealmTransformWritable.
    CrossRealm(CrossRealmTransformPort),
}

impl AbortAlgorithm {
    /// Performs the algorithm given the reason, with the context of the realm of the stream.
    pub(crate) fn run(&self, cx: &CallContext, reason: Value) -> WebIdlPromise {
        match self {
            Self::ReturnsResolvedUndefined => resolved_with_undefined(cx),
            Self::Callback { abort, underlying_sink } => abort.invoke(Some(*underlying_sink), &[reason]),
            Self::TransformStream(stream) => transform_stream_default_sink_abort_algorithm(cx, *stream, reason),
            Self::CrossRealm(port) => port.abort_algorithm(cx, reason),
        }
    }
}

/// The transformAlgorithm of a TransformStreamDefaultController, https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-transformalgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum TransformAlgorithm {
    /// An algorithm that takes an argument chunk and returns the result of invoking `transform` with argument list
    /// « chunk, controller » and callback this value `transformer`.
    Callback {
        transform: PromiseCallbackFunction,
        transformer: Value,
        controller: Ref<TransformStreamDefaultController>,
    },
    /// The transformAlgorithm of SetUpTransformStreamDefaultControllerFromTransformer for a transformer without a
    /// transform member, which enqueues the chunk.
    Enqueue(Ref<TransformStreamDefaultController>),
    /// The transformAlgorithmWrapper of setting up a TransformStream, given the transformAlgorithm that C++ set the
    /// stream up with.
    CppWrapper(CppPromiseAlgorithmWithArgument),
    /// The transformAlgorithmWrapper of setting up a TransformStream, given steps of an implementation in Rust.
    RustWrapper(RustTransformSteps),
}

impl TransformAlgorithm {
    /// Performs the algorithm given the chunk, with the context of the realm of the stream.
    pub(crate) fn run(&self, cx: &CallContext, chunk: Value) -> WebIdlPromise {
        match self {
            Self::Callback {
                transform,
                transformer,
                controller,
            } => {
                let controller = Value::from_object(wrap(cx, *controller));
                transform.invoke(Some(*transformer), &[chunk, controller])
            }
            Self::Enqueue(controller) => {
                // 1. Let result be TransformStreamDefaultControllerEnqueue(controller, chunk).
                // 2. If result is an abrupt completion, return a promise rejected with result.[[Value]].
                if let Err(exception) = transform_stream_default_controller_enqueue(cx, *controller, chunk) {
                    let reason = crate::bindings::promise::exception_value(cx.vm(), cx.realm(), exception);
                    return create_rejected_promise(cx.vm(), cx.realm(), reason);
                }

                // 3. Otherwise, return a promise resolved with undefined.
                resolved_with_undefined(cx)
            }
            // 1. Let result be the result of running transformAlgorithm given chunk. If this throws an exception e,
            //    return a promise rejected with e.
            // 2. If result is a Promise, then return result.
            Self::CppWrapper(transform_algorithm) => transform_algorithm.call(chunk),
            Self::RustWrapper(steps) => steps.transform(cx, chunk),
        }
    }
}

/// The flushAlgorithm of a TransformStreamDefaultController, https://streams.spec.whatwg.org/#transformstreamdefaultcontroller-flushalgorithm
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum FlushAlgorithm {
    /// An algorithm that returns a promise resolved with undefined.
    ReturnsResolvedUndefined,
    /// An algorithm that returns the result of invoking `flush` with argument list « controller » and callback this
    /// value `transformer`.
    Callback {
        flush: PromiseCallbackFunction,
        transformer: Value,
        controller: Ref<TransformStreamDefaultController>,
    },
    /// The flushAlgorithmWrapper of setting up a TransformStream, given the flushAlgorithm that C++ set the stream up
    /// with, if any.
    CppWrapper(Option<CppPromiseAlgorithm>),
    /// The flushAlgorithmWrapper of setting up a TransformStream, given steps of an implementation in Rust.
    RustWrapper(RustTransformSteps),
}

impl FlushAlgorithm {
    /// Performs the algorithm, with the context of the realm of the stream.
    pub(crate) fn run(&self, cx: &CallContext) -> WebIdlPromise {
        match self {
            Self::ReturnsResolvedUndefined => resolved_with_undefined(cx),
            Self::Callback {
                flush,
                transformer,
                controller,
            } => {
                let controller = Value::from_object(wrap(cx, *controller));
                flush.invoke(Some(*transformer), &[controller])
            }
            Self::CppWrapper(flush_algorithm) => {
                // 1. Let result be the result of running flushAlgorithm, if flushAlgorithm was given, or null
                //    otherwise. If this throws an exception e, return a promise rejected with e.
                // 2. If result is a Promise, then return result.
                if let Some(flush_algorithm) = flush_algorithm {
                    return flush_algorithm.call();
                }

                // 3. Return a promise resolved with undefined.
                resolved_with_undefined(cx)
            }
            Self::RustWrapper(steps) => steps.flush(cx),
        }
    }
}
