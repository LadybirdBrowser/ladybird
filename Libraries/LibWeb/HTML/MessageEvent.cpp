/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MessageEvent);

static Bindings::WrapperWorldWeakValueCacheMap<MessageEvent, JS::Array>& message_event_ports_caches()
{
    static NeverDestroyed<Bindings::WrapperWorldWeakValueCacheMap<MessageEvent, JS::Array>> caches;
    return *caches;
}

static void clear_ports_cache(MessageEvent& event)
{
    message_event_ports_caches().cache_for(event).clear();
}

static Bindings::WrapperWorldWeakValueCache<JS::Array>& ports_cache_for(MessageEvent& event)
{
    return message_event_ports_caches().cache_for(event);
}

static MessageEventInit message_event_init_from_bindings(Bindings::MessageEventInit const& event_init)
{
    return {
        {
            .bubbles = event_init.bubbles,
            .cancelable = event_init.cancelable,
            .composed = event_init.composed,
        },
        event_init.data,
        event_init.last_event_id,
        event_init.origin,
        event_init.ports,
        event_init.source,
    };
}

WebIDL::ExceptionOr<GC::Ref<MessageEvent>> MessageEvent::construct_impl(JS::Realm& realm, String const& event_name, Bindings::MessageEventInit const& event_init)
{
    auto& global_scope = HTML::relevant_window_or_worker_global_scope(realm.global_object());
    return MessageEvent::create(FlyString { event_name }, message_event_init_from_bindings(event_init), HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope)));
}

GC::Ref<MessageEvent> MessageEvent::create(JS::Object const& relevant_global_object, FlyString const& event_name, MessageEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

GC::Ref<MessageEvent> MessageEvent::create(JS::Object const& relevant_global_object, FlyString const& event_name, MessageEventInit const& event_init, URL::Origin const& origin)
{
    return create(event_name, event_init, origin, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

GC::Ref<MessageEvent> MessageEvent::create(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MessageEvent>(event_name, time_stamp);
}

GC::Ref<MessageEvent> MessageEvent::create(FlyString const& event_name, MessageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MessageEvent>(event_name, event_init, time_stamp);
}

GC::Ref<MessageEvent> MessageEvent::create(FlyString const& event_name, MessageEventInit const& event_init, URL::Origin const& origin, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MessageEvent>(event_name, event_init, origin, time_stamp);
}

MessageEvent::MessageEvent(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, time_stamp)
    , m_data(JS::js_null())
    , m_origin(String {})
{
}

MessageEvent::MessageEvent(FlyString const& event_name, MessageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MessageEvent(event_name, event_init, String { event_init.origin }, time_stamp)
{
}

MessageEvent::MessageEvent(FlyString const& event_name, MessageEventInit const& event_init, URL::Origin const& origin, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MessageEvent(event_name, event_init, Variant<URL::Origin, String, Empty> { origin }, time_stamp)
{
}

MessageEvent::MessageEvent(FlyString const& event_name, MessageEventInit const& event_init, Variant<URL::Origin, String, Empty> origin, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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

JS::Value MessageEvent::ports(JS::Realm& realm)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto& ports_cache = ports_cache_for(*this);

    if (auto ports_array = ports_cache.get(wrapper_world))
        return JS::Value(ports_array);

    GC::RootVector<JS::Value> port_vector;
    for (auto const& port : message_ports())
        port_vector.append(Bindings::wrap(wrapper_world, realm, port));

    auto ports_array = JS::Array::create_from(realm, port_vector);
    MUST(ports_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

    ports_cache.set(wrapper_world, ports_array);

    return JS::Value(ports_array);
}

// https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-initmessageevent
void MessageEvent::init_message_event(String const& type, bool bubbles, bool cancelable, JS::Value data, String const& origin, String const& last_event_id, NullableMessageEventSource source, GC::RootVector<GC::Ref<MessagePort>> const& ports)
{
    auto was_dispatched = dispatched();

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

    m_ports.clear();
    m_ports.ensure_capacity(ports.size());
    for (auto const& port : ports) {
        m_ports.unchecked_append(port);
    }

    if (!was_dispatched)
        clear_ports_cache(*this);
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
