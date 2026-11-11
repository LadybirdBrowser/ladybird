/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <Compositor/CompositorCoreRustFFI.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
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
using ReplaySelection = Compositing::RustFFI::FfiReplaySelection;

constexpr ReplaySelection replaying_everything()
{
    return { nullptr, 0, false, false };
}

inline ReplaySelection replaying_only(ReadonlySpan<Compositing::RustFFI::RecordSpan> record_spans, bool skip_root_isolation_layer = false)
{
    return { record_spans.data(), record_spans.size(), true, skip_root_isolation_layer };
}

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

// The canvases, video sinks and image frames whose pixels changed since a display list was rasterized. The list draws
// them with commands that stay the same, so comparing lists cannot see the change.
struct LiveContentChanges {
    ReadonlySpan<Compositing::CanvasId> canvases;
    ReadonlySpan<Compositing::VideoSinkResourceId> video_sinks;
    ReadonlySpan<Compositing::ImageFrameResourceId> image_frames;
    // What the compositor repaints whatever the lists hold, such as a blinking caret.
    Gfx::IntRect forced_damage_rect;
    ReadonlySpan<u32> offsets_of_records_composited_as_media_layers;
};

struct DisplayListDamage {
    // Empty where the damage has no bounds.
    Optional<Gfx::IntRect> rect;
    // With the frames' layer plans, the damage of each layer, empty where that has no bounds.
    Vector<Optional<Gfx::IntRect>> layer_rects;
};

// What a frame is painted with besides the display list and its resources, which the scroll and input logic holds:
// where the frame presents everything that scrolls is known to it alone.
class FrameInputs {
    AK_MAKE_NONCOPYABLE(FrameInputs);

public:
    static FrameInputs adopt(RustFFI::FrameInputs*);
    FrameInputs(FrameInputs&&);
    FrameInputs& operator=(FrameInputs&&);
    ~FrameInputs();

    Compositing::AccumulatedVisualContextTree const& visual_context_tree() const { return m_visual_context_tree; }
    ReadonlySpan<ScrollbarOfFrame> scrollbars() const;

    // Replays the list through a player's callbacks as the frame shows it.
    void replay(Compositing::DisplayList const&, Compositing::RustFFI::FfiDisplayListReplayCallbacks const&, ReplaySelection const& = replaying_everything()) const;
    // Replays a stream of records that a command of the list being replayed nests.
    void replay_records(ReadonlyBytes records, Compositing::RustFFI::FfiDisplayListReplayCallbacks const&) const;

    RustFFI::ContextLayerPlan* build_layer_plan(Compositing::DisplayList const&, Gfx::IntRect viewport_rect, ReadonlySpan<RustFFI::FfiLiveMedia> active_media) const;
    bool places_content_like(FrameInputs const&) const;

    // What painting the list with these inputs repaints of the last frame, which painted its list with its inputs.
    // Given the layer plans of both frames, which must have the same layers, also what it repaints of each layer.
    DisplayListDamage damage_since(Compositing::DisplayList const&, FrameInputs const& last_frame_inputs, Compositing::DisplayList const& last_display_list, Gfx::IntRect viewport_rect, LiveContentChanges, RustFFI::ContextLayerPlan const* last_layer_plan = nullptr, RustFFI::ContextLayerPlan const* layer_plan = nullptr) const;

private:
    FrameInputs(RustFFI::FrameInputs*, Compositing::AccumulatedVisualContextTree);

    RustFFI::FrameInputs* m_frame_inputs { nullptr };
    Compositing::AccumulatedVisualContextTree m_visual_context_tree;
};

}
