/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/MouseEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(MouseEvent);

MouseEvent::MouseEvent(FlyString const& event_name, MouseEventOptions const& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : UIEvent(event_name, time_stamp)
    , m_screen_x(options.screen_x)
    , m_screen_y(options.screen_y)
    , m_page_x(page_x)
    , m_page_y(page_y)
    , m_client_x(options.client_x)
    , m_client_y(options.client_y)
    , m_offset_x(offset_x)
    , m_offset_y(offset_y)
    , m_ctrl_key(options.ctrl_key)
    , m_shift_key(options.shift_key)
    , m_alt_key(options.alt_key)
    , m_meta_key(options.meta_key)
    , m_modifier_alt_graph(options.modifier_alt_graph)
    , m_modifier_caps_lock(options.modifier_caps_lock)
    , m_modifier_fn(options.modifier_fn)
    , m_modifier_fn_lock(options.modifier_fn_lock)
    , m_modifier_hyper(options.modifier_hyper)
    , m_modifier_num_lock(options.modifier_num_lock)
    , m_modifier_scroll_lock(options.modifier_scroll_lock)
    , m_modifier_super(options.modifier_super)
    , m_modifier_symbol(options.modifier_symbol)
    , m_modifier_symbol_lock(options.modifier_symbol_lock)
    , m_movement_x(options.movement_x)
    , m_movement_y(options.movement_y)
    , m_button(options.button)
    , m_buttons(options.buttons)
    , m_related_target(options.related_target)
{
    set_bubbles(options.bubbles);
    set_cancelable(options.cancelable);
    set_composed(options.composed);
    m_view = options.view;
    m_detail = options.detail;
}

MouseEvent::~MouseEvent() = default;

void MouseEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_related_target);
}

bool MouseEvent::get_modifier_state(String const& key_arg) const
{
    if (key_arg == "Control")
        return m_ctrl_key;
    if (key_arg == "Shift")
        return m_shift_key;
    if (key_arg == "Alt")
        return m_alt_key;
    if (key_arg == "Meta")
        return m_meta_key;
    if (key_arg == "AltGraph")
        return m_modifier_alt_graph;
    if (key_arg == "CapsLock")
        return m_modifier_caps_lock;
    if (key_arg == "Fn")
        return m_modifier_fn;
    if (key_arg == "FnLock")
        return m_modifier_fn_lock;
    if (key_arg == "Hyper")
        return m_modifier_hyper;
    if (key_arg == "NumLock")
        return m_modifier_num_lock;
    if (key_arg == "ScrollLock")
        return m_modifier_scroll_lock;
    if (key_arg == "Super")
        return m_modifier_super;
    if (key_arg == "Symbol")
        return m_modifier_symbol;
    if (key_arg == "SymbolLock")
        return m_modifier_symbol_lock;
    return false;
}

// https://w3c.github.io/uievents/#dom-mouseevent-initmouseevent
void MouseEvent::init_mouse_event(String const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, WebIDL::Long detail, WebIDL::Long screen_x, WebIDL::Long screen_y, WebIDL::Long client_x, WebIDL::Long client_y, bool ctrl_key, bool alt_key, bool shift_key, bool meta_key, WebIDL::Short button, DOM::EventTarget* related_target)
{
    // Initializes attributes of a MouseEvent object. This method has the same behavior as UIEvent.initUIEvent().

    // 1. If this’s dispatch flag is set, then return.
    if (dispatched())
        return;

    // 2. Initialize this with type, bubbles, and cancelable.
    initialize_event(type, bubbles, cancelable);

    // Implementation Defined: Initialise other values.
    m_view = view;
    m_detail = detail;
    m_screen_x = screen_x;
    m_screen_y = screen_y;
    m_client_x = client_x;
    m_client_y = client_y;
    m_ctrl_key = ctrl_key;
    m_shift_key = shift_key;
    m_alt_key = alt_key;
    m_meta_key = meta_key;
    m_button = button;
    m_related_target = related_target;
}

GC::Ref<MouseEvent> MouseEvent::create(
    FlyString const& event_name, MouseEventOptions const& options,
    double page_x, double page_y, double offset_x, double offset_y,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MouseEvent>(event_name, options, page_x, page_y, offset_x, offset_y, time_stamp);
}

