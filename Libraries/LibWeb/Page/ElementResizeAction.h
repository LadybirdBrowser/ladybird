/*
 * Copyright (c) 2025, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

// https://drafts.csswg.org/css-ui#resize

namespace Web {

class ElementResizeAction {
public:
    ElementResizeAction(GC::Ref<DOM::Element> element, CSSPixelPoint pointer_down_origin);

    void handle_pointer_move(CSSPixelPoint pointer_position);

    void visit_edges(GC::Cell::Visitor&) const;

private:
    GC::Ref<DOM::Element> m_element;
    CSSPixelPoint m_pointer_down_origin;
    CSSPixelSize m_initial_border_box_size;
};

}
