/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Read requests, https://streams.spec.whatwg.org/#read-request, and read-into requests,
//! https://streams.spec.whatwg.org/#read-into-request: one variant for each place that defines one, holding what its
//! steps refer to. The steps of a variant are next to the algorithm that defines them.

use libjs_rust::api::{EmbedderCell, Gc, Realm, Trace, Value, Vm, create_iterator_result_object};

use crate::bindings::interface::Ref;
use crate::bindings::promise::{WebIdlPromise, reject_promise, resolve_promise};
use crate::streams::cpp::CppReadRequest;
use crate::streams::readable_stream::async_iterator_read_request_steps;
use crate::streams::readable_stream_default_reader::{ReadLoop, ReadableStreamDefaultReader};
use crate::streams::readable_stream_pipe_to::PipeTo;
use crate::streams::readable_stream_tee::{ByteTeeBYOBReadRequest, ByteTeeState, DefaultTeeState};

/// A read request, https://streams.spec.whatwg.org/#read-request
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ReadRequest {
    /// The read request of ReadableStreamDefaultReader's read(), https://streams.spec.whatwg.org/#default-reader-read
    DefaultReaderRead(WebIdlPromise),
    /// The read request of a read-loop, https://streams.spec.whatwg.org/#read-loop
    ReadLoop(Gc<EmbedderCell<ReadLoop>>),
    /// The read request of getting the next iteration result of a ReadableStream's asynchronous iterator,
    /// https://streams.spec.whatwg.org/#ref-for-dfn-get-the-next-iteration-result
    AsyncIterator {
        reader: Ref<ReadableStreamDefaultReader>,
        promise: WebIdlPromise,
    },
    /// The read request of ReadableStreamDefaultTee's pullAlgorithm.
    DefaultTee(Gc<EmbedderCell<DefaultTeeState>>),
    /// The read request of ReadableByteStreamTee's pullWithDefaultReader.
    ByteTee(Gc<EmbedderCell<ByteTeeState>>),
    /// The read request through which ReadableStreamPipeTo reads chunks from its source.
    PipeTo(Gc<EmbedderCell<PipeTo>>),
    /// A read request of LibWeb's C++ code.
    Cpp(CppReadRequest),
}

impl ReadRequest {
    // chunk steps, given chunk
    pub(crate) fn chunk_steps(&self, vm: &Vm, chunk: Value) {
        match self {
            Self::DefaultReaderRead(promise) => {
                // Resolve promise with «[ "value" → chunk, "done" → false ]».
                let result = create_iterator_result_object(vm, current_realm(vm), chunk, false);
                resolve_promise(vm, *promise, Value::from_object(result));
            }
            Self::ReadLoop(read_loop) => ReadLoop::chunk_steps(*read_loop, vm, chunk),
            Self::AsyncIterator { reader, promise } => {
                async_iterator_read_request_steps::chunk_steps(vm, *reader, *promise, chunk);
            }
            Self::DefaultTee(state) => DefaultTeeState::read_request_chunk_steps(*state, vm, chunk),
            Self::ByteTee(state) => ByteTeeState::default_read_request_chunk_steps(*state, vm, chunk),
            Self::PipeTo(pipe_to) => PipeTo::read_request_chunk_steps(*pipe_to, vm, chunk),
            Self::Cpp(read_request) => read_request.chunk_steps(chunk),
        }
    }

    // close steps
    pub(crate) fn close_steps(&self, vm: &Vm) {
        match self {
            Self::DefaultReaderRead(promise) => {
                // Resolve promise with «[ "value" → undefined, "done" → true ]».
                let result = create_iterator_result_object(vm, current_realm(vm), Value::UNDEFINED, true);
                resolve_promise(vm, *promise, Value::from_object(result));
            }
            Self::ReadLoop(read_loop) => read_loop.close_steps(),
            Self::AsyncIterator { reader, promise } => {
                async_iterator_read_request_steps::close_steps(vm, *reader, *promise);
            }
            Self::DefaultTee(state) => DefaultTeeState::read_request_close_steps(*state, vm),
            Self::ByteTee(state) => ByteTeeState::default_read_request_close_steps(*state, vm),
            Self::PipeTo(pipe_to) => PipeTo::read_request_close_steps(*pipe_to, vm),
            Self::Cpp(read_request) => read_request.close_steps(),
        }
    }

    // error steps, given e
    pub(crate) fn error_steps(&self, vm: &Vm, error: Value) {
        match self {
            // Reject promise with e.
            Self::DefaultReaderRead(promise) => reject_promise(vm, *promise, error),
            Self::ReadLoop(read_loop) => read_loop.error_steps(error),
            Self::AsyncIterator { reader, promise } => {
                async_iterator_read_request_steps::error_steps(vm, *reader, *promise, error);
            }
            Self::DefaultTee(state) => DefaultTeeState::read_request_error_steps(*state, vm, error),
            Self::ByteTee(state) => ByteTeeState::default_read_request_error_steps(*state, vm, error),
            Self::PipeTo(pipe_to) => PipeTo::read_request_error_steps(*pipe_to, vm, error),
            Self::Cpp(read_request) => read_request.error_steps(error),
        }
    }
}

/// A read-into request, https://streams.spec.whatwg.org/#read-into-request
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ReadIntoRequest {
    /// The read-into request of ReadableStreamBYOBReader's read(), https://streams.spec.whatwg.org/#byob-reader-read
    BYOBReaderRead(WebIdlPromise),
    /// The read-into request of ReadableByteStreamTee's pullWithBYOBReader.
    ByteTee(Gc<EmbedderCell<ByteTeeBYOBReadRequest>>),
}

impl ReadIntoRequest {
    // chunk steps, given chunk
    pub(crate) fn chunk_steps(&self, vm: &Vm, chunk: Value) {
        match self {
            Self::BYOBReaderRead(promise) => {
                // 1. Resolve promise with «[ "value" → chunk, "done" → false ]».
                let result = create_iterator_result_object(vm, current_realm(vm), chunk, false);
                resolve_promise(vm, *promise, Value::from_object(result));
            }
            Self::ByteTee(read_into_request) => read_into_request.chunk_steps(vm, chunk),
        }
    }

    // close steps, given chunk
    pub(crate) fn close_steps(&self, vm: &Vm, chunk: Value) {
        match self {
            Self::BYOBReaderRead(promise) => {
                // 1. Resolve promise with «[ "value" → chunk, "done" → true ]».
                let result = create_iterator_result_object(vm, current_realm(vm), chunk, true);
                resolve_promise(vm, *promise, Value::from_object(result));
            }
            Self::ByteTee(read_into_request) => read_into_request.close_steps(vm, chunk),
        }
    }

    // error steps, given e
    pub(crate) fn error_steps(&self, vm: &Vm, error: Value) {
        match self {
            // 1. Reject promise with e.
            Self::BYOBReaderRead(promise) => reject_promise(vm, *promise, error),
            Self::ByteTee(read_into_request) => read_into_request.error_steps(vm, error),
        }
    }
}

/// The current realm, which resolving a promise with the read result converts the result in: the realm of the function
/// that ran the request's steps, as https://webidl.spec.whatwg.org/#js-dictionary creates the object with its own
/// %Object.prototype%.
fn current_realm(vm: &Vm) -> Gc<Realm> {
    vm.current_realm().expect("a read request runs in a realm")
}
