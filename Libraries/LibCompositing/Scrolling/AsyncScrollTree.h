/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Types.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

namespace Compositing {

enum class WheelHitTestOutcome : u8 {
    FoundScrollTarget,
    NoScrollTarget,
    BlockedByMainThreadRegion,
    BlockedByWheelEventRegion,
    HitTestTargetsOfAnotherVisualContextTree,
};

struct WheelHitTestResult {
    WheelHitTestOutcome outcome { WheelHitTestOutcome::NoScrollTarget };
    // Set exactly when the outcome is FoundScrollTarget.
    Optional<AsyncScrollNodeID> node_id {};
    // The scroller the wheel is over, which is where the search for `node_id` started. It is set even if neither it
    // nor any scroller around it can scroll by the delta.
    Optional<AsyncScrollNodeID> innermost_scroll_node_id {};
    // The scroller that keeps the delta from the scrollers around it, when the outcome is NoScrollTarget for that
    // reason rather than because no scroller is left to offer it to.
    Optional<AsyncScrollNodeID> node_keeping_the_scroll {};

    bool is_blocked_by_a_region() const
    {
        return outcome == WheelHitTestOutcome::BlockedByMainThreadRegion || outcome == WheelHitTestOutcome::BlockedByWheelEventRegion;
    }
};

// A first step of a wheel gesture is latched to the scroller the hit test found, and is otherwise left to the main
// thread for the outcome's reason.
COMPOSITING_API Web::InputRoutingDecision input_routing_decision_for_first_wheel_step(WheelHitTestOutcome);

// Where the first step of a wheel gesture over a position goes, as far as the admission and the hit test decide it.
struct FirstWheelStepRoute {
    Web::InputRoutingDecision decision;
    Optional<Web::AsyncScrollNodeStableID> scroller {};
};

struct FirstWheelStepRouteForTesting {
    FirstWheelStepRoute route;
    // The document whose scrolling state decided the route.
    Optional<Web::UniqueNodeID> document_id;
};

// What the wheel hit test does where a nested context's content is painted.
enum class NestedContextRegions : u8 {
    // The step is left to the main thread, which hands it to the nested navigable.
    LeaveToMainThread,
    // The step is handed to the nested context under it, which only gets here if there is none.
    HandToNestedContext,
    // The nested context passed the step on, so it goes to whatever is under the nested context.
    Ignore,
};

// Where a wheel or the pointer over the content of a nested context lands in it.
struct NestedContextHit {
    Web::CompositorContextId child_context_id;
    // Relative to the origin of the nested context's content box, in the device pixels of the containing context.
    Gfx::FloatPoint position_in_content_box;
    Gfx::FloatSize content_box_size;
    // Where the nested context's content is painted among the other things in the containing context that take
    // pointer input.
    u32 paint_order_index { 0 };
};

struct WheelScrollTarget {
    Web::AsyncScrollNodeStableID stable_node_id;
    Gfx::FloatPoint current_offset;
    Gfx::FloatPoint start_offset;
    Gfx::FloatPoint destination_offset;
};

struct CachedWheelHitTestTarget {
    Optional<AsyncScrollNodeID> target_node_id;
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    Optional<Gfx::FloatRect> viewport_rect;
    u32 paint_order_index { 0 };
};

struct CachedMainThreadWheelEventTarget {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
    Optional<Gfx::FloatRect> viewport_rect;
};

struct CachedNestedContextWheelRegion {
    NestedContextWheelRegion region;
    Optional<Gfx::FloatRect> viewport_rect;
};

struct CachedBlockingWheelEventTarget {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
    Optional<Gfx::FloatRect> viewport_rect;
};

struct CachedScrollChainingBarrier {
    ScrollChainingBarrier barrier;
    Optional<Gfx::FloatRect> viewport_rect;
};

enum class ScrollChaining : u8 {
    ToScrollableAncestors,
    None,
};

// Mutable compositor-side copy of AsyncScrollingState. Current scroll offsets live in ScrollStateSnapshot; this tree
// owns scroll node geometry and derived hit-test targets.
class COMPOSITING_API AsyncScrollTree {
public:
    void set_state(AsyncScrollingState&&);

