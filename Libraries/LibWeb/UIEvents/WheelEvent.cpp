/*
 * Copyright (c) 2022, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WheelEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/WheelEvent.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(WheelEvent);

WheelEvent::WheelEvent(FlyString const& event_name, WheelEventOptions const& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MouseEvent(event_name, options, page_x, page_y, offset_x, offset_y, time_stamp)
    , m_delta_x(options.delta_x)
    , m_delta_y(options.delta_y)
    , m_delta_z(options.delta_z)
    , m_delta_mode(options.delta_mode)
{
}

WheelEvent::~WheelEvent() = default;

GC::Ref<WheelEvent> WheelEvent::create(FlyString const& event_name, WheelEventOptions const& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<WheelEvent>(event_name, options, page_x, page_y, offset_x, offset_y, time_stamp);
}

GC::Ref<WheelEvent> WheelEvent::create_for_constructor(FlyString const& event_name, Bindings::WheelEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    WheelEventOptions options;
    static_cast<MouseEventOptions&>(options) = mouse_event_options_from_bindings(event_init);
    options.delta_x = event_init.delta_x;
    options.delta_y = event_init.delta_y;
    options.delta_z = event_init.delta_z;
    options.delta_mode = event_init.delta_mode;
    return create(event_name, options, event_init.client_x, event_init.client_y, event_init.client_x, event_init.client_y, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<WheelEvent>> WheelEvent::create_from_platform_event(JS::Object const& relevant_global_object, GC::Ptr<HTML::WindowProxy> window_proxy, FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, double delta_x, double delta_y, unsigned button, unsigned buttons, unsigned modifiers, WheelEventIsCancelable is_cancelable)
{
    WheelEventOptions event_options;
    event_options.ctrl_key = modifiers & Mod_Ctrl;
    event_options.shift_key = modifiers & Mod_Shift;
    event_options.alt_key = modifiers & Mod_Alt;
    event_options.meta_key = modifiers & Mod_Super;
    event_options.screen_x = screen.x().to_double();
    event_options.screen_y = screen.y().to_double();
    event_options.client_x = client.x().to_double();
    event_options.client_y = client.y().to_double();
    event_options.button = button;
    event_options.buttons = buttons;
    event_options.delta_x = delta_x;
    event_options.delta_y = delta_y;
    event_options.delta_mode = WheelDeltaMode::DOM_DELTA_PIXEL;
    event_options.view = window_proxy;
    auto event = WheelEvent::create(event_name, event_options, page.x().to_double(), page.y().to_double(), offset.x().to_double(), offset.y().to_double(), HighResolutionTime::current_high_resolution_time(relevant_global_object));
    event->set_is_trusted(true);
    event->set_bubbles(true);
    event->set_cancelable(is_cancelable == WheelEventIsCancelable::Yes);
    event->set_composed(true);
    return event;
}

}
