/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Handles to the C++ global objects and Documents.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, MarkedVec, Object, Trace, Vm};
use liburl_rust::Url;

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEventTarget;
use crate::bindings::interface::Wrappable;

unsafe extern "C" {
    fn web_bindings_window_from_global_object(global_object: *mut c_void) -> *mut c_void;
    fn web_bindings_for_each_active_window(ctx: *mut c_void, on_window: unsafe extern "C" fn(*mut c_void, *mut c_void));
    fn web_bindings_window_associated_document(window: *mut c_void) -> *mut c_void;
    fn web_bindings_window_page_id(window: *mut c_void) -> u64;
    fn web_bindings_window_as_event_target(window: *mut c_void) -> *mut c_void;
    fn web_bindings_document_has_navigable(document: *mut c_void) -> bool;
    fn web_bindings_document_is_fully_active(document: *mut c_void) -> bool;
    fn web_bindings_document_has_been_destroyed(document: *mut c_void) -> bool;
    fn web_bindings_document_url(document: *mut c_void) -> liburl_rust::RustUrl;
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

unsafe extern "C" fn on_active_window(windows: *mut c_void, window: *mut c_void) {
    // SAFETY: windows is the list of the caller, and LibWeb passes a live Window.
    unsafe { &*windows.cast::<MarkedVec<'_, CppWindow>>() }.push(CppWindow(unsafe { CppCell::from_raw(window) }));
}

/// Every Window of this process whose associated Document is fully active.
pub(crate) fn active_windows(vm: &Vm) -> MarkedVec<'_, CppWindow> {
    let windows = MarkedVec::new(vm);
    // SAFETY: The list outlives the call.
    unsafe { web_bindings_for_each_active_window((&raw const windows).cast_mut().cast(), on_active_window) };
    windows
}

impl CppWindow {
    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// The Window as an EventTarget.
    pub(crate) fn as_event_target(&self) -> CppEventTarget {
        // SAFETY: The Window is live, and LibWeb returns it as an EventTarget.
        unsafe { CppEventTarget::from_raw(web_bindings_window_as_event_target(self.0.as_ptr())) }
    }

    /// The id of the page whose traversable navigable the Window's navigable belongs to.
    pub(crate) fn page_id(&self) -> u64 {
        // SAFETY: The Window is live.
        unsafe { web_bindings_window_page_id(self.0.as_ptr()) }
    }

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

    /// https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document
    pub(crate) fn has_been_destroyed(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_has_been_destroyed(self.0.as_ptr()) }
    }

    /// Whether the Document's node navigable is not null, https://html.spec.whatwg.org/multipage/document-sequences.html#node-navigable
    pub(crate) fn has_navigable(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_has_navigable(self.0.as_ptr()) }
    }

    /// https://dom.spec.whatwg.org/#concept-document-url
    pub(crate) fn url(&self) -> Url {
        // SAFETY: The Document is live, and LibWeb lends its URL, which this copies before the Document can change it.
        unsafe { web_bindings_document_url(self.0.as_ptr()).to_url() }
    }
}

impl CppWorkerGlobalScope {
    /// https://html.spec.whatwg.org/multipage/workers.html#dom-workerglobalscope-closing
    pub(crate) fn is_closing(&self) -> bool {
        // SAFETY: The WorkerGlobalScope is live.
        unsafe { web_bindings_worker_global_scope_is_closing(self.0.as_ptr()) }
    }
}
