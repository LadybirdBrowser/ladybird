/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessageEventPrototype.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(MessageEvent);

JS::NonnullGCPtr<MessageEvent> MessageEvent::create(JS::Realm& realm, FlyString const& event_name, MessageEventInit const& event_init)
{
    return realm.heap().allocate<MessageEvent>(realm, realm, event_name, event_init);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<MessageEvent>> MessageEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, MessageEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

MessageEvent::MessageEvent(JS::Realm& realm, FlyString const& event_name, MessageEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_data(event_init.data)
    , m_origin(event_init.origin)
    , m_last_event_id(event_init.last_event_id)
    , m_source(event_init.source)
{
    m_ports.ensure_capacity(event_init.ports.size());
    for (auto const& port : event_init.ports) {
        VERIFY(port);
        m_ports.unchecked_append(static_cast<JS::Object&>(*port));
    }
}

MessageEvent::~MessageEvent() = default;

void MessageEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MessageEvent);
}

void MessageEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
    visitor.visit(m_ports_array);
    visitor.visit(m_ports);
}

Variant<JS::Handle<WindowProxy>, JS::Handle<MessagePort>, Empty> MessageEvent::source() const
{
    if (!m_source.has_value())
        return Empty {};

    return m_source.value().downcast<JS::Handle<WindowProxy>, JS::Handle<MessagePort>>();
}

JS::NonnullGCPtr<JS::Object> MessageEvent::ports() const
{
    if (!m_ports_array) {
        Vector<JS::Value> port_vector;
        for (auto const& port : m_ports) {
            port_vector.append(port);
        }
        m_ports_array = JS::Array::create_from(realm(), port_vector);
        MUST(m_ports_array->set_integrity_level(IntegrityLevel::Frozen));
    }
    return *m_ports_array;
}

// https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-initmessageevent
void MessageEvent::init_message_event(String const& type, bool bubbles, bool cancelable, JS::Value data, String const& origin, String const& last_event_id, Optional<MessageEventSource> source, Vector<JS::Handle<MessagePort>> const& ports)
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
    m_ports.clear();
    m_ports.ensure_capacity(ports.size());
    for (auto const& port : ports) {
        VERIFY(port);
        m_ports.unchecked_append(static_cast<JS::Object&>(*port));
    }
}

}
