/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/MessageEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

// FIXME: Include ServiceWorker
// https://html.spec.whatwg.org/multipage/comms.html#messageeventsource
using MessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>>;
using NullableMessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>, Empty>;

struct MessageEventInit : DOM::EventInit {
    JS::Value data { JS::js_null() };
    Utf16String last_event_id {};
    Utf16String origin {};
    GC::RootVector<GC::Ref<MessagePort>> ports {};
    NullableMessageEventSource source { Empty {} };
};

// https://html.spec.whatwg.org/multipage/comms.html#messageevent
class WEB_API MessageEvent : public DOM::Event {
    WEB_WRAPPABLE(MessageEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MessageEvent);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<MessageEvent>> create_for_constructor(Utf16String const& event_name, Bindings::MessageEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Object const& relevant_global_object, FlyString const& event_name, MessageEventInit const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Object const& relevant_global_object, FlyString const& event_name, MessageEventInit const&, URL::Origin const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Object const& relevant_global_object, Utf16FlyString const& event_name, MessageEventInit const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Object const& relevant_global_object, Utf16FlyString const& event_name, MessageEventInit const&, URL::Origin const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MessageEvent> create(FlyString const& event_name, MessageEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MessageEvent> create(FlyString const& event_name, MessageEventInit const&, URL::Origin const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MessageEvent> create(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MessageEvent> create(Utf16FlyString const& event_name, MessageEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MessageEvent> create(Utf16FlyString const& event_name, MessageEventInit const&, URL::Origin const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~MessageEvent() override;

    JS::Value const& data() const { return m_data; }
    Utf16String origin() const;
    Utf16String const& last_event_id() const { return m_last_event_id; }
    Vector<GC::Ref<MessagePort>> const& message_ports() const { return m_ports; }
    JS::Value ports(JS::Realm&);

    NullableMessageEventSource source() const;

    virtual Optional<URL::Origin> extract_an_origin() const override;

    void init_message_event(Utf16FlyString const& type, bool bubbles, bool cancelable, JS::Value data, Utf16View origin, Utf16View last_event_id, NullableMessageEventSource source, GC::RootVector<GC::Ref<MessagePort>> const& ports);

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;

    MessageEvent(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(FlyString const& event_name, MessageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(FlyString const& event_name, MessageEventInit const& event_init, URL::Origin const&, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(FlyString const& event_name, MessageEventInit const& event_init, Variant<URL::Origin, Utf16String, Empty>, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(Utf16FlyString const& event_name, MessageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(Utf16FlyString const& event_name, MessageEventInit const& event_init, URL::Origin const&, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(Utf16FlyString const& event_name, MessageEventInit const& event_init, Variant<URL::Origin, Utf16String, Empty>, HighResolutionTime::DOMHighResTimeStamp);

    JS::Value m_data;

    // https://html.spec.whatwg.org/multipage/comms.html#concept-messageevent-origin
    // Each MessageEvent has an origin (an origin, a string, or null), initially null.
    Variant<URL::Origin, Utf16String, Empty> m_origin;

    Utf16String m_last_event_id;
    NullableMessageEventSource m_source;
    Vector<GC::Ref<MessagePort>> m_ports;
};

}
