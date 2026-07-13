/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessageEvent.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Infra/SerializedURL.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MessageEvent);

GC::Ref<MessageEvent> MessageEvent::create(JS::Realm& realm, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init)
{
    return realm.create<MessageEvent>(realm, event_name, event_init);
}

GC::Ref<MessageEvent> MessageEvent::create(JS::Realm& realm, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const& origin)
{
    return realm.create<MessageEvent>(realm, event_name, event_init, origin);
}

WebIDL::ExceptionOr<GC::Ref<MessageEvent>> MessageEvent::construct_impl(JS::Realm& realm, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

MessageEvent::MessageEvent(JS::Realm& realm, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init)
    : MessageEvent(realm, event_name, event_init, event_init.origin)
{
}

MessageEvent::MessageEvent(JS::Realm& realm, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const& origin)
    : MessageEvent(realm, event_name, event_init, Variant<URL::Origin, Utf16String, Empty> { origin })
{
}

MessageEvent::MessageEvent(JS::Realm& realm, Utf16FlyString const& event_name, Bindings::MessageEventInit const& event_init, Variant<URL::Origin, Utf16String, Empty> origin)
    : DOM::Event(realm, event_name, event_init)
    , m_data(event_init.data)
    , m_origin(move(origin))
    , m_last_event_id(event_init.last_event_id)
    , m_source(event_init.source)

{
    m_ports.ensure_capacity(event_init.ports.size());
    for (auto const& port : event_init.ports) {
        m_ports.unchecked_append(port);
    }
}

MessageEvent::~MessageEvent() = default;

void MessageEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MessageEvent);
    Base::initialize(realm);
}

void MessageEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
    visitor.visit(m_ports_array);
    visitor.visit(m_ports);
    visitor.visit(m_source);
}

// https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-origin
Utf16String MessageEvent::origin() const
{
    return m_origin.visit(
        // 1. If this's origin is an origin, then return the serialization of this's origin.
        [](URL::Origin const& origin) {
            return utf16_string_from_url_ascii(origin.serialize());
        },
        // 2. If this's origin is null, then return the empty string.
        [](Empty) {
            return Utf16String {};
        },
        // 3. Return this's origin.
        [](Utf16String const& origin) {
            return origin;
        });
}

NullableMessageEventSource MessageEvent::source() const
{
    return m_source;
}

GC::Ref<JS::Object> MessageEvent::ports() const
{
    if (!m_ports_array) {
        GC::RootVector<JS::Value> port_vector;
        for (auto const& port : m_ports)
            port_vector.append(port);

        m_ports_array = JS::Array::create_from(realm(), port_vector);
        MUST(m_ports_array->set_integrity_level(IntegrityLevel::Frozen));
    }
    return *m_ports_array;
}

// https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-initmessageevent
void MessageEvent::init_message_event(Utf16FlyString const& type, bool bubbles, bool cancelable, JS::Value data, Utf16View origin, Utf16View last_event_id, NullableMessageEventSource source, GC::RootVector<GC::Ref<MessagePort>> const& ports)
{
    // The initMessageEvent(type, bubbles, cancelable, data, origin, lastEventId, source, ports) method must initialize the event in a
    // manner analogous to the similarly-named initEvent() method.

    // 1. If this’s dispatch flag is set, then return.
    if (dispatched())
        return;

    // 2. Initialize this with type, bubbles, and cancelable.
    initialize_event(type, bubbles, cancelable);

    // Implementation Defined: Initialise other values.
    m_data = data;
    m_origin = Utf16String::from_utf16(origin);
    m_last_event_id = Utf16String::from_utf16(last_event_id);
    m_source = source;

    m_ports_array = nullptr;
    m_ports.clear();
    m_ports.ensure_capacity(ports.size());
    for (auto const& port : ports) {
        m_ports.unchecked_append(static_cast<JS::Object&>(*port));
    }
}

// https://html.spec.whatwg.org/multipage/comms.html#the-messageevent-interface:extract-an-origin
Optional<URL::Origin> MessageEvent::extract_an_origin() const
{
    // Objects implementing the MessageEvent interface's extract an origin steps are to return this's origin if it is an origin; otherwise null.
    return m_origin.visit(
        [](URL::Origin const& origin) -> Optional<URL::Origin> {
            return origin;
        },
        [](auto const&) -> Optional<URL::Origin> {
            return {};
        });
}

}
