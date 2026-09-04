/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TextAffinity.h>

namespace Web::Painting {

enum class ScrollDirection : u8 {
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
CSS::Overflow overflow_value_applied_to_viewport_for_wheel_scrolling(DOM::Document const&, ScrollDirection);
struct WheelScrollableAxes {
    bool horizontal { false };
    bool vertical { false };
};

WheelScrollableAxes wheel_scrollable_axes(Layout::Node const&);
bool could_be_scrolled_by_wheel_event(Layout::Node const&);
bool could_be_scrolled_by_wheel_event(Layout::Node const&, ScrollDirection);
WEB_API Optional<Compositor::AsyncScrollNodeStableID> async_scroll_node_stable_id(Layout::Node const&);
ScrollHandled set_scroll_offset(Layout::Node&, CSSPixelPoint);
ScrollHandled set_scroll_offset_from_user_input(Layout::Node&, CSSPixelPoint);
ScrollHandled scroll_by(Layout::Node&, double delta_x, double delta_y);
ScrollHandled wheel_scroll_along_containing_block_chain(Layout::Node&, double wheel_delta_x, double wheel_delta_y);

WEB_API Layout::Node* scrolling_box_for_scroll_step_in_containing_block_chain(Layout::Node&, CSSPixelPoint delta);
WEB_API Layout::Node* first_wheel_scrollable_box_in_containing_block_chain(Layout::Node const&);
void scroll_text_offset_into_view(DOM::Text const&, size_t offset, TextAffinity = TextAffinity::Downstream, ScrollBlockDirection = ScrollBlockDirection::Yes);

}
