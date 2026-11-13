/*
 * Copyright (c) 2025, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceResourceTiming, https://w3c.github.io/resource-timing/#dom-performanceresourcetiming

use core::ffi::c_void;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Gc, MarkedVec, Realm, Trace, Vm};

use crate::bindings::conversions::ByteString;
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, realm_arguments};
use crate::bindings::{CallContext, WebIdlResult};
use crate::fetch::infrastructure::fetch_timing_info::CppFetchTimingInfo;
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, PerformanceEntry};
use crate::performance_timeline::performance_timeline::PerformanceTimeline;
use crate::server_timing::performance_server_timing::{PerformanceServerTiming, parse_a_server_timing_header_field};

/// https://w3c.github.io/resource-timing/#dom-renderblockingstatustype
#[derive(Clone, Copy)]
pub(crate) enum RenderBlockingStatusType {
    Blocking,
    NonBlocking,
}

/// https://fetch.spec.whatwg.org/#concept-response-cache-state
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum CacheState {
    Local,
    Validated,
}

impl CacheState {
    pub(crate) fn from_abi(cache_state: u8) -> Option<Self> {
        match cache_state {
            0 => None,
            1 => Some(Self::Local),
            2 => Some(Self::Validated),
            _ => unreachable!("C++ passes a cache state"),
        }
    }
}

/// https://fetch.spec.whatwg.org/#response-body-info
pub(crate) struct ResponseBodyInfo {
    pub(crate) encoded_size: u64,
    pub(crate) decoded_size: u64,
    pub(crate) content_type: Utf16String,
}

#[repr(C)]
pub(crate) struct ResponseBodyInfoAbi {
    encoded_size: u64,
    decoded_size: u64,
    content_type: usize,
}

impl ResponseBodyInfoAbi {
    /// # Safety
    ///
    /// The content type must be a raw string that C++ hands over.
    pub(crate) unsafe fn into_body_info(self) -> ResponseBodyInfo {
        ResponseBodyInfo {
            encoded_size: self.encoded_size,
            decoded_size: self.decoded_size,
            // SAFETY: The caller hands over the string.
            content_type: unsafe { Utf16String::from_raw_owned(self.content_type) },
        }
    }
}

/// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceResourceTiming {
    entry: PerformanceEntry,
    initiator_type: Utf16String,
    #[gc(untraced)]
    timing_info: CppFetchTimingInfo,
    time_origin: f64,
    #[gc(untraced)]
    response_body_info: ResponseBodyInfo,
    #[gc(untraced)]
    cache_mode: Option<CacheState>,
    response_status: u16,
    delivery_type: Utf16String,
}

impl Wrappable for PerformanceResourceTiming {}

impl Inherits<PerformanceEntry> for PerformanceResourceTiming {
    fn parent(&self) -> &PerformanceEntry {
        &self.entry
    }
}

// https://w3c.github.io/resource-timing/#dfn-convert-fetch-timestamp
pub(crate) fn convert_fetch_timestamp(time_stamp: f64, time_origin: f64) -> f64 {
    // 1. If ts is zero, return zero.
    if time_stamp == 0.0 {
        return 0.0;
    }

    // 2. Otherwise, return the relative high resolution coarse time given ts and global.
    time_stamp - time_origin
}

impl PerformanceResourceTiming {
    // https://w3c.github.io/resource-timing/#dfn-setup-the-resource-timing-entry
    #[expect(clippy::too_many_arguments, reason = "these are the inputs of the algorithm")]
    pub(crate) fn setup_the_resource_timing_entry(
        realm: Gc<Realm>,
        initiator_type: Utf16String,
        requested_url: Utf16String,
        timing_info: CppFetchTimingInfo,
        cache_mode: Option<CacheState>,
        body_info: ResponseBodyInfo,
        response_status: u16,
        mut delivery_type: Utf16String,
    ) -> Self {
        // 1. Assert that cacheMode is the empty string, "local", or "validated".

        // 2. Let global be entry's relevant global object.
        // NB: The realm is that of global.
        let time_origin = relevant_settings_object(realm.global_object()).time_origin();

        // 3. Initialize entry given the result of converting timingInfo's start time given global, "resource",
        //    requestedURL, and the result of converting timingInfo's end time given global.
        let timing = timing_info.read();
        let entry = PerformanceEntry::initialize(
            convert_fetch_timestamp(timing.start_time, time_origin),
            Utf16String::from_utf8("resource"),
            requested_url,
            convert_fetch_timestamp(timing.end_time, time_origin),
        );

        // 4. Set entry's initiator type to initiatorType.
        // 5. Set entry's requested URL to requestedURL.
        // NB: The entry's name, which step 3 initialized, is its requested URL.
        // 6. Set entry's timing info to timingInfo.
        // 7. Set entry's resource info to bodyInfo.
        // 8. Set entry's cache mode to cacheMode.
        // 9. Set entry's response status to responseStatus.

        // 10. If deliveryType is the empty string and cacheMode is not, then set deliveryType to "cache".
        if delivery_type.is_empty() && cache_mode.is_some() {
            delivery_type = Utf16String::from_utf8("cache");
        }

        // 11. Set entry's delivery type to deliveryType.
        Self {
            entry,
            initiator_type,
            timing_info,
            time_origin,
            response_body_info: body_info,
            cache_mode,
            response_status,
            delivery_type,
        }
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-initiatortype
    pub(crate) fn initiator_type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The initiatorType getter steps are to return the initiator type for this.
        Ok(self.initiator_type.clone())
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-deliverytype
    pub(crate) fn delivery_type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The deliveryType getter steps are to return the delivery type for this.
        Ok(self.delivery_type.clone())
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-nexthopprotocol
    pub(crate) fn next_hop_protocol(&self, _: &mut CallContext) -> WebIdlResult<ByteString> {
        // The nextHopProtocol getter steps are to isomorphic decode this's timing info's final connection timing info's
        // ALPN negotiated protocol. See Recording connection timing info for more info.
        // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
        //       the Timing-Allow-Origin check fails. We return empty string in this case.
        Ok(ByteString::from_bytes(self.timing_info.alpn_negotiated_protocol()))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-workerstart
    pub(crate) fn worker_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The workerStart getter steps are to convert fetch timestamp for this's timing info's final service worker start
        // time and the relevant global object for this. See HTTP fetch for more info.
        Ok(convert_fetch_timestamp(
            self.timing_info.read().final_service_worker_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-redirectstart
    pub(crate) fn redirect_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        if let AnyPerformanceEntry::PerformanceNavigationTiming(entry) = self.entry.object() {
            return Ok(entry.redirect_start());
        }
        Ok(self.resource_timing_redirect_start())
    }

    pub(crate) fn resource_timing_redirect_start(&self) -> f64 {
        // The redirectStart getter steps are to convert fetch timestamp for this's timing info's redirect start time and
        // the relevant global object for this. See HTTP-redirect fetch for more info.
        convert_fetch_timestamp(self.timing_info.read().redirect_start_time, self.time_origin)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-redirectend
    pub(crate) fn redirect_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        if let AnyPerformanceEntry::PerformanceNavigationTiming(entry) = self.entry.object() {
            return Ok(entry.redirect_end());
        }
        Ok(self.resource_timing_redirect_end())
    }

    pub(crate) fn resource_timing_redirect_end(&self) -> f64 {
        // The redirectEnd getter steps are to convert fetch timestamp for this's timing info's redirect end time and the
        // relevant global object for this. See HTTP-redirect fetch for more info.
        convert_fetch_timestamp(self.timing_info.read().redirect_end_time, self.time_origin)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-fetchstart
    pub(crate) fn fetch_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The fetchStart getter steps are to convert fetch timestamp for this's timing info's post-redirect start time and
        // the relevant global object for this. See HTTP fetch for more info.
        Ok(convert_fetch_timestamp(
            self.timing_info.read().post_redirect_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-domainlookupstart
    pub(crate) fn domain_lookup_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The domainLookupStart getter steps are to convert fetch timestamp for this's timing info's final connection
        // timing info's domain lookup start time and the relevant global object for this. See Recording connection timing
        // info for more info.
        // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
        //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
        let Some(connection_timing_info) = self.timing_info.read().final_connection_timing_info() else {
            return Ok(0.0);
        };

        Ok(convert_fetch_timestamp(
            connection_timing_info.domain_lookup_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-domainlookupend
    pub(crate) fn domain_lookup_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The domainLookupEnd getter steps are to convert fetch timestamp for this's timing info's final connection timing
        // info's domain lookup end time and the relevant global object for this. See Recording connection timing info for
        // more info.
        // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
        //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
        let Some(connection_timing_info) = self.timing_info.read().final_connection_timing_info() else {
            return Ok(0.0);
        };

        Ok(convert_fetch_timestamp(
            connection_timing_info.domain_lookup_end_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-connectstart
    pub(crate) fn connect_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The connectStart getter steps are to convert fetch timestamp for this's timing info's final connection timing
        // info's connection start time and the relevant global object for this. See Recording connection timing info for
        // more info.
        // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
        //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
        let Some(connection_timing_info) = self.timing_info.read().final_connection_timing_info() else {
            return Ok(0.0);
        };

        Ok(convert_fetch_timestamp(
            connection_timing_info.connection_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-connectend
    pub(crate) fn connect_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The connectEnd getter steps are to convert fetch timestamp for this's timing info's final connection timing
        // info's connection end time and the relevant global object for this. See Recording connection timing info for
        // more info.
        // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
        //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
        let Some(connection_timing_info) = self.timing_info.read().final_connection_timing_info() else {
            return Ok(0.0);
        };

        Ok(convert_fetch_timestamp(
            connection_timing_info.connection_end_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-secureconnectionstart
    pub(crate) fn secure_connection_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The secureConnectionStart getter steps are to convert fetch timestamp for this's timing info's final connection
        // timing info's secure connection start time and the relevant global object for this. See Recording connection
        // timing info for more info.
        // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
        //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
        let Some(connection_timing_info) = self.timing_info.read().final_connection_timing_info() else {
            return Ok(0.0);
        };

        Ok(convert_fetch_timestamp(
            connection_timing_info.secure_connection_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-requeststart
    pub(crate) fn request_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The requestStart getter steps are to convert fetch timestamp for this's timing info's final network-request
        // start time and the relevant global object for this. See HTTP fetch for more info.
        Ok(convert_fetch_timestamp(
            self.timing_info.read().final_network_request_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-finalresponseheadersstart
    pub(crate) fn final_response_headers_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The finalResponseHeadersStart getter steps are to convert fetch timestamp for this's timing info's final
        // network-response start time and the relevant global object for this. See HTTP fetch for more info.
        Ok(convert_fetch_timestamp(
            self.timing_info.read().final_network_response_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-firstinterimresponsestart
    pub(crate) fn first_interim_response_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The firstInterimResponseStart getter steps are to convert fetch timestamp for this's timing info's first interim
        // network-response start time and the relevant global object for this. See HTTP fetch for more info.
        Ok(convert_fetch_timestamp(
            self.timing_info.read().first_interim_network_response_start_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-responsestart
    pub(crate) fn response_start(&self, cx: &mut CallContext) -> WebIdlResult<f64> {
        // The responseStart getter steps are to return this's firstInterimResponseStart if it is not 0;
        // Otherwise this's finalResponseHeadersStart.
        let first_interim_response_start_time = self.first_interim_response_start(cx)?;
        if first_interim_response_start_time != 0.0 {
            return Ok(first_interim_response_start_time);
        }

        self.final_response_headers_start(cx)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-responseend
    pub(crate) fn response_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The responseEnd getter steps are to convert fetch timestamp for this's timing info's end time and the relevant
        // global object for this. See fetch for more info.
        Ok(convert_fetch_timestamp(
            self.timing_info.read().end_time,
            self.time_origin,
        ))
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-encodedbodysize
    pub(crate) fn encoded_body_size(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // The encodedBodySize getter steps are to return this's resource info's encoded size.
        Ok(self.response_body_info.encoded_size)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-decodedbodysize
    pub(crate) fn decoded_body_size(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        // The decodedBodySize getter steps are to return this's resource info's decoded size.
        Ok(self.response_body_info.decoded_size)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-transfersize
    pub(crate) fn transfer_size(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        if let Some(cache_mode) = self.cache_mode {
            // 1. If this's cache mode is "local", then return 0.
            if cache_mode == CacheState::Local {
                return Ok(0);
            }

            // 2. If this's cache mode is "validated", then return 300.
            if cache_mode == CacheState::Validated {
                return Ok(300);
            }
        }

        // 3. Return this's response body info's encoded size plus 300.
        // Spec Note: The constant number added to transferSize replaces exposing the total byte size of the HTTP headers,
        //            as that may expose the presence of certain cookies. See this issue: https://github.com/w3c/resource-timing/issues/238
        Ok(self.response_body_info.encoded_size + 300)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-responsestatus
    pub(crate) fn response_status(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        // The responseStatus getter steps are to return this's response status.
        // Spec Note: responseStatus is determined in Fetch. For a cross-origin no-cors request it would be 0 because the
        // response would be an opaque filtered response.
        Ok(self.response_status)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-renderblockingstatus
    pub(crate) fn render_blocking_status(&self, _: &mut CallContext) -> WebIdlResult<RenderBlockingStatusType> {
        // The renderBlockingStatus getter steps are to return blocking if this's timing info's render-blocking is true;
        // otherwise non-blocking.
        if self.timing_info.read().render_blocking {
            return Ok(RenderBlockingStatusType::Blocking);
        }
        Ok(RenderBlockingStatusType::NonBlocking)
    }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-contenttype
    pub(crate) fn content_type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The contentType getter steps are to return this's resource info's content type.
        Ok(self.response_body_info.content_type.clone())
    }

    // https://w3c.github.io/server-timing/#dom-performanceresourcetiming-servertiming
    pub(crate) fn server_timing<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
    ) -> WebIdlResult<MarkedVec<'vm, Ref<PerformanceServerTiming>>> {
        // The serverTiming getter steps are the following:
        // 1. Let entries be a new list.
        let entries = MarkedVec::new(cx.vm());

        // 2. For each field in this’s timing info’s server-timing headers:
        for field in self.timing_info.server_timing_headers() {
            // 1. Let metric be the result of parsing field.
            let metric = parse_a_server_timing_header_field(&field);

            // 2. If metric is not null, append metric to entries.
            if let Some(metric) = metric {
                entries.push(allocate(cx.vm(), |_| metric));
            }
        }

        // 3. Return entries.
        Ok(entries)
    }
}

// https://w3c.github.io/resource-timing/#dfn-mark-resource-timing
#[expect(clippy::too_many_arguments, reason = "these are the inputs of the algorithm")]
fn mark_resource_timing(
    vm: &Vm,
    timing_info: CppFetchTimingInfo,
    requested_url: Utf16String,
    initiator_type: Utf16String,
    realm: Gc<Realm>,
    cache_mode: Option<CacheState>,
    body_info: ResponseBodyInfo,
    response_status: u16,
    delivery_type: Utf16String,
) {
    // 1. Create a PerformanceResourceTiming object entry in global's realm.
    // 2. Setup the resource timing entry for entry, given initiatorType, requestedURL, timingInfo, cacheMode, bodyInfo,
    //    responseStatus, and deliveryType.
    let entry = PerformanceResourceTiming::setup_the_resource_timing_entry(
        realm,
        initiator_type,
        requested_url,
        timing_info,
        cache_mode,
        body_info,
        response_status,
        delivery_type,
    );
    let entry = allocate(vm, |_| entry);
    let entry = AnyPerformanceEntry::PerformanceResourceTiming(entry);
    entry.entry().set_object(entry);

    // 3. Queue entry.
    let timeline = PerformanceTimeline::of(vm, realm);
    timeline.queue_performance_entry(vm, &entry);

    // 4. Add entry to global's performance entry buffer.
    timeline.add_resource_timing_entry(vm, &entry);
}

/// https://w3c.github.io/resource-timing/#dfn-mark-resource-timing
///
/// # Safety
///
/// `vm` must be the live VM, `realm` the live realm of the global object, `timing_info` a fetch timing info that LibWeb
/// hands over a reference to, `requested_url` and the content type of `body_info` raw AK::Utf16Strings that LibWeb hands
/// over, and `initiator_type` and `delivery_type` raw AK::Utf16FlyStrings that LibWeb lends for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_resource_timing_mark_resource_timing(
    vm: *mut c_void,
    realm: *mut c_void,
    timing_info: *mut c_void,
    requested_url: usize,
    initiator_type: usize,
    cache_mode: u8,
    body_info: ResponseBodyInfoAbi,
    response_status: u16,
    delivery_type: usize,
) {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller hands over a reference to the timing info and the strings, and lends the fly strings.
    unsafe {
        mark_resource_timing(
            vm,
            CppFetchTimingInfo::adopt(timing_info),
            Utf16String::from_raw_owned(requested_url),
            Utf16FlyString::from_raw_borrowed(initiator_type).into(),
            realm,
            CacheState::from_abi(cache_mode),
            body_info.into_body_info(),
            response_status,
            Utf16FlyString::from_raw_borrowed(delivery_type).into(),
        );
    }
}
