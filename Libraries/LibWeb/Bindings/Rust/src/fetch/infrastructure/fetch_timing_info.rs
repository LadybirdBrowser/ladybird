/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Span;

unsafe extern "C" {
    fn web_fetch_timing_info_read(timing_info: *mut c_void, values: *mut FetchTimingInfo);
    fn web_fetch_timing_info_alpn_negotiated_protocol(
        timing_info: *mut c_void,
        context: *mut c_void,
        callback: unsafe extern "C" fn(*mut c_void, Span<'_, u8>),
    );
    fn web_fetch_timing_info_for_each_server_timing_header(
        timing_info: *mut c_void,
        context: *mut c_void,
        callback: unsafe extern "C" fn(*mut c_void, Span<'_, u8>),
    );
    fn web_fetch_timing_info_unref(timing_info: *mut c_void);
}

/// https://fetch.spec.whatwg.org/#connection-timing-info
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub(crate) struct ConnectionTimingInfo {
    pub(crate) domain_lookup_start_time: f64,
    pub(crate) domain_lookup_end_time: f64,
    pub(crate) connection_start_time: f64,
    pub(crate) connection_end_time: f64,
    pub(crate) secure_connection_start_time: f64,
}

/// https://fetch.spec.whatwg.org/#fetch-timing-info
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub(crate) struct FetchTimingInfo {
    pub(crate) start_time: f64,
    pub(crate) redirect_start_time: f64,
    pub(crate) redirect_end_time: f64,
    pub(crate) post_redirect_start_time: f64,
    pub(crate) final_service_worker_start_time: f64,
    pub(crate) final_network_request_start_time: f64,
    pub(crate) first_interim_network_response_start_time: f64,
    pub(crate) final_network_response_start_time: f64,
    pub(crate) end_time: f64,
    final_connection_timing_info: ConnectionTimingInfo,
    has_final_connection_timing_info: bool,
    pub(crate) render_blocking: bool,
}

impl FetchTimingInfo {
    pub(crate) fn final_connection_timing_info(&self) -> Option<ConnectionTimingInfo> {
        self.has_final_connection_timing_info
            .then_some(self.final_connection_timing_info)
    }
}

/// https://fetch.spec.whatwg.org/#fetch-timing-info
pub(crate) struct CppFetchTimingInfo(NonNull<c_void>);

impl CppFetchTimingInfo {
    /// # Safety
    ///
    /// `timing_info` must be a fetch timing info that LibWeb hands over a reference to.
    pub(crate) unsafe fn adopt(timing_info: *mut c_void) -> Self {
        Self(NonNull::new(timing_info).expect("LibWeb hands over a fetch timing info"))
    }

    pub(crate) fn read(&self) -> FetchTimingInfo {
        let mut values = FetchTimingInfo::default();
        // SAFETY: This holds a reference to the timing info, and LibWeb writes its values.
        unsafe { web_fetch_timing_info_read(self.0.as_ptr(), &raw mut values) };
        values
    }

    /// https://fetch.spec.whatwg.org/#connection-timing-info-alpn-negotiated-protocol
    pub(crate) fn alpn_negotiated_protocol(&self) -> Vec<u8> {
        unsafe extern "C" fn copy(context: *mut c_void, protocol: Span<'_, u8>) {
            // SAFETY: The context is the protocol below, and LibWeb lends the bytes for the call.
            *unsafe { &mut *context.cast::<Vec<u8>>() } = protocol.as_slice().to_vec();
        }
        let mut protocol = Vec::new();
        // SAFETY: This holds a reference to the timing info, and the protocol outlives the call.
        unsafe { web_fetch_timing_info_alpn_negotiated_protocol(self.0.as_ptr(), (&raw mut protocol).cast(), copy) };
        protocol
    }

    /// https://fetch.spec.whatwg.org/#fetch-timing-info-server-timing-headers
    pub(crate) fn server_timing_headers(&self) -> Vec<Vec<u8>> {
        unsafe extern "C" fn append(context: *mut c_void, header: Span<'_, u8>) {
            // SAFETY: The context is the list below, and LibWeb lends the header for the call.
            unsafe { &mut *context.cast::<Vec<Vec<u8>>>() }.push(header.as_slice().to_vec());
        }
        let mut headers = Vec::new();
        // SAFETY: This holds a reference to the timing info, and the list outlives the call.
        unsafe {
            web_fetch_timing_info_for_each_server_timing_header(self.0.as_ptr(), (&raw mut headers).cast(), append);
        };
        headers
    }
}

impl Drop for CppFetchTimingInfo {
    fn drop(&mut self) {
        // SAFETY: This holds a reference to the timing info, which it lets go of.
        unsafe { web_fetch_timing_info_unref(self.0.as_ptr()) };
    }
}
