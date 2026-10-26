/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashFunctions.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollSnapSelection.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Color.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>
#include <LibWebCommon/Page/CompositorContextId.h>
#include <LibWebCommon/Page/InputRoutingDecision.h>

namespace Compositing {

using AsyncScrollOperationID = u64;

// Stable identifier for a scroll node in a document; the node index alone is not unique across nested documents.
struct AsyncScrollNodeID {
    Web::UniqueNodeID document_id;
    Compositing::SpatialNodeIndex scroll_node_index;

    bool operator==(AsyncScrollNodeID const&) const = default;
};

COMPOSITING_API Web::AsyncScrollNodeKind async_scroll_node_kind_for(Compositing::CompositorScrollNodeKind);

struct AsyncScrollOffset {
    Web::AsyncScrollNodeStableID stable_node_id;
    Gfx::FloatPoint compositor_scroll_offset;
    Gfx::FloatPoint unadopted_scroll_delta;
    // The most recent nonzero delta of a relative scroll in each axis, such as a wheel or panning scroll. Absolute
    // scrolls, such as dragging a scrollbar thumb, leave it as it is.
    Gfx::FloatPoint last_relative_scroll_delta;
    // The scale the visual viewport was zoomed to, which only its scroll node reports.
    Optional<float> visual_viewport_scale;
    // The last of the main thread's scroll requests for the box that the compositor applied before it reported this.
    u64 applied_scroll_request_id { 0 };
    // How many times the compositor had scrolled the box for user input of its own when it reported this.
    u64 user_input_scroll_count { 0 };

    void merge_later_scroll(AsyncScrollOffset const& later)
    {
        compositor_scroll_offset = later.compositor_scroll_offset;
        applied_scroll_request_id = later.applied_scroll_request_id;
        user_input_scroll_count = later.user_input_scroll_count;
        if (later.visual_viewport_scale.has_value())
            visual_viewport_scale = later.visual_viewport_scale;
        unadopted_scroll_delta.translate_by(later.unadopted_scroll_delta);
        if (later.last_relative_scroll_delta.x() != 0)
            last_relative_scroll_delta.set_x(later.last_relative_scroll_delta.x());
        if (later.last_relative_scroll_delta.y() != 0)
            last_relative_scroll_delta.set_y(later.last_relative_scroll_delta.y());
    }
};

// One scrollable area from the paint snapshot.
struct AsyncScrollNode {
    AsyncScrollNodeID node_id;
    Web::AsyncScrollNodeStableID stable_node_id;
    Optional<AsyncScrollNodeID> parent_node_id;
    Gfx::IntRect scrollport_rect;
    Gfx::FloatPoint min_scroll_offset;
    Gfx::FloatPoint max_scroll_offset;
    bool is_viewport { false };
    bool can_be_wheel_scrolled_horizontally { false };
    bool can_be_wheel_scrolled_vertically { false };
    // Whether a scroll the scroller cannot scroll by is passed on to the scrollers around it.
    bool chains_scroll_to_ancestors_horizontally { true };
    bool chains_scroll_to_ancestors_vertically { true };
    // Whether a touchpad may pull the scroller past the edges of its scroll range.
    bool shows_overscroll_effect_horizontally { true };
    bool shows_overscroll_effect_vertically { true };
};

// A region with a non-passive wheel listener. Wheels inside it must stay on the main thread because script may cancel.
struct BlockingWheelEventRegion {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
};

struct WheelHitTestTarget {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    Optional<AsyncScrollNodeID> target_node_id;
    // Position among the wheel hit test targets and scrollbars of the display list, which are recorded in paint order.
    u32 paint_order_index { 0 };
};

// A region that must always use main-thread wheel routing even without a blocking listener, such as a nested navigable.
struct MainThreadWheelEventRegion {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
};

// Where a nested context's content is painted. A wheel inside it that no wheel hit test target painted later covers
// can be handed to the nested context.
struct NestedContextWheelRegion {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    Web::CompositorContextId child_context_id;
    u32 paint_order_index { 0 };
};

// A scroll container without a scroll node of its own that keeps the scrolls along an axis to itself. A wheel inside
// its rect is not passed on along that axis to the scroll node above it.
struct ScrollChainingBarrier {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
    AsyncScrollNodeID scroll_node_id_above_barrier;
    bool keeps_horizontal_scrolls { false };
    bool keeps_vertical_scrolls { false };
};

struct AsyncScrollbar {
    AsyncScrollNodeID scroll_node_id;
    Optional<Web::AsyncScrollNodeStableID> scroller_stable_node_id;
    Compositing::SpatialNodeIndex scroll_node_index;
    // The rects of a scrollbar the display list paints are in the space of this context.
    Compositing::ContextRef context;
    u32 paint_order_index { 0 };
    Gfx::IntRect gutter_rect;
    Gfx::IntRect thumb_rect;
    Gfx::IntRect track_rect;
    Gfx::IntRect expanded_gutter_rect;
    Gfx::IntRect expanded_thumb_rect;
    double scroll_size { 0 };
    double expanded_scroll_size { 0 };
    float min_scroll_offset { 0 };
    float max_scroll_offset { 0 };
    Color thumb_color;
    Color track_color;
    bool vertical { false };
    // The compositor paints the viewport's scrollbars itself and owns their hover expansion. The display list paints
    // every other scrollbar, in whichever of the two geometries the main thread currently gives it.
    bool is_painted_by_compositor { false };
    bool display_list_paints_enlarged_scrollbar { false };
};

// A scroll node that is a snap container, with the geometry snap positions are selected from.
struct AsyncSnapContainer {
    AsyncScrollNodeID node_id;
    SnapContainerGeometry geometry;
    Vector<SnapAreaGeometry> areas;
};

struct AsyncScrollingState {
    Optional<Web::UniqueNodeID> document_id;
    Vector<AsyncScrollNode> scroll_nodes;
    Vector<AsyncSnapContainer> snap_containers;
    Vector<WheelHitTestTarget> wheel_hit_test_targets;
    Vector<MainThreadWheelEventRegion> main_thread_wheel_event_regions;
    Vector<NestedContextWheelRegion> nested_context_wheel_regions;
    Vector<ScrollChainingBarrier> scroll_chaining_barriers;
    Vector<AsyncScrollbar> scrollbars;

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
    bool viewport_keeps_horizontal_scrolls { false };
    bool viewport_keeps_vertical_scrolls { false };
    double device_pixels_per_css_pixel { 1.0 };
};

enum class WheelRoutingAdmission {
    Accepted,
    NoAsyncScrollingState,
    BlockingWheelEventListeners,
    NoScrollNode,
    StaleWheelEventListeners,
};

COMPOSITING_API AsyncScrollingState async_scrolling_state_from_display_list(Compositing::DisplayList const&);
// The scrolling state as tests read it, from WebContent and from the compositor alike.
COMPOSITING_API JsonObject async_scrolling_state_to_json(AsyncScrollingState const&);
COMPOSITING_API WheelRoutingAdmission wheel_routing_admission_for(AsyncScrollingState const&);
// The admission of a scrolling state recorded before the newest change to the page's blocking wheel event listeners is
// stale, as the state may not have the regions that change made.
COMPOSITING_API WheelRoutingAdmission wheel_routing_admission_for(AsyncScrollingState const&, u64 newest_wheel_event_listener_state_generation);
// Why a wheel step that the admission does not let the compositor scroll is left to the main thread.
COMPOSITING_API Web::InputRoutingDecision input_routing_decision_for_wheel_step_not_admitted_by(WheelRoutingAdmission);

}
