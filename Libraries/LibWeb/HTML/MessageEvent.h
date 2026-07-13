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
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// FIXME: Include ServiceWorker
// https://html.spec.whatwg.org/multipage/comms.html#messageeventsource
using MessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>>;
using NullableMessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>, Empty>;

// https://html.spec.whatwg.org/multipage/comms.html#messageevent
class WEB_API MessageEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(MessageEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MessageEvent);

public:
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::MessageEventInit const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::MessageEventInit const&, URL::Origin const&);
    static WebIDL::ExceptionOr<GC::Ref<MessageEvent>> construct_impl(JS::Realm&, Utf16FlyString const& event_name, Bindings::MessageEventInit const&);

    MessageEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init);
    MessageEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const&);
    virtual ~MessageEvent() override;

    JS::Value data() const { return m_data; }
    Utf16String origin() const;
    Utf16String const& last_event_id() const { return m_last_event_id; }
    GC::Ref<JS::Object> ports() const;

    NullableMessageEventSource source() const;

    virtual Optional<URL::Origin> extract_an_origin() const override;

    void init_message_event(Utf16FlyString const& type, bool bubbles, bool cancelable, JS::Value data, Utf16View origin, Utf16View last_event_id, NullableMessageEventSource source, GC::RootVector<GC::Ref<MessagePort>> const& ports);

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    MessageEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init, Variant<URL::Origin, Utf16String, Empty>);

    JS::Value m_data;

    // https://html.spec.whatwg.org/multipage/comms.html#concept-messageevent-origin
    // Each MessageEvent has an origin (an origin, a string, or null), initially null.
    Variant<URL::Origin, Utf16String, Empty> m_origin;

    Utf16String m_last_event_id;
    NullableMessageEventSource m_source;
    Vector<GC::Ref<JS::Object>> m_ports;
    mutable GC::Ptr<JS::Array> m_ports_array;
};

}
