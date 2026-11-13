/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceNavigationTiming, https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::interface::{Inherits, Wrappable, allocate, realm_arguments};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::document::CppDocument;
use crate::fetch::infrastructure::fetch_timing_info::CppFetchTimingInfo;
use crate::performance_timeline::performance_entry::AnyPerformanceEntry;
use crate::performance_timeline::performance_timeline::PerformanceTimeline;
use crate::resource_timing::performance_resource_timing::{
    CacheState, PerformanceResourceTiming, ResponseBodyInfo, ResponseBodyInfoAbi,
};

/// https://w3c.github.io/navigation-timing/#dom-navigationtimingtype
#[derive(Clone, Copy)]
pub(crate) enum NavigationTimingType {
    Navigate,
    Reload,
    BackForward,
}

impl NavigationTimingType {
    fn from_abi(navigation_type: u8) -> Self {
        match navigation_type {
            0 => Self::Navigate,
            1 => Self::Reload,
            2 => Self::BackForward,
            _ => unreachable!("C++ passes a navigation timing type"),
        }
    }
}

/// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceNavigationTiming {
    resource_timing: PerformanceResourceTiming,
    document: CppDocument,
    redirect_count: u16,
    #[gc(untraced)]
    navigation_type: NavigationTimingType,
}

impl Wrappable for PerformanceNavigationTiming {}

impl Inherits<PerformanceResourceTiming> for PerformanceNavigationTiming {
    fn parent(&self) -> &PerformanceResourceTiming {
        &self.resource_timing
    }
}

impl PerformanceNavigationTiming {
    // https://w3c.github.io/navigation-timing/#performanceentry
    pub(crate) fn entry_type(&self) -> Utf16String {
        // The entryType getter step is to return the DOMString "navigation".
        Utf16String::from_utf8("navigation")
    }

    // https://w3c.github.io/navigation-timing/#performanceentry
    pub(crate) fn start_time(&self) -> f64 {
        // The startTime getter step is to return a DOMHighResTimeStamp with a time value of 0.
        0.0
    }

    // https://w3c.github.io/navigation-timing/#performanceentry
    pub(crate) fn duration(&self) -> f64 {
        // The duration getter step is to return a DOMHighResTimeStamp equal to the difference between loadEventEnd and
        // this's startTime.
        self.document.load_timing_info().load_event_end_time - self.start_time()
    }

    // https://w3c.github.io/navigation-timing/#PerformanceResourceTiming
    pub(crate) fn redirect_start(&self) -> f64 {
        // 1. If this's redirect count is 0, return 0.
        if self.redirect_count == 0 {
            return 0.0;
        }

        // 2. Otherwise return this's redirectStart.
        self.resource_timing.resource_timing_redirect_start()
    }

