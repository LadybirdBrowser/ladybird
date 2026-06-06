/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGC/RootVector.h>
#include <LibGC/Weak.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// FIXME: Include ServiceWorker
// https://html.spec.whatwg.org/multipage/comms.html#messageeventsource
using MessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>>;
using NullableMessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>, Empty>;

// https://html.spec.whatwg.org/multipage/comms.html#messageevent
class WEB_API MessageEvent : public DOM::Event {
    WEB_WRAPPABLE(MessageEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MessageEvent);

public:
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Realm&, FlyString const& event_name, Bindings::MessageEventInit const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Realm&, FlyString const& event_name, Bindings::MessageEventInit const&, URL::Origin const&);
    static WebIDL::ExceptionOr<GC::Ref<MessageEvent>> construct_impl(JS::Realm&, FlyString const& event_name, Bindings::MessageEventInit const&);

    MessageEvent(JS::Realm&, FlyString const& event_name, Bindings::MessageEventInit const& event_init);
    MessageEvent(JS::Realm&, FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const&);
    virtual ~MessageEvent() override;

    JS::Value data() const { return m_data; }
    String origin() const;
    String const& last_event_id() const { return m_last_event_id; }
    GC::Ref<JS::Object> ports() const;

    NullableMessageEventSource source() const;

    virtual Optional<URL::Origin> extract_an_origin() const override;

    void init_message_event(String const& type, bool bubbles, bool cancelable, JS::Value data, String const& origin, String const& last_event_id, NullableMessageEventSource source, GC::RootVector<GC::Ref<MessagePort>> const& ports);

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;

    MessageEvent(JS::Realm&, FlyString const& event_name, Bindings::MessageEventInit const& event_init, Variant<URL::Origin, String, Empty>);

    JS::Value m_data;

    // https://html.spec.whatwg.org/multipage/comms.html#concept-messageevent-origin
    // Each MessageEvent has an origin (an origin, a string, or null), initially null.
    Variant<URL::Origin, String, Empty> m_origin;

    String m_last_event_id;
    NullableMessageEventSource m_source;
    Vector<GC::Ref<MessagePort>> m_ports;
    mutable GC::Weak<JS::Array> m_ports_array;
    mutable Vector<GC::Weak<JS::Array>> m_live_ports_arrays;
};

}
