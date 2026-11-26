/*
 * Copyright (c) 2024, Maciej <sppmacd@pm.me>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustEventTarget.h>
#include <LibWeb/HTML/DragDataStore.h>
#include <LibWeb/HTML/DragEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

struct MouseEventConstructorOptions {
    JSObject* view;
    JSObject* related_target;
    double screen_x;
    double screen_y;
    double client_x;
    double client_y;
    double movement_x;
    double movement_y;
    i32 detail;
    i16 button;
    u16 buttons;
    bool bubbles;
    bool cancelable;
    bool composed;
    bool ctrl_key;
    bool shift_key;
    bool alt_key;
    bool meta_key;
    bool modifier_alt_graph;
    bool modifier_caps_lock;
    bool modifier_fn;
    bool modifier_fn_lock;
    bool modifier_hyper;
    bool modifier_num_lock;
    bool modifier_scroll_lock;
    bool modifier_super;
    bool modifier_symbol;
    bool modifier_symbol_lock;
};

extern "C" {

void web_rust_initialize_drag_event(JSVM*, DOM::Event const*, RustDragDataStore const*, FlatPtr current_drag_operation);
void web_rust_finish_drag_event(Bindings::RustInterfaceCell const*);
FlatPtr web_rust_drag_event_drop_effect(Bindings::RustInterfaceCell const*);
FlatPtr web_rust_drag_event_operation(Bindings::RustInterfaceCell const*);

DOM::Event* web_drag_event_create_mouse_event(JSObject* global_object, FlatPtr, MouseEventConstructorOptions const*);
}

DOM::Event* web_drag_event_create_mouse_event(JSObject* global_object, FlatPtr name, MouseEventConstructorOptions const* init)
{
    UIEvents::MouseEventOptions options {
        .bubbles = init->bubbles,
        .cancelable = init->cancelable,
        .composed = init->composed,
        .view = init->view ? WindowProxy::from_object(*JS::HostABI::object_from_abi(init->view)) : nullptr,
        .detail = init->detail,
        .ctrl_key = init->ctrl_key,
        .shift_key = init->shift_key,
        .alt_key = init->alt_key,
        .meta_key = init->meta_key,
        .modifier_alt_graph = init->modifier_alt_graph,
        .modifier_caps_lock = init->modifier_caps_lock,
        .modifier_fn = init->modifier_fn,
        .modifier_fn_lock = init->modifier_fn_lock,
        .modifier_hyper = init->modifier_hyper,
        .modifier_num_lock = init->modifier_num_lock,
        .modifier_scroll_lock = init->modifier_scroll_lock,
        .modifier_super = init->modifier_super,
        .modifier_symbol = init->modifier_symbol,
        .modifier_symbol_lock = init->modifier_symbol_lock,
        .screen_x = init->screen_x,
        .screen_y = init->screen_y,
        .client_x = init->client_x,
        .client_y = init->client_y,
        .movement_x = init->movement_x,
        .movement_y = init->movement_y,
        .button = init->button,
        .buttons = init->buttons,
        .related_target = Bindings::impl_from<DOM::EventTarget>(JS::HostABI::object_from_abi(init->related_target)),
    };
    auto time_stamp = HighResolutionTime::current_high_resolution_time(relevant_global_object(*JS::HostABI::object_from_abi(global_object)));
    return Bindings::RustMouseEvent::create(Utf16FlyString::from_raw(name), options, options.client_x, options.client_y, options.client_x, options.client_y, time_stamp).ptr();
}

GC::Ref<DOM::Event> create_drag_event(Utf16FlyString const& name, UIEvents::MouseEventOptions const& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp, DragDataStore const& store, Utf16FlyString const& current_drag_operation)
{
    auto event = Bindings::RustMouseEvent::create(name, options, page_x, page_y, offset_x, offset_y, time_stamp);
    web_rust_initialize_drag_event(JS::HostABI::vm_to_abi(JS::VM::the()), event.ptr(), store.rust_store(), current_drag_operation.raw_identity());
    return event;
}

GC::Ref<DOM::Event> create_drag_event_for_create_event(HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = Bindings::RustMouseEvent::create(Utf16FlyString {}, UIEvents::MouseEventOptions {}, 0, 0, 0, 0, time_stamp);
    web_rust_initialize_drag_event(JS::HostABI::vm_to_abi(JS::VM::the()), event.ptr(), nullptr, "none"_utf16_fly_string.raw_identity());
    return event;
}

static Bindings::RustInterfaceCell const* drag_event_owner(DOM::Event const& event)
{
    auto* owner = event.rust_event_owner(Bindings::InterfaceName::DragEvent);
    VERIFY(owner);
    return owner;
}

void finish_drag_event(DOM::Event const& event)
{
    web_rust_finish_drag_event(drag_event_owner(event));
}

Utf16FlyString drag_event_drop_effect(DOM::Event const& event)
{
    return Utf16FlyString::adopt_raw(web_rust_drag_event_drop_effect(drag_event_owner(event)));
}

Utf16FlyString drag_event_operation(DOM::Event const& event)
{
    return Utf16FlyString::adopt_raw(web_rust_drag_event_operation(drag_event_owner(event)));
}

}