    // https://w3c.github.io/navigation-timing/#PerformanceResourceTiming
    pub(crate) fn redirect_end(&self) -> f64 {
        // 1. If this's redirect count is 0, return 0.
        if self.redirect_count == 0 {
            return 0.0;
        }

        // 2. Otherwise return this's redirectEnd.
        self.resource_timing.resource_timing_redirect_end()
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventstart
    pub(crate) fn unload_event_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The unloadEventStart getter steps are to return this's previous document unload timing's unload event start
        // time.
        Ok(self.document.previous_document_unload_timing().unload_event_start_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventend
    pub(crate) fn unload_event_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The unloadEventEnd getter steps are to return this's previous document unload timing's unload event end time.
        Ok(self.document.previous_document_unload_timing().unload_event_end_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-dominteractive
    pub(crate) fn dom_interactive(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The domInteractive getter steps are to return this's document load timing's DOM interactive time.
        Ok(self.document.load_timing_info().dom_interactive_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventstart
    pub(crate) fn dom_content_loaded_event_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The domContentLoadedEventStart getter steps are to return this's document load timing's DOM content loaded
        // event start time.
        Ok(self.document.load_timing_info().dom_content_loaded_event_start_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventend
    pub(crate) fn dom_content_loaded_event_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The domContentLoadedEventEnd getter steps are to return this's document load timing's DOM content loaded event
        // end time.
        Ok(self.document.load_timing_info().dom_content_loaded_event_end_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcomplete
    pub(crate) fn dom_complete(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The domComplete getter steps are to return this's document load timing's DOM complete time.
        Ok(self.document.load_timing_info().dom_complete_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventstart
    pub(crate) fn load_event_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The loadEventStart getter steps are to return this's document load timing's load event start time.
        Ok(self.document.load_timing_info().load_event_start_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventend
    pub(crate) fn load_event_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The loadEventEnd getter steps are to return this's document load timing's load event end time.
        Ok(self.document.load_timing_info().load_event_end_time)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<NavigationTimingType> {
        // The type getter steps are to run the this's navigation type.
        Ok(self.navigation_type)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-redirectcount
    pub(crate) fn redirect_count(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        // The redirectCount getter steps are to return this's redirect count.
        Ok(self.redirect_count)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-criticalchrestart
    pub(crate) fn critical_ch_restart(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The criticalCHRestart getter steps are to return this's `Critical-CH` restart time.
        // FIXME: Implement the `Critical-CH` restart time.
        Ok(0.0)
    }
}

// https://w3c.github.io/navigation-timing/#dfn-create-the-navigation-timing-entry
#[expect(clippy::too_many_arguments, reason = "these are the inputs of the algorithm")]
fn create_the_navigation_timing_entry(
    vm: &Vm,
    realm: Gc<Realm>,
    document: CppDocument,
    document_url: Utf16String,
    timing_info: CppFetchTimingInfo,
    redirect_count: u16,
    navigation_type: NavigationTimingType,
    cache_mode: Option<CacheState>,
    body_info: ResponseBodyInfo,
    response_status: u16,
) {
    // 1. Let global be document's relevant global object.
    // NB: The realm is that of global.

    // 2. Let navigationTimingEntry be a new PerformanceNavigationTiming object in global's realm.
    // 3. Setup the resource timing entry for navigationTimingEntry given "navigation", document's URL, fetchTiming,
    //    cacheMode, and bodyInfo.
    let resource_timing = PerformanceResourceTiming::setup_the_resource_timing_entry(
        realm,
        Utf16String::from_utf8("navigation"),
        document_url,
        timing_info,
        cache_mode,
        body_info,
        response_status,
        Utf16String::default(),
    );

    // 4. Set navigationTimingEntry's document load timing to document's load timing info
    // 5. Set navigationTimingEntry's previous document unload timing to document's previous document unload timing.
    // NB: The entry reads both from the document, which it keeps.
    // 6. Set navigationTimingEntry's redirect count to redirectCount.
    // 7. Set navigationTimingEntry's navigation type to navigationType.
    // 8. Set navigationTimingEntry's service worker timing to serviceWorkerTiming.
    // FIXME: Implement service worker timing.
    let navigation_timing_entry = allocate(vm, |_| PerformanceNavigationTiming {
        resource_timing,
        document: document.clone(),
        redirect_count,
        navigation_type,
    });
    let entry = AnyPerformanceEntry::PerformanceNavigationTiming(navigation_timing_entry);
    entry.entry().set_object(entry);

    // 9. Set document's navigation timing entry to navigationTimingEntry.
    document.set_navigation_timing_entry(navigation_timing_entry);

    // 10. Set navigationTimingEntry's `Critical-CH` restart time to criticalCHRestart.
    // FIXME: Implement the `Critical-CH` restart time.
    // 11. Set navigationTimingEntry's not restored reasons to the result of creating a NotRestoredReasons object given
    //     document's not restored reasons.
    // FIXME: Implement not restored reasons.

    // 12. Add navigationTimingEntry to global's performance entry buffer.
    PerformanceTimeline::of(vm, realm).add_performance_entry(vm, &entry, false);
}

/// https://w3c.github.io/navigation-timing/#dfn-create-the-navigation-timing-entry
///
/// # Safety
///
/// `vm` must be the live VM, `realm` the live realm of the document's relevant global object, `document` a live
/// Document, `timing_info` a fetch timing info that LibWeb hands over a reference to, and `document_url` and the content
/// type of `body_info` raw AK::Utf16Strings that LibWeb hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_navigation_timing_create_the_navigation_timing_entry(
    vm: *mut c_void,
    realm: *mut c_void,
    document: *mut c_void,
    document_url: usize,
    timing_info: *mut c_void,
    redirect_count: u16,
    navigation_type: u8,
    cache_mode: u8,
    body_info: ResponseBodyInfoAbi,
    response_status: u16,
) {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller passes a live Document, hands over the timing info and the strings.
    unsafe {
        create_the_navigation_timing_entry(
            vm,
            realm,
            CppDocument::from_raw(document),
            Utf16String::from_raw_owned(document_url),
            CppFetchTimingInfo::adopt(timing_info),
            redirect_count,
            NavigationTimingType::from_abi(navigation_type),
            CacheState::from_abi(cache_mode),
            body_info.into_body_info(),
            response_status,
        );
    }
}
