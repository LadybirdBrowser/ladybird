/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cells of LibWeb's C++ code that streams hold: the algorithms that C++ sets a stream up with, and the read
//! requests that C++ reads a stream with, which are defined in Streams/ReadableStream.cpp.

use core::ffi::c_void;

use libjs_rust::api::{Trace, Value};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::promise::{WebIdlPromise, promise_from_abi};

unsafe extern "C" {
    /// Calls a GC::Function<GC::Ref<WebIDL::Promise>()>.
    fn web_streams_call_promise_algorithm(function: *mut c_void) -> *mut c_void;

    /// Calls a GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>.
    fn web_streams_call_promise_algorithm_with_argument(function: *mut c_void, argument: u64) -> *mut c_void;

    /// Performs the chunk steps of a Streams::ReadRequest.
    fn web_streams_read_request_chunk_steps(read_request: *mut c_void, chunk: u64);

    /// Performs the close steps of a Streams::ReadRequest.
    fn web_streams_read_request_close_steps(read_request: *mut c_void);

    /// Performs the error steps of a Streams::ReadRequest.
    fn web_streams_read_request_error_steps(read_request: *mut c_void, error: u64);

    /// Calls a GC::Function<void(ByteBuffer)> with a copy of the bytes.
    fn web_streams_call_bytes_steps(function: *mut c_void, bytes: ak::Span<'_, u8>);

    /// Calls a GC::Function<void(JS::Value)>.
    fn web_streams_call_value_steps(function: *mut c_void, value: u64);
}

/// A promise-returning algorithm with no arguments that C++ set a stream up with, a pullAlgorithm or a flushAlgorithm.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppPromiseAlgorithm(pub(crate) CppCell);

impl CppPromiseAlgorithm {
    pub(crate) fn call(&self) -> WebIdlPromise {
        // SAFETY: The cell is a live GC::Function<GC::Ref<WebIDL::Promise>()>, which returns a live promise.
        unsafe { promise_from_abi(web_streams_call_promise_algorithm(self.0.as_ptr())) }
    }
}

/// A promise-returning algorithm with an argument that C++ set a stream up with, a cancelAlgorithm or a
/// transformAlgorithm.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppPromiseAlgorithmWithArgument(pub(crate) CppCell);

impl CppPromiseAlgorithmWithArgument {
    pub(crate) fn call(&self, argument: Value) -> WebIdlPromise {
        // SAFETY: The cell is a live GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>, which returns a live promise.
        unsafe {
            promise_from_abi(web_streams_call_promise_algorithm_with_argument(
                self.0.as_ptr(),
                argument.to_raw(),
            ))
        }
    }
}

/// A read request of LibWeb's C++ code, https://streams.spec.whatwg.org/#read-request
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppReadRequest(pub(crate) CppCell);

impl CppReadRequest {
    pub(crate) fn chunk_steps(&self, chunk: Value) {
        // SAFETY: The cell is a live Streams::ReadRequest.
        unsafe { web_streams_read_request_chunk_steps(self.0.as_ptr(), chunk.to_raw()) };
    }

    pub(crate) fn close_steps(&self) {
        // SAFETY: The cell is a live Streams::ReadRequest.
        unsafe { web_streams_read_request_close_steps(self.0.as_ptr()) };
    }

    pub(crate) fn error_steps(&self, error: Value) {
        // SAFETY: The cell is a live Streams::ReadRequest.
        unsafe { web_streams_read_request_error_steps(self.0.as_ptr(), error.to_raw()) };
    }
}

/// Steps of LibWeb's C++ code that take a byte sequence.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppBytesSteps(pub(crate) CppCell);

impl CppBytesSteps {
    pub(crate) fn call(&self, bytes: &[u8]) {
        // SAFETY: The cell is a live GC::Function<void(ByteBuffer)>, which copies the bytes.
        unsafe { web_streams_call_bytes_steps(self.0.as_ptr(), ak::Span::new(bytes)) };
    }
}

/// Steps of LibWeb's C++ code that take a JS value.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppValueSteps(pub(crate) CppCell);

impl CppValueSteps {
    pub(crate) fn call(&self, value: Value) {
        // SAFETY: The cell is a live GC::Function<void(JS::Value)>.
        unsafe { web_streams_call_value_steps(self.0.as_ptr(), value.to_raw()) };
    }
}