    void rebuild_wheel_hit_test_targets(RefPtr<Compositing::DisplayList const> const&, Compositing::AccumulatedVisualContextTree const*, Compositing::ScrollStateSnapshot const&);

    Optional<Gfx::FloatPoint> scroll_offset_for_node(AsyncScrollNodeID, Compositing::ScrollStateSnapshot const&) const;
    Optional<Web::UniqueNodeID> document_id() const;
    Optional<AsyncScrollNodeID> viewport_scroll_node_id() const;
    Optional<AsyncScrollNodeID> scroll_node_id_for_stable_id(Web::AsyncScrollNodeStableID) const;
    AsyncScrollNode const* scroll_node_for_id(AsyncScrollNodeID) const;
    ReadonlySpan<AsyncScrollNode> scroll_nodes() const { return m_scroll_nodes; }
    AsyncSnapContainer const* snap_container_for_node(AsyncScrollNodeID) const;

    // Scroll offsets are held in the device pixels of the display list; snap geometry is in the CSS pixels it was
    // recorded from, at the scale the display list carries.
    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }
    Web::CSSPixelPoint css_pixels_from_device_offset(Gfx::FloatPoint) const;
    Gfx::FloatPoint device_offset_from_css_pixels(Web::CSSPixelPoint) const;
    Optional<Web::CSSPixelPoint> css_scroll_offset_for_node(AsyncScrollNodeID, Compositing::ScrollStateSnapshot const&) const;
    bool blocks_wheel_event_at_position(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    WheelHitTestResult hit_test_scroll_node_for_wheel(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position, Gfx::FloatPoint delta, Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> const& in_flight_destination_for = {}, WheelStepDispatch = WheelStepDispatch::NotYetDispatched, NestedContextRegions = NestedContextRegions::LeaveToMainThread) const;
    // The nested context whose content is the topmost thing taking pointer input at the position, if any.
    Optional<NestedContextHit> nested_context_at(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    // As above, unless the main thread takes the wheel events at the position.
    Optional<NestedContextHit> nested_context_for_wheel_at(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    // Where the position lands in the content of the nested context, even outside of that content and its clips, as
    // the pointer of a drag that began in there moves on.
    Optional<NestedContextHit> position_in_content_of_nested_context(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    bool has_nested_contexts() const { return !m_nested_context_wheel_regions.is_empty(); }
    bool has_wheel_hit_test_targets_for(Compositing::AccumulatedVisualContextTree const& visual_context_tree) const { return m_visual_context_tree_structural_epoch == visual_context_tree.structural_epoch(); }
    // Whether something painted above the given place in paint order takes pointer input at the position. It is taken
    // to be covered whenever that cannot be told.
    bool is_covered_by_hit_test_target_painted_after(u32 paint_order_index, Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    bool scroll_node_is_viewport(AsyncScrollNodeID) const;
    // Whether the viewport keeps a scroll by the delta from the document it is nested in, whether or not it has a
    // scroll node.
    bool viewport_keeps_scrolls_along_main_axis_of(Gfx::FloatPoint delta) const;
    // Leaves out what only a compositor holding a gesture decides: whether a step continues a latched gesture, which
    // scroller fingers at its edge stretch, and whether a zoomed page pans its visual viewport.
    FirstWheelStepRoute route_first_wheel_step(WheelRoutingAdmission, Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position, Gfx::FloatPoint delta, NestedContextRegions = NestedContextRegions::LeaveToMainThread) const;
    // Of the scroller and the scrollers around it that a scroll by the delta may be passed on to, the outermost one a
    // wheel can scroll along the main axis of the delta at all, wherever it is scrolled to.
    Optional<AsyncScrollNodeID> outermost_scroll_node_wheel_scrollable_along_main_axis_of(AsyncScrollNodeID innermost_node_id, Gfx::FloatPoint delta) const;
    Optional<AsyncScrollNodeID> scroll_node_for_keyboard_scroll(Web::AsyncScrollNodeStableID, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot const&) const;
    Gfx::FloatPoint clamped_scroll_offset_for_node(AsyncScrollNodeID, Gfx::FloatPoint) const;
    Optional<AsyncScrollOffset> apply_scroll_delta(AsyncScrollNodeID, Gfx::FloatPoint delta, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot&, ScrollChaining);
    Optional<WheelScrollTarget> select_wheel_scroll_target(AsyncScrollNodeID, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot const&, Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> const& in_flight_destination_for, ScrollChaining scroll_chaining) const;
    Optional<Gfx::FloatPoint> set_scroll_offset(AsyncScrollNodeID, Gfx::FloatPoint, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot&);

private:
    static Gfx::FloatPoint clamp_scroll_offset_to_node(AsyncScrollNode const&, Gfx::FloatPoint);
    static Gfx::FloatPoint scroll_offset_for_node(AsyncScrollNode const&, Compositing::ScrollStateSnapshot const&);
    static bool can_scroll_node_by_delta(AsyncScrollNode const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint);
    static bool can_scroll_node_from_offset_by_delta(AsyncScrollNode const&, Gfx::FloatPoint scroll_offset, Gfx::FloatPoint delta);

    WheelHitTestResult hit_test_result_for_scroll_node(AsyncScrollNodeID, Gfx::FloatPoint delta, Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> const& in_flight_destination_for, ReadonlySpan<AsyncScrollNodeID> scroll_nodes_behind_scroll_chaining_barriers) const;
    AsyncScrollNode const* scroll_node_for_stable_id(Web::AsyncScrollNodeStableID) const;
    bool main_thread_wheel_event_region_contains(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    struct NestedContextWheelRegionHit {
        NestedContextWheelRegion const& region;
        Gfx::FloatPoint position_in_context;
    };
    Optional<NestedContextWheelRegionHit> topmost_nested_context_wheel_region_at(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    Optional<AsyncScrollNodeID> scrollable_ancestor_for_node(AsyncScrollNodeID, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint delta) const;
    Gfx::FloatPoint apply_scroll_delta_to_node(AsyncScrollNode const&, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot&);

    Optional<Web::UniqueNodeID> m_document_id;
    Vector<AsyncScrollNode> m_scroll_nodes;
    Vector<AsyncSnapContainer> m_snap_containers;
    double m_device_pixels_per_css_pixel { 1.0 };
    Vector<WheelHitTestTarget> m_wheel_hit_test_regions;
    Vector<MainThreadWheelEventRegion> m_main_thread_wheel_event_regions;
    Vector<NestedContextWheelRegion> m_nested_context_wheel_regions;
    Vector<CachedWheelHitTestTarget> m_cached_wheel_hit_test_targets;
    Vector<BlockingWheelEventRegion> m_blocking_wheel_event_regions;
    Vector<CachedMainThreadWheelEventTarget> m_cached_main_thread_wheel_event_targets;
    Vector<CachedNestedContextWheelRegion> m_cached_nested_context_wheel_regions;
    Vector<CachedBlockingWheelEventTarget> m_cached_blocking_wheel_event_targets;
    Vector<ScrollChainingBarrier> m_scroll_chaining_barriers;
    Vector<CachedScrollChainingBarrier> m_cached_scroll_chaining_barriers;
    Optional<u64> m_visual_context_tree_structural_epoch;
    Compositing::ScrollStateSnapshot m_scroll_state_snapshot;
    bool m_has_blocking_wheel_event_region_covering_viewport { false };
    bool m_viewport_keeps_horizontal_scrolls { false };
    bool m_viewport_keeps_vertical_scrolls { false };
};

}
