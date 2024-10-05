/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/EventSourcePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/EventSource.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(EventSource);

// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
WebIDL::ExceptionOr<JS::NonnullGCPtr<EventSource>> EventSource::construct_impl(JS::Realm& realm, StringView url, EventSourceInit event_source_init_dict)
{
    auto& vm = realm.vm();

    // 1. Let ev be a new EventSource object.
    auto event_source = realm.heap().allocate<EventSource>(realm, realm);

    // 2. Let settings be ev's relevant settings object.
    auto& settings = relevant_settings_object(event_source);

    // 3. Let urlRecord be the result of encoding-parsing a URL given url, relative to settings.
    auto url_record = settings.parse_url(url);

    // 4. If urlRecord is failure, then throw a "SyntaxError" DOMException.
    if (!url_record.is_valid())
        return WebIDL::SyntaxError::create(realm, MUST(String::formatted("Invalid URL '{}'", url)));

    // 5. Set ev's url to urlRecord.
    event_source->m_url = move(url_record);

    // 6. Let corsAttributeState be Anonymous.
    auto cors_attribute_state = CORSSettingAttribute::Anonymous;

    // 7. If the value of eventSourceInitDict's withCredentials member is true, then set corsAttributeState to Use Credentials
    //    and set ev's withCredentials attribute to true.
    if (event_source_init_dict.with_credentials) {
        cors_attribute_state = CORSSettingAttribute::UseCredentials;
        event_source->m_with_credentials = true;
    }

    // 8. Let request be the result of creating a potential-CORS request given urlRecord, the empty string, and corsAttributeState.
    auto request = create_potential_CORS_request(vm, event_source->m_url, {}, cors_attribute_state);

    // 9. Set request's client to settings.
    request->set_client(&settings);

    // 10. User agents may set (`Accept`, `text/event-stream`) in request's header list.
    auto header = Fetch::Infrastructure::Header::from_string_pair("Accept"sv, "text/event-stream"sv);
    request->header_list()->set(move(header));

    // 11. Set request's cache mode to "no-store".
    request->set_cache_mode(Fetch::Infrastructure::Request::CacheMode::NoStore);

    // 12. Set request's initiator type to "other".
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Other);

    // AD-HOC: We must not buffer the response as the connection generally never ends, thus we can't wait for the end
    //         of the response body.
    request->set_buffer_policy(Fetch::Infrastructure::Request::BufferPolicy::DoNotBufferResponse);

    // 13. Set ev's request to request.
    event_source->m_request = request;

    // 14. Let processEventSourceEndOfBody given response res be the following step: if res is not a network error, then
    //     reestablish the connection.
    auto process_event_source_end_of_body = [event_source](JS::NonnullGCPtr<Fetch::Infrastructure::Response> response) {
        if (!response->is_network_error())
            event_source->reestablish_the_connection();
    };

    // 15. Fetch request, with processResponseEndOfBody set to processEventSourceEndOfBody and processResponse set to the
    //     following steps given response res:
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_end_of_body = move(process_event_source_end_of_body);

    fetch_algorithms_input.process_response = [event_source](JS::NonnullGCPtr<Fetch::Infrastructure::Response> response) {
        auto& realm = event_source->realm();

        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        response = response->unsafe_response();

        auto content_type_is_text_event_stream = [&]() {
            auto content_type = response->header_list()->extract_mime_type();
            if (!content_type.has_value())
                return false;

            return content_type->essence() == "text/event-stream"sv;
        };

        // 1. If res is an aborted network error, then fail the connection.
        if (response->is_aborted_network_error()) {
            event_source->fail_the_connection();
        }
        // 2. Otherwise, if res is a network error, then reestablish the connection, unless the user agent knows that
        //    to be futile, in which case the user agent may fail the connection.
        else if (response->is_network_error()) {
            event_source->reestablish_the_connection();
        }
        // 3. Otherwise, if res's status is not 200, or if res's `Content-Type` is not `text/event-stream`, then fail
        //    the connection.
        else if (response->status() != 200 || !content_type_is_text_event_stream()) {
            event_source->fail_the_connection();
        }
        // 4. Otherwise, announce the connection and interpret res's body line by line.
        else {
            event_source->announce_the_connection();

            auto process_body_chunk = JS::create_heap_function(realm.heap(), [event_source, pending_data = ByteBuffer()](ByteBuffer body) mutable {
                if (pending_data.is_empty())
                    pending_data = move(body);
                else
                    pending_data.append(body);

                auto last_line_break = AK::StringUtils::find_any_of(pending_data, "\r\n"sv, AK::StringUtils::SearchDirection::Backward);
                if (!last_line_break.has_value())
                    return;

                auto end_index = *last_line_break + 1;
                event_source->interpret_response({ pending_data.bytes().slice(0, end_index) });

                pending_data = MUST(pending_data.slice(end_index, pending_data.size() - end_index));
            });

            auto process_end_of_body = JS::create_heap_function(realm.heap(), []() {
                // This case is handled by `process_event_source_end_of_body` above.
            });
            auto process_body_error = JS::create_heap_function(realm.heap(), [](JS::Value) {
                // This case is handled by `process_event_source_end_of_body` above.
            });

            response->body()->incrementally_read(process_body_chunk, process_end_of_body, process_body_error, { realm.global_object() });
        }
    };

    event_source->m_fetch_algorithms = Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input));
    event_source->m_fetch_controller = TRY(Fetch::Fetching::fetch(realm, request, *event_source->m_fetch_algorithms));

    // 16. Return ev.
    return event_source;
}

