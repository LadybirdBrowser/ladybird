/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! XMLHttpRequest, https://xhr.spec.whatwg.org/#interface-xmlhttprequest

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::time::{Duration, Instant};

use ak::Utf16String;
use encoding_rs::{Encoding, UTF_8};
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferShared, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, JSONObject, Object,
    PrimitiveString, Realm, Trace, Utf16View, Value, Visitor, Vm,
};
use liburl_rust::{RustUrl, Url};
use libwebcommon_rust::mime_sniff::mime_type::MimeType;

use super::form_data::FormData;
use super::progress_event::{ProgressEvent, ProgressEventInit};
use super::xml_http_request_event_target::XMLHttpRequestEventTarget;
use super::xml_http_request_upload::XMLHttpRequestUpload;
use crate::bindings::conversions::{BufferSource, ByteString, CppInterface, UsvString};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::event_target::{CppEvent, CppEventTarget, EventListenerListChanged, GetTheParent};
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{
    Inherits, Ref, Wrappable, allocate, cell_into_abi, realm_is_window, with_temporary_execution_context, wrap,
};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::realm_data::{WrapperWorld, wrapper_world_of};
use crate::bindings::{CallContext, Exception, MustNotThrow as _, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::dom::event::EventInit;
use crate::domurl::url_input;
use crate::domurl::url_search_params::URLSearchParams;
use crate::fetch::body_init::{XMLHttpRequestBodyInit, safely_extract_body};
use crate::fetch::infrastructure::fetch_controller::CppFetchController;
use crate::fetch::infrastructure::http::bodies::CppBody;
use crate::fetch::infrastructure::http::requests::CppRequest;
use crate::fetch::infrastructure::http::response_view::CppResponseView;
use crate::file_api::blob::{AnyBlob, Blob};
use crate::platform::timer::CppTimer;

#[repr(C)]
struct RequestAbi<'a> {
    method: ak::Span<'a, u8>,
    url: *const RustUrl,
    header_names: *const ak::Span<'a, u8>,
    header_values: *const ak::Span<'a, u8>,
    header_count: usize,
    body: *mut c_void,
    use_cors_preflight: bool,
    include_credentials: bool,
    use_url_credentials: bool,
}

