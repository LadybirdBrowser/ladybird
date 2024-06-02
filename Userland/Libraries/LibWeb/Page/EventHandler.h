/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/WeakPtr.h>
#include <Kernel/API/KeyCode.h>
#include <LibGfx/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/EditEventHandler.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

class EventHandler {
public:
    explicit EventHandler(Badge<HTML::Navigable>, HTML::Navigable&);
    ~EventHandler();

    bool handle_mouseup(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    bool handle_mousedown(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    bool handle_mousemove(CSSPixelPoint, CSSPixelPoint screen_position, unsigned buttons, unsigned modifiers);
    bool handle_mousewheel(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int wheel_delta_x, int wheel_delta_y);
    bool handle_doubleclick(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);

    bool handle_keydown(KeyCode, unsigned modifiers, u32 code_point);
    bool handle_keyup(KeyCode, unsigned modifiers, u32 code_point);

    void set_mouse_event_tracking_paintable(Painting::Paintable*);

    void set_edit_event_handler(NonnullOwnPtr<EditEventHandler> value) { m_edit_event_handler = move(value); }

    void handle_paste(String const& text);

    void visit_edges(JS::Cell::Visitor& visitor) const;

private:
    bool focus_next_element();
    bool focus_previous_element();

    bool fire_keyboard_event(FlyString const& event_name, HTML::Navigable&, KeyCode, unsigned modifiers, u32 code_point);
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

    JS::NonnullGCPtr<HTML::Navigable> m_navigable;

    bool m_in_mouse_selection { false };

    JS::GCPtr<Painting::Paintable> m_mouse_event_tracking_paintable;

    NonnullOwnPtr<EditEventHandler> m_edit_event_handler;

    WeakPtr<DOM::EventTarget> m_mousedown_target;

    Optional<CSSPixelPoint> m_mousemove_previous_screen_position;
};

}
