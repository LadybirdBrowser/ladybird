/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Trace, Value};

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    /// The DOM::AbortSignal that a wrapper wraps.
    fn web_dom_abort_signal_from_wrapper(wrapper: *mut c_void) -> *mut c_void;

    /// The wrapper of a DOM::AbortSignal in the realm.
    fn web_dom_abort_signal_wrapper(realm: *mut c_void, signal: *mut c_void) -> *mut c_void;

    /// A new DOM::AbortSignal.
    fn web_dom_create_abort_signal() -> *mut c_void;

    fn web_dom_abort_signal_aborted(signal: *mut c_void) -> bool;

    fn web_dom_abort_signal_reason(signal: *mut c_void) -> u64;

    /// Adds an abort algorithm that calls the function, a native function with no arguments, to the signal, and
    /// returns whether it did, with its identifier in `id`: a signal that is aborted takes no algorithms.
    fn web_dom_abort_signal_add_algorithm(signal: *mut c_void, function: *mut c_void, id: &mut u64) -> bool;

    fn web_dom_abort_signal_remove_algorithm(signal: *mut c_void, id: u64);

    /// Signals abort on the signal with the reason, https://dom.spec.whatwg.org/#abortsignal-signal-abort, given the
    /// global object of the realm.
    fn web_dom_abort_signal_signal_abort(signal: *mut c_void, reason: u64, realm: *mut c_void);
}

/// A DOM::AbortSignal, https://dom.spec.whatwg.org/#abortsignal
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AbortSignal(CppCell);

impl AbortSignal {
    /// The signal that a wrapper of an AbortSignal wraps.
    pub(crate) fn from_wrapper(wrapper: crate::bindings::conversions::CppInterface) -> Self {
        // SAFETY: The wrapper is a live wrapper of an AbortSignal, and LibWeb returns the signal it wraps.
        Self(unsafe { CppCell::from_raw(web_dom_abort_signal_from_wrapper(wrapper.wrapper().as_ptr().cast())) })
    }

    pub(crate) fn create() -> Self {
        // SAFETY: LibWeb returns a live signal.
        Self(unsafe { CppCell::from_raw(web_dom_create_abort_signal()) })
    }

    /// The wrapper of the signal in the realm.
    pub(crate) fn wrapper(&self, realm: Gc<libjs_rust::api::Realm>) -> crate::bindings::conversions::CppInterface {
        // SAFETY: The realm and the signal are live, and LibWeb returns the wrapper of the signal.
        let wrapper = unsafe { web_dom_abort_signal_wrapper(realm.as_ptr().cast(), self.0.as_ptr()) };
        let wrapper = NonNull::new(wrapper.cast::<libjs_rust::api::Object>()).expect("LibWeb returns a wrapper");
        // SAFETY: LibWeb returns a live wrapper of an AbortSignal.
        unsafe { crate::bindings::conversions::CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }

    // https://dom.spec.whatwg.org/#abortsignal-aborted
    pub(crate) fn aborted(&self) -> bool {
        // SAFETY: The signal is live.
        unsafe { web_dom_abort_signal_aborted(self.0.as_ptr()) }
    }

    // https://dom.spec.whatwg.org/#abortsignal-abort-reason
    pub(crate) fn reason(&self) -> Value {
        // SAFETY: The signal is live, and so is its reason.
        unsafe { Value::from_raw(web_dom_abort_signal_reason(self.0.as_ptr())) }
    }

    /// Adds the abort algorithm, a native function with no arguments, to the signal,
    /// https://dom.spec.whatwg.org/#abortsignal-add, returning its identifier, or None if the signal is aborted.
    pub(crate) fn add_algorithm(&self, algorithm: Gc<libjs_rust::api::NativeFunction>) -> Option<u64> {
        let mut id = 0;
        // SAFETY: The signal and the function are live.
        unsafe { web_dom_abort_signal_add_algorithm(self.0.as_ptr(), algorithm.as_ptr().cast(), &mut id) }.then_some(id)
    }

    // https://dom.spec.whatwg.org/#abortsignal-remove
    pub(crate) fn remove_algorithm(&self, id: u64) {
        // SAFETY: The signal is live.
        unsafe { web_dom_abort_signal_remove_algorithm(self.0.as_ptr(), id) };
    }

    // https://dom.spec.whatwg.org/#abortsignal-signal-abort
    pub(crate) fn signal_abort(&self, reason: Value, realm: Gc<libjs_rust::api::Realm>) {
        // SAFETY: The signal, the reason and the realm are live.
        unsafe { web_dom_abort_signal_signal_abort(self.0.as_ptr(), reason.to_raw(), realm.as_ptr().cast()) };
    }
}
