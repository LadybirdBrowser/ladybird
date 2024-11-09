/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibURL/URL.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

struct EventSourceInit {
    bool with_credentials { false };
};

class EventSource : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(EventSource, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(EventSource);

public:
    virtual ~EventSource() override;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<EventSource>> construct_impl(JS::Realm&, StringView url, EventSourceInit event_source_init_dict = {});

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-url
    String url() const { return MUST(String::from_byte_string(m_url.serialize())); }

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-withcredentials
    bool with_credentials() const { return m_with_credentials; }

    enum class ReadyState : WebIDL::UnsignedShort {
        Connecting = 0,
        Open = 1,
        Closed = 2,
    };

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource-readystate
    ReadyState ready_state() const { return m_ready_state; }

    void set_onopen(WebIDL::CallbackType*);
    WebIDL::CallbackType* onopen();

    void set_onmessage(WebIDL::CallbackType*);
    WebIDL::CallbackType* onmessage();

    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

    void close();
    void forcibly_close();

private:
    explicit EventSource(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    void announce_the_connection();
    void reestablish_the_connection();
    void fail_the_connection();

    void interpret_response(StringView);
    void process_field(StringView field, StringView value);
    void dispatch_the_event();

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-eventsource-url
    URL::URL m_url;

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-event-stream-request
    JS::GCPtr<Fetch::Infrastructure::Request> m_request;

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-event-stream-reconnection-time
    AK::Duration m_reconnection_time { AK::Duration::from_seconds(3) };

    // https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-event-stream-last-event-id
    String m_last_event_id;

    String m_event_type;
    StringBuilder m_data;

    bool m_with_credentials { false };

    ReadyState m_ready_state { ReadyState::Connecting };

    JS::GCPtr<Fetch::Infrastructure::FetchAlgorithms> m_fetch_algorithms;
    JS::GCPtr<Fetch::Infrastructure::FetchController> m_fetch_controller;
};

}
