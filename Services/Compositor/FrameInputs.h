/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <Compositor/CompositorCoreRustFFI.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>
#include <math.h>

namespace Compositor {

// A scrollbar as it is painted, somewhere between its thin and its expanded geometry.
struct PresentedScrollbar {
    Gfx::FloatRect gutter_rect;
    Gfx::FloatRect thumb_rect;
    Color thumb_color;
    Color thumb_outline_color;
    Color track_color;
    float opacity { 1 };
};

using ScrollbarOfFrame = RustFFI::ScrollbarOfFrame;

// A scroll offset outside the scroll range moves the thumb past an end of its track, where it is cut short instead.
inline Gfx::IntRect thumb_rect_kept_inside_track(Gfx::IntRect thumb_rect, Gfx::IntRect const& track_rect, bool vertical)
{
    if (vertical) {
        auto top = max(thumb_rect.top(), track_rect.top());
        auto bottom = min(thumb_rect.bottom(), track_rect.bottom());
        thumb_rect.set_y(top);
        thumb_rect.set_height(max(0, bottom - top));
    } else {
        auto left = max(thumb_rect.left(), track_rect.left());
        auto right = min(thumb_rect.right(), track_rect.right());
        thumb_rect.set_x(left);
        thumb_rect.set_width(max(0, right - left));
    }
    return thumb_rect;
}

inline PresentedScrollbar presented_scrollbar_with_colors(ScrollbarOfFrame const& scrollbar, Color thumb_color, Color thumb_outline_color, Color track_color)
{
    // The gutter of a thin scrollbar is not painted, so the gutter grows out of the thin track as it fades in.
    auto track_alpha = roundf(static_cast<float>(track_color.alpha()) * scrollbar.expansion);
    return PresentedScrollbar {
        .gutter_rect = scrollbar.gutter_rect,
        .thumb_rect = scrollbar.thumb_rect,
        .thumb_color = thumb_color,
        .thumb_outline_color = thumb_outline_color,
        .track_color = track_color.with_alpha(static_cast<u8>(track_alpha)),
        .opacity = scrollbar.opacity,
    };
}

// What a frame is painted with besides the display list and its resources.
struct FrameInputs {
    Compositing::AccumulatedVisualContextTree visual_context_tree;
    Compositing::ScrollStateSnapshot presented_scroll_state_snapshot;
    Vector<ScrollbarOfFrame> scrollbars;
    Optional<Compositing::SpatialNodeIndex> viewport_scroll_node_index;
};

}
