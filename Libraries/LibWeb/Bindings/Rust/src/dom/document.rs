/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The members of Document that are implemented in Rust, https://dom.spec.whatwg.org/#interface-document

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace, Vm};
use liburl_rust::origin::{Host, Origin};
use liburl_rust::{FfiOrigin, FfiUrlHost, Url};
use libweb_rust::web_animations::TimelineRecordRef;

use crate::animations::animation::AnyAnimation;
use crate::animations::animation_timeline::{AnyAnimationTimeline, timeline_object_of_record};
use crate::animations::document_or_shadow_root;
use crate::animations::document_timeline::DocumentTimeline;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, WeakCppCell, cpp_class};
use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::navigable::CppNavigable;
use crate::html::visibility_state::VisibilityState;
use crate::navigation_timing::performance_navigation_timing::PerformanceNavigationTiming;
use crate::permissions_policy::permissions_policy::PolicyControlledFeature;

unsafe extern "C" {
    fn web_animation_document_of_wrapper(document_wrapper: *mut c_void) -> *mut c_void;
    fn web_bindings_is_a_registrable_domain_suffix_of_or_is_equal_to(
        host_suffix_string: usize,
        original_host: *const FfiUrlHost,
    ) -> bool;
    fn web_bindings_document_navigable(document: *mut c_void) -> *mut c_void;
    fn web_bindings_document_is_fully_active(document: *mut c_void) -> bool;
    fn web_bindings_document_is_allowed_to_use_feature(document: *mut c_void, feature: u8) -> bool;
    fn web_bindings_document_visibility_state(document: *mut c_void) -> u8;
    fn web_bindings_document_origin(
        document: *mut c_void,
        origin: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const FfiOrigin),
    );
    fn web_bindings_document_has_focus(document: *mut c_void) -> bool;
    fn web_bindings_document_is_fully_active_descendant_of_a_top_level_traversable_with_user_attention(
        document: *mut c_void,
    ) -> bool;
    fn web_bindings_document_observe_visibility_state(
        document: *mut c_void,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void, bool),
    ) -> *mut c_void;
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

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WeakCppDocument(WeakCppCell);

impl WeakCppDocument {
    pub(crate) fn get(&self) -> Option<CppDocument> {
        self.0.get().map(CppDocument)
    }
}

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
    pub(crate) fn from_lent(document: &LentCppCell<cpp_class::Document>) -> Self {
        Self(CppCell::from_lent(document))
    }

    /// # Safety
    ///
    /// `document` must be a live Document.
    pub(crate) unsafe fn from_raw(document: *mut c_void) -> Self {
        // SAFETY: The caller passes a live Document.
        Self(unsafe { CppCell::from_raw(document) })
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub(crate) fn downgrade(&self, vm: &Vm) -> WeakCppDocument {
        WeakCppDocument(WeakCppCell::new(vm, &self.0))
    }

    /// https://html.spec.whatwg.org/multipage/document-sequences.html#fully-active
    pub(crate) fn is_fully_active(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_is_fully_active(self.0.as_ptr()) }
    }

    /// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#allowed-to-use
    pub(crate) fn is_allowed_to_use_feature(&self, feature: PolicyControlledFeature) -> bool {
        // SAFETY: The Document is live.
        unsafe { web_bindings_document_is_allowed_to_use_feature(self.0.as_ptr(), feature as u8) }
    }

    /// https://dom.spec.whatwg.org/#concept-document-origin
    pub(crate) fn origin(&self) -> Origin {
        unsafe extern "C" fn on_origin(origin: *mut c_void, ffi_origin: *const FfiOrigin) {
            // SAFETY: origin is the origin below, and LibWeb lends the origin for the call.
            unsafe { *origin.cast::<Option<Origin>>() = Some((*ffi_origin).to_origin()) };
        }

        let mut origin = None;
        // SAFETY: The Document is live, and the origin outlives the call.
        unsafe { web_bindings_document_origin(self.0.as_ptr(), (&raw mut origin).cast(), on_origin) };
        origin.expect("LibWeb lends the origin")
    }

    /// https://html.spec.whatwg.org/multipage/interaction.html#visibility-state
    pub(crate) fn visibility_state(&self) -> VisibilityState {
        // SAFETY: The Document is live.
        match unsafe { web_bindings_document_visibility_state(self.0.as_ptr()) } {
            0 => VisibilityState::Visible,
            1 => VisibilityState::Hidden,
            _ => unreachable!("LibWeb returns a VisibilityState"),
        }
    }

    /// https://html.spec.whatwg.org/multipage/interaction.html#fully-active-descendant-of-a-top-level-traversable-with-user-attention
    pub(crate) fn is_fully_active_descendant_of_a_top_level_traversable_with_user_attention(&self) -> bool {
        // SAFETY: The Document is live.
        unsafe {
            web_bindings_document_is_fully_active_descendant_of_a_top_level_traversable_with_user_attention(
                self.0.as_ptr(),
            )
        }
    }

    /// Calls the steps with the VM, the cell and whether the Document is visible whenever its visibility state
    /// changes, for as long as the returned DocumentObserver lives.
    pub(crate) fn observe_visibility_state(
        &self,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void, bool),
    ) -> CppCell {
        // SAFETY: The Document and the cell are live, and LibWeb returns a live DocumentObserver.
        unsafe {
            CppCell::from_raw(web_bindings_document_observe_visibility_state(
                self.0.as_ptr(),
                cell,
                steps,
            ))
        }
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

/// https://html.spec.whatwg.org/multipage/browsers.html#is-a-registrable-domain-suffix-of-or-is-equal-to
pub(crate) fn is_a_registrable_domain_suffix_of_or_is_equal_to(
    host_suffix_string: &Utf16String,
    original_host: &Host,
) -> bool {
    let original_host = FfiUrlHost::new(original_host.kind, &original_host.serialization);
    // SAFETY: The host suffix string and the host are lent for the call.
    unsafe {
        web_bindings_is_a_registrable_domain_suffix_of_or_is_equal_to(
            host_suffix_string.raw_identity(),
            &raw const original_host,
        )
    }
}

/// https://drafts.csswg.org/web-animations-1/#dom-document-timeline
pub(crate) fn timeline(this: &CppInterface, cx: &mut CallContext) -> WebIdlResult<Ref<DocumentTimeline>> {
    // The DocumentTimeline object representing the default document timeline.
    // SAFETY: The wrapper is live, and LibWeb returns the document it wraps.
    let document = NonNull::new(unsafe { web_animation_document_of_wrapper(this.wrapper().as_ptr().cast()) })
        .expect("a wrapper of a Document wraps a document");
    let record = TimelineRecordRef::default_of_document(document);
    let AnyAnimationTimeline::DocumentTimeline(timeline) = timeline_object_of_record(cx.vm(), &record) else {
        unreachable!("the default document timeline is a document timeline");
    };
    Ok(timeline)
}

/// https://drafts.csswg.org/web-animations-1/#dom-documentorshadowroot-getanimations
pub(crate) fn get_animations<'vm>(
    this: &CppInterface,
    cx: &mut CallContext<'vm>,
) -> WebIdlResult<MarkedVec<'vm, AnyAnimation>> {
    document_or_shadow_root::get_animations(this, cx)
}
