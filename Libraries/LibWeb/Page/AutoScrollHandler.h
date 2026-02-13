/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

class AutoScrollHandler {
public:
    AutoScrollHandler(HTML::Navigable&, DOM::Element& container);

    void visit_edges(JS::Cell::Visitor&) const;

    CSSPixelPoint process(CSSPixelPoint mouse_position);
    void perform_tick();

    bool is_active() const { return m_active; }

    static GC::Ptr<DOM::Element> find_scrollable_ancestor(Painting::Paintable const&);

private:
    void activate();
    void deactivate();

    GC::Ref<HTML::Navigable> m_navigable;
    GC::Ref<DOM::Element> m_container_element;
    CSSPixelPoint m_mouse_position;
    CSSPixelPoint m_fractional_delta;
    bool m_active { false };
};

}