unsafe extern "C" {
    fn web_xhr_is_method(method: ak::Span<'_, u8>) -> bool;
    fn web_xhr_is_forbidden_method(method: ak::Span<'_, u8>) -> bool;
    fn web_xhr_normalize_method(
        method: ak::Span<'_, u8>,
        bytes: *mut c_void,
        on_bytes: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
    );
    fn web_xhr_normalize_header_value(
        value: ak::Span<'_, u8>,
        bytes: *mut c_void,
        on_bytes: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
    );
    fn web_xhr_is_header_name(name: ak::Span<'_, u8>) -> bool;
    fn web_xhr_is_header_value(value: ak::Span<'_, u8>) -> bool;
    fn web_xhr_is_forbidden_request_header(name: ak::Span<'_, u8>, value: ak::Span<'_, u8>) -> bool;
    fn web_xhr_document_is_html_document(document: *mut c_void) -> bool;
    fn web_xhr_document_as_body(realm: *mut c_void, document: *mut c_void) -> *mut c_void;
    fn web_xhr_create_request(realm: *mut c_void, request: *const RequestAbi<'_>) -> *mut c_void;
    fn web_xhr_fetch(
        realm: *mut c_void,
        request: *mut c_void,
        xhr: *mut c_void,
        sync_send: *mut c_void,
        request_body_length: u64,
    ) -> *mut c_void;
    fn web_xhr_incrementally_read_body(realm: *mut c_void, response: *mut c_void, xhr: *mut c_void, length: u64);
    fn web_xhr_pause_until_sync_send_is_over(sync_send: *mut c_void);
    fn web_html_get_an_xml_encoding(
        bytes: ak::Span<'_, u8>,
        label: *mut c_void,
        on_label: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
    );
    fn web_xhr_create_document_response(
        global_object: *mut c_void,
        response: *mut c_void,
        received_bytes: ak::Span<'_, u8>,
        final_mime: usize,
        final_mime_is_html: bool,
        final_encoding: ak::Span<'_, u8>,
        has_final_encoding: bool,
    ) -> *mut c_void;
}

unsafe extern "C" fn on_bytes(bytes: *mut c_void, lent: ak::Span<'_, u8>) {
    // SAFETY: bytes is the vector of the caller, and LibWeb lends the bytes for the call.
    unsafe { *bytes.cast::<Vec<u8>>() = lent.as_slice().to_vec() };
}

unsafe extern "C" fn on_label(label: *mut c_void, lent: ak::Span<'_, u8>) {
    // SAFETY: label is the label of the caller, and LibWeb lends the label for the call.
    unsafe { *label.cast::<Option<Vec<u8>>>() = Some(lent.as_slice().to_vec()) };
}

// https://xhr.spec.whatwg.org/#xmlhttprequestresponsetype
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum XMLHttpRequestResponseType {
    Empty,
    Arraybuffer,
    Blob,
    Document,
    Json,
    Text,
}

pub(crate) enum DocumentOrXMLHttpRequestBodyInit {
    Document(CppInterface),
    Blob(AnyBlob),
    BufferSource(BufferSource),
    FormData(Ref<FormData>),
    URLSearchParams(Ref<URLSearchParams>),
    USVString(UsvString),
}

// https://xhr.spec.whatwg.org/#concept-xmlhttprequest-state
#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Unsent = 0,
    Opened = 1,
    HeadersReceived = 2,
    Loading = 3,
    Done = 4,
}

#[derive(Clone)]
enum Response {
    NetworkError { message: &'static str, aborted: bool },
    Fetched(CppResponseView),
}

// SAFETY: A response that the fetch handed over is the only cell this reaches.
unsafe impl Trace for Response {
    fn trace(&self, visitor: &mut Visitor) {
        if let Self::Fetched(response) = self {
            response.trace(visitor);
        }
    }
}

impl Response {
    fn network_error(message: &'static str) -> Self {
        Self::NetworkError {
            message,
            aborted: false,
        }
    }

    fn aborted_network_error() -> Self {
        Self::NetworkError {
            message: "Fetch was aborted",
            aborted: true,
        }
    }

    fn fetched(&self) -> Option<&CppResponseView> {
        match self {
            Self::NetworkError { .. } => None,
            Self::Fetched(response) => Some(response),
        }
    }

    fn is_network_error(&self) -> bool {
        self.fetched().is_none_or(CppResponseView::is_network_error)
    }

    fn aborted(&self) -> bool {
        match self {
            Self::NetworkError { aborted, .. } => *aborted,
            Self::Fetched(response) => response.aborted(),
        }
    }

    fn has_body(&self) -> bool {
        self.fetched().is_some_and(CppResponseView::has_body)
    }

    fn network_error_message(&self) -> Option<Utf16String> {
        match self {
            Self::NetworkError { message, .. } => Some(Utf16String::from_ascii(message.as_bytes())),
            Self::Fetched(response) => response.network_error_message(),
        }
    }

    fn extract_length(&self) -> Option<u64> {
        self.fetched().and_then(CppResponseView::extract_length)
    }
}

/// https://xhr.spec.whatwg.org/#response-object
enum ResponseObject {
    Null,
    Document(CppCell),
    Blob(Ref<Blob>),
}

// SAFETY: The Document and the Blob are the only cells this reaches.
unsafe impl Trace for ResponseObject {
    fn trace(&self, visitor: &mut Visitor) {
        match self {
            Self::Null => {}
            Self::Document(document) => document.trace(visitor),
            Self::Blob(blob) => blob.trace(visitor),
        }
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct CachedResponseObject {
    #[gc(untraced)]
    wrapper_world: WrapperWorld,
    object: Value,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SyncSend {
    #[gc(untraced)]
    processed_response: Cell<bool>,
    #[gc(untraced)]
    did_time_out: Cell<bool>,
}

static SYNC_SEND_CLASS: EmbedderClass<SyncSend> = EmbedderClass::new("XMLHttpRequestSyncSend");

impl EmbedderValue for SyncSend {
    const CLASS: &'static EmbedderClass<Self> = &SYNC_SEND_CLASS;
}

pub(crate) type SyncSendRef = Gc<EmbedderCell<SyncSend>>;

impl SyncSend {
    pub(crate) fn is_over(&self) -> bool {
        self.processed_response.get() || self.did_time_out.get()
    }
}

pub(crate) enum NullOrFailureOrBytes<'a> {
    Null,
    Failure,
    Bytes(&'a [u8]),
}

// https://xhr.spec.whatwg.org/#xmlhttprequest
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct XMLHttpRequest {
    target: XMLHttpRequestEventTarget,
    realm: Gc<Realm>,

    // https://xhr.spec.whatwg.org/#upload-object
    // upload object
    //     An XMLHttpRequestUpload object.
    upload_object: Ref<XMLHttpRequestUpload>,

    // https://xhr.spec.whatwg.org/#concept-xmlhttprequest-state
    // state
    //     One of unsent, opened, headers received, loading, and done; initially unsent.
    #[gc(untraced)]
    state: Cell<State>,

    // https://xhr.spec.whatwg.org/#send-flag
    // send() flag
    //     A flag, initially unset.
    #[gc(untraced)]
    send: Cell<bool>,

    // https://xhr.spec.whatwg.org/#timeout
    // timeout
    //     An unsigned integer, initially 0.
    #[gc(untraced)]
    timeout: Cell<u32>,

    // https://xhr.spec.whatwg.org/#cross-origin-credentials
    // cross-origin credentials
    //     A boolean, initially false.
    #[gc(untraced)]
    cross_origin_credentials: Cell<bool>,

    // https://xhr.spec.whatwg.org/#request-method
    // request method
    //     A method.
    #[gc(untraced)]
    request_method: RefCell<Vec<u8>>,

    // https://xhr.spec.whatwg.org/#request-url
    // request URL
    //     A URL.
    #[gc(untraced)]
    request_url: RefCell<Option<Url>>,

    // https://xhr.spec.whatwg.org/#author-request-headers
    // author request headers
    //     A header list, initially empty.
    #[gc(untraced)]
    author_request_headers: RefCell<Vec<(Vec<u8>, Vec<u8>)>>,

    // https://xhr.spec.whatwg.org/#request-body
    // request body
    //     Initially null.
    request_body: GcRefCell<Option<CppBody>>,

    // https://xhr.spec.whatwg.org/#synchronous-flag
    // synchronous flag
    //     A flag, initially unset.
    #[gc(untraced)]
    synchronous: Cell<bool>,

    // https://xhr.spec.whatwg.org/#upload-complete-flag
    // upload complete flag
    //     A flag, initially unset.
    #[gc(untraced)]
    upload_complete: Cell<bool>,

    // https://xhr.spec.whatwg.org/#upload-listener-flag
    // upload listener flag
    //     A flag, initially unset.
    #[gc(untraced)]
    upload_listener: Cell<bool>,

    // https://xhr.spec.whatwg.org/#timed-out-flag
    // timed out flag
    //     A flag, initially unset.
    #[gc(untraced)]
    timed_out: Cell<bool>,

    // https://xhr.spec.whatwg.org/#response
    // response
    //     A response, initially a network error.
    response: GcRefCell<Response>,

    // https://xhr.spec.whatwg.org/#received-bytes
    // received bytes
    //     A byte sequence, initially the empty byte sequence.
    #[gc(untraced)]
    received_bytes: RefCell<Vec<u8>>,

    // https://xhr.spec.whatwg.org/#response-type
    // response type
    //     One of the empty string, "arraybuffer", "blob", "document", "json", and "text"; initially the empty string.
    #[gc(untraced)]
    response_type: Cell<XMLHttpRequestResponseType>,

    // https://xhr.spec.whatwg.org/#response-object
    // response object
    //     An object, failure, or null, initially null.
    response_object: GcRefCell<ResponseObject>,
    cached_response_objects: GcRefCell<Vec<CachedResponseObject>>,

    // https://xhr.spec.whatwg.org/#xmlhttprequest-fetch-controller
    // fetch controller
    //     A fetch controller, initially a new fetch controller.
    //     NOTE: The send() method sets it to a useful fetch controller, but for simplicity it always holds a fetch controller.
    fetch_controller: GcRefCell<CppFetchController>,

    // https://xhr.spec.whatwg.org/#override-mime-type
    // override MIME type
    //     A MIME type or null, initially null.
    //     NOTE: Can get a value when overrideMimeType() is invoked.
    #[gc(untraced)]
    override_mime_type: RefCell<Option<MimeType>>,

    // Non-standard, see async path in `send()`
    #[gc(untraced)]
    request_body_transmitted: Cell<u64>,
    #[gc(untraced)]
    last_upload_progress_timestamp: Cell<Option<Instant>>,
    #[gc(untraced)]
    last_download_progress_timestamp: Cell<Option<Instant>>,

    #[gc(untraced)]
    async_request: RefCell<Option<CppRequest>>,
    #[gc(untraced)]
    async_send_start_time: Cell<Option<Instant>>,
    timeout_timer: GcRefCell<Option<CppTimer>>,
}

impl Inherits<XMLHttpRequestEventTarget> for XMLHttpRequest {
    fn parent(&self) -> &XMLHttpRequestEventTarget {
        &self.target
    }
}

impl Wrappable for XMLHttpRequest {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.target.cpp_parent()
    }
}

impl GetTheParent for XMLHttpRequest {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl EventListenerListChanged for XMLHttpRequest {
    fn event_listener_list_changed(this: Ref<Self>) {
        this.update_pending_activity();
    }
}

fn bytes_to_code_points(bytes: &[u8]) -> Vec<char> {
    bytes.iter().map(|&byte| char::from(byte)).collect()
}

fn string_to_code_points(string: &Utf16String) -> Vec<char> {
    char::decode_utf16(string.to_utf16().iter().copied())
        .map(|code_point| code_point.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect()
}

fn new_mime_type(r#type: &str, subtype: &str) -> MimeType {
    MimeType::new(String::from(r#type), String::from(subtype))
}

// https://infra.spec.whatwg.org/#isomorphic-encode
fn isomorphic_encode(string: &str) -> Vec<u8> {
    string
        .chars()
        .map(|code_point| u8::try_from(u32::from(code_point)).expect("the code point is at most U+00FF"))
        .collect()
}

// https://fetch.spec.whatwg.org/#header-list-contains
fn find_header(list: &[(Vec<u8>, Vec<u8>)], name: &[u8]) -> Option<usize> {
    list.iter()
        .position(|(header_name, _)| header_name.eq_ignore_ascii_case(name))
}

// https://fetch.spec.whatwg.org/#concept-header-list-get
fn get_header(list: &[(Vec<u8>, Vec<u8>)], name: &[u8]) -> Option<Vec<u8>> {
    // 1. If list does not contain name, then return null.
    find_header(list, name)?;

    // 2. Return the values of all headers in list whose name is a byte-case-insensitive match for name, separated from
    //    each other by 0x2C 0x20, in order.
    let values: Vec<&[u8]> = list
        .iter()
        .filter(|(header_name, _)| header_name.eq_ignore_ascii_case(name))
        .map(|(_, value)| value.as_slice())
        .collect();
    Some(values.join(&b", "[..]))
}

// https://fetch.spec.whatwg.org/#concept-header-list-set
fn set_header(list: &mut Vec<(Vec<u8>, Vec<u8>)>, name: &[u8], value: Vec<u8>) {
    // 1. If list contains name, then set the value of the first such header to value and remove the others.
    if let Some(index) = find_header(list, name) {
        list[index].1 = value;
        let mut position = 0;
        list.retain(|(header_name, _)| {
            let keep = position <= index || !header_name.eq_ignore_ascii_case(name);
            position += 1;
            keep
        });
    }
    // 2. Otherwise, append (name, value) to list.
    else {
        list.push((name.to_vec(), value));
    }
}

// https://fetch.spec.whatwg.org/#concept-header-list-combine
fn combine_header(list: &mut Vec<(Vec<u8>, Vec<u8>)>, name: Vec<u8>, value: Vec<u8>) {
    // 1. If list contains name, then set the value of the first such header to its value, followed by 0x2C 0x20,
    //    followed by value.
    if let Some(index) = find_header(list, &name) {
        let header_value = &mut list[index].1;
        header_value.extend_from_slice(b", ");
        header_value.extend_from_slice(&value);
    }
    // 2. Otherwise, append (name, value) to list.
    else {
        list.push((name, value));
    }
}

// https://xhr.spec.whatwg.org/#legacy-uppercased-byte-less-than
fn is_legacy_uppercased_byte_less_than(a: &[u8], b: &[u8]) -> bool {
    // 1. Let A be a, byte-uppercased.
    let uppercased_a = a.to_ascii_uppercase();

    // 2. Let B be b, byte-uppercased.
    let uppercased_b = b.to_ascii_uppercase();

    // 3. Return A is byte less than B.
    uppercased_a < uppercased_b
}

// https://xhr.spec.whatwg.org/#concept-event-fire-progress
fn fire_progress_event(
    vm: &Vm,
    realm: Gc<Realm>,
    target: &CppEventTarget,
    event_name: &str,
    transmitted: u64,
    length: u64,
) {
    // To fire a progress event named e at target, given transmitted and length, means to fire an event named e at target, using ProgressEvent,
    // with the loaded attribute initialized to transmitted, and if length is not 0, with the lengthComputable attribute initialized to true
    // and the total attribute initialized to length.
    let event_init = ProgressEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        length_computable: length != 0,
        loaded: transmitted as f64,
        total: length as f64,
    };
    let event = ProgressEvent::create(vm, realm, &Utf16String::from_ascii(event_name.as_bytes()), event_init);
    target.dispatch_event(event.event());
}

fn current_global_object_is_window(vm: &Vm) -> bool {
    realm_is_window(vm.current_realm().expect("the steps run in a realm"))
}

impl XMLHttpRequest {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        let upload_object = XMLHttpRequestUpload::create(vm);
        let fetch_controller = CppFetchController::create(realm.global_object());
        let xhr = allocate(vm, |_| Self {
            target: XMLHttpRequestEventTarget::new(),
            realm,
            upload_object,
            state: Cell::new(State::Unsent),
            send: Cell::new(false),
            timeout: Cell::new(0),
            cross_origin_credentials: Cell::new(false),
            request_method: RefCell::new(Vec::new()),
            request_url: RefCell::new(None),
            author_request_headers: RefCell::new(Vec::new()),
            request_body: GcRefCell::new(None),
            synchronous: Cell::new(false),
            upload_complete: Cell::new(false),
            upload_listener: Cell::new(false),
            timed_out: Cell::new(false),
            response: GcRefCell::new(Response::network_error("Not sent yet")),
            received_bytes: RefCell::new(Vec::new()),
            response_type: Cell::new(XMLHttpRequestResponseType::Empty),
            response_object: GcRefCell::new(ResponseObject::Null),
            cached_response_objects: GcRefCell::new(Vec::new()),
            fetch_controller: GcRefCell::new(fetch_controller),
            override_mime_type: RefCell::new(None),
            request_body_transmitted: Cell::new(0),
            last_upload_progress_timestamp: Cell::new(None),
            last_download_progress_timestamp: Cell::new(None),
            async_request: RefCell::new(None),
            async_send_start_time: Cell::new(None),
            timeout_timer: GcRefCell::new(None),
        });
        xhr.target.event_target().set_owner(xhr);
        xhr.target
            .event_target()
            .set_event_listener_list_changed_steps::<Self>();
        xhr
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        Ok(Self::create(cx.vm(), cx.realm()))
    }

    fn event_target(&self) -> &CppEventTarget {
        self.target.event_target()
    }

    fn relevant_global_object(&self) -> Gc<Object> {
        self.realm.global_object()
    }

    fn fetch_controller(&self) -> CppFetchController {
        self.fetch_controller.borrow().clone()
    }

    fn set_response(&self, response: Response) {
        self.response.replace(response);
    }

    fn response_record(&self) -> Response {
        self.response.borrow().clone()
    }

    fn fire_event(&self, event_name: &str) {
        let event = CppEvent::create(self.realm, event_name, false, false, false);
        self.event_target().dispatch_event(&event);
    }

    fn fire_progress_event(&self, vm: &Vm, event_name: &str, transmitted: u64, length: u64) {
        fire_progress_event(vm, self.realm, self.event_target(), event_name, transmitted, length);
    }

    fn fire_upload_progress_event(&self, vm: &Vm, event_name: &str, transmitted: u64, length: u64) {
        fire_progress_event(
            vm,
            self.realm,
            self.upload_object.event_target(),
            event_name,
            transmitted,
            length,
        );
    }

    fn restart_timeout_timer(this: Ref<Self>, vm: &Vm) {
        this.stop_timeout_timer();
        let Some(send_start_time) = this.async_send_start_time.get() else {
            return;
        };
        let request_is_done = this.async_request.borrow().as_ref().is_none_or(CppRequest::done);
        if !this.send.get() || this.synchronous.get() || request_is_done || this.timeout.get() == 0 {
            return;
        }

        // NB: Timeout changes keep the deadline relative to send(), rather than starting a fresh timeout period.
        let elapsed = Instant::now().saturating_duration_since(send_start_time);
        let remaining = Duration::from_millis(u64::from(this.timeout.get())).saturating_sub(elapsed);
        let milliseconds = u64::try_from(remaining.as_millis()).unwrap_or(u64::MAX);
        let timer = CppTimer::create_single_shot(
            vm,
            milliseconds,
            (this, this.fetch_controller()),
            |vm, (this, fetch_controller)| {
                if this.async_request.borrow().is_none() || this.fetch_controller() != fetch_controller {
                    return;
                }

                this.stop_timeout_timer();
                let request = this
                    .async_request
                    .take()
                    .expect("the timer runs for an ongoing request");
                if !request.done() {
                    this.timed_out.set(true);
                    fetch_controller.terminate();
                    this.handle_errors(vm).must();
                }
            },
        );
        this.timeout_timer.replace(Some(timer));
    }

    fn stop_timeout_timer(&self) {
        if let Some(timer) = self.timeout_timer.replace(None) {
            timer.stop();
        }
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-responsetext
    pub(crate) fn response_text(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. If this’s response type is not the empty string or "text", then throw an "InvalidStateError" DOMException.
        let response_type = self.response_type.get();
        if response_type != XMLHttpRequestResponseType::Empty && response_type != XMLHttpRequestResponseType::Text {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR responseText can only be used for responseType \"\" or \"text\""),
            ));
        }

        // 2. If this’s state is not loading or done, then return the empty string.
        if self.state.get() != State::Loading && self.state.get() != State::Done {
            return Ok(UsvString::from_ascii(b""));
        }

        // 3. Return the result of getting a text response for this.
        Ok(UsvString::from_utf16_string(&self.text_response()))
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-responsexml
    pub(crate) fn response_xml(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // 1. If this’s response type is not the empty string or "document", then throw an "InvalidStateError" DOMException.
        let response_type = self.response_type.get();
        if response_type != XMLHttpRequestResponseType::Empty && response_type != XMLHttpRequestResponseType::Document {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR responseXML can only be used for responseXML \"\" or \"document\""),
            ));
        }

        // 2. If this’s state is not done, then return null.
        if self.state.get() != State::Done {
            return Ok(None);
        }

        // 3. Assert: this’s response object is not failure.
        // NB: Nothing sets the response object to failure, see response().

        // 4. If this’s response object is non-null, then return it.
        if let Some(document) = self.response_document() {
            return Ok(Some(wrap_cpp_cell(cx, &document)));
        }

        // 5. Set a document response for this.
        self.set_document_response();

        // 6. Return this’s response object.
        Ok(self.response_document().map(|document| wrap_cpp_cell(cx, &document)))
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-response
    pub(crate) fn response(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        let vm = cx.vm();
        let realm = cx.realm();
        let wrapper_world = wrapper_world_of(vm, realm);
        let cached_response_object = || {
            self.cached_response_objects
                .borrow()
                .iter()
                .find(|cached| cached.wrapper_world == wrapper_world)
                .map(|cached| cached.object)
        };
        let cache_response_object = |object: Value| {
            self.cached_response_objects
                .borrow_mut(vm)
                .push(CachedResponseObject { wrapper_world, object });
        };

        // 1. If this’s response type is the empty string or "text", then:
        let response_type = self.response_type.get();
        if response_type == XMLHttpRequestResponseType::Empty || response_type == XMLHttpRequestResponseType::Text {
            // 1. If this’s state is not loading or done, then return the empty string.
            if self.state.get() != State::Loading && self.state.get() != State::Done {
                return Ok(Value::from_string(PrimitiveString::create(vm, Utf16String::default())));
            }

            // 2. Return the result of getting a text response for this.
            return Ok(Value::from_string(PrimitiveString::create(vm, self.text_response())));
        }

        // 2. If this’s state is not done, then return null.
        if self.state.get() != State::Done {
            return Ok(Value::NULL);
        }

        // 3. If this’s response object is failure, then return null.
        // NB: Nothing sets the response object to failure, see step 5.

        // 4. If this’s response object is non-null, then return it.
        if let Some(document) = self.response_document() {
            return Ok(Value::from_object(wrap_cpp_cell(cx, &document).wrapper()));
        }
        if let ResponseObject::Blob(blob) = &*self.response_object.borrow() {
            return Ok(Value::from_object(wrap(cx, *blob)));
        }

        // 5. If this’s response type is "arraybuffer",
        if response_type == XMLHttpRequestResponseType::Arraybuffer {
            if let Some(cached_object) = cached_response_object() {
                return Ok(cached_object);
            }

            // then set this’s response object to a new ArrayBuffer object representing this’s received bytes. If this throws
            // an exception, then set this’s response object to failure and return null.
            let buffer =
                ArrayBuffer::create_from_bytes(vm, realm, &self.received_bytes.borrow(), ArrayBufferShared::No);
            let buffer = Value::from_object(buffer);
            cache_response_object(buffer);
            return Ok(buffer);
        }

        // 6. Otherwise, if this’s response type is "blob", set this’s response object to a new Blob object representing
        // this’s received bytes with type set to the result of get a final MIME type for this.
        if response_type == XMLHttpRequestResponseType::Blob {
            let mime_type_as_string = Utf16String::from_utf8(&self.final_mime_type().serialize());
            let bytes = self.received_bytes.borrow().clone();
            let blob = allocate(vm, |_| Blob::new(bytes, mime_type_as_string));
            self.set_response_object(ResponseObject::Blob(blob));
            return Ok(Value::from_object(wrap(cx, blob)));
        }

        // 7. Otherwise, if this’s response type is "document", set a document response for this.
        if response_type == XMLHttpRequestResponseType::Document {
            self.set_document_response();
            return Ok(self.response_document().map_or(Value::NULL, |document| {
                Value::from_object(wrap_cpp_cell(cx, &document).wrapper())
            }));
        }

        // 8. Otherwise:
        // 1. Assert: this’s response type is "json".
        // Note: Automatically done by the layers above us.

        // 2. If this’s response’s body is null, then return null.
        if !self.response.borrow().has_body() {
            return Ok(Value::NULL);
        }

        if let Some(cached_object) = cached_response_object() {
            return Ok(cached_object);
        }

        // 3. Let jsonObject be the result of running parse JSON from bytes on this’s received bytes. If that threw an
        // exception, then return null.
        let json = {
            let received_bytes = self.received_bytes.borrow();
            let (json, _) = UTF_8.decode_with_bom_removal(&received_bytes);
            Utf16String::from_utf8(&json)
        };
        let json_object_result =
            with_temporary_execution_context(realm, || JSONObject::parse_json(vm, Utf16View::of_string(&json), None));
        let Ok(json_object) = json_object_result else {
            return Ok(Value::NULL);
        };

        // 4. Set this’s response object to jsonObject.
        cache_response_object(json_object);

        // 9. Return this’s response object.
        Ok(json_object)
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-responsetype
    pub(crate) fn response_type(&self, _: &mut CallContext) -> WebIdlResult<XMLHttpRequestResponseType> {
        Ok(self.response_type.get())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-responsetype
    pub(crate) fn set_response_type(
        &self,
        cx: &mut CallContext,
        response_type: XMLHttpRequestResponseType,
    ) -> WebIdlResult<()> {
        let current_global_object_is_window = current_global_object_is_window(cx.vm());

        // 1. If the current global object is not a Window object and the given value is "document", then return.
        if !current_global_object_is_window && response_type == XMLHttpRequestResponseType::Document {
            return Ok(());
        }

        // 2. If this’s state is loading or done, then throw an "InvalidStateError" DOMException.
        if self.state.get() == State::Loading || self.state.get() == State::Done {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Can't readyState when XHR is loading or done"),
            ));
        }

        // 3. If the current global object is a Window object and this’s synchronous flag is set, then throw an "InvalidAccessError" DOMException.
        if current_global_object_is_window && self.synchronous.get() {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_ascii(b"Can't set readyState on synchronous XHR in Window environment"),
            ));
        }

        // 4. Set this’s response type to the given value.
        self.response_type.set(response_type);
        Ok(())
    }

    fn response_document(&self) -> Option<CppCell> {
        match &*self.response_object.borrow() {
            ResponseObject::Document(document) => Some(document.clone()),
            ResponseObject::Null | ResponseObject::Blob(_) => None,
        }
    }

    fn set_response_object(&self, response_object: ResponseObject) {
        self.response_object.replace(response_object);
        self.cached_response_objects.replace(Vec::new());
    }

    fn reset_response_object(&self) {
        self.set_response_object(ResponseObject::Null);
    }

    // https://xhr.spec.whatwg.org/#text-response
    fn text_response(&self) -> Utf16String {
        // 1. If xhr’s response’s body is null, then return the empty string.
        if !self.response.borrow().has_body() {
            return Utf16String::default();
        }

        // 2. Let charset be the result of get a final encoding for xhr.
        let mut charset = self.final_encoding();

        let received_bytes = self.received_bytes.borrow();

        // 3. If xhr’s response type is the empty string, charset is null, and the result of get a final MIME type for xhr is an XML MIME type,
        if self.response_type.get() == XMLHttpRequestResponseType::Empty
            && charset.is_none()
            && self.final_mime_type().essence().is_xml()
        {
            // then use the rules set forth in the XML specifications to determine the encoding. Let charset be the determined encoding. [XML] [XML-NAMES]
            // NB: The encoding declaration names the encoding, which this finds the way the HTML Standard does when
            //     sniffing an encoding. Without one, charset stays null and is set to UTF-8 below, as the XML
            //     specification requires.
            let mut xml_encoding: Option<Vec<u8>> = None;
            // SAFETY: The bytes are lent for the call, and the label outlives it.
            unsafe {
                web_html_get_an_xml_encoding(ak::Span::new(&received_bytes), (&raw mut xml_encoding).cast(), on_label);
            }
            if let Some(xml_encoding) = xml_encoding {
                charset = Encoding::for_label(&xml_encoding);
            }
        }

        // 4. If charset is null, then set charset to UTF-8.
        let charset = charset.unwrap_or(UTF_8);

        // 5. Return the result of running decode on xhr’s received bytes using fallback encoding charset.
        let (decoded, _, _) = charset.decode(&received_bytes);
        Utf16String::from_utf8(&decoded)
    }

    // https://xhr.spec.whatwg.org/#document-response
    fn set_document_response(&self) {
        // 1. If xhr’s response’s body is null, then return.
        let response = self.response_record();
        if !response.has_body() {
            return;
        }
        let response = response
            .fetched()
            .expect("a response with a body is one that the fetch handed over");

        // 2. Let finalMIME be the result of get a final MIME type for xhr.
        let final_mime = self.final_mime_type();
        let final_mime_is_html = final_mime.essence().is_html();

        // 3. If finalMIME is not an HTML MIME type or an XML MIME type, then return.
        if !final_mime_is_html && !final_mime.essence().is_xml() {
            return;
        }

        // 4. If xhr’s response type is the empty string and finalMIME is an HTML MIME type, then return.
        if self.response_type.get() == XMLHttpRequestResponseType::Empty && final_mime_is_html {
            return;
        }

        // NB: LibWeb creates and parses the document in steps 5 to 11.
        let final_mime = Utf16String::from_utf8(&final_mime.serialize());
        let final_encoding = self.final_encoding();
        let final_encoding_name = final_encoding.map_or("", Encoding::name);
        let received_bytes = self.received_bytes.borrow().clone();
        // SAFETY: The global object and the response are live, and the bytes, the MIME type and the encoding are lent
        //         for the call. LibWeb returns null or a live Document.
        let document = unsafe {
            let document = web_xhr_create_document_response(
                self.relevant_global_object().as_ptr().cast(),
                response.as_ptr(),
                ak::Span::new(&received_bytes),
                final_mime.raw_identity(),
                final_mime_is_html,
                ak::Span::new(final_encoding_name.as_bytes()),
                final_encoding.is_some(),
            );
            (!document.is_null()).then(|| CppCell::from_raw(document))
        };
        let Some(document) = document else {
            self.reset_response_object();
            return;
        };

        // 12. Set xhr’s response object to document.
        self.set_response_object(ResponseObject::Document(document));
    }

    // https://xhr.spec.whatwg.org/#final-mime-type
    fn final_mime_type(&self) -> MimeType {
        // 1. If xhr’s override MIME type is null, return the result of get a response MIME type for xhr.
        // 2. Return xhr’s override MIME type.
        self.override_mime_type
            .borrow()
            .clone()
            .unwrap_or_else(|| self.response_mime_type())
    }

    // https://xhr.spec.whatwg.org/#response-mime-type
    fn response_mime_type(&self) -> MimeType {
        // 1. Let mimeType be the result of extracting a MIME type from xhr’s response’s header list.
        let mime_type = self
            .response
            .borrow()
            .fetched()
            .and_then(CppResponseView::extract_mime_type);

        // 2. If mimeType is failure, then set mimeType to text/xml.
        // 3. Return mimeType.
        mime_type.unwrap_or_else(|| new_mime_type("text", "xml"))
    }

    // https://xhr.spec.whatwg.org/#final-charset
    fn final_encoding(&self) -> Option<&'static Encoding> {
        // 1. Let label be null.
        let mut label = None;

        // 2. Let responseMIME be the result of get a response MIME type for xhr.
        let response_mime = self.response_mime_type();

        // 3. If responseMIME’s parameters["charset"] exists, then set label to it.
        if let Some((_, charset)) = response_mime.parameters().find(|(name, _)| *name == "charset") {
            label = Some(String::from(charset));
        }

        // 4. If xhr’s override MIME type’s parameters["charset"] exists, then set label to it.
        if let Some(override_mime_type) = &*self.override_mime_type.borrow()
            && let Some((_, charset)) = override_mime_type.parameters().find(|(name, _)| *name == "charset")
        {
            label = Some(String::from(charset));
        }

        // 5. If label is null, then return null.
        // 6. Let encoding be the result of getting an encoding from label.
        // 7. If encoding is failure, then return null.
        // 8. Return encoding.
        Encoding::for_label(label?.as_bytes())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-setrequestheader
    pub(crate) fn set_request_header(
        &self,
        _: &mut CallContext,
        name: ByteString,
        value: ByteString,
    ) -> WebIdlResult<()> {
        // 1. If this’s state is not opened, then throw an "InvalidStateError" DOMException.
        if self.state.get() != State::Opened {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR readyState is not OPENED"),
            ));
        }

        // 2. If this’s send() flag is set, then throw an "InvalidStateError" DOMException.
        if self.send.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR send() flag is already set"),
            ));
        }

        let name = name.into_bytes();

        // 3. Normalize value.
        let mut normalized_value = Vec::new();
        // SAFETY: The value is lent for the call, and the normalized value outlives it.
        unsafe {
            web_xhr_normalize_header_value(
                ak::Span::new(value.as_bytes()),
                (&raw mut normalized_value).cast(),
                on_bytes,
            );
        }
        let value = normalized_value;

        // 4. If name is not a header name or value is not a header value, then throw a "SyntaxError" DOMException.
        // SAFETY: The name is lent for the call.
        if !unsafe { web_xhr_is_header_name(ak::Span::new(&name)) } {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_ascii(b"Header name contains invalid characters."),
            ));
        }
        // SAFETY: The value is lent for the call.
        if !unsafe { web_xhr_is_header_value(ak::Span::new(&value)) } {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_ascii(b"Header value contains invalid characters."),
            ));
        }

        // 5. If (name, value) is a forbidden request-header, then return.
        // SAFETY: The name and the value are lent for the call.
        if unsafe { web_xhr_is_forbidden_request_header(ak::Span::new(&name), ak::Span::new(&value)) } {
            return Ok(());
        }

        // 6. Combine (name, value) in this’s author request headers.
        combine_header(&mut self.author_request_headers.borrow_mut(), name, value);

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-open
    pub(crate) fn open(&self, cx: &mut CallContext, method: ByteString, url: UsvString) -> WebIdlResult<()> {
        // 7. If the async argument is omitted, set async to true, and set username and password to null.
        self.open_with_async(cx, method, url, true, None, None)
    }

    pub(crate) fn open_with_async(
        &self,
        cx: &mut CallContext,
        method: ByteString,
        url: UsvString,
        r#async: bool,
        username: Option<UsvString>,
        password: Option<UsvString>,
    ) -> WebIdlResult<()> {
        // 1. If this’s relevant global object is a Window object and its associated Document is not fully active, then throw an "InvalidStateError" DOMException.
        if let Some(window) = window_from_global_object(self.relevant_global_object())
            && !window.associated_document().is_fully_active()
        {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Invalid state: Window's associated document is not fully active."),
            ));
        }

        // 2. If method is not a method, then throw a "SyntaxError" DOMException.
        // SAFETY: The method is lent for the call.
        if !unsafe { web_xhr_is_method(ak::Span::new(method.as_bytes())) } {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_ascii(b"An invalid or illegal string was specified."),
            ));
        }

        // 3. If method is a forbidden method, then throw a "SecurityError" DOMException.
        // SAFETY: The method is lent for the call.
        if unsafe { web_xhr_is_forbidden_method(ak::Span::new(method.as_bytes())) } {
            return Err(Exception::Dom(
                "SecurityError",
                Utf16String::from_ascii(b"Forbidden method, must not be 'CONNECT', 'TRACE', or 'TRACK'"),
            ));
        }

        // 4. Normalize method.
        let mut normalized_method = Vec::new();
        // SAFETY: The method is lent for the call, and the normalized method outlives it.
        unsafe {
            web_xhr_normalize_method(
                ak::Span::new(method.as_bytes()),
                (&raw mut normalized_method).cast(),
                on_bytes,
            );
        }

        // 5. Let parsedURL be the result of encoding-parsing a URL url, relative to this’s relevant settings object.
        let parsed_url =
            relevant_settings_object(self.relevant_global_object()).encoding_parse_url(url.as_utf16_string());

        // 6. If parsedURL is failure, then throw a "SyntaxError" DOMException.
        let Some(mut parsed_url) = parsed_url else {
            return Err(Exception::Dom("SyntaxError", Utf16String::from_ascii(b"Invalid URL")));
        };

        // 7. If the async argument is omitted, set async to true, and set username and password to null.
        // NOTE: This is handled in the overload lacking the async argument.

        // 8. If parsedURL’s host is non-null, then:
        if parsed_url.serialized_host().is_some() {
            // 1. If the username argument is not null, set the username given parsedURL and username.
            if let Some(username) = &username {
                parsed_url.set_username(url_input(username.units()));
            }
            // 2. If the password argument is not null, set the password given parsedURL and password.
            if let Some(password) = &password {
                parsed_url.set_password(url_input(password.units()));
            }
        }

        // 9. If async is false, the current global object is a Window object, and either this’s timeout is
        //     not 0 or this’s response type is not the empty string, then throw an "InvalidAccessError" DOMException.
        if !r#async
            && current_global_object_is_window(cx.vm())
            && (self.timeout.get() != 0 || self.response_type.get() != XMLHttpRequestResponseType::Empty)
        {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_ascii(
                    b"Synchronous XMLHttpRequests in a Window context do not support timeout or a non-empty responseType",
                ),
            ));
        }

        // 10. Terminate this’s fetch controller.
        // Spec Note: A fetch can be ongoing at this point.
        self.fetch_controller().terminate();
        self.stop_timeout_timer();
        self.async_request.take();
        self.event_target().set_has_pending_activity(self.realm, false);

        // 11. Set variables associated with the object as follows:
        // Unset this’s send() flag.
        self.send.set(false);
        // Set this’s request method to method.
        *self.request_method.borrow_mut() = normalized_method;
        // Set this’s request URL to parsedURL.
        *self.request_url.borrow_mut() = Some(parsed_url);
        // Empty this’s author request headers.
        self.author_request_headers.borrow_mut().clear();
        // Set this’s request body to null.
        self.request_body.replace(None);
        // Set this’s synchronous flag if async is false; otherwise unset this’s synchronous flag.
        self.synchronous.set(!r#async);
        // Unset this’s upload listener flag.
        self.upload_listener.set(false);
        // Set this’s response to a network error.
        self.set_response(Response::network_error("Not yet sent"));
        // Set this’s received bytes to the empty byte sequence.
        self.received_bytes.borrow_mut().clear();
        // Set this’s response object to null.
        self.reset_response_object();
        // Spec Note: Override MIME type is not overridden here as the overrideMimeType() method can be invoked before the open() method.

        // 12. If this’s state is not opened, then:
        if self.state.get() != State::Opened {
            // 1. Set this’s state to opened.
            self.state.set(State::Opened);

            // 2. Fire an event named readystatechange at this.
            self.fire_event("readystatechange");
        }

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn send(
        this: &Ref<Self>,
        cx: &mut CallContext,
        body: Option<DocumentOrXMLHttpRequestBodyInit>,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();
        let associated_realm = this.realm;

        // 1. If this’s state is not opened, then throw an "InvalidStateError" DOMException.
        if this.state.get() != State::Opened {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR readyState is not OPENED"),
            ));
        }

        // 2. If this’s send() flag is set, then throw an "InvalidStateError" DOMException.
        if this.send.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR send() flag is already set"),
            ));
        }

        // 3. If this’s request method is `GET` or `HEAD`, then set body to null.
        let request_method = this.request_method.borrow().clone();
        let body = if request_method == b"GET" || request_method == b"HEAD" {
            None
        } else {
            body
        };

        // 4. If body is not null, then:
        if let Some(body) = body {
            // 1. Let extractedContentType be null.
            let mut extracted_content_type = None;

            // 2. If body is a Document, then set this’s request body to body, serialized, converted, and UTF-8 encoded.
            if let DocumentOrXMLHttpRequestBodyInit::Document(document) = &body {
                // SAFETY: The realm and the Document are live, and LibWeb returns a live body.
                let request_body = unsafe {
                    CppBody::from_raw(web_xhr_document_as_body(
                        associated_realm.as_ptr().cast(),
                        document.wrapper().as_ptr().cast(),
                    ))
                };
                this.request_body.replace(Some(request_body));
            }
            // 3. Otherwise:
            else {
                let body_init = match &body {
                    DocumentOrXMLHttpRequestBodyInit::Document(_) => unreachable!("a Document is handled above"),
                    DocumentOrXMLHttpRequestBodyInit::Blob(blob) => XMLHttpRequestBodyInit::Blob(blob.clone()),
                    DocumentOrXMLHttpRequestBodyInit::BufferSource(buffer_source) => {
                        let mut bytes = Vec::new();
                        // SAFETY: Copying the bytes neither runs script nor allocates a cell.
                        unsafe { buffer_source.with_bytes(|source| bytes.extend_from_slice(source)) };
                        XMLHttpRequestBodyInit::Bytes(bytes)
                    }
                    DocumentOrXMLHttpRequestBodyInit::FormData(form_data) => {
                        XMLHttpRequestBodyInit::FormData(*form_data)
                    }
                    DocumentOrXMLHttpRequestBodyInit::URLSearchParams(url_search_params) => {
                        XMLHttpRequestBodyInit::URLSearchParams(*url_search_params)
                    }
                    DocumentOrXMLHttpRequestBodyInit::USVString(string) => {
                        XMLHttpRequestBodyInit::String(string.clone())
                    }
                };

                // 1. Let bodyWithType be the result of safely extracting body.
                let body_with_type = safely_extract_body(associated_realm, &body_init);

                // 2. Set this’s request body to bodyWithType’s body.
                this.request_body.replace(Some(body_with_type.body));

                // 3. Set extractedContentType to bodyWithType’s type.
                extracted_content_type = body_with_type.r#type;
            }

            // 4. Let originalAuthorContentType be the result of getting `Content-Type` from this’s author request headers.
            let original_author_content_type = get_header(&this.author_request_headers.borrow(), b"Content-Type");

            // 5. If originalAuthorContentType is non-null, then:
            if let Some(original_author_content_type) = original_author_content_type {
                // 1. If body is a Document object, a URLSearchParams object, or a string:
                if matches!(
                    body,
                    DocumentOrXMLHttpRequestBodyInit::Document(_)
                        | DocumentOrXMLHttpRequestBodyInit::URLSearchParams(_)
                        | DocumentOrXMLHttpRequestBodyInit::USVString(_)
                ) {
                    // 1. Let contentTypeRecord be the result of parsing originalAuthorContentType.
                    let content_type_record = MimeType::parse(&bytes_to_code_points(&original_author_content_type));

                    // 2. If contentTypeRecord is not failure, contentTypeRecord’s parameters["charset"] exists, and parameters["charset"] is not an ASCII case-insensitive match for "UTF-8", then:
                    if let Some(mut content_type_record) = content_type_record
                        && content_type_record
                            .parameters()
                            .any(|(name, value)| name == "charset" && !value.eq_ignore_ascii_case("UTF-8"))
                    {
                        // 1. Set contentTypeRecord’s parameters["charset"] to "UTF-8".
                        content_type_record.set_parameter(String::from("charset"), String::from("UTF-8"));

                        // 2. Let newContentTypeSerialized be the result of serializing contentTypeRecord.
                        let new_content_type_serialized = content_type_record.serialize();

                        // 3. Set (`Content-Type`, newContentTypeSerialized) in this’s author request headers.
                        set_header(
                            &mut this.author_request_headers.borrow_mut(),
                            b"Content-Type",
                            isomorphic_encode(&new_content_type_serialized),
                        );
                    }
                }
            }
            // 6. Otherwise:
            else if let DocumentOrXMLHttpRequestBodyInit::Document(document) = &body {
                // NOTE: A document can only be an HTML document or XML document.
                // SAFETY: The Document is live.
                let is_html_document = unsafe { web_xhr_document_is_html_document(document.wrapper().as_ptr().cast()) };

                // 1. If body is an HTML document, then set (`Content-Type`, `text/html;charset=UTF-8`) in this’s author request headers.
                // 2. Otherwise, if body is an XML document, set (`Content-Type`, `application/xml;charset=UTF-8`) in this’s author request headers.
                let content_type: &[u8] = if is_html_document {
                    b"text/html;charset=UTF-8"
                } else {
                    b"application/xml;charset=UTF-8"
                };
                set_header(
                    &mut this.author_request_headers.borrow_mut(),
                    b"Content-Type",
                    content_type.to_vec(),
                );
            }
            // 3. Otherwise, if extractedContentType is not null, set (`Content-Type`, extractedContentType) in this’s author request headers.
            else if let Some(extracted_content_type) = extracted_content_type {
                let extracted_content_type: String =
                    string_to_code_points(&extracted_content_type).into_iter().collect();
                set_header(
                    &mut this.author_request_headers.borrow_mut(),
                    b"Content-Type",
                    isomorphic_encode(&extracted_content_type),
                );
            }
        }

        // 5. If one or more event listeners are registered on this’s upload object, then set this’s upload listener flag.
        this.upload_listener
            .set(this.upload_object.event_target().has_event_listeners());

        // 6. Let req be a new request, initialized as follows:
        // NB: XHR/XMLHttpRequest.cpp creates the request from these fields.
        let request_body = this.request_body.borrow().clone();
        let request = {
            let request_url = this.request_url.borrow();
            let request_url = request_url.as_ref().expect("open() set the request URL");
            let author_request_headers = this.author_request_headers.borrow();
            let header_names: Vec<ak::Span<'_, u8>> = author_request_headers
                .iter()
                .map(|(name, _)| ak::Span::new(name))
                .collect();
            let header_values: Vec<ak::Span<'_, u8>> = author_request_headers
                .iter()
                .map(|(_, value)| ak::Span::new(value))
                .collect();
            let rust_url = RustUrl::from_url(request_url);
            let request = RequestAbi {
                method: ak::Span::new(&request_method),
                url: &raw const rust_url,
                header_names: header_names.as_ptr(),
                header_values: header_values.as_ptr(),
                header_count: author_request_headers.len(),
                body: request_body.as_ref().map_or(core::ptr::null_mut(), CppBody::as_ptr),
                use_cors_preflight: this.upload_listener.get(),
                include_credentials: this.cross_origin_credentials.get(),
                use_url_credentials: request_url.includes_credentials(),
            };
            // SAFETY: The realm is live, the fields are lent for the call, and LibWeb hands over a reference to the
            //         request.
            unsafe {
                CppRequest::adopt(web_xhr_create_request(
                    associated_realm.as_ptr().cast(),
                    &raw const request,
                ))
            }
        };

        // 7. Unset this’s upload complete flag.
        this.upload_complete.set(false);

        // 8. Unset this’s timed out flag.
        this.timed_out.set(false);

        // 9. If req’s body is null, then set this’s upload complete flag.
        // NOTE: req's body is always this's request body, see step 6.
        if request_body.is_none() {
            this.upload_complete.set(true);
        }

        // 10. Set this’s send() flag.
        this.send.set(true);
        this.update_pending_activity();

        // 11. If this’s synchronous flag is unset, then:
        if !this.synchronous.get() {
            // 1. Fire a progress event named loadstart at this with 0 and 0.
            this.fire_progress_event(vm, "loadstart", 0, 0);

            // 2. Let requestBodyTransmitted be 0.
            // NOTE: This is kept on the XHR object itself instead of the stack, as we cannot capture references to stack variables in an async context.
            this.request_body_transmitted.set(0);
            this.last_upload_progress_timestamp.set(None);
            this.last_download_progress_timestamp.set(None);

            // 3. Let requestBodyLength be req’s body’s length, if req’s body is non-null; otherwise 0.
            // NOTE: req's body is always this's request body, see step 6.
            // 4. Assert: requestBodyLength is an integer.
            let request_body_length = request_body.as_ref().map_or(0, |body| {
                body.length()
                    .expect("the length of a body that send() extracted is an integer")
            });

            // 5. If this’s upload complete flag is unset and this’s upload listener flag is set, then fire a progress event named loadstart at this’s upload object with requestBodyTransmitted and requestBodyLength.
            if !this.upload_complete.get() && this.upload_listener.get() {
                this.fire_upload_progress_event(
                    vm,
                    "loadstart",
                    this.request_body_transmitted.get(),
                    request_body_length,
                );
            }

            // 6. If this’s state is not opened or this’s send() flag is unset, then return.
            if this.state.get() != State::Opened || !this.send.get() {
                return Ok(());
            }

            // 7. Let processRequestBodyChunkLength, given a bytesLength, be these steps:
            // NB: See process_request_body_chunk_length().

            // 8. Let processRequestEndOfBody be these steps:
            // NB: See process_request_end_of_body().

            // 9. Let processResponse, given a response, be these steps:
            // NB: See process_response().

            // 10. Set this’s fetch controller to the result of fetching req with processRequestBodyChunkLength set to processRequestBodyChunkLength, processRequestEndOfBody set to processRequestEndOfBody, and processResponse set to processResponse.
            // SAFETY: The realm, the request and this are live, and LibWeb returns a live fetch controller.
            let fetch_controller = unsafe {
                CppFetchController::from_raw(web_xhr_fetch(
                    associated_realm.as_ptr().cast(),
                    request.as_ptr(),
                    cell_into_abi(*this),
                    core::ptr::null_mut(),
                    request_body_length,
                ))
            };
            this.fetch_controller.replace(fetch_controller);

            // 11. Let now be the present time.
            this.async_send_start_time.set(Some(Instant::now()));
            *this.async_request.borrow_mut() = Some(request);

            // 12. Run these steps in parallel:
            //     1. Wait until either req’s done flag is set or this’s timeout is not 0 and this’s timeout milliseconds have passed since now.
            //     2. If req’s done flag is unset, then set this’s timed out flag and terminate this’s fetch controller.
            Self::restart_timeout_timer(*this, vm);
        }
        // 12. Otherwise, if this’s synchronous flag is set:
        else {
            // NB: These flags can't live in this stack frame: The fetch algorithms below share them, and can run after
            //     send() has returned (a response that completes after the fetch was terminated for timing out, e.g.).
            // 1. Let processedResponse be false.
            let sync_send = SYNC_SEND_CLASS.allocate(vm, |_| SyncSend {
                processed_response: Cell::new(false),
                did_time_out: Cell::new(false),
            });

            // 2. Let processResponseConsumeBody, given a response and nullOrFailureOrBytes, be these steps:
            // NB: See process_response_consume_body().

            // 3. Set this’s fetch controller to the result of fetching req with processResponseConsumeBody set to processResponseConsumeBody and useParallelQueue set to true.
            // SAFETY: The realm, the request, this and the flags are live, and LibWeb returns a live fetch controller.
            let fetch_controller = unsafe {
                CppFetchController::from_raw(web_xhr_fetch(
                    associated_realm.as_ptr().cast(),
                    request.as_ptr(),
                    cell_into_abi(*this),
                    sync_send.as_ptr().cast(),
                    0,
                ))
            };
            this.fetch_controller.replace(fetch_controller.clone());

            // 4. Let now be the present time.
            // 5. Pause until either processedResponse is true or this’s timeout is not 0 and this’s timeout milliseconds have passed since now.
            if this.timeout.get() != 0 {
                this.stop_timeout_timer();
                let timer = CppTimer::create_single_shot(
                    vm,
                    u64::from(this.timeout.get()),
                    (*this, sync_send),
                    |_, (this, sync_send)| {
                        sync_send.did_time_out.set(true);
                        this.stop_timeout_timer();
                    },
                );
                this.timeout_timer.replace(Some(timer));
            }

            // NB: XHR/XMLHttpRequest.cpp pauses the event loop for the wait.
            // SAFETY: The flags are live.
            unsafe { web_xhr_pause_until_sync_send_is_over(sync_send.as_ptr().cast()) };

            this.stop_timeout_timer();

            // 6. If processedResponse is false, then set this’s timed out flag and terminate this’s fetch controller.
            if !sync_send.processed_response.get() {
                this.timed_out.set(true);
                fetch_controller.terminate();
            }

            // 7. Report timing for this’s fetch controller given the current global object.
            // AD-HOC: A request that timed out before its fetch set up the report timing steps has no timing to report.
            //         Spec issue: https://github.com/whatwg/xhr/issues/412
            if !this.timed_out.get() || fetch_controller.has_report_timing_steps() {
                let current_realm = vm.current_realm().expect("send() runs in a realm");
                fetch_controller.report_timing(current_realm.global_object());
            }

            // 8. Run handle response end-of-body for this.
            this.handle_response_end_of_body(vm)?;
        }
        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn process_request_body_chunk_length(&self, vm: &Vm, request_body_length: u64, bytes_length: u64) {
        // AD-HOC: Fetch transmits the body again for each redirect, and runs process request end-of-body for a
        //         null body, so ignore everything after the upload completed.
        //         Spec issue: https://github.com/whatwg/xhr/issues/242
        if self.upload_complete.get() {
            return;
        }

        // 1. Increase requestBodyTransmitted by bytesLength.
        self.request_body_transmitted
            .set(self.request_body_transmitted.get() + bytes_length);

        // 2. If not roughly 50ms have passed since these steps were last invoked, then return.
        let now = Instant::now();
        let enough_time_passed = self
            .last_upload_progress_timestamp
            .get()
            .is_none_or(|timestamp| now - timestamp >= Duration::from_millis(50));
        if !enough_time_passed {
            return;
        }
        self.last_upload_progress_timestamp.set(Some(now));

        // 3. If this’s upload listener flag is set, then fire a progress event named progress at this’s upload object with requestBodyTransmitted and requestBodyLength.
        if self.upload_listener.get() {
            self.fire_upload_progress_event(vm, "progress", self.request_body_transmitted.get(), request_body_length);
        }
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn process_request_end_of_body(&self, vm: &Vm, request_body_length: u64) {
        // AD-HOC: Fetch transmits the body again for each redirect, and runs process request end-of-body for a
        //         null body, so ignore everything after the upload completed.
        //         Spec issue: https://github.com/whatwg/xhr/issues/242
        if self.upload_complete.get() {
            return;
        }

        // 1. Set this’s upload complete flag.
        self.upload_complete.set(true);

        // 2. If this’s upload listener flag is unset, then return.
        if !self.upload_listener.get() {
            return;
        }

        // 3. Fire a progress event named progress at this’s upload object with requestBodyTransmitted and requestBodyLength.
        self.fire_upload_progress_event(vm, "progress", self.request_body_transmitted.get(), request_body_length);

        // 4. Fire a progress event named load at this’s upload object with requestBodyTransmitted and requestBodyLength.
        self.fire_upload_progress_event(vm, "load", self.request_body_transmitted.get(), request_body_length);

        // 5. Fire a progress event named loadend at this’s upload object with requestBodyTransmitted and requestBodyLength.
        self.fire_upload_progress_event(vm, "loadend", self.request_body_transmitted.get(), request_body_length);
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn process_response(this: Ref<Self>, vm: &Vm, response: CppResponseView) {
        // 1. Set this’s response to response.
        this.set_response(Response::Fetched(response.clone()));

        // 2. Handle errors for this.
        // NOTE: This cannot throw, as `handle_errors` only throws in a synchronous context.
        this.handle_errors(vm).must();

        // 3. If this’s response is a network error, then return.
        if this.response.borrow().is_network_error() {
            return;
        }

        // 4. Set this’s state to headers received.
        this.state.set(State::HeadersReceived);
        this.update_pending_activity();

        // 5. Fire an event named readystatechange at this.
        this.fire_event("readystatechange");

        // 6. If this’s state is not headers received, then return.
        if this.state.get() != State::HeadersReceived {
            return;
        }

        // 7. If this’s response’s body is null, then run handle response end-of-body for this and return.
        if !this.response.borrow().has_body() {
            // NOTE: This cannot throw, as `handle_response_end_of_body` only throws in a synchronous context.
            this.handle_response_end_of_body(vm).must();
            return;
        }

        // 8. Let length be the result of extracting a length from this’s response’s header list.
        // 9. If length is not an integer, then set it to 0.
        let length = this.response.borrow().extract_length().unwrap_or(0);

        // Pre-allocate received bytes buffer if Content-Length is known.
        if let Ok(length) = usize::try_from(length) {
            this.received_bytes.borrow_mut().reserve(length);
        }

        // 10. Let processBodyChunk given bytes be these steps:
        // NB: See process_body_chunk().

        // 11. Let processEndOfBody be this step: run handle response end-of-body for this.
        // NB: See process_end_of_body().

        // 12. Let processBodyError be these steps:
        // NB: See process_body_error().

        // 13. Incrementally read this’s response’s body, given processBodyChunk, processEndOfBody, processBodyError, and this’s relevant global object.
        // SAFETY: The realm, the response and this are live.
        unsafe {
            web_xhr_incrementally_read_body(
                this.realm.as_ptr().cast(),
                response.as_ptr(),
                cell_into_abi(this),
                length,
            );
        }
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn process_body_chunk(&self, vm: &Vm, length: u64, bytes: &[u8]) {
        // 1. Append bytes to this’s received bytes.
        self.received_bytes.borrow_mut().extend_from_slice(bytes);

        // 2. If not roughly 50ms have passed since these steps were last invoked, then return.
        let now = Instant::now();
        let enough_time_passed = self
            .last_download_progress_timestamp
            .get()
            .is_none_or(|timestamp| now - timestamp >= Duration::from_millis(50));
        if !enough_time_passed {
            return;
        }
        self.last_download_progress_timestamp.set(Some(now));

        // 3. If this’s state is headers received, then set this’s state to loading.
        if self.state.get() == State::HeadersReceived {
            self.state.set(State::Loading);
        }
        self.update_pending_activity();

        // 4. Fire an event named readystatechange at this.
        // Spec Note: Web compatibility is the reason readystatechange fires more often than this’s state changes.
        self.fire_event("readystatechange");

        // 5. Fire a progress event named progress at this with this’s received bytes’s length and length.
        let received_length = self.received_bytes.borrow().len() as u64;
        self.fire_progress_event(vm, "progress", received_length, length);
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn process_end_of_body(&self, vm: &Vm) {
        // NOTE: This cannot throw, as `handle_response_end_of_body` only throws in a synchronous context.
        self.handle_response_end_of_body(vm).must();
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn process_body_error(&self, vm: &Vm) {
        // 1. Set this’s response to a network error.
        self.set_response(Response::network_error("A network error occurred processing body."));
        // 2. Run handle errors for this.
        // NOTE: This cannot throw, as `handle_errors` only throws in a synchronous context.
        self.handle_errors(vm).must();
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
    pub(crate) fn process_response_consume_body(
        &self,
        sync_send: &SyncSend,
        response: CppResponseView,
        null_or_failure_or_bytes: NullOrFailureOrBytes<'_>,
    ) {
        // AD-HOC: Leave this's response and received bytes alone once send() returned for a timeout.
        if sync_send.did_time_out.get() {
            return;
        }

        // 1. If nullOrFailureOrBytes is not failure, then set this’s response to response.
        if !matches!(null_or_failure_or_bytes, NullOrFailureOrBytes::Failure) {
            self.set_response(Response::Fetched(response));
        }

        // 2. If nullOrFailureOrBytes is a byte sequence, then append nullOrFailureOrBytes to this’s received bytes.
        if let NullOrFailureOrBytes::Bytes(bytes) = null_or_failure_or_bytes {
            self.received_bytes.borrow_mut().extend_from_slice(bytes);
        }

        // 3. Set processedResponse to true.
        sync_send.processed_response.set(true);
    }

    // AD-HOC: Stopping the fetch, as aborting the document does, discards it without a response. Hand this an
    //         aborted network error instead, so that it fires abort as for a canceled fetch.
    pub(crate) fn fetch_stopped(this: Ref<Self>, vm: &Vm, fetch_controller: CppFetchController) {
        queue_a_global_task(
            vm,
            this.relevant_global_object(),
            TaskSource::Networking,
            (this, fetch_controller),
            |vm, (this, fetch_controller)| {
                if this.fetch_controller() != fetch_controller {
                    return;
                }
                this.set_response(Response::aborted_network_error());
                this.handle_errors(vm).must();
            },
        );
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-getresponseheader
    pub(crate) fn get_response_header(
        &self,
        _: &mut CallContext,
        name: ByteString,
    ) -> WebIdlResult<Option<ByteString>> {
        // The getResponseHeader(name) method steps are to return the result of getting name from this’s response’s header list.
        Ok(self
            .response
            .borrow()
            .fetched()
            .and_then(|response| response.get_header(name.as_bytes()))
            .map(ByteString::from_bytes))
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-getallresponseheaders
    pub(crate) fn get_all_response_headers(&self, _: &mut CallContext) -> WebIdlResult<ByteString> {
        // 1. Let output be an empty byte sequence.
        let mut output = Vec::new();

        // 2. Let initialHeaders be the result of running sort and combine with this’s response’s header list.
        let mut initial_headers = self
            .response
            .borrow()
            .fetched()
            .map(CppResponseView::sort_and_combine)
            .unwrap_or_default();

        // 3. Let headers be the result of sorting initialHeaders in ascending order, with a being less than b if a’s name is legacy-uppercased-byte less than b’s name.
        // NOTE: Unfortunately, this is needed for compatibility with deployed content.
        initial_headers.sort_by(|(a, _), (b, _)| {
            if is_legacy_uppercased_byte_less_than(a, b) {
                core::cmp::Ordering::Less
            } else if is_legacy_uppercased_byte_less_than(b, a) {
                core::cmp::Ordering::Greater
            } else {
                core::cmp::Ordering::Equal
            }
        });

        // 4. For each header in headers, append header’s name, followed by a 0x3A 0x20 byte pair, followed by header’s value, followed by a 0x0D 0x0A byte pair, to output.
        for (name, value) in initial_headers {
            output.extend_from_slice(&name);
            output.extend_from_slice(b": ");
            output.extend_from_slice(&value);
            output.extend_from_slice(b"\r\n");
        }

        // 5. Return output.
        Ok(ByteString::from_bytes(output))
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-overridemimetype
    pub(crate) fn override_mime_type(&self, _: &mut CallContext, mime: Utf16String) -> WebIdlResult<()> {
        // 1. If this’s state is loading or done, then throw an "InvalidStateError" DOMException.
        if self.state.get() == State::Loading || self.state.get() == State::Done {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Cannot override MIME type when state is Loading or Done."),
            ));
        }

        // 2. Set this’s override MIME type to the result of parsing mime.
        let override_mime_type = MimeType::parse(&string_to_code_points(&mime));

        // 3. If this’s override MIME type is failure, then set this’s override MIME type to application/octet-stream.
        *self.override_mime_type.borrow_mut() =
            Some(override_mime_type.unwrap_or_else(|| new_mime_type("application", "octet-stream")));

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-timeout
    pub(crate) fn timeout(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.timeout.get())
    }

    // https://xhr.spec.whatwg.org/#ref-for-dom-xmlhttprequest-timeout%E2%91%A2
    pub(crate) fn set_timeout(this: &Ref<Self>, cx: &mut CallContext, timeout: u32) -> WebIdlResult<()> {
        // 1. If the current global object is a Window object and this’s synchronous flag is set,
        //    then throw an "InvalidAccessError" DOMException.
        if current_global_object_is_window(cx.vm()) && this.synchronous.get() {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_ascii(
                    b"Use of XMLHttpRequest's timeout attribute is not supported in the synchronous mode in window context.",
                ),
            ));
        }

        // 2. Set this’s timeout to the given value.
        this.timeout.set(timeout);

        // NB: The asynchronous send() timeout wait observes changes to this value.
        if this.send.get() && !this.synchronous.get() {
            Self::restart_timeout_timer(*this, cx.vm());
        }

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-withcredentials
    pub(crate) fn with_credentials(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The withCredentials getter steps are to return this’s cross-origin credentials.
        Ok(self.cross_origin_credentials.get())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-withcredentials
    pub(crate) fn set_with_credentials(&self, _: &mut CallContext, with_credentials: bool) -> WebIdlResult<()> {
        // 1. If this’s state is not unsent or opened, then throw an "InvalidStateError" DOMException.
        if self.state.get() != State::Unsent && self.state.get() != State::Opened {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR readyState is not UNSENT or OPENED"),
            ));
        }

        // 2. If this’s send() flag is set, then throw an "InvalidStateError" DOMException.
        if self.send.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"XHR send() flag is already set"),
            ));
        }

        // 3. Set this’s cross-origin credentials to the given value.
        self.cross_origin_credentials.set(with_credentials);

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-abort
    pub(crate) fn abort(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        let vm = cx.vm();
        self.stop_timeout_timer();
        self.async_request.take();

        // 1. Abort this’s fetch controller.
        self.fetch_controller().abort(self.realm);

        // 2. If this’s state is opened with this’s send() flag set, headers received, or loading, then run the request error steps for this and abort.
        let state = self.state.get();
        if (state == State::Opened && self.send.get()) || state == State::HeadersReceived || state == State::Loading {
            // NOTE: This cannot throw as we don't pass in an exception. XHR::abort cannot be reached in a synchronous context where the state matches above.
            //       This is because it pauses inside XHR::send until the request is done or times out and then immediately calls `handle_response_end_of_body`
            //       which will always set `m_state` to `Done`.
            self.request_error_steps(vm, "abort", None).must();
        }

        // 3. If this’s state is done, then set this’s state to unsent and this’s response to a network error.
        // Spec Note: No readystatechange event is dispatched.
        if self.state.get() == State::Done {
            self.state.set(State::Unsent);
            self.set_response(Response::network_error("Not yet sent"));
            self.update_pending_activity();
        }

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-upload
    pub(crate) fn upload(&self, _: &mut CallContext) -> WebIdlResult<Ref<XMLHttpRequestUpload>> {
        // The upload getter steps are to return this’s upload object.
        Ok(self.upload_object)
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-readystate
    pub(crate) fn ready_state(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        Ok(self.state.get() as u16)
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-status
    pub(crate) fn status(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        // The status getter steps are to return this’s response’s status.
        Ok(self.response.borrow().fetched().map_or(0, CppResponseView::status))
    }

    // https://xhr.spec.whatwg.org/#dom-xmlhttprequest-statustext
    pub(crate) fn status_text(&self, _: &mut CallContext) -> WebIdlResult<ByteString> {
        // The statusText getter steps are to return this’s response’s status message.
        let status_message = self
            .response
            .borrow()
            .fetched()
            .map(CppResponseView::status_message)
            .unwrap_or_default();
        Ok(ByteString::from_bytes(status_message))
    }

    // https://xhr.spec.whatwg.org/#handle-response-end-of-body
    fn handle_response_end_of_body(&self, vm: &Vm) -> WebIdlResult<()> {
        self.stop_timeout_timer();
        self.async_request.take();

        // 1. Handle errors for xhr.
        self.handle_errors(vm)?;

        // 2. If xhr’s response is a network error, then return.
        if self.response.borrow().is_network_error() {
            return Ok(());
        }

        // 3. Let transmitted be xhr’s received bytes’s length.
        let transmitted = self.received_bytes.borrow().len() as u64;

        // 4. Let length be the result of extracting a length from this’s response’s header list.
        // 5. If length is not an integer, then set it to 0.
        let length = self.response.borrow().extract_length().unwrap_or(0);

        // 6. If xhr’s synchronous flag is unset, then fire a progress event named progress at xhr with transmitted and length.
        if !self.synchronous.get() {
            self.fire_progress_event(vm, "progress", transmitted, length);
        }

        // 7. Set xhr’s state to done.
        self.state.set(State::Done);

        // 8. Unset xhr’s send() flag.
        self.send.set(false);
        self.update_pending_activity();

        // 9. Fire an event named readystatechange at xhr.
        self.fire_event("readystatechange");

        // 10. Fire a progress event named load at xhr with transmitted and length.
        self.fire_progress_event(vm, "load", transmitted, length);

        // 11. Fire a progress event named loadend at xhr with transmitted and length.
        self.fire_progress_event(vm, "loadend", transmitted, length);

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#handle-errors
    fn handle_errors(&self, vm: &Vm) -> WebIdlResult<()> {
        // 1. If xhr’s send() flag is unset, then return.
        if !self.send.get() {
            return Ok(());
        }

        // 2. If xhr’s timed out flag is set, then run the request error steps for xhr, timeout, and "TimeoutError" DOMException.
        if self.timed_out.get() {
            return self.request_error_steps(
                vm,
                "timeout",
                Some(("TimeoutError", Utf16String::from_ascii(b"Timed out"))),
            );
        }

        // 3. Otherwise, if xhr’s response’s aborted flag is set, run the request error steps for xhr, abort, and "AbortError" DOMException.
        let response = self.response_record();
        if response.aborted() {
            return self.request_error_steps(vm, "abort", Some(("AbortError", Utf16String::from_ascii(b"Aborted"))));
        }

        // 4. Otherwise, if xhr’s response is a network error, then run the request error steps for xhr, error, and "NetworkError" DOMException.
        if response.is_network_error() {
            let message = response
                .network_error_message()
                .unwrap_or_else(|| Utf16String::from_ascii(b"Unknown error"));
            return self.request_error_steps(vm, "error", Some(("NetworkError", message)));
        }

        Ok(())
    }

    // https://xhr.spec.whatwg.org/#request-error-steps
    fn request_error_steps(
        &self,
        vm: &Vm,
        event_name: &str,
        exception: Option<(&'static str, Utf16String)>,
    ) -> WebIdlResult<()> {
        self.stop_timeout_timer();
        self.async_request.take();

        // 1. Set xhr’s state to done.
        self.state.set(State::Done);

        // 2. Unset xhr’s send() flag.
        self.send.set(false);
        self.update_pending_activity();

        // 3. Set xhr’s response to a network error.
        self.set_response(Response::network_error("Failed to load"));

        // 4. If xhr’s synchronous flag is set, then throw exception.
        if self.synchronous.get() {
            let (name, message) = exception.expect("a synchronous request has an exception to throw");
            return Err(Exception::Dom(name, message));
        }

        // 5. Fire an event named readystatechange at xhr.
        self.fire_event("readystatechange");

        // 6. If xhr’s upload complete flag is unset, then:
        if !self.upload_complete.get() {
            // 1. Set xhr’s upload complete flag.
            self.upload_complete.set(true);

            // 2. If xhr’s upload listener flag is set, then:
            if self.upload_listener.get() {
                // 1. Fire a progress event named event at xhr’s upload object with 0 and 0.
                self.fire_upload_progress_event(vm, event_name, 0, 0);

                // 2. Fire a progress event named loadend at xhr’s upload object with 0 and 0.
                self.fire_upload_progress_event(vm, "loadend", 0, 0);
            }
        }

        // 7. Fire a progress event named event at xhr with 0 and 0.
        self.fire_progress_event(vm, event_name, 0, 0);

        // 8. Fire a progress event named loadend at xhr with 0 and 0.
        self.fire_progress_event(vm, "loadend", 0, 0);

        Ok(())
    }

    fn should_be_kept_alive(&self) -> bool {
        if let Some(window) = window_from_global_object(self.relevant_global_object())
            && window.associated_document().has_been_destroyed()
        {
            return false;
        }

        let state = self.state.get();
        if !((state == State::Opened && self.send.get()) || state == State::HeadersReceived || state == State::Loading)
        {
            return false;
        }

        [
            "readystatechange",
            "progress",
            "abort",
            "error",
            "load",
            "timeout",
            "loadend",
        ]
        .iter()
        .any(|event_type| self.event_target().has_event_listener(event_type))
    }

    fn update_pending_activity(&self) {
        self.event_target()
            .set_has_pending_activity(self.realm, self.should_be_kept_alive());
    }

    // https://xhr.spec.whatwg.org/#the-responseurl-attribute
    pub(crate) fn response_url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The responseURL getter steps are to return the empty string if this’s response’s URL is null;
        // otherwise its serialization with the exclude fragment flag set.
        let url = self.response.borrow().fetched().and_then(|response| response.url(true));
        Ok(url.map_or_else(|| UsvString::from_ascii(b""), |url| UsvString::from_utf16_string(&url)))
    }
}
