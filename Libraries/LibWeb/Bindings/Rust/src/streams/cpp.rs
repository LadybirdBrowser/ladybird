/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cells of LibWeb's C++ code that streams hold: the algorithms that C++ sets a stream up with, and the read
//! requests that C++ reads a stream with, which are defined in Streams/ReadableStream.cpp.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{ForeignCellSlot, Gc, Trace, Value};

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

/// A cell of LibWeb's C++ code, which this keeps alive.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppCell(ForeignCellSlot);

impl Clone for CppCell {
    fn clone(&self) -> Self {
        // SAFETY: The slot keeps the cell alive.
        unsafe { Self::from_raw(self.0.as_ptr()) }
    }
}

impl CppCell {
    /// # Safety
    ///
    /// `cell` must be a live cell of LibWeb's C++ code.
    pub(crate) unsafe fn from_raw(cell: *mut c_void) -> Self {
        let slot = ForeignCellSlot::empty();
        // SAFETY: The caller passes a live cell.
        unsafe { slot.set(Some(NonNull::new(cell).expect("C++ passes a cell"))) };
        Self(slot)
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }
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
                argument.0,
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
        unsafe { web_streams_read_request_chunk_steps(self.0.as_ptr(), chunk.0) };
    }

    pub(crate) fn close_steps(&self) {
        // SAFETY: The cell is a live Streams::ReadRequest.
        unsafe { web_streams_read_request_close_steps(self.0.as_ptr()) };
    }

    pub(crate) fn error_steps(&self, error: Value) {
        // SAFETY: The cell is a live Streams::ReadRequest.
        unsafe { web_streams_read_request_error_steps(self.0.as_ptr(), error.0) };
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
        unsafe { web_streams_call_value_steps(self.0.as_ptr(), value.0) };
    }
}

unsafe extern "C" {
    /// The DOM::AbortSignal that a wrapper wraps.
    fn web_streams_abort_signal_from_wrapper(wrapper: *mut c_void) -> *mut c_void;

    /// The wrapper of a DOM::AbortSignal in the realm.
    fn web_streams_abort_signal_wrapper(realm: *mut c_void, signal: *mut c_void) -> *mut c_void;

    /// A new DOM::AbortSignal.
    fn web_streams_create_abort_signal() -> *mut c_void;

    fn web_streams_abort_signal_aborted(signal: *mut c_void) -> bool;

    fn web_streams_abort_signal_reason(signal: *mut c_void) -> u64;

    /// Adds an abort algorithm that calls the function, a native function with no arguments, to the signal, and
    /// returns whether it did, with its identifier in `id`: a signal that is aborted takes no algorithms.
    fn web_streams_abort_signal_add_algorithm(signal: *mut c_void, function: *mut c_void, id: &mut u64) -> bool;

    fn web_streams_abort_signal_remove_algorithm(signal: *mut c_void, id: u64);

    /// Signals abort on the signal with the reason, https://dom.spec.whatwg.org/#abortsignal-signal-abort, given the
    /// global object of the realm.
    fn web_streams_abort_signal_signal_abort(signal: *mut c_void, reason: u64, realm: *mut c_void);
}

/// A DOM::AbortSignal, https://dom.spec.whatwg.org/#abortsignal
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AbortSignal(CppCell);

impl AbortSignal {
    /// The signal that a wrapper of an AbortSignal wraps.
    pub(crate) fn from_wrapper(wrapper: crate::bindings::conversions::CppInterface) -> Self {
        // SAFETY: The wrapper is a live wrapper of an AbortSignal, and LibWeb returns the signal it wraps.
        Self(unsafe { CppCell::from_raw(web_streams_abort_signal_from_wrapper(wrapper.wrapper().as_ptr().cast())) })
    }

    pub(crate) fn create() -> Self {
        // SAFETY: LibWeb returns a live signal.
        Self(unsafe { CppCell::from_raw(web_streams_create_abort_signal()) })
    }

    /// The wrapper of the signal in the realm.
    pub(crate) fn wrapper(&self, realm: Gc<libjs_rust::api::Realm>) -> crate::bindings::conversions::CppInterface {
        // SAFETY: The realm and the signal are live, and LibWeb returns the wrapper of the signal.
        let wrapper = unsafe { web_streams_abort_signal_wrapper(realm.as_ptr().cast(), self.0.as_ptr()) };
        let wrapper = NonNull::new(wrapper.cast::<libjs_rust::api::Object>()).expect("LibWeb returns a wrapper");
        // SAFETY: LibWeb returns a live wrapper of an AbortSignal.
        unsafe { crate::bindings::conversions::CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }

    // https://dom.spec.whatwg.org/#abortsignal-aborted
    pub(crate) fn aborted(&self) -> bool {
        // SAFETY: The signal is live.
        unsafe { web_streams_abort_signal_aborted(self.0.as_ptr()) }
    }

    // https://dom.spec.whatwg.org/#abortsignal-abort-reason
    pub(crate) fn reason(&self) -> Value {
        // SAFETY: The signal is live.
        Value(unsafe { web_streams_abort_signal_reason(self.0.as_ptr()) })
    }

    /// Adds the abort algorithm, a native function with no arguments, to the signal,
    /// https://dom.spec.whatwg.org/#abortsignal-add, returning its identifier, or None if the signal is aborted.
    pub(crate) fn add_algorithm(&self, algorithm: Gc<libjs_rust::api::NativeFunction>) -> Option<u64> {
        let mut id = 0;
        // SAFETY: The signal and the function are live.
        unsafe { web_streams_abort_signal_add_algorithm(self.0.as_ptr(), algorithm.as_ptr().cast(), &mut id) }
            .then_some(id)
    }

    // https://dom.spec.whatwg.org/#abortsignal-remove
    pub(crate) fn remove_algorithm(&self, id: u64) {
        // SAFETY: The signal is live.
        unsafe { web_streams_abort_signal_remove_algorithm(self.0.as_ptr(), id) };
    }

    // https://dom.spec.whatwg.org/#abortsignal-signal-abort
    pub(crate) fn signal_abort(&self, reason: Value, realm: Gc<libjs_rust::api::Realm>) {
        // SAFETY: The signal, the reason and the realm are live.
        unsafe { web_streams_abort_signal_signal_abort(self.0.as_ptr(), reason.0, realm.as_ptr().cast()) };
    }
}
