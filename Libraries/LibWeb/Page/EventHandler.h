/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibUnicode/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Gamepad/SDLGamepadForward.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/KeyCode.h>

namespace Web {

class WEB_API EventHandler {
public:
    explicit EventHandler(Badge<HTML::Navigable>, HTML::Navigable&);
    ~EventHandler();

    EventResult handle_mouseup(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousedown(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousemove(CSSPixelPoint, CSSPixelPoint screen_position, unsigned buttons, unsigned modifiers);
    EventResult handle_mouseleave();
    EventResult handle_mousewheel(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int wheel_delta_x, int wheel_delta_y);
    EventResult handle_doubleclick(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);

    EventResult handle_drag_and_drop_event(DragEvent::Type, CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files);

    EventResult handle_pinch_event(CSSPixelPoint, double scale_delta);

    EventResult handle_keydown(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    EventResult handle_keyup(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);

    void set_mouse_event_tracking_paintable(GC::Ptr<Painting::Paintable>);

    EventResult handle_paste(Utf16String const& text);

    void handle_sdl_input_events();

    void visit_edges(JS::Cell::Visitor& visitor) const;

    Unicode::Segmenter& word_segmenter();

private:
    EventResult focus_next_element();
    EventResult focus_previous_element();

    EventResult fire_keyboard_event(FlyString const& event_name, HTML::Navigable&, UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    [[nodiscard]] EventResult input_event(FlyString const& event_name, FlyString const& input_type, HTML::Navigable&, Variant<u32, Utf16String> code_point_or_string);
    CSSPixelPoint compute_mouse_event_client_offset(CSSPixelPoint event_page_position) const;
    CSSPixelPoint compute_mouse_event_page_offset(CSSPixelPoint event_client_offset) const;
    CSSPixelPoint compute_mouse_event_movement(CSSPixelPoint event_client_offset) const;

    struct Target {
        GC::Ptr<Painting::Paintable> paintable;
        Optional<int> index_in_node;
    };
    Optional<Target> target_for_mouse_position(CSSPixelPoint position);

    GC::Ptr<Painting::PaintableBox> paint_root();
    GC::Ptr<Painting::PaintableBox const> paint_root() const;

    bool should_ignore_device_input_event() const;

    void handle_gamepad_connected(SDL_JoystickID);
    void handle_gamepad_updated(SDL_JoystickID);
    void handle_gamepad_disconnected(SDL_JoystickID);

    GC::Ref<HTML::Navigable> m_navigable;

    bool m_in_mouse_selection { false };
    InputEventsTarget* m_mouse_selection_target { nullptr };

    GC::Ptr<Painting::Paintable> m_mouse_event_tracking_paintable;

    NonnullOwnPtr<DragAndDropEventHandler> m_drag_and_drop_event_handler;

    GC::Weak<DOM::EventTarget> m_mousedown_target;

    // Tracks the scrollable container that should receive keyboard scroll events.
    // Set when user clicks inside a scrollable element.
    GC::Ptr<DOM::Node> m_keyboard_scroll_container;
    // Parent scrollable container, used as fallback when primary is removed
    GC::Ptr<DOM::Node> m_keyboard_scroll_container_parent;

    Optional<CSSPixelPoint> m_mousemove_previous_screen_position;

    OwnPtr<Unicode::Segmenter> m_word_segmenter;
};

}
