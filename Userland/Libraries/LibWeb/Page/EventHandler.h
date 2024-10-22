/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/WeakPtr.h>
#include <LibGfx/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibUnicode/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/KeyCode.h>

namespace Web {

class EventHandler {
public:
    explicit EventHandler(Badge<HTML::Navigable>, HTML::Navigable&);
    ~EventHandler();

    EventResult handle_mouseup(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousedown(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousemove(CSSPixelPoint, CSSPixelPoint screen_position, unsigned buttons, unsigned modifiers);
    EventResult handle_mousewheel(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int wheel_delta_x, int wheel_delta_y);
    EventResult handle_doubleclick(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);

    EventResult handle_drag_and_drop_event(DragEvent::Type, CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files);

    EventResult handle_keydown(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    EventResult handle_keyup(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);

    void set_mouse_event_tracking_paintable(Painting::Paintable*);

    void handle_paste(String const& text);

    void visit_edges(JS::Cell::Visitor& visitor) const;

    Unicode::Segmenter& word_segmenter();

private:
    bool focus_next_element();
    bool focus_previous_element();

    EventResult fire_keyboard_event(FlyString const& event_name, HTML::Navigable&, UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    [[nodiscard]] EventResult input_event(FlyString const& event_name, FlyString const& input_type, HTML::Navigable&, u32 code_point);
    CSSPixelPoint compute_mouse_event_client_offset(CSSPixelPoint event_page_position) const;
    CSSPixelPoint compute_mouse_event_page_offset(CSSPixelPoint event_client_offset) const;
    CSSPixelPoint compute_mouse_event_movement(CSSPixelPoint event_client_offset) const;

    struct Target {
        JS::GCPtr<Painting::Paintable> paintable;
        Optional<int> index_in_node;
    };
    Optional<Target> target_for_mouse_position(CSSPixelPoint position);

    Painting::PaintableBox* paint_root();
    Painting::PaintableBox const* paint_root() const;

    bool should_ignore_device_input_event() const;
    void update_selection_range_for_input_or_textarea();

    JS::NonnullGCPtr<HTML::Navigable> m_navigable;

    bool m_in_mouse_selection { false };

    JS::GCPtr<Painting::Paintable> m_mouse_event_tracking_paintable;

    NonnullOwnPtr<EditEventHandler> m_edit_event_handler;
    NonnullOwnPtr<DragAndDropEventHandler> m_drag_and_drop_event_handler;

    WeakPtr<DOM::EventTarget> m_mousedown_target;

    Optional<CSSPixelPoint> m_mousemove_previous_screen_position;

    OwnPtr<Unicode::Segmenter> m_word_segmenter;
};

}
