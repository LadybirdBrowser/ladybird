/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Handles to the C++ global objects and Documents.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Object, Trace};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::Wrappable;

unsafe extern "C" {
    fn web_bindings_window_from_global_object(global_object: *mut c_void) -> *mut c_void;
    fn web_bindings_window_associated_document(window: *mut c_void) -> *mut c_void;
    fn web_bindings_document_is_fully_active(document: *mut c_void) -> bool;
    fn web_bindings_worker_global_scope_from_global_object(global_object: *mut c_void) -> *mut c_void;
    fn web_bindings_worker_global_scope_is_closing(worker_global_scope: *mut c_void) -> bool;
}

/// https://html.spec.whatwg.org/multipage/nav-history-apis.html#window
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppWindow(CppCell);

/// https://html.spec.whatwg.org/multipage/workers.html#workerglobalscope
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppWorkerGlobalScope(CppCell);

/// https://dom.spec.whatwg.org/#document
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppDocument(CppCell);

/// The cell of a C++ object that LibWeb returns, or None for a null pointer.
///
/// # Safety
///
/// `cell` must be null or a live cell of LibWeb's C++ code.
unsafe fn cell_if_not_null(cell: *mut c_void) -> Option<CppCell> {
    // SAFETY: The caller passes a live cell.
    NonNull::new(cell).map(|cell| unsafe { CppCell::from_raw(cell.as_ptr()) })
}

/// https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-global
pub(crate) fn relevant_global_object<T: Wrappable>(object: &T) -> Gc<Object> {
    object
        .relevant_realm()
        .expect("the implementation knows the realm of the relevant global object")
        .global_object()
}

/// The Window that the global object is, if it is one.
pub(crate) fn window_from_global_object(global_object: Gc<Object>) -> Option<CppWindow> {
    // SAFETY: The global object is live, and LibWeb returns null or the live Window that it is.
    unsafe { cell_if_not_null(web_bindings_window_from_global_object(global_object.as_ptr().cast())) }.map(CppWindow)
}

/// The WorkerGlobalScope that the global object is, if it is one.
pub(crate) fn worker_global_scope_from_global_object(global_object: Gc<Object>) -> Option<CppWorkerGlobalScope> {
    // SAFETY: The global object is live, and LibWeb returns null or the live WorkerGlobalScope that it is.
    unsafe {
        cell_if_not_null(web_bindings_worker_global_scope_from_global_object(
            global_object.as_ptr().cast(),
        ))
    }
    .map(CppWorkerGlobalScope)
}

impl CppWindow {
    /// https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-document-window
    pub(crate) fn associated_document(&self) -> CppDocument {
        // SAFETY: The Window is live, and LibWeb returns its live associated Document.
        CppDocument(unsafe { CppCell::from_raw(web_bindings_window_associated_document(self.0.as_ptr())) })
    }
}

impl CppDocument {
    /// https://html.spec.whatwg.org/multipage/document-sequences.html#fully-active
    pub(crate) fn is_fully_active(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_is_fully_active(self.0.as_ptr()) }
    }
}

impl CppWorkerGlobalScope {
    /// https://html.spec.whatwg.org/multipage/workers.html#dom-workerglobalscope-closing
    pub(crate) fn is_closing(&self) -> bool {
        // SAFETY: The WorkerGlobalScope is live.
        unsafe { web_bindings_worker_global_scope_is_closing(self.0.as_ptr()) }
    }
}
