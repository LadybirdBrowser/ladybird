/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! EventSource, https://html.spec.whatwg.org/multipage/server-sent-events.html#the-eventsource-interface

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::time::{Duration, Instant};

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, Gc, GcRefCell, GcWeak, MarkedVec, PrimitiveString, Realm, Trace, Value, Vm};
use liburl_rust::origin::Origin;
use liburl_rust::url::ExcludeFragment;
use liburl_rust::{BasicParseOptions, RustUrl, Url, basic_parse};

use crate::bindings::conversions::UsvString;
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, Wrapped, allocate, cell_into_abi};
use crate::bindings::promise::{TaskSource, queue_a_task};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::domurl::url_input;
use crate::fetch::infrastructure::fetch_controller::CppFetchController;
use crate::fetch::infrastructure::http::requests::CppRequest;
use crate::fetch::infrastructure::http::response_view::CppResponseView;
use crate::platform::timer::CppTimer;

unsafe extern "C" {
    fn web_html_event_source_create_request(
        settings: *mut c_void,
        url: *const RustUrl,
        use_credentials: bool,
    ) -> *mut c_void;
    fn web_html_event_source_fetch(realm: *mut c_void, request: *mut c_void, event_source: *mut c_void) -> *mut c_void;
    fn web_html_event_source_incrementally_read_body(
        realm: *mut c_void,
        response: *mut c_void,
        event_source: *mut c_void,
    );
}

/// The dictionary EventSourceInit of HTML/EventSource.idl.
pub(crate) struct EventSourceInit {
    pub(crate) with_credentials: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
enum ReadyState {
    Connecting = 0,
    Open = 1,
    Closed = 2,
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#eventsource
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct EventSource {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-eventsource-url
    #[gc(untraced)]
    url: OnceCell<Url>,

    /// The origin of the event stream's final URL.
    #[gc(untraced)]
    final_url_origin: RefCell<Option<Origin>>,

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-event-stream-request
    #[gc(untraced)]
    request: OnceCell<CppRequest>,

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-event-stream-reconnection-time
    #[gc(untraced)]
    reconnection_time: Cell<Duration>,

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-event-stream-last-event-id
    #[gc(untraced)]
    last_event_id: RefCell<String>,

    #[gc(untraced)]
    event_type: RefCell<String>,
    #[gc(untraced)]
    data: RefCell<String>,
    #[gc(untraced)]
    last_event_id_buffer: RefCell<String>,

    with_credentials: Cell<bool>,

    #[gc(untraced)]
    ready_state: Cell<ReadyState>,

    /// The bytes of the body that follow its last line break, and whether none of the body has been interpreted yet.
    #[gc(untraced)]
    pending_data: RefCell<Vec<u8>>,
    is_start_of_stream: Cell<bool>,
    /// Whether the last byte of the body that has been interpreted is a CR, which a LF that follows it pairs with.
    last_was_cr: Cell<bool>,

    /// When the delay of step 2 of reestablishing the connection ends.
    #[gc(untraced)]
    end_of_delay: Cell<Option<Instant>>,

    fetch_controller: GcRefCell<Option<CppFetchController>>,
    reconnection_timer: GcRefCell<Option<CppTimer>>,
}

impl Wrappable for EventSource {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for EventSource {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

/// The event sources of the thread's agent: references that do not keep them alive, which a collected event source
/// leaves empty. The empty ones are pruned once the list has grown to twice what was live after the last pruning.
struct EventSources {
    event_sources: Vec<GcWeak<EmbedderCell<Wrapped<EventSource>>>>,
    live_after_pruning: usize,
}

impl EventSources {
    const EVENT_SOURCES_BEFORE_FIRST_PRUNING: usize = 128;

