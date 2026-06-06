/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/MessageEvent.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MessageEvent);

GC::Ref<MessageEvent> MessageEvent::create(JS::Object const& relevant_global_object, FlyString const& event_name, Bindings::MessageEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

GC::Ref<MessageEvent> MessageEvent::create(JS::Object const& relevant_global_object, FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const& origin)
{
    return create(event_name, event_init, origin, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

GC::Ref<MessageEvent> MessageEvent::create(FlyString const& event_name, Bindings::MessageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MessageEvent>(event_name, event_init, time_stamp);
}

GC::Ref<MessageEvent> MessageEvent::create(FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const& origin, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MessageEvent>(event_name, event_init, origin, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<MessageEvent>> MessageEvent::construct_impl(WindowOrWorkerGlobalScopeMixin& global_scope, FlyString const& event_name, Bindings::MessageEventInit const& event_init)
{
    return create(relevant_global_object(global_scope), event_name, event_init);
}

MessageEvent::MessageEvent(FlyString const& event_name, Bindings::MessageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MessageEvent(event_name, event_init, String { event_init.origin }, time_stamp)
{
}

MessageEvent::MessageEvent(FlyString const& event_name, Bindings::MessageEventInit const& event_init, URL::Origin const& origin, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MessageEvent(event_name, event_init, Variant<URL::Origin, String, Empty> { origin }, time_stamp)
{
}

MessageEvent::MessageEvent(FlyString const& event_name, Bindings::MessageEventInit const& event_init, Variant<URL::Origin, String, Empty> origin, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
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

void MessageEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
    visitor.visit(m_ports);
    visitor.visit(m_source);
}

// https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-origin
String MessageEvent::origin() const
{
    return m_origin.visit(
        // 1. If this's origin is an origin, then return the serialization of this's origin.
        [](URL::Origin const& origin) {
            return origin.serialize();
        },
        // 2. If this's origin is null, then return the empty string.
        [](Empty) {
            return String {};
        },
        // 3. Return this's origin.
        [](String const& origin) {
            return origin;
        });
}

NullableMessageEventSource MessageEvent::source() const
{
    return m_source;
}

GC::Ref<JS::Object> MessageEvent::ports(JS::Realm& realm) const
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);

    if (auto ports_array = m_ports_arrays.get(wrapper_world))
        return *ports_array;

    GC::RootVector<JS::Value> port_vector;
    for (auto const& port : m_ports)
        port_vector.append(Bindings::wrap(wrapper_world, realm, port));

    auto ports_array = JS::Array::create_from(realm, port_vector);
    MUST(ports_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

    m_ports_arrays.set(wrapper_world, ports_array);

    return ports_array;
}

// https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-initmessageevent
void MessageEvent::init_message_event(String const& type, bool bubbles, bool cancelable, JS::Value data, String const& origin, String const& last_event_id, NullableMessageEventSource source, GC::RootVector<GC::Ref<MessagePort>> const& ports)
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
    m_origin = origin;
    m_last_event_id = last_event_id;
    m_source = source;

    m_ports_arrays.clear();
    m_ports.clear();
    m_ports.ensure_capacity(ports.size());
    for (auto const& port : ports) {
        m_ports.unchecked_append(port);
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
