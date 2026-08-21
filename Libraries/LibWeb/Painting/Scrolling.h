/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TextAffinity.h>

namespace Web::Painting {

enum class ScrollDirection {
    Horizontal,
    Vertical,
};

enum class ScrollHandled {
    No,
    Yes,
};

enum class ScrollBlockDirection {
    No,
    Yes,
};

CSSPixelPoint scroll_offset(Layout::Node const&);
CSSPixelPoint minimum_scroll_offset(Layout::Node const&);
CSSPixelPoint maximum_scroll_offset(Layout::Node const&);
CSSPixelPoint clamp_scroll_offset(Layout::Node const&, CSSPixelPoint);
CSSPixelRect scroll_snapport_rect(Layout::Node const&);
CSSPixelRect scroll_snapport_rect(Layout::Node const&, CSSPixelRect scrollport);
bool could_be_scrolled_by_wheel_event(Layout::Node const&);
bool could_be_scrolled_by_wheel_event(Layout::Node const&, ScrollDirection);
RefPtr<Paintable const> nearest_scrollable_ancestor(Layout::Node const&);
ScrollHandled set_scroll_offset(Layout::Node&, CSSPixelPoint);
ScrollHandled set_scroll_offset_from_user_input(Layout::Node&, CSSPixelPoint);
ScrollHandled scroll_by(Layout::Node&, double delta_x, double delta_y);
void scroll_text_offset_into_view(DOM::Text const&, size_t offset, TextAffinity = TextAffinity::Downstream, ScrollBlockDirection = ScrollBlockDirection::Yes);

}
