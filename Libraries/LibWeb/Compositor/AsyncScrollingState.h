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
#include <LibGfx/Color.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Compositor/ScrollSnapSelection.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Compositor {

using AsyncScrollOperationID = u64;

// Stable identifier for a scroll node in a document; the node index alone is not unique across nested documents.
struct AsyncScrollNodeID {
    UniqueNodeID document_id;
    Painting::SpatialNodeIndex scroll_node_index;

    bool operator==(AsyncScrollNodeID const&) const = default;
};

enum class AsyncScrollNodeKind : u8 {
    Viewport,
    Element,
    PseudoElement,
};

WEB_API AsyncScrollNodeKind async_scroll_node_kind_for(Painting::CompositorScrollNodeKind);

// Stable identity for reconciling compositor-side scroll offsets after the paint snapshot has been rebuilt.
struct AsyncScrollNodeStableID {
    UniqueNodeID node_id;
    AsyncScrollNodeKind kind { AsyncScrollNodeKind::Element };
    u8 pseudo_element_type { 0 };

    bool operator==(AsyncScrollNodeStableID const&) const = default;
};

struct AsyncScrollOffset {
    AsyncScrollNodeStableID stable_node_id;
    Gfx::FloatPoint compositor_scroll_offset;
    Gfx::FloatPoint unadopted_scroll_delta;
    // The most recent nonzero delta of a relative scroll in each axis, such as a wheel or panning scroll. Absolute
    // scrolls, such as dragging a scrollbar thumb, leave it as it is.
    Gfx::FloatPoint last_relative_scroll_delta;

    void merge_later_scroll(AsyncScrollOffset const& later)
    {
        compositor_scroll_offset = later.compositor_scroll_offset;
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
    AsyncScrollNodeStableID stable_node_id;
    Optional<AsyncScrollNodeID> parent_node_id;
    Gfx::IntRect scrollport_rect;
    Gfx::FloatPoint min_scroll_offset;
    Gfx::FloatPoint max_scroll_offset;
    bool is_viewport { false };
    bool can_be_wheel_scrolled_horizontally { false };
    bool can_be_wheel_scrolled_vertically { false };
};

// A region with a non-passive wheel listener. Wheels inside it must stay on the main thread because script may cancel.
struct BlockingWheelEventRegion {
    Painting::ContextRef context;
    Gfx::FloatRect rect;
};

struct WheelHitTestTarget {
    Painting::ContextRef context;
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    Optional<AsyncScrollNodeID> target_node_id;
    // Position among the wheel hit test targets and scrollbars of the display list, which are recorded in paint order.
    u32 paint_order_index { 0 };
};

// A region that must always use main-thread wheel routing even without a blocking listener, such as a nested navigable.
struct MainThreadWheelEventRegion {
    Painting::ContextRef context;
    Gfx::FloatRect rect;
};

struct AsyncScrollbar {
    AsyncScrollNodeID scroll_node_id;
    Optional<AsyncScrollNodeStableID> scroller_stable_node_id;
    Painting::SpatialNodeIndex scroll_node_index;
    // The rects of a scrollbar the display list paints are in the space of this context.
    Painting::ContextRef context;
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
    Vector<AsyncScrollNode> scroll_nodes;
    Vector<AsyncSnapContainer> snap_containers;
    Vector<WheelHitTestTarget> wheel_hit_test_targets;
    Vector<MainThreadWheelEventRegion> main_thread_wheel_event_regions;
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
    double device_pixels_per_css_pixel { 1.0 };
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

WEB_API AsyncScrollingState async_scrolling_state_from_display_list(Painting::DisplayList const&);
WEB_API WheelRoutingAdmission wheel_routing_admission_for(AsyncScrollingState const&);
WEB_API Utf16View wheel_routing_admission_to_utf16_view(WheelRoutingAdmission);
WEB_API bool blocks_wheel_event_at_position(AsyncScrollingState const&, RefPtr<Painting::DisplayList const> const&, Painting::AccumulatedVisualContextTree const*, Painting::ScrollStateSnapshot const&, Gfx::FloatPoint position);
WEB_API WheelScrollAdmission admit_wheel_scroll(AsyncScrollingState const&, RefPtr<Painting::DisplayList const> const&, Painting::AccumulatedVisualContextTree const*, Painting::ScrollStateSnapshot const&, Gfx::FloatPoint position, Gfx::FloatPoint delta, bool blocking_wheel_event_regions_are_current);

}

template<>
struct AK::Traits<Web::Compositor::AsyncScrollNodeStableID> : DefaultTraits<Web::Compositor::AsyncScrollNodeStableID> {
    static unsigned hash(Web::Compositor::AsyncScrollNodeStableID const& stable_node_id)
    {
        return pair_int_hash(u64_hash(static_cast<u64>(stable_node_id.node_id.value())),
            pair_int_hash(to_underlying(stable_node_id.kind), stable_node_id.pseudo_element_type));
    }
};
