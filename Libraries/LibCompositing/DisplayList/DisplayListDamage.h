/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Rect.h>

namespace Compositing {

class AccumulatedVisualContextTree;
class ScrollStateSnapshot;

// The canvases, video sinks and image frames whose pixels changed since the old list was rasterized. The lists
// draw them with commands that stay the same, so comparing the lists cannot see the change.
struct LiveContentChanges {
    ReadonlySpan<CanvasId> canvases;
    ReadonlySpan<VideoSinkResourceId> video_sinks;
    ReadonlySpan<ImageFrameResourceId> image_frames;
    // What the compositor repaints whatever the lists hold, such as a blinking caret.
    Gfx::IntRect forced_damage_rect;
};

COMPOSITING_API Optional<Gfx::IntRect> compute_display_list_damage(
    DisplayList const& old_display_list,
    AccumulatedVisualContextTree const& old_visual_context_tree,
    ScrollStateSnapshot const& old_scroll_state,
    DisplayList const& new_display_list,
    AccumulatedVisualContextTree const& new_visual_context_tree,
    ScrollStateSnapshot const& new_scroll_state,
    Gfx::IntRect viewport_rect,
    LiveContentChanges);

}
