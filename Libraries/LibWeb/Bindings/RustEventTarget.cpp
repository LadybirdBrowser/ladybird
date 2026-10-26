/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/RustEventTarget.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebIDL/CallbackType.h>

// Defined by the generated Rust bindings.
extern "C" JSObject* web_rust_bindings_wrap(JSVM*, void* realm, u16 interface_name, void* cell);

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(RustEventTarget);
GC_DEFINE_ALLOCATOR(RustEvent);

static GC::Ref<JS::HostObject> wrapper_of_owner(JS::Realm& realm, InterfaceName interface_name, RustInterfaceCell& owner)
{
    auto* wrapper = web_rust_bindings_wrap(JS::HostABI::vm_to_abi(realm.vm()), &realm, to_underlying(interface_name), &owner);
    return as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
}

GC::Ref<RustEventTarget> RustEventTarget::create()
{
    return GC::Heap::the().allocate<RustEventTarget>();
}

void RustEventTarget::set_owner(InterfaceName interface_name, RustInterfaceCell& owner, GetTheParent get_the_parent)
{
    VERIFY(!m_owner);
    m_owner_interface_name = interface_name;
    m_owner = owner;
    m_get_the_parent = get_the_parent;
}

DOM::EventTarget* RustEventTarget::get_parent(DOM::Event const&)
{
    VERIFY(m_owner);
    return m_get_the_parent(m_owner.ptr());
}

GC::Ref<JS::HostObject> RustEventTarget::create_wrapper(JS::Realm& realm)
{
    VERIFY(m_owner);
    return wrapper_of_owner(realm, m_owner_interface_name, *m_owner);
}

void RustEventTarget::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_owner);
}

GC::Ref<RustEvent> RustEvent::create(Utf16FlyString const& type, DOM::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RustEvent>(type, event_init, time_stamp);
}

RustEvent::RustEvent(Utf16FlyString const& type, DOM::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
{
}

void RustEvent::set_owner(InterfaceName interface_name, RustInterfaceCell& owner)
{
    VERIFY(!m_owner);
    m_owner_interface_name = interface_name;
    m_owner = owner;
}

GC::Ref<JS::HostObject> RustEvent::create_wrapper(JS::Realm& realm)
{
    VERIFY(m_owner);
    return wrapper_of_owner(realm, m_owner_interface_name, *m_owner);
}

void RustEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_owner);
}

}

using namespace Web;
using namespace Web::Bindings;

// Declared on the Rust side in Libraries/LibWeb/Bindings/Rust/src/bindings/event_target.rs.
extern "C" {

RustEventTarget* web_bindings_create_event_target();
void web_bindings_set_event_target_owner(RustEventTarget*, u16 interface_name, RustInterfaceCell* owner, RustEventTarget::GetTheParent);
JSValue web_bindings_event_handler_attribute(RustEventTarget*, ReadonlyBytes name);
void web_bindings_set_event_handler_attribute(RustEventTarget*, ReadonlyBytes name, JSValue);
DOM::Event* web_bindings_create_event(void* realm, ReadonlyBytes type, bool bubbles, bool cancelable, bool composed);
RustEvent* web_bindings_create_rust_event(void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed);
void web_bindings_set_event_owner(RustEvent*, u16 interface_name, RustInterfaceCell* owner);
bool web_bindings_dispatch_event(DOM::EventTarget*, DOM::Event*);
bool web_bindings_dispatch_event_with_legacy_output_did_listeners_throw_flag(DOM::EventTarget*, DOM::Event*);
bool web_bindings_event_canceled(DOM::Event*);
}

RustEventTarget* web_bindings_create_event_target()
{
    return RustEventTarget::create().ptr();
}

void web_bindings_set_event_target_owner(RustEventTarget* event_target, u16 interface_name, RustInterfaceCell* owner, RustEventTarget::GetTheParent get_the_parent)
{
    event_target->set_owner(static_cast<InterfaceName>(interface_name), *owner, get_the_parent);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-idl-attributes
JSValue web_bindings_event_handler_attribute(RustEventTarget* event_target, ReadonlyBytes name)
{
    auto* callback = event_target->event_handler_attribute(Utf16FlyString::from_utf8_without_validation(name));
    if (!callback)
        return JS::HostABI::value_to_abi(JS::js_null());
    return JS::HostABI::value_to_abi(callback->callback);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-idl-attributes
void web_bindings_set_event_handler_attribute(RustEventTarget* event_target, ReadonlyBytes name, JSValue raw_value)
{
    // NB: EventHandler is [LegacyTreatNonObjectAsNull], so a value that is not an object sets the handler to null.
    auto value = JS::HostABI::value_from_abi(raw_value);
    GC::Ptr<WebIDL::CallbackType> callback;
    if (value.is_object())
        callback = GC::Heap::the().allocate<WebIDL::CallbackType>(value.as_object(), HTML::incumbent_realm(), WebIDL::OperationReturnsPromise::No);
    event_target->set_event_handler_attribute(Utf16FlyString::from_utf8_without_validation(name), callback);
}

DOM::Event* web_bindings_create_event(void* realm, ReadonlyBytes type, bool bubbles, bool cancelable, bool composed)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    DOM::EventInit event_init { .bubbles = bubbles, .cancelable = cancelable, .composed = composed };
    return DOM::Event::create(Utf16FlyString::from_utf8_without_validation(type), event_init, HighResolutionTime::current_high_resolution_time(js_realm.global_object())).ptr();
}

RustEvent* web_bindings_create_rust_event(void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    DOM::EventInit event_init { .bubbles = bubbles, .cancelable = cancelable, .composed = composed };
    auto type_string = Utf16String::from_raw(type);
    return RustEvent::create(Utf16FlyString::from_utf16(type_string.utf16_view()), event_init, HighResolutionTime::current_high_resolution_time(js_realm.global_object())).ptr();
}

void web_bindings_set_event_owner(RustEvent* event, u16 interface_name, RustInterfaceCell* owner)
{
    event->set_owner(static_cast<InterfaceName>(interface_name), *owner);
}

bool web_bindings_dispatch_event(DOM::EventTarget* target, DOM::Event* event)
{
    return target->dispatch_event(*event);
}

bool web_bindings_dispatch_event_with_legacy_output_did_listeners_throw_flag(DOM::EventTarget* target, DOM::Event* event)
{
    bool legacy_output_did_listeners_throw_flag = false;
    DOM::EventDispatcher::dispatch(*target, *event, false, legacy_output_did_listeners_throw_flag);
    return legacy_output_did_listeners_throw_flag;
}

bool web_bindings_event_canceled(DOM::Event* event)
{
    return event->cancelled();
}
