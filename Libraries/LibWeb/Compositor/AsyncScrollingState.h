/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Compositor {

// Stable identifier for a scroll frame in a document; the frame index alone is not unique across nested documents.
struct AsyncScrollNodeID {
    UniqueNodeID document_id;
    Painting::ScrollFrameIndex scroll_frame_index;

    bool operator==(AsyncScrollNodeID const&) const = default;
};

enum class AsyncScrollNodeKind : u8 {
    Viewport,
    Element,
    PseudoElement,
};

// Stable identity for reconciling compositor-side scroll offsets after the paint snapshot has been rebuilt.
struct AsyncScrollNodeStableID {
    UniqueNodeID node_id;
    AsyncScrollNodeKind kind { AsyncScrollNodeKind::Element };
    u8 pseudo_element_type { 0 };

    bool operator==(AsyncScrollNodeStableID const&) const = default;
};

struct AsyncScrollOffset {
    AsyncScrollNodeStableID stable_node_id;
    Gfx::FloatPoint scroll_offset;
};

// One scrollable area from the paint snapshot. Non-viewport scrollports are stored in hit_test_visual_context_index
// coordinates and transformed to viewport coordinates when the compositor rebuilds wheel targets.
struct AsyncScrollNode {
    AsyncScrollNodeID node_id;
    AsyncScrollNodeStableID stable_node_id;
    Optional<AsyncScrollNodeID> parent_node_id;
    Painting::VisualContextIndex hit_test_visual_context_index;
    Gfx::IntRect scrollport_rect;
    Gfx::FloatPoint max_scroll_offset;
    bool is_viewport { false };
};

// Sticky elements are represented as scroll frames whose offset is derived from ancestor scroll offsets. Keep only
// the precomputed geometry needed to replay that calculation on the compositor thread after an async scroll mutation.
struct AsyncStickyArea {
    UniqueNodeID document_id;
    Painting::ScrollFrameIndex scroll_frame_index;
    Painting::ScrollFrameIndex parent_scroll_frame_index;
    Painting::ScrollFrameIndex nearest_scrolling_ancestor_index;
    Gfx::FloatPoint position_relative_to_scroll_ancestor;
    Gfx::FloatSize border_box_size;
    Gfx::FloatSize scrollport_size;
    Gfx::FloatRect containing_block_region;
    bool needs_parent_offset_adjustment { false };
    Optional<float> inset_top;
    Optional<float> inset_right;
    Optional<float> inset_bottom;
    Optional<float> inset_left;
};

// A region with a non-passive wheel listener. Wheels inside it must stay on the main thread because script may cancel.
struct BlockingWheelEventRegion {
    Painting::VisualContextIndex visual_context_index;
    Gfx::FloatRect rect;
};

struct WheelHitTestTarget {
    Painting::VisualContextIndex visual_context_index;
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    Optional<AsyncScrollNodeID> target_node_id;
};

// A region that must always use main-thread wheel routing even without a blocking listener, such as a nested navigable.
struct MainThreadWheelEventRegion {
    Painting::VisualContextIndex visual_context_index;
    Gfx::FloatRect rect;
};

struct ViewportScrollbar {
    AsyncScrollNodeID scroll_node_id;
    Painting::ScrollFrameIndex scroll_frame_index;
    Gfx::IntRect gutter_rect;
    Gfx::IntRect thumb_rect;
    Gfx::IntRect expanded_gutter_rect;
    Gfx::IntRect expanded_thumb_rect;
    double scroll_size { 0 };
    double expanded_scroll_size { 0 };
    float max_scroll_offset { 0 };
    Color thumb_color;
    Color track_color;
    bool vertical { false };
};

struct AsyncScrollingState {
    Vector<AsyncScrollNode> scroll_nodes;
    Vector<AsyncStickyArea> sticky_areas;
    Vector<WheelHitTestTarget> wheel_hit_test_targets;
    Vector<MainThreadWheelEventRegion> main_thread_wheel_event_regions;
    Vector<ViewportScrollbar> viewport_scrollbars;

    // Non-passive wheel listeners can cancel scrolling, so async scrolling must treat them as hard barriers.
    // Viewport-wide barriers cover listeners on the root targets; element regions let input hit-testing accept
    // async scrolling elsewhere.
    Vector<BlockingWheelEventRegion> blocking_wheel_event_regions;
    Gfx::IntRect viewport_rect;

    // Bumped whenever wheel listener state changes so queued compositor snapshots
    // cannot re-enable async wheel routing after a non-passive listener has been
    // added.
    u64 wheel_event_listener_state_generation { 0 };
    bool has_blocking_wheel_event_listeners { false };
    bool has_blocking_wheel_event_region_covering_viewport { false };
};

enum class WheelRoutingAdmission {
    Accepted,
    NoAsyncScrollingState,
    BlockingWheelEventListeners,
    NoScrollNode,
    StaleWheelEventListeners,
};

enum class WheelScrollAdmission {
    Accepted,
    NoScrollableTarget,
    BlockedByMainThreadRegion,
    StaleBlockingWheelEventRegions,
    BlockedByWheelEventRegion,
};

void initialize_async_scrolling_metadata_recording(DisplayListRecordingContext&, Painting::ViewportPaintable&);
void record_async_scrolling_metadata_for_paintable(Painting::PaintableBox const&, DisplayListRecordingContext&);
void finalize_async_scrolling_metadata_recording(DisplayListRecordingContext&, HTML::Navigable&, Gfx::IntRect viewport_rect);
AsyncScrollingState async_scrolling_state_from_display_list(Painting::DisplayList const&);
WheelRoutingAdmission wheel_routing_admission_for(AsyncScrollingState const&);
StringView wheel_routing_admission_to_string(WheelRoutingAdmission);
bool blocks_wheel_event_at_position(AsyncScrollingState const&, RefPtr<Painting::DisplayList> const&, Painting::ScrollStateSnapshot const&, Gfx::FloatPoint position);
WheelScrollAdmission admit_wheel_scroll(AsyncScrollingState const&, RefPtr<Painting::DisplayList> const&, Painting::ScrollStateSnapshot const&, Gfx::FloatPoint position, Gfx::FloatPoint delta, bool blocking_wheel_event_regions_are_current);

}