    fn add(&mut self, vm: &Vm, event_source: Ref<EventSource>) {
        if self.event_sources.len() >= (2 * self.live_after_pruning).max(Self::EVENT_SOURCES_BEFORE_FIRST_PRUNING) {
            self.event_sources.retain(|event_source| event_source.get().is_some());
            self.live_after_pruning = self.event_sources.len();
        }
        self.event_sources.push(GcWeak::new(vm.heap(), event_source));
    }
}

thread_local! {
    static EVENT_SOURCES: RefCell<EventSources> = const {
        RefCell::new(EventSources {
            event_sources: Vec::new(),
            live_after_pruning: 0,
        })
    };
}

/// Forgets the event sources of the thread's agent, whose references into its heap have to go before the heap does.
pub(crate) fn forget_event_sources_of_agent() {
    EVENT_SOURCES.with_borrow_mut(|event_sources| {
        event_sources.event_sources.clear();
        event_sources.live_after_pruning = 0;
    });
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#event-stream-interpretation
fn for_each_line(string: &str, last_was_cr: &mut bool, mut callback: impl FnMut(&str)) {
    let mut substart = 0;
    for (i, ch) in string.char_indices() {
        if ch == '\n' {
            if !*last_was_cr {
                callback(&string[substart..i]);
            }
            substart = i + 1;
            *last_was_cr = false;
        } else if ch == '\r' {
            callback(&string[substart..i]);
            substart = i + 1;
            *last_was_cr = true;
        } else {
            *last_was_cr = false;
        }
    }
    if substart < string.len() {
        callback(&string[substart..]);
    }
}

impl EventSource {
    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
    pub(crate) fn construct(
        cx: &mut CallContext,
        url: UsvString,
        event_source_init_dict: EventSourceInit,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 1. Let ev be a new EventSource object.
        let ev = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            realm,
            url: OnceCell::new(),
            final_url_origin: RefCell::new(None),
            request: OnceCell::new(),
            reconnection_time: Cell::new(Duration::from_secs(3)),
            last_event_id: RefCell::new(String::new()),
            event_type: RefCell::new(String::new()),
            data: RefCell::new(String::new()),
            last_event_id_buffer: RefCell::new(String::new()),
            with_credentials: Cell::new(false),
            ready_state: Cell::new(ReadyState::Connecting),
            pending_data: RefCell::new(Vec::new()),
            is_start_of_stream: Cell::new(true),
            last_was_cr: Cell::new(false),
            end_of_delay: Cell::new(None),
            fetch_controller: GcRefCell::new(None),
            reconnection_timer: GcRefCell::new(None),
        });
        ev.event_target.set_owner(ev);

        // 2. Let settings be ev's relevant settings object.
        let settings = relevant_settings_object(realm.global_object());

        // 3. Let urlRecord be the result of encoding-parsing a URL given url, relative to settings.
        let url_record = settings.encoding_parse_url(url.as_utf16_string());

        // 4. If urlRecord is failure, then throw a "SyntaxError" DOMException.
        let Some(url_record) = url_record else {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_utf8(&format!(
                    "Invalid URL '{}'",
                    String::from_utf16_lossy(url.as_utf16_string().to_utf16().as_ref())
                )),
            ));
        };

        // 5. Set ev's url to urlRecord.
        let url_record = ev.url.get_or_init(|| url_record);

        // 6. Let corsAttributeState be Anonymous.
        let mut use_credentials = false;

        // 7. If the value of eventSourceInitDict's withCredentials member is true, then set corsAttributeState to Use Credentials
        //    and set ev's withCredentials attribute to true.
        if event_source_init_dict.with_credentials {
            use_credentials = true;
            ev.with_credentials.set(true);
        }

        // Steps 8-12.
        let rust_url = RustUrl::from_url(url_record);
        // SAFETY: The settings object is live, the URL is lent for the call, and LibWeb hands over a reference to the
        //         request.
        let request = unsafe {
            CppRequest::adopt(web_html_event_source_create_request(
                settings.as_ptr(),
                &raw const rust_url,
                use_credentials,
            ))
        };

        // 13. Set ev's request to request.
        let request = ev.request.get_or_init(|| request);

        // 14. Let processEventSourceEndOfBody given response res be the following step: if res is not a network error, then
        //     reestablish the connection.
        // NB: See process_event_source_end_of_body().

