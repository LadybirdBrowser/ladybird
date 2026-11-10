/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libwebcommon_rust::mime_sniff::mime_type::MimeType;

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_fetch_response_is_network_error(response: *mut c_void) -> bool;
    fn web_fetch_response_aborted(response: *mut c_void) -> bool;
    fn web_fetch_response_status(response: *mut c_void) -> u16;
    fn web_fetch_response_status_message(
        response: *mut c_void,
        bytes: *mut c_void,
        on_bytes: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
    );
    fn web_fetch_response_get_header(
        response: *mut c_void,
        name: ak::Span<'_, u8>,
        bytes: *mut c_void,
        on_bytes: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
    ) -> bool;
    fn web_fetch_response_sort_and_combine(
        response: *mut c_void,
        headers: *mut c_void,
        on_header: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, ak::Span<'_, u8>),
    );
    fn web_fetch_response_extract_length(response: *mut c_void, out_length: *mut u64) -> bool;
    fn web_fetch_response_extract_mime_type(response: *mut c_void) -> usize;
    fn web_fetch_response_url(response: *mut c_void, exclude_fragment: bool) -> usize;
    fn web_fetch_response_has_body(response: *mut c_void) -> bool;
    fn web_fetch_response_network_error_message(response: *mut c_void) -> usize;
}

unsafe extern "C" fn on_bytes(bytes: *mut c_void, lent: ak::Span<'_, u8>) {
    // SAFETY: bytes is the vector of the caller, and LibWeb lends the bytes for the call.
    unsafe { *bytes.cast::<Vec<u8>>() = lent.as_slice().to_vec() };
}

unsafe extern "C" fn on_header(headers: *mut c_void, name: ak::Span<'_, u8>, value: ak::Span<'_, u8>) {
    // SAFETY: headers is the list of the caller, and LibWeb lends the name and the value for the call.
    unsafe {
        (*headers.cast::<Vec<(Vec<u8>, Vec<u8>)>>()).push((name.as_slice().to_vec(), value.as_slice().to_vec()));
    }
}

/// # Safety
///
/// `string` must be 0 or a raw string that LibWeb hands over.
unsafe fn string_if_not_null(string: usize) -> Option<Utf16String> {
    // SAFETY: The caller passes a raw string that LibWeb hands over.
    (string != 0).then(|| unsafe { Utf16String::from_raw_owned(string) })
}

/// https://fetch.spec.whatwg.org/#concept-response
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppResponseView(CppCell);

impl CppResponseView {
    /// # Safety
    ///
    /// `response` must be a live ResponseView.
    pub(crate) unsafe fn from_raw(response: *mut c_void) -> Self {
        // SAFETY: The caller passes a live ResponseView.
        Self(unsafe { CppCell::from_raw(response) })
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://fetch.spec.whatwg.org/#concept-network-error
    pub(crate) fn is_network_error(&self) -> bool {
        // SAFETY: The response is live.
        unsafe { web_fetch_response_is_network_error(self.as_ptr()) }
    }

    /// https://fetch.spec.whatwg.org/#concept-response-aborted
    pub(crate) fn aborted(&self) -> bool {
        // SAFETY: The response is live.
        unsafe { web_fetch_response_aborted(self.as_ptr()) }
    }

    /// https://fetch.spec.whatwg.org/#concept-response-status
    pub(crate) fn status(&self) -> u16 {
        // SAFETY: The response is live.
        unsafe { web_fetch_response_status(self.as_ptr()) }
    }

    /// https://fetch.spec.whatwg.org/#concept-response-status-message
    pub(crate) fn status_message(&self) -> Vec<u8> {
        let mut bytes = Vec::new();
        // SAFETY: The response is live, and the bytes outlive the call.
        unsafe { web_fetch_response_status_message(self.as_ptr(), (&raw mut bytes).cast(), on_bytes) };
        bytes
    }

    /// https://fetch.spec.whatwg.org/#concept-header-list-get
    pub(crate) fn get_header(&self, name: &[u8]) -> Option<Vec<u8>> {
        let mut bytes = Vec::new();
        // SAFETY: The response is live, the name is lent for the call, and the bytes outlive it.
        unsafe { web_fetch_response_get_header(self.as_ptr(), ak::Span::new(name), (&raw mut bytes).cast(), on_bytes) }
            .then_some(bytes)
    }

    /// https://fetch.spec.whatwg.org/#concept-header-list-sort-and-combine
    pub(crate) fn sort_and_combine(&self) -> Vec<(Vec<u8>, Vec<u8>)> {
        let mut headers = Vec::new();
        // SAFETY: The response is live, and the headers outlive the call.
        unsafe { web_fetch_response_sort_and_combine(self.as_ptr(), (&raw mut headers).cast(), on_header) };
        headers
    }

    /// https://fetch.spec.whatwg.org/#header-list-extract-a-length
    pub(crate) fn extract_length(&self) -> Option<u64> {
        let mut length = 0;
        // SAFETY: The response is live, and the length outlives the call.
        unsafe { web_fetch_response_extract_length(self.as_ptr(), &raw mut length) }.then_some(length)
    }

    /// https://fetch.spec.whatwg.org/#concept-header-extract-mime-type
    pub(crate) fn extract_mime_type(&self) -> Option<MimeType> {
        // SAFETY: The response is live, and LibWeb hands over 0 or the serialization of the MIME type.
        let serialized = unsafe { string_if_not_null(web_fetch_response_extract_mime_type(self.as_ptr())) }?;
        let code_points: Vec<char> = char::decode_utf16(serialized.to_utf16().iter().copied())
            .map(|code_point| code_point.expect("the serialization of a MIME type is well-formed"))
            .collect();
        Some(MimeType::parse(&code_points).expect("the serialization of a MIME type parses"))
    }

    /// https://fetch.spec.whatwg.org/#concept-response-url
    pub(crate) fn url(&self, exclude_fragment: bool) -> Option<Utf16String> {
        // SAFETY: The response is live, and LibWeb hands over 0 or the serialization of the URL.
        unsafe { string_if_not_null(web_fetch_response_url(self.as_ptr(), exclude_fragment)) }
    }

    /// https://fetch.spec.whatwg.org/#concept-response-body
    pub(crate) fn has_body(&self) -> bool {
        // SAFETY: The response is live.
        unsafe { web_fetch_response_has_body(self.as_ptr()) }
    }

    pub(crate) fn network_error_message(&self) -> Option<Utf16String> {
        // SAFETY: The response is live, and LibWeb hands over 0 or the message.
        unsafe { string_if_not_null(web_fetch_response_network_error_message(self.as_ptr())) }
    }
}
