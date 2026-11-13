/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceTiming, https://w3c.github.io/navigation-timing/#the-performancetiming-interface

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Inherits, Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::document::CppDocument;
use crate::navigation_timing::performance_navigation_timing::PerformanceNavigationTiming;

unsafe extern "C" {
    fn web_high_resolution_time_estimated_monotonic_time_of_the_unix_epoch() -> f64;
    fn web_high_resolution_time_coarsen_time(timestamp: f64) -> f64;
}

type Attribute = fn(&PerformanceTiming, &mut CallContext<'_>) -> WebIdlResult<u64>;

const ATTRIBUTES: [(&str, Attribute); 21] = [
    ("connectEnd", PerformanceTiming::connect_end),
    ("connectStart", PerformanceTiming::connect_start),
    ("domainLookupEnd", PerformanceTiming::domain_lookup_end),
    ("domainLookupStart", PerformanceTiming::domain_lookup_start),
    ("domComplete", PerformanceTiming::dom_complete),
    (
        "domContentLoadedEventEnd",
        PerformanceTiming::dom_content_loaded_event_end,
    ),
    (
        "domContentLoadedEventStart",
        PerformanceTiming::dom_content_loaded_event_start,
    ),
    ("domInteractive", PerformanceTiming::dom_interactive),
    ("domLoading", PerformanceTiming::dom_loading),
    ("fetchStart", PerformanceTiming::fetch_start),
    ("loadEventEnd", PerformanceTiming::load_event_end),
    ("loadEventStart", PerformanceTiming::load_event_start),
    ("navigationStart", PerformanceTiming::navigation_start),
    ("redirectEnd", PerformanceTiming::redirect_end),
    ("redirectStart", PerformanceTiming::redirect_start),
    ("requestStart", PerformanceTiming::request_start),
    ("responseEnd", PerformanceTiming::response_end),
    ("responseStart", PerformanceTiming::response_start),
    ("secureConnectionStart", PerformanceTiming::secure_connection_start),
    ("unloadEventEnd", PerformanceTiming::unload_event_end),
    ("unloadEventStart", PerformanceTiming::unload_event_start),
];

fn attribute_named(name: &Utf16String) -> Option<Attribute> {
    ATTRIBUTES
        .iter()
        .find(|(attribute_name, _)| *name == Utf16String::from_utf8(attribute_name))
        .map(|(_, attribute)| *attribute)
}

pub(crate) fn is_performance_timing_attribute(name: &Utf16String) -> bool {
    attribute_named(name).is_some()
}

/// https://w3c.github.io/navigation-timing/#the-performancetiming-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceTiming {
    window: CppWindow,
}

impl Wrappable for PerformanceTiming {}

// All time values are measured in milliseconds since midnight of January 1, 1970 (UTC).
fn monotonic_timestamp_to_wall_time_milliseconds(timestamp: f64) -> u64 {
    if timestamp == 0.0 {
        return 0;
    }

    // SAFETY: These read the clock.
    let wall_time = timestamp - unsafe { web_high_resolution_time_estimated_monotonic_time_of_the_unix_epoch() };
    let coarsened_time = unsafe { web_high_resolution_time_coarsen_time(wall_time) };
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "C++ casts it to u64"
    )]
    let milliseconds = coarsened_time as u64;
    milliseconds
}

impl PerformanceTiming {
    pub(crate) fn new(window: CppWindow) -> Self {
        Self { window }
    }

    pub(crate) fn attribute(&self, cx: &mut CallContext, name: &Utf16String) -> WebIdlResult<u64> {
        attribute_named(name).expect("the name is that of a PerformanceTiming attribute")(self, cx)
    }

    fn document(&self) -> CppDocument {
        self.window.associated_document()
    }

    fn navigation_timing_entry(&self) -> Option<Ref<PerformanceNavigationTiming>> {
        self.document().navigation_timing_entry()
    }

    // The document's time origin is its navigation start time, so a timestamp relative to it becomes monotonic by adding
    // that back. A fetch phase coinciding with navigation start is 0 relative to the time origin without being unset.
    fn fetch_phase_to_wall_time_milliseconds(&self, relative_timestamp: f64) -> u64 {
        monotonic_timestamp_to_wall_time_milliseconds(
            relative_timestamp + self.document().load_timing_info().navigation_start_time,
        )
    }