EventSource::EventSource(JS::Realm& realm)
    : EventTarget(realm)
{
}

EventSource::~EventSource() = default;

void EventSource::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(EventSource);

    auto* relevant_global = dynamic_cast<HTML::WindowOrWorkerGlobalScopeMixin*>(&HTML::relevant_global_object(*this));
    VERIFY(relevant_global);
    relevant_global->register_event_source({}, *this);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#garbage-collection
void EventSource::finalize()
{
    // If an EventSource object is garbage collected while its connection is still open, the user agent must abort any
    // instance of the fetch algorithm opened by this EventSource.
    if (m_ready_state != ReadyState::Closed) {
        if (m_fetch_controller)
            m_fetch_controller->abort(realm(), {});
    }

    auto* relevant_global = dynamic_cast<HTML::WindowOrWorkerGlobalScopeMixin*>(&HTML::relevant_global_object(*this));
    VERIFY(relevant_global);
    relevant_global->unregister_event_source({}, *this);
}

void EventSource::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request);
    visitor.visit(m_fetch_algorithms);
    visitor.visit(m_fetch_controller);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#handler-eventsource-onopen
void EventSource::set_onopen(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::open, event_handler);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#handler-eventsource-onopen
WebIDL::CallbackType* EventSource::onopen()
{
    return event_handler_attribute(HTML::EventNames::open);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#handler-eventsource-onmessage
void EventSource::set_onmessage(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::message, event_handler);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#handler-eventsource-onmessage
WebIDL::CallbackType* EventSource::onmessage()
{
    return event_handler_attribute(HTML::EventNames::message);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#handler-eventsource-onerror
void EventSource::set_onerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::error, event_handler);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#handler-eventsource-onerror
WebIDL::CallbackType* EventSource::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-close
void EventSource::close()
{
    // The close() method must abort any instances of the fetch algorithm started for this EventSource object, and must
    // set the readyState attribute to CLOSED.
    if (m_fetch_controller)
        m_fetch_controller->abort(realm(), {});

    m_ready_state = ReadyState::Closed;
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-eventsource-forcibly-close
void EventSource::forcibly_close()
{
    // If a user agent is to forcibly close an EventSource object (this happens when a Document object goes away
    // permanently), the user agent must abort any instances of the fetch algorithm started for this EventSource
    // object, and must set the readyState attribute to CLOSED.
    if (m_fetch_controller)
        m_fetch_controller->abort(realm(), {});

    m_ready_state = ReadyState::Closed;
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#announce-the-connection
void EventSource::announce_the_connection()
{
    // When a user agent is to announce the connection, the user agent must queue a task which, if the readyState attribute
    // is set to a value other than CLOSED, sets the readyState attribute to OPEN and fires an event named open at the
    // EventSource object.
    HTML::queue_a_task(HTML::Task::Source::RemoteEvent, nullptr, nullptr, JS::create_heap_function(heap(), [this]() {
        if (m_ready_state != ReadyState::Closed) {
            m_ready_state = ReadyState::Open;
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::open));
        }
    }));
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#reestablish-the-connection
void EventSource::reestablish_the_connection()
{
    bool initial_task_has_run { false };

    // 1. Queue a task to run the following steps:
    HTML::queue_a_task(HTML::Task::Source::RemoteEvent, nullptr, nullptr, JS::create_heap_function(heap(), [&]() {
        ScopeGuard guard { [&]() { initial_task_has_run = true; } };

        // 1. If the readyState attribute is set to CLOSED, abort the task.
        if (m_ready_state == ReadyState::Closed)
            return;

        // 2. Set the readyState attribute to CONNECTING.
        m_ready_state = ReadyState::Connecting;

        // 3. Fire an event named error at the EventSource object.
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
    }));

    // 2. Wait a delay equal to the reconnection time of the event source.
    HTML::main_thread_event_loop().spin_until([&, delay_start = MonotonicTime::now()]() {
        return (MonotonicTime::now() - delay_start) >= m_reconnection_time;
    });

    // 3. Optionally, wait some more. In particular, if the previous attempt failed, then user agents might introduce
    //    an exponential backoff delay to avoid overloading a potentially already overloaded server. Alternatively, if
    //    the operating system has reported that there is no network connectivity, user agents might wait for the
    //    operating system to announce that the network connection has returned before retrying.

    // 4. Wait until the aforementioned task has run, if it has not yet run.
    if (!initial_task_has_run) {
        HTML::main_thread_event_loop().spin_until([&]() { return initial_task_has_run; });
    }

    // 5. Queue a task to run the following steps:
    HTML::queue_a_task(HTML::Task::Source::RemoteEvent, nullptr, nullptr, JS::create_heap_function(heap(), [this]() {
        // 1. If the EventSource object's readyState attribute is not set to CONNECTING, then return.
        if (m_ready_state != ReadyState::Connecting)
            return;

        // 2. Let request be the EventSource object's request.
        JS::NonnullGCPtr request { *m_request };

        // 3. If the EventSource object's last event ID string is not the empty string, then:
        if (!m_last_event_id.is_empty()) {
            // 1. Let lastEventIDValue be the EventSource object's last event ID string, encoded as UTF-8.
            // 2. Set (`Last-Event-ID`, lastEventIDValue) in request's header list.
            auto header = Fetch::Infrastructure::Header::from_string_pair("Last-Event-ID"sv, m_last_event_id);
            request->header_list()->set(header);
        }

        // 4. Fetch request and process the response obtained in this fashion, if any, as described earlier in this section.
        m_fetch_controller = Fetch::Fetching::fetch(realm(), request, *m_fetch_algorithms).release_value_but_fixme_should_propagate_errors();
    }));
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#fail-the-connection
void EventSource::fail_the_connection()
{
    // When a user agent is to fail the connection, the user agent must queue a task which, if the readyState attribute
    // is set to a value other than CLOSED, sets the readyState attribute to CLOSED and fires an event named error at the
    // EventSource object. Once the user agent has failed the connection, it does not attempt to reconnect.
    HTML::queue_a_task(HTML::Task::Source::RemoteEvent, nullptr, nullptr, JS::create_heap_function(heap(), [this]() {
        if (m_ready_state != ReadyState::Closed) {
            m_ready_state = ReadyState::Closed;
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
        }
    }));
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#event-stream-interpretation
void EventSource::interpret_response(StringView response)
{
    // Lines must be processed, in the order they are received, as follows:
    for (auto line : response.lines(StringView::ConsiderCarriageReturn::Yes)) {
        // -> If the line is empty (a blank line)
        if (line.is_empty()) {
            // Dispatch the event, as defined below.
            dispatch_the_event();
        }
        // -> If the line starts with a U+003A COLON character (:)
        else if (line.starts_with(':')) {
            // Ignore the line.
        }
        // -> If the line contains a U+003A COLON character (:)
        else if (auto index = line.find(':'); index.has_value()) {
            // Collect the characters on the line before the first U+003A COLON character (:), and let field be that string.
            auto field = line.substring_view(0, *index);

            // Collect the characters on the line after the first U+003A COLON character (:), and let value be that string.
            // If value starts with a U+0020 SPACE character, remove it from value.
            auto value = line.substring_view(*index + 1);

            if (value.starts_with(' '))
                value = value.substring_view(1);

            // Process the field using the steps described below, using field as the field name and value as the field value.
            process_field(field, value);
        }
        // -> Otherwise, the string is not empty but does not contain a U+003A COLON character (:)
        else {
            // Process the field using the steps described below, using the whole line as the field name, and the empty
            // string as the field value.
            process_field(line, {});
        }
    }
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#processField
void EventSource::process_field(StringView field, StringView value)
{
    // -> If the field name is "event"
    if (field == "event"sv) {
        // Set the event type buffer to field value.
        m_event_type = MUST(String::from_utf8(value));
    }
    // -> If the field name is "data"
    else if (field == "data"sv) {
        // Append the field value to the data buffer, then append a single U+000A LINE FEED (LF) character to the data buffer.
        m_data.append(value);
        m_data.append('\n');
    }
    // -> If the field name is "id"
    else if (field == "id"sv) {
        // If the field value does not contain U+0000 NULL, then set the last event ID buffer to the field value.
        // Otherwise, ignore the field.
        if (!value.contains('\0'))
            m_last_event_id = MUST(String::from_utf8(value));
    }
    // -> If the field name is "retry"
    else if (field == "retry"sv) {
        // If the field value consists of only ASCII digits, then interpret the field value as an integer in base ten,
        // and set the event stream's reconnection time to that integer. Otherwise, ignore the field.
        if (auto retry = value.to_number<i64>(); retry.has_value())
            m_reconnection_time = AK::Duration::from_seconds(*retry);
    }
    // -> Otherwise
    else {
        // The field is ignored.
    }
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#dispatchMessage
void EventSource::dispatch_the_event()
{
    // 1. Set the last event ID string of the event source to the value of the last event ID buffer. The buffer does not
    //    get reset, so the last event ID string of the event source remains set to this value until the next time it is
    //    set by the server.
    auto const& last_event_id = m_last_event_id;

    // 2. If the data buffer is an empty string, set the data buffer and the event type buffer to the empty string and return.
    auto data_buffer = m_data.string_view();

    if (data_buffer.is_empty()) {
        m_event_type = {};
        m_data.clear();
        return;
    }

    // 3. If the data buffer's last character is a U+000A LINE FEED (LF) character, then remove the last character from the data buffer.
    if (data_buffer.ends_with('\n'))
        data_buffer = data_buffer.substring_view(0, data_buffer.length() - 1);

    // 4. Let event be the result of creating an event using MessageEvent, in the relevant realm of the EventSource object.
    // 5. Initialize event's type attribute to "message", its data attribute to data, its origin attribute to the serialization
    //    of the origin of the event stream's final URL (i.e., the URL after redirects), and its lastEventId attribute to the
    //    last event ID string of the event source.
    // 6. If the event type buffer has a value other than the empty string, change the type of the newly created event to equal
    //    the value of the event type buffer.
    MessageEventInit init {};
    init.data = JS::PrimitiveString::create(vm(), data_buffer);
    init.origin = MUST(String::from_byte_string(m_url.origin().serialize()));
    init.last_event_id = last_event_id;

    auto type = m_event_type.is_empty() ? HTML::EventNames::message : m_event_type;
    auto event = MessageEvent::create(realm(), type, init);

    // 7. Set the data buffer and the event type buffer to the empty string.
    m_event_type = {};
    m_data.clear();

    // 8. Queue a task which, if the readyState attribute is set to a value other than CLOSED, dispatches the newly created
    //    event at the EventSource object.
    HTML::queue_a_task(HTML::Task::Source::RemoteEvent, nullptr, nullptr, JS::create_heap_function(heap(), [this, event]() {
        if (m_ready_state != ReadyState::Closed)
            dispatch_event(event);
    }));
}

}