        // 15. Fetch request, with processResponseEndOfBody set to processEventSourceEndOfBody and processResponse set to the
        //     following steps given response res:
        // NB: See process_response().
        Self::fetch(ev, request);
        EVENT_SOURCES.with_borrow_mut(|event_sources| event_sources.add(vm, ev));
        ev.update_pending_activity();

        // 16. Return ev.
        Ok(ev)
    }

    fn fetch(this: Ref<Self>, request: &CppRequest) {
        // SAFETY: The realm, the request and this are live, and LibWeb returns a live fetch controller.
        let fetch_controller = unsafe {
            CppFetchController::from_raw(web_html_event_source_fetch(
                this.realm.as_ptr().cast(),
                request.as_ptr(),
                cell_into_abi(this),
            ))
        };
        this.fetch_controller.replace(Some(fetch_controller));
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
    pub(crate) fn process_event_source_end_of_body(this: Ref<Self>, vm: &Vm, response: CppResponseView) {
        if !response.is_network_error() {
            Self::reestablish_the_connection(this, vm);
        }
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
    pub(crate) fn process_response(this: Ref<Self>, vm: &Vm, response: CppResponseView) {
        let content_type_is_text_event_stream = || {
            response.extract_mime_type().is_some_and(|content_type| {
                let essence = content_type.essence();
                essence.type_ == "text" && essence.subtype == "event-stream"
            })
        };

        // 1. If res is an aborted network error, then fail the connection.
        if response.is_network_error() && response.aborted() {
            Self::fail_the_connection(this, vm);
        }
        // 2. Otherwise, if res is a network error, then reestablish the connection, unless the user agent knows that
        //    to be futile, in which case the user agent may fail the connection.
        else if response.is_network_error() {
            Self::reestablish_the_connection(this, vm);
        }
        // 3. Otherwise, if res's status is not 200, or if res's `Content-Type` is not `text/event-stream`, then fail
        //    the connection.
        else if response.status() != 200 || !content_type_is_text_event_stream() {
            Self::fail_the_connection(this, vm);
        }
        // 4. Otherwise, announce the connection and interpret res's body line by line.
        else {
            Self::announce_the_connection(this, vm);

            let final_url = response
                .url(false)
                .expect("A response that is not a network error has a URL");
            let final_url = basic_parse(url_input(final_url.as_units()), BasicParseOptions::new())
                .expect("The URL of a response parses");
            this.final_url_origin.replace(Some(final_url.origin()));

            this.pending_data.borrow_mut().clear();
            this.is_start_of_stream.set(true);
            this.last_was_cr.set(false);

            // When a stream is parsed, a data buffer, an event type buffer, and a last event ID buffer must be associated with
            // it. They must be initialized to the empty string.
            // AD-HOC: The last event ID buffer starts as the last event ID string, which other engines keep across streams.
            this.data.borrow_mut().clear();
            this.event_type.borrow_mut().clear();
            *this.last_event_id_buffer.borrow_mut() = this.last_event_id.borrow().clone();

            // SAFETY: The realm, the response and this are live.
            unsafe {
                web_html_event_source_incrementally_read_body(
                    this.realm.as_ptr().cast(),
                    response.as_ptr(),
                    cell_into_abi(this),
                );
            }
        }
    }

    pub(crate) fn process_body_chunk(this: Ref<Self>, vm: &Vm, body: &[u8]) {
        let mut pending_data = this.pending_data.borrow_mut();
        // The bytes kept from earlier chunks hold no line break.
        let kept_length = pending_data.len();
        pending_data.extend_from_slice(body);

        let Some(last_line_break) = body.iter().rposition(|byte| *byte == b'\r' || *byte == b'\n') else {
            return;
        };

        let end_index = kept_length + last_line_break + 1;
        let response: Vec<u8> = pending_data.drain(..end_index).collect();
        drop(pending_data);

        Self::interpret_response(this, vm, &response, this.is_start_of_stream.get());
        this.is_start_of_stream.set(false);
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-url
    pub(crate) fn url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        let url = self.url.get().expect("The constructor sets the URL");
        Ok(UsvString::from_utf8(url.serialize(ExcludeFragment::No)))
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-withcredentials
    pub(crate) fn with_credentials(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.with_credentials.get())
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-readystate
    pub(crate) fn ready_state(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        Ok(self.ready_state.get() as u16)
    }

    fn set_ready_state(&self, ready_state: ReadyState) {
        self.ready_state.set(ready_state);
        self.update_pending_activity();
    }

    fn update_pending_activity(&self) {
        self.event_target
            .set_has_pending_activity(self.realm, self.ready_state.get() != ReadyState::Closed);
    }

    fn abort_the_fetch(&self) {
        if let Some(fetch_controller) = self.fetch_controller.borrow().as_ref() {
            fetch_controller.abort(self.realm);
        }
        if let Some(reconnection_timer) = self.reconnection_timer.borrow().as_ref() {
            reconnection_timer.stop();
        }
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-close
    pub(crate) fn close(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The close() method must abort any instances of the fetch algorithm started for this EventSource object, and must
        // set the readyState attribute to CLOSED.
        self.abort_the_fetch();
        self.set_ready_state(ReadyState::Closed);
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-eventsource-forcibly-close
    fn forcibly_close(&self) {
        // If a user agent is to forcibly close an EventSource object (this happens when a Document object goes away
        // permanently), the user agent must abort any instances of the fetch algorithm started for this EventSource
        // object, and must set the readyState attribute to CLOSED.
        self.abort_the_fetch();
        self.set_ready_state(ReadyState::Closed);
    }

    /// Forcibly closes the event sources whose relevant realm is the realm.
    pub(crate) fn forcibly_close_all(vm: &Vm, realm: Gc<Realm>) {
        let event_sources = MarkedVec::new(vm);
        EVENT_SOURCES.with_borrow(|all_event_sources| {
            for event_source in all_event_sources.event_sources.iter().filter_map(GcWeak::get) {
                if event_source.realm == realm {
                    event_sources.push(event_source);
                }
            }
        });
        for event_source in event_sources.values().iter() {
            event_source.forcibly_close();
        }
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#announce-the-connection
    fn announce_the_connection(this: Ref<Self>, vm: &Vm) {
        // When a user agent is to announce the connection, the user agent must queue a task which, if the readyState attribute
        // is set to a value other than CLOSED, sets the readyState attribute to OPEN and fires an event named open at the
        // EventSource object.
        queue_a_task(vm, TaskSource::RemoteEvent, this, |_, this| {
            if this.ready_state.get() != ReadyState::Closed {
                this.set_ready_state(ReadyState::Open);
                this.event_target.dispatch_event(&this.create_associated_event("open"));
            }
        });
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#reestablish-the-connection
    fn reestablish_the_connection(this: Ref<Self>, vm: &Vm) {
        // OPTIMIZATION: readyState never leaves CLOSED, so both tasks below would return without doing anything.
        if this.ready_state.get() == ReadyState::Closed {
            return;
        }

        this.end_of_delay
            .set(Some(Instant::now() + this.reconnection_time.get()));

        // 1. Queue a task to run the following steps:
        queue_a_task(vm, TaskSource::RemoteEvent, this, |vm, this| {
            // 1. If the readyState attribute is set to CLOSED, abort the task.
            if this.ready_state.get() == ReadyState::Closed {
                return;
            }

            // 2. Set the readyState attribute to CONNECTING.
            this.set_ready_state(ReadyState::Connecting);

            // 3. Fire an event named error at the EventSource object.
            this.event_target.dispatch_event(&this.create_associated_event("error"));

            // NB: Step 4 waits for this task to finish, and an error event handler can spin a nested event loop. So the
            //     timer for the rest of the delay of step 2 starts only once this task has finished.
            if this.ready_state.get() != ReadyState::Closed {
                Self::start_the_reconnection_timer(this, vm);
            }
        });
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#reestablish-the-connection
    fn start_the_reconnection_timer(this: Ref<Self>, vm: &Vm) {
        let end_of_delay = this
            .end_of_delay
            .get()
            .expect("Reestablishing the connection sets the end of the delay");
        let remaining_delay = end_of_delay.saturating_duration_since(Instant::now());

        // 2. Wait a delay equal to the reconnection time of the event source.
        let timer = CppTimer::create_single_shot(
            vm,
            u64::try_from(remaining_delay.as_millis()).unwrap_or(u64::MAX),
            this,
            |vm, this| {
                // 3. Optionally, wait some more. In particular, if the previous attempt failed, then user agents might introduce
                //    an exponential backoff delay to avoid overloading a potentially already overloaded server. Alternatively, if
                //    the operating system has reported that there is no network connectivity, user agents might wait for the
                //    operating system to announce that the network connection has returned before retrying.

                // 4. Wait until the aforementioned task has run, if it has not yet run.
                // NB: The timer starts only once that task has run. See step 1.

                // 5. Queue a task to run the following steps:
                queue_a_task(vm, TaskSource::RemoteEvent, this, |_, this| {
                    // 1. If the EventSource object's readyState attribute is not set to CONNECTING, then return.
                    if this.ready_state.get() != ReadyState::Connecting {
                        return;
                    }

                    // 2. Let request be the EventSource object's request.
                    let request = this.request.get().expect("The constructor sets the request");

                    // 3. If the EventSource object's last event ID string is not the empty string, then:
                    let last_event_id = this.last_event_id.borrow();
                    if !last_event_id.is_empty() {
                        // 1. Let lastEventIDValue be the EventSource object's last event ID string, encoded as UTF-8.
                        let last_event_id_value = last_event_id.as_bytes();
                        // 2. Set (`Last-Event-ID`, lastEventIDValue) in request's header list.
                        request.set_header(b"Last-Event-ID", last_event_id_value);
                    }
                    // AD-HOC: Delete the header an earlier reconnection set, as other engines do.
                    else {
                        request.delete_header(b"Last-Event-ID");
                    }
                    drop(last_event_id);

                    // 4. Fetch request and process the response obtained in this fashion, if any, as described earlier in this
                    //    section.
                    Self::fetch(this, request);
                });
            },
        );
        this.reconnection_timer.replace(Some(timer));
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#fail-the-connection
    fn fail_the_connection(this: Ref<Self>, vm: &Vm) {
        // When a user agent is to fail the connection, the user agent must queue a task which, if the readyState attribute
        // is set to a value other than CLOSED, sets the readyState attribute to CLOSED and fires an event named error at the
        // EventSource object. Once the user agent has failed the connection, it does not attempt to reconnect.
        queue_a_task(vm, TaskSource::RemoteEvent, this, |_, this| {
            if this.ready_state.get() != ReadyState::Closed {
                this.set_ready_state(ReadyState::Closed);
                this.event_target.dispatch_event(&this.create_associated_event("error"));
            }
        });
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#event-stream-interpretation
    fn interpret_response(this: Ref<Self>, vm: &Vm, response: &[u8], is_start_of_stream: bool) {
        // Streams must be decoded using the UTF-8 decode algorithm.
        let response = if is_start_of_stream {
            response.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(response)
        } else {
            response
        };
        let response_string = String::from_utf8_lossy(response);

        // Lines must be processed, in the order they are received, as follows:
        let mut last_was_cr = this.last_was_cr.get();
        for_each_line(&response_string, &mut last_was_cr, |line| {
            // -> If the line is empty (a blank line)
            if line.is_empty() {
                // Dispatch the event, as defined below.
                Self::dispatch_the_event(this, vm);
            }
            // -> If the line starts with a U+003A COLON character (:)
            else if line.starts_with(':') {
                // Ignore the line.
            }
            // -> If the line contains a U+003A COLON character (:)
            else if let Some((field, value)) = line.split_once(':') {
                // Collect the characters on the line before the first U+003A COLON character (:), and let field be that string.
                // Collect the characters on the line after the first U+003A COLON character (:), and let value be that string.
                // If value starts with a U+0020 SPACE character, remove it from value.
                let value = value.strip_prefix(' ').unwrap_or(value);

                // Process the field using the steps described below, using field as the field name and value as the field value.
                this.process_field(field, value);
            }
            // -> Otherwise, the string is not empty but does not contain a U+003A COLON character (:)
            else {
                // Process the field using the steps described below, using the whole line as the field name, and the empty
                // string as the field value.
                this.process_field(line, "");
            }
        });
        this.last_was_cr.set(last_was_cr);
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#processField
    fn process_field(&self, field: &str, value: &str) {
        match field {
            // -> If the field name is "event"
            "event" => {
                // Set the event type buffer to the field value.
                *self.event_type.borrow_mut() = value.to_owned();
            }
            // -> If the field name is "data"
            "data" => {
                // Append the field value to the data buffer, then append a single U+000A LINE FEED (LF) character to the data buffer.
                let mut data = self.data.borrow_mut();
                data.push_str(value);
                data.push('\n');
            }
            // -> If the field name is "id"
            "id" => {
                // If the field value does not contain U+0000 NULL, then set the last event ID buffer to the field value.
                // Otherwise, ignore the field.
                if !value.contains('\0') {
                    *self.last_event_id_buffer.borrow_mut() = value.to_owned();
                }
            }
            // -> If the field name is "retry"
            "retry" => {
                // If the field value consists of only ASCII digits, then interpret the field value as an integer in base ten,
                // and set the event stream's reconnection time to that integer. Otherwise, ignore the field.
                if value.bytes().all(|byte| byte.is_ascii_digit())
                    && let Ok(retry) = value.parse::<i64>()
                {
                    self.reconnection_time.set(Duration::from_millis(retry.unsigned_abs()));
                }
            }
            // -> Otherwise
            _ => {
                // The field is ignored.
            }
        }
    }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dispatchMessage
    fn dispatch_the_event(this: Ref<Self>, vm: &Vm) {
        // 1. Set the last event ID string of the event source to the value of the last event ID buffer. The buffer does not
        //    get reset, so the last event ID string of the event source remains set to this value until the next time it is
        //    set by the server.
        *this.last_event_id.borrow_mut() = this.last_event_id_buffer.borrow().clone();
        let last_event_id = this.last_event_id.borrow();

        // 2. If the data buffer is an empty string, set the data buffer and the event type buffer to the empty string and return.
        let mut data = this.data.borrow_mut();
        let mut event_type = this.event_type.borrow_mut();
        let mut data_buffer = data.as_str();

        if data_buffer.is_empty() {
            event_type.clear();
            data.clear();
            return;
        }

        // 3. If the data buffer's last character is a U+000A LINE FEED (LF) character, then remove the last character from the data buffer.
        if let Some(stripped) = data_buffer.strip_suffix('\n') {
            data_buffer = stripped;
        }

        // 4. Let event be the result of creating an event using MessageEvent, in the relevant realm of the EventSource object.
        // 5. Initialize event's type attribute to "message", its data attribute to data, its origin to the origin of the event
        //    stream's final URL (i.e., the URL after redirects), and its lastEventId attribute to the last event ID string of
        //    the event source.
        // 6. If the event type buffer has a value other than the empty string, change the type of the newly created event to equal
        //    the value of the event type buffer.
        let r#type = if event_type.is_empty() {
            "message"
        } else {
            event_type.as_str()
        };
        let origin = this.final_url_origin.borrow();
        let origin = origin
            .as_ref()
            .expect("Interpreting the event stream follows its response");
        let event = CppEvent::create_message_event(
            this.realm,
            r#type,
            Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(data_buffer))),
            &[],
            Some(origin),
            &last_event_id,
            None,
        );
        event.set_is_trusted(true);

        // 7. Set the data buffer and the event type buffer to the empty string.
        event_type.clear();
        data.clear();

        // 8. Queue a task which, if the readyState attribute is set to a value other than CLOSED, dispatches the newly created
        //    event at the EventSource object.
        queue_a_task(vm, TaskSource::RemoteEvent, (this, event), |_, (this, event)| {
            if this.ready_state.get() != ReadyState::Closed {
                this.event_target.dispatch_event(&event);
            }
        });
    }

    fn create_associated_event(&self, event_name: &str) -> CppEvent {
        CppEvent::create(self.realm, event_name, false, false, false)
    }
}
