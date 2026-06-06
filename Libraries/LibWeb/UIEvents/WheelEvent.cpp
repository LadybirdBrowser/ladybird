/*
 * Copyright (c) 2022, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WheelEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/WheelEvent.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(WheelEvent);

static HighResolutionTime::DOMHighResTimeStamp event_time_stamp(HTML::Window& window)
{
    return HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window));
}

WheelEvent::WheelEvent(FlyString const& event_name, Bindings::WheelEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MouseEvent(event_name, event_init, page_x, page_y, offset_x, offset_y, time_stamp)
    , m_delta_x(event_init.delta_x)
    , m_delta_y(event_init.delta_y)
    , m_delta_z(event_init.delta_z)
    , m_delta_mode(event_init.delta_mode)
{
}

WheelEvent::~WheelEvent() = default;

GC::Ref<WheelEvent> WheelEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::WheelEventInit const& wheel_event_init)
{
    return GC::Heap::the().allocate<WheelEvent>(event_name, wheel_event_init, wheel_event_init.client_x, wheel_event_init.client_y, wheel_event_init.client_x, wheel_event_init.client_y, event_time_stamp(window));
}

GC::Ref<WheelEvent> WheelEvent::create(FlyString const& event_name, Bindings::WheelEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<WheelEvent>(event_name, event_init, page_x, page_y, offset_x, offset_y, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<WheelEvent>> WheelEvent::create_from_platform_event(JS::Object const& relevant_global_object, GC::Ptr<HTML::WindowProxy> window_proxy, FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, double delta_x, double delta_y, unsigned button, unsigned buttons, unsigned modifiers, WheelEventIsCancelable is_cancelable)
{
    Bindings::WheelEventInit event_init {};
    event_init.ctrl_key = modifiers & Mod_Ctrl;
    event_init.shift_key = modifiers & Mod_Shift;
    event_init.alt_key = modifiers & Mod_Alt;
    event_init.meta_key = modifiers & Mod_Super;
    event_init.screen_x = screen.x().to_double();
    event_init.screen_y = screen.y().to_double();
    event_init.client_x = client.x().to_double();
    event_init.client_y = client.y().to_double();
    event_init.button = button;
    event_init.buttons = buttons;
    event_init.delta_x = delta_x;
    event_init.delta_y = delta_y;
    event_init.delta_mode = WheelDeltaMode::DOM_DELTA_PIXEL;
    event_init.view = window_proxy;
    auto event = WheelEvent::create(event_name, event_init, page.x().to_double(), page.y().to_double(), offset.x().to_double(), offset.y().to_double(), HighResolutionTime::current_high_resolution_time(relevant_global_object));
    event->set_is_trusted(true);
    event->set_bubbles(true);
    event->set_cancelable(is_cancelable == WheelEventIsCancelable::Yes);
    event->set_composed(true);
    return event;
}

}
