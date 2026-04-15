/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

class MiddleButtonScrollHandler {
public:
    MiddleButtonScrollHandler(DOM::Element& container, CSSPixelPoint origin);
    ~MiddleButtonScrollHandler();

    void visit_edges(JS::Cell::Visitor&) const;

    static GC::Ptr<DOM::Element> find_scrollable_ancestor(DOM::Document&, Painting::Paintable&);

    void update_mouse_position(CSSPixelPoint position) { m_mouse_position = position; }
    void perform_tick();

    CSSPixelPoint origin() const { return m_origin; }
    bool mouse_has_moved_beyond_dead_zone() const { return m_mouse_has_moved_beyond_dead_zone; }

private:
    GC::Ref<DOM::Element> m_container_element;
    CSSPixelPoint m_origin;
    CSSPixelPoint m_mouse_position;
    CSSPixelPoint m_fractional_delta;
    bool m_mouse_has_moved_beyond_dead_zone { false };
};

}
