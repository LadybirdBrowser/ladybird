/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

// FIXME: Include ServiceWorker
using MessageEventSource = Variant<JS::Handle<WindowProxy>, JS::Handle<MessagePort>>;

struct MessageEventInit : public DOM::EventInit {
    JS::Value data { JS::js_null() };
    String origin {};
    String last_event_id {};
    Optional<MessageEventSource> source;
    Vector<JS::Handle<MessagePort>> ports;
};

class MessageEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(MessageEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(MessageEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<MessageEvent> create(JS::Realm&, FlyString const& event_name, MessageEventInit const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<MessageEvent>> construct_impl(JS::Realm&, FlyString const& event_name, MessageEventInit const&);

    MessageEvent(JS::Realm&, FlyString const& event_name, MessageEventInit const& event_init);
    virtual ~MessageEvent() override;

    JS::Value data() const { return m_data; }
    String const& origin() const { return m_origin; }
    String const& last_event_id() const { return m_last_event_id; }
    JS::NonnullGCPtr<JS::Object> ports() const;
    Variant<JS::Handle<WindowProxy>, JS::Handle<MessagePort>, Empty> source() const;

    void init_message_event(String const& type, bool bubbles, bool cancelable, JS::Value data, String const& origin, String const& last_event_id, Optional<MessageEventSource> source, Vector<JS::Handle<MessagePort>> const& ports);

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::Value m_data;
    String m_origin;
    String m_last_event_id;
    Optional<MessageEventSource> m_source;
    Vector<JS::NonnullGCPtr<JS::Object>> m_ports;
    mutable JS::GCPtr<JS::Array> m_ports_array;
};

}