MouseEventOptions mouse_event_options_from_bindings(Bindings::MouseEventInit const& event_init)
{
    MouseEventOptions options;
    options.bubbles = event_init.bubbles;
    options.cancelable = event_init.cancelable;
    options.composed = event_init.composed;
    options.view = event_init.view;
    options.detail = event_init.detail;
    options.ctrl_key = event_init.ctrl_key;
    options.shift_key = event_init.shift_key;
    options.alt_key = event_init.alt_key;
    options.meta_key = event_init.meta_key;
    options.modifier_alt_graph = event_init.modifier_alt_graph;
    options.modifier_caps_lock = event_init.modifier_caps_lock;
    options.modifier_fn = event_init.modifier_fn;
    options.modifier_fn_lock = event_init.modifier_fn_lock;
    options.modifier_hyper = event_init.modifier_hyper;
    options.modifier_num_lock = event_init.modifier_num_lock;
    options.modifier_scroll_lock = event_init.modifier_scroll_lock;
    options.modifier_super = event_init.modifier_super;
    options.modifier_symbol = event_init.modifier_symbol;
    options.modifier_symbol_lock = event_init.modifier_symbol_lock;
    options.screen_x = event_init.screen_x;
    options.screen_y = event_init.screen_y;
    options.client_x = event_init.client_x;
    options.client_y = event_init.client_y;
    options.movement_x = event_init.movement_x;
    options.movement_y = event_init.movement_y;
    options.button = event_init.button;
    options.buttons = event_init.buttons;
    options.related_target = event_init.related_target;
    return options;
}

WebIDL::ExceptionOr<GC::Ref<MouseEvent>> MouseEvent::create_for_constructor(FlyString const& event_name, Bindings::MouseEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    // https://drafts.csswg.org/cssom-view/#dom-mouseevent-pagex
    // For a newly constructed event, pageX/pageY default to clientX/clientY (scrollX/scrollY are 0).
    // https://drafts.csswg.org/cssom-view/#dom-mouseevent-offsetx
    // For a newly constructed event with no target, offsetX/offsetY default to clientX/clientY.
    return create(event_name, mouse_event_options_from_bindings(event_init), event_init.client_x, event_init.client_y, event_init.client_x, event_init.client_y, time_stamp);
}

GC::Ref<MouseEvent> MouseEvent::create_from_mouse_event(MouseEvent const& mouse_event, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    MouseEventOptions options;
    options.screen_x = mouse_event.m_screen_x;
    options.screen_y = mouse_event.m_screen_y;
    options.client_x = mouse_event.m_client_x;
    options.client_y = mouse_event.m_client_y;
    options.movement_x = mouse_event.m_movement_x;
    options.movement_y = mouse_event.m_movement_y;
    options.button = mouse_event.m_button;
    options.buttons = mouse_event.m_buttons;
    options.related_target = mouse_event.m_related_target;
    options.ctrl_key = mouse_event.m_ctrl_key;
    options.shift_key = mouse_event.m_shift_key;
    options.alt_key = mouse_event.m_alt_key;
    options.meta_key = mouse_event.m_meta_key;
    options.modifier_alt_graph = mouse_event.m_modifier_alt_graph;
    options.modifier_caps_lock = mouse_event.m_modifier_caps_lock;
    options.modifier_fn = mouse_event.m_modifier_fn;
    options.modifier_fn_lock = mouse_event.m_modifier_fn_lock;
    options.modifier_hyper = mouse_event.m_modifier_hyper;
    options.modifier_num_lock = mouse_event.m_modifier_num_lock;
    options.modifier_scroll_lock = mouse_event.m_modifier_scroll_lock;
    options.modifier_super = mouse_event.m_modifier_super;
    options.modifier_symbol = mouse_event.m_modifier_symbol;
    options.modifier_symbol_lock = mouse_event.m_modifier_symbol_lock;
    options.view = mouse_event.view();
    options.detail = mouse_event.detail();
    return create(mouse_event.type(), options, mouse_event.m_page_x, mouse_event.m_page_y, mouse_event.m_offset_x, mouse_event.m_offset_y, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<MouseEvent>> MouseEvent::create_from_platform_event(JS::Object const& relevant_global_object, GC::Ptr<HTML::WindowProxy> window_proxy, FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, Optional<CSSPixelPoint> movement, unsigned button, unsigned buttons, unsigned modifiers, int detail)
{
    MouseEventOptions event_options;
    event_options.detail = detail;
    event_options.ctrl_key = modifiers & Mod_Ctrl;
    event_options.shift_key = modifiers & Mod_Shift;
    event_options.alt_key = modifiers & Mod_Alt;
    event_options.meta_key = modifiers & Mod_Super;
    event_options.screen_x = screen.x().to_double();
    event_options.screen_y = screen.y().to_double();
    event_options.client_x = client.x().to_double();
    event_options.client_y = client.y().to_double();
    event_options.view = window_proxy;
    if (movement.has_value()) {
        event_options.movement_x = movement.value().x().to_double();
        event_options.movement_y = movement.value().y().to_double();
    }
    event_options.button = mouse_button_to_button_code(static_cast<MouseButton>(button));
    event_options.buttons = buttons;
    auto event = MouseEvent::create(event_name, event_options, page.x().to_double(), page.y().to_double(), offset.x().to_double(), offset.y().to_double(), HighResolutionTime::current_high_resolution_time(relevant_global_object));
    event->set_is_trusted(true);
    event->set_bubbles(true);
    event->set_cancelable(true);
    event->set_composed(true);
    return event;
}

}
