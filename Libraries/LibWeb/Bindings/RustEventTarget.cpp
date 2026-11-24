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
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/Utils.h>
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

void RustEventTarget::event_listener_list_changed()
{
    if (m_event_listener_list_changed)
        m_event_listener_list_changed(m_owner.ptr());
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

RustInterfaceCell* rust_event_owner(DOM::Event const& event, InterfaceName interface_name)
{
    if (!event.is_owned_by_rust_interface_object())
        return nullptr;
    auto const& rust_event = static_cast<RustEvent const&>(event);
    if (rust_event.m_owner_interface_name != interface_name)
        return nullptr;
    return rust_event.m_owner.ptr();
}

}

using namespace Web;
using namespace Web::Bindings;

// Declared on the Rust side in Libraries/LibWeb/Bindings/Rust/src/bindings/event_target.rs.
extern "C" {

RustEventTarget* web_bindings_create_event_target();
void web_bindings_set_event_target_owner(RustEventTarget*, u16 interface_name, RustInterfaceCell* owner, RustEventTarget::GetTheParent);
void web_bindings_set_event_listener_list_changed_steps(RustEventTarget*, RustEventTarget::EventListenerListChanged);
bool web_bindings_has_event_listener(DOM::EventTarget*, ReadonlyBytes type);
bool web_bindings_has_event_listeners(DOM::EventTarget*);
JSValue web_bindings_event_handler_attribute(RustEventTarget*, ReadonlyBytes name);
void web_bindings_set_event_handler_attribute(RustEventTarget*, ReadonlyBytes name, JSValue);
void web_bindings_add_event_listener(void* realm, DOM::EventTarget*, ReadonlyBytes type, JSObject* callback);
void web_bindings_add_event_listener_without_options(DOM::EventTarget*, ReadonlyBytes type, WebIDL::CallbackType*);
void web_bindings_remove_event_listener_without_options(DOM::EventTarget*, ReadonlyBytes type, WebIDL::CallbackType*);
DOM::Event* web_bindings_create_event(void* realm, ReadonlyBytes type, bool bubbles, bool cancelable, bool composed);
RustEvent* web_bindings_create_rust_event(void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed);
void web_bindings_set_event_owner(RustEvent*, u16 interface_name, RustInterfaceCell* owner);
void web_bindings_event_set_is_trusted(DOM::Event*, bool is_trusted);
bool web_bindings_dispatch_event(DOM::EventTarget*, DOM::Event*);
bool web_bindings_dispatch_event_with_legacy_output_did_listeners_throw_flag(DOM::EventTarget*, DOM::Event*);
bool web_bindings_event_canceled(DOM::Event*);
bool web_bindings_event_dispatched(DOM::Event*);
FlatPtr web_bindings_event_type(DOM::Event const*);
bool web_bindings_event_is_trusted(DOM::Event const*);
double web_bindings_event_time_stamp(DOM::Event const*);
bool web_bindings_event_cancelable(DOM::Event const*);
void web_bindings_initialize_event(RustEvent*, FlatPtr type, bool bubbles, bool cancelable);
DOM::EventTarget* web_bindings_event_retarget_against_current_target(DOM::Event*, DOM::EventTarget* object);
}

RustEventTarget* web_bindings_create_event_target()
{
    return RustEventTarget::create().ptr();
}

void web_bindings_set_event_target_owner(RustEventTarget* event_target, u16 interface_name, RustInterfaceCell* owner, RustEventTarget::GetTheParent get_the_parent)
{
    event_target->set_owner(static_cast<InterfaceName>(interface_name), *owner, get_the_parent);
}

void web_bindings_set_event_listener_list_changed_steps(RustEventTarget* event_target, RustEventTarget::EventListenerListChanged steps)
{
    event_target->set_event_listener_list_changed_steps(steps);
}

bool web_bindings_has_event_listener(DOM::EventTarget* event_target, ReadonlyBytes type)
{
    return event_target->has_event_listener(Utf16FlyString::from_utf8_without_validation(type));
}

bool web_bindings_has_event_listeners(DOM::EventTarget* event_target)
{
    return event_target->has_event_listeners();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-idl-attributes
JSValue web_bindings_event_handler_attribute(RustEventTarget* event_target, ReadonlyBytes name)
{
    auto* callback = event_target->event_handler_attribute(Utf16FlyString::from_utf8_without_validation(name));
    if (!callback)
        return JS::HostABI::value_to_abi(JS::js_null());
    return JS::HostABI::value_to_abi(callback->callback.ptr());
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

// https://dom.spec.whatwg.org/#add-an-event-listener, for a listener whose callback is the function, with no options.
void web_bindings_add_event_listener(void* realm, DOM::EventTarget* target, ReadonlyBytes type, JSObject* callback)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto callback_type = GC::Heap::the().allocate<WebIDL::CallbackType>(*JS::HostABI::object_from_abi(callback), js_realm);
    target->add_event_listener_without_options(Utf16FlyString::from_utf8_without_validation(type), DOM::IDLEventListener::create(callback_type));
}

void web_bindings_add_event_listener_without_options(DOM::EventTarget* target, ReadonlyBytes type, WebIDL::CallbackType* callback)
{
    target->add_event_listener_without_options(Utf16FlyString::from_utf8_without_validation(type), DOM::IDLEventListener::create(*callback));
}

void web_bindings_remove_event_listener_without_options(DOM::EventTarget* target, ReadonlyBytes type, WebIDL::CallbackType* callback)
{
    target->remove_event_listener_without_options(Utf16FlyString::from_utf8_without_validation(type), DOM::IDLEventListener::create(*callback));
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

void web_bindings_event_set_is_trusted(DOM::Event* event, bool is_trusted)
{
    event->set_is_trusted(is_trusted);
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

bool web_bindings_event_dispatched(DOM::Event* event)
{
    return event->dispatched();
}

FlatPtr web_bindings_event_type(DOM::Event const* event)
{
    return event->type().raw_identity();
}

bool web_bindings_event_is_trusted(DOM::Event const* event)
{
    return event->is_trusted();
}

double web_bindings_event_time_stamp(DOM::Event const* event)
{
    return event->time_stamp();
}

bool web_bindings_event_cancelable(DOM::Event const* event)
{
    return event->cancelable();
}

void web_bindings_initialize_event(RustEvent* event, FlatPtr type, bool bubbles, bool cancelable)
{
    auto type_string = Utf16String::from_raw(type);
    event->initialize_event(Utf16FlyString::from_utf16(type_string.utf16_view()), bubbles, cancelable);
}

DOM::EventTarget* web_bindings_event_retarget_against_current_target(DOM::Event* event, DOM::EventTarget* object)
{
    return DOM::retarget(object, event->current_target().ptr());
}
