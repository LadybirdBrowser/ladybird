/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::Trace;
use liburl_rust::Url;

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi};
use crate::html::navigable::CppNavigable;
use crate::navigation_timing::performance_navigation_timing::PerformanceNavigationTiming;

unsafe extern "C" {
    fn web_bindings_document_navigable(document: *mut c_void) -> *mut c_void;
    fn web_bindings_document_is_fully_active(document: *mut c_void) -> bool;
    fn web_bindings_document_has_focus(document: *mut c_void) -> bool;
    fn web_bindings_document_has_been_destroyed(document: *mut c_void) -> bool;
    fn web_bindings_document_url(document: *mut c_void) -> liburl_rust::RustUrl;
    fn web_bindings_document_load_timing_info(document: *mut c_void, load_timing_info: *mut DocumentLoadTimingInfo);
    fn web_bindings_document_previous_document_unload_timing(
        document: *mut c_void,
        unload_timing: *mut DocumentUnloadTimingInfo,
    );
    fn web_bindings_document_navigation_timing_entry(document: *mut c_void) -> *mut c_void;
    fn web_bindings_document_set_navigation_timing_entry(document: *mut c_void, entry: *mut c_void);
}

/// https://dom.spec.whatwg.org/#document
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppDocument(CppCell);

/// https://html.spec.whatwg.org/multipage/dom.html#document-load-timing-info
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub(crate) struct DocumentLoadTimingInfo {
    pub(crate) navigation_start_time: f64,
    pub(crate) dom_interactive_time: f64,
    pub(crate) dom_content_loaded_event_start_time: f64,
    pub(crate) dom_content_loaded_event_end_time: f64,
    pub(crate) dom_complete_time: f64,
    pub(crate) load_event_start_time: f64,
    pub(crate) load_event_end_time: f64,
    pub(crate) dom_loading_time: f64,
}

/// https://html.spec.whatwg.org/multipage/dom.html#document-unload-timing-info
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub(crate) struct DocumentUnloadTimingInfo {
    pub(crate) unload_event_start_time: f64,
    pub(crate) unload_event_end_time: f64,
}

impl CppDocument {
    /// # Safety
    ///
    /// `document` must be a live Document.
    pub(crate) unsafe fn from_raw(document: *mut c_void) -> Self {
        // SAFETY: The caller passes a live Document.
        Self(unsafe { CppCell::from_raw(document) })
    }

    /// https://html.spec.whatwg.org/multipage/document-sequences.html#fully-active
    pub(crate) fn is_fully_active(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_is_fully_active(self.0.as_ptr()) }
    }

    /// https://html.spec.whatwg.org/multipage/interaction.html#has-focus-steps
    pub(crate) fn has_focus(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_has_focus(self.0.as_ptr()) }
    }

    /// https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document
    pub(crate) fn has_been_destroyed(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_has_been_destroyed(self.0.as_ptr()) }
    }

    /// https://html.spec.whatwg.org/multipage/document-sequences.html#node-navigable
    pub(crate) fn navigable(&self) -> Option<CppNavigable> {
        // SAFETY: The Document is live, and LibWeb returns null or its live node navigable.
        let navigable = unsafe { web_bindings_document_navigable(self.0.as_ptr()) };
        // SAFETY: The navigable is live.
        (!navigable.is_null()).then(|| unsafe { CppNavigable::from_raw(navigable) })
    }

    /// https://dom.spec.whatwg.org/#concept-document-url
    pub(crate) fn url(&self) -> Url {
        // SAFETY: The Document is live, and LibWeb lends its URL, which this copies before the Document can change it.
        unsafe { web_bindings_document_url(self.0.as_ptr()).to_url() }
    }

    /// https://html.spec.whatwg.org/multipage/dom.html#load-timing-info
    pub(crate) fn load_timing_info(&self) -> DocumentLoadTimingInfo {
        let mut load_timing_info = DocumentLoadTimingInfo::default();
        // SAFETY: The Document is live, and LibWeb writes its load timing info.
        unsafe { web_bindings_document_load_timing_info(self.0.as_ptr(), &raw mut load_timing_info) };
        load_timing_info
    }

    /// https://html.spec.whatwg.org/multipage/dom.html#previous-document-unload-timing
    pub(crate) fn previous_document_unload_timing(&self) -> DocumentUnloadTimingInfo {
        let mut unload_timing = DocumentUnloadTimingInfo::default();
        // SAFETY: The Document is live, and LibWeb writes its previous document unload timing.
        unsafe { web_bindings_document_previous_document_unload_timing(self.0.as_ptr(), &raw mut unload_timing) };
        unload_timing
    }

    /// https://w3c.github.io/navigation-timing/#dfn-document-navigation-timing-entry
    pub(crate) fn navigation_timing_entry(&self) -> Option<Ref<PerformanceNavigationTiming>> {
        // SAFETY: The Document is live.
        let entry = unsafe { web_bindings_document_navigation_timing_entry(self.0.as_ptr()) };
        // SAFETY: LibWeb returns null or the cell of the Document's PerformanceNavigationTiming.
        (!entry.is_null()).then(|| unsafe { cell_from_abi(entry) })
    }

    pub(crate) fn set_navigation_timing_entry(&self, entry: Ref<PerformanceNavigationTiming>) {
        // SAFETY: The Document and the entry are live.
        unsafe { web_bindings_document_set_navigation_timing_entry(self.0.as_ptr(), cell_into_abi(entry)) };
    }
}