    fn relative_timestamp_to_wall_time_milliseconds(&self, relative_timestamp: f64) -> u64 {
        if relative_timestamp == 0.0 {
            return 0;
        }

        self.fetch_phase_to_wall_time_milliseconds(relative_timestamp)
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-navigationstart
    pub(crate) fn navigation_start(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately after the user agent finishes prompting to unload the previous
        // document. If there is no previous document, this attribute must return the time the current document is created.
        Ok(monotonic_timestamp_to_wall_time_milliseconds(
            self.document().load_timing_info().navigation_start_time,
        ))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-unloadeventstart
    pub(crate) fn unload_event_start(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // If the previous document and the current document have the same origin, this attribute must return the time
        // immediately before the user agent starts the unload event of the previous document. If there is no previous
        // document or the previous document has a different origin than the current document, this attribute must return
        // zero.
        Ok(self.relative_timestamp_to_wall_time_milliseconds(
            self.document()
                .previous_document_unload_timing()
                .unload_event_start_time,
        ))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-unloadeventend
    pub(crate) fn unload_event_end(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // If the previous document and the current document have the same origin, this attribute must return the time
        // immediately after the user agent finishes the unload event of the previous document. If there is no previous
        // document or the previous document has a different origin than the current document or the unload is not yet
        // completed, this attribute must return zero.
        Ok(self.relative_timestamp_to_wall_time_milliseconds(
            self.document().previous_document_unload_timing().unload_event_end_time,
        ))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-redirectstart
    pub(crate) fn redirect_start(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // If there are HTTP redirects when navigating and if all the redirects are from the same origin, this attribute
        // must return the starting time of the fetch that initiates the redirect. Otherwise, this attribute must return
        // zero.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };

        if entry.redirect_count(cx)? == 0 {
            return Ok(0);
        }
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.redirect_start()))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-redirectend
    pub(crate) fn redirect_end(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // If there are HTTP redirects when navigating and all redirects are from the same origin, this attribute must
        // return the time immediately after receiving the last byte of the response of the last redirect. Otherwise, this
        // attribute must return zero.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };

        if entry.redirect_count(cx)? == 0 {
            return Ok(0);
        }
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.redirect_end()))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-fetchstart
    pub(crate) fn fetch_start(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // If the new resource is to be fetched using a "GET" request method, fetchStart must return the time immediately
        // before the user agent starts checking the HTTP cache. Otherwise, it must return the time when the user agent
        // starts fetching the resource.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };

        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.parent().fetch_start(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-domainlookupstart
    pub(crate) fn domain_lookup_start(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the user agent starts the domain name lookup for the
        // current document.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.parent().domain_lookup_start(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-domainlookupend
    pub(crate) fn domain_lookup_end(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately after the user agent finishes the domain name lookup for the
        // current document.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.parent().domain_lookup_end(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-connectstart
    pub(crate) fn connect_start(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the user agent start establishing the connection to the
        // server to retrieve the document.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.parent().connect_start(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-connectend
    pub(crate) fn connect_end(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately after the user agent finishes establishing the connection to
        // the server to retrieve the current document.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.parent().connect_end(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-secureconnectionstart
    pub(crate) fn secure_connection_start(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // When this attribute is available, if the scheme of the current page is "https", this attribute must return the
        // time immediately before the user agent starts the handshake process to secure the current connection. If this
        // attribute is available but HTTPS is not used, this attribute must return zero.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.relative_timestamp_to_wall_time_milliseconds(entry.parent().secure_connection_start(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-requeststart
    pub(crate) fn request_start(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the user agent starts requesting the current document
        // from the server, or HTTP cache or from local resources.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.parent().request_start(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-responsestart
    pub(crate) fn response_start(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately after the user agent receives the first byte of the response
        // from the server, or HTTP cache or from local resources.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.fetch_phase_to_wall_time_milliseconds(entry.parent().response_start(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-responseend
    pub(crate) fn response_end(&self, cx: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately after the user agent receives the last byte of the current
        // document or immediately before the transport connection is closed, whichever comes first.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };
        Ok(self.relative_timestamp_to_wall_time_milliseconds(entry.parent().response_end(cx)?))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-domloading
    pub(crate) fn dom_loading(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the user agent sets the current document readiness to
        // "loading".
        Ok(self.relative_timestamp_to_wall_time_milliseconds(self.document().load_timing_info().dom_loading_time))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-dominteractive
    pub(crate) fn dom_interactive(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the user agent sets the current document readiness to
        // "interactive".
        Ok(self.relative_timestamp_to_wall_time_milliseconds(self.document().load_timing_info().dom_interactive_time))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-domcontentloadedeventstart
    pub(crate) fn dom_content_loaded_event_start(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the user agent fires the DOMContentLoaded event at the
        // Document.
        Ok(self.relative_timestamp_to_wall_time_milliseconds(
            self.document().load_timing_info().dom_content_loaded_event_start_time,
        ))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-domcontentloadedeventend
    pub(crate) fn dom_content_loaded_event_end(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately after the document's DOMContentLoaded event completes.
        Ok(self.relative_timestamp_to_wall_time_milliseconds(
            self.document().load_timing_info().dom_content_loaded_event_end_time,
        ))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-domcomplete
    pub(crate) fn dom_complete(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the user agent sets the current document readiness to
        // "complete".
        Ok(self.relative_timestamp_to_wall_time_milliseconds(self.document().load_timing_info().dom_complete_time))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-loadeventstart
    pub(crate) fn load_event_start(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time immediately before the load event of the current document is fired. It must
        // return zero when the load event is not fired yet.
        Ok(self.relative_timestamp_to_wall_time_milliseconds(self.document().load_timing_info().load_event_start_time))
    }

    // https://w3c.github.io/navigation-timing/#dom-performancetiming-loadeventend
    pub(crate) fn load_event_end(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // This attribute must return the time when the load event of the current document is completed. It must return
        // zero when the load event is not fired or is not completed.
        Ok(self.relative_timestamp_to_wall_time_milliseconds(self.document().load_timing_info().load_event_end_time))
    }
}
