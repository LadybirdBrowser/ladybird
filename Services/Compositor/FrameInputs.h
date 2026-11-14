/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <Compositor/CompositorCoreRustFFI.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>
#include <math.h>

namespace Compositor {

using PresentedScrollbar = Compositing::RustFFI::PresentedScrollbar;

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

    // Replays the list through a painter as the frame shows it.
    void replay(Compositing::DisplayList const&, Compositing::RustFFI::FfiRasterPainter const&, ReplaySelection const& = replaying_everything()) const;
    // The scrollbars the compositor paints over the page, as the frame shows them.
    Vector<PresentedScrollbar> scrollbars_painted_by_compositor() const;

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
