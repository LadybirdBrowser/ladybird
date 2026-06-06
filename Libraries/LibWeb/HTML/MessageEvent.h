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
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

// FIXME: Include ServiceWorker
// https://html.spec.whatwg.org/multipage/comms.html#messageeventsource
using MessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>>;
using NullableMessageEventSource = Variant<GC::Ref<WindowProxy>, GC::Ref<MessagePort>, Empty>;

// https://html.spec.whatwg.org/multipage/comms.html#messageevent
class WEB_API MessageEvent : public DOM::Event {
    WEB_WRAPPABLE(MessageEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MessageEvent);

public:
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Object const& relevant_global_object, FlyString const& event_name, Bindings::MessageEventInit const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(JS::Object const& relevant_global_object, FlyString const& event_name, Bindings::MessageEventInit const&, URL::Origin const&);
    [[nodiscard]] static GC::Ref<MessageEvent> create(FlyString const& event_name, Bindings::MessageEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MessageEvent> create(FlyString const& event_name, Bindings::MessageEventInit const&, URL::Origin const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<MessageEvent>> construct_impl(WindowOrWorkerGlobalScopeMixin&, FlyString const& event_name, Bindings::MessageEventInit const&);

    virtual ~MessageEvent() override;

    JS::Value data() const { return m_data; }
    String origin() const;
    String const& last_event_id() const { return m_last_event_id; }
    GC::Ref<JS::Object> ports(JS::Realm&) const;

    NullableMessageEventSource source() const;

    virtual Optional<URL::Origin> extract_an_origin() const override;

    void init_message_event(String const& type, bool bubbles, bool cancelable, JS::Value data, String const& origin, String const& last_event_id, NullableMessageEventSource source, GC::RootVector<GC::Ref<MessagePort>> const& ports);

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;

    MessageEvent(FlyString const& event_name, Bindings::MessageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const&, HighResolutionTime::DOMHighResTimeStamp);
    MessageEvent(FlyString const& event_name, Bindings::MessageEventInit const& event_init, Variant<URL::Origin, String, Empty>, HighResolutionTime::DOMHighResTimeStamp);

    JS::Value m_data;

    // https://html.spec.whatwg.org/multipage/comms.html#concept-messageevent-origin
    // Each MessageEvent has an origin (an origin, a string, or null), initially null.
    Variant<URL::Origin, String, Empty> m_origin;

    String m_last_event_id;
    NullableMessageEventSource m_source;
    Vector<GC::Ref<MessagePort>> m_ports;
    mutable Bindings::WrapperWorldWeakValueCache<JS::Array> m_ports_arrays;
};

}
