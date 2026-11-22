/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web {

class MiddleButtonScrollHandler {
public:
    AK_ALLOC_WITH_KMALLOC;

    MiddleButtonScrollHandler(DOM::Element& container, CSSPixelPoint origin, CSSPixelPoint screen_origin);
    ~MiddleButtonScrollHandler();

    void visit_edges(JS::Cell::Visitor&) const;

    static GC::Ptr<DOM::Element> find_scrollable_ancestor(DOM::Document&, Layout::Node&);

    void update_mouse_screen_position(CSSPixelPoint screen_position);
    void perform_tick(double frame_timestamp);
    void request_indicator_repaint();

    CSSPixelPoint origin() const { return m_origin; }
    bool mouse_has_moved_beyond_dead_zone() const { return m_mouse_has_moved_beyond_dead_zone; }

private:
    GC::Ref<DOM::Element> m_container_element;
    CSSPixelPoint m_origin;

    // The speed follows the pointer's distance from where the scroll started, measured on the screen: Wherever the
    // pointer moves (over a nested document, e.g.), its screen position is known.
    CSSPixelPoint m_screen_origin;
    CSSPixelPoint m_mouse_screen_position;
    Optional<double> m_last_frame_time;
    CSSPixelPoint m_fractional_delta;
    OwnPtr<HTML::UserScrollGestureHold> m_scroll_gesture_hold;
    bool m_mouse_has_moved_beyond_dead_zone { false };
};

}
