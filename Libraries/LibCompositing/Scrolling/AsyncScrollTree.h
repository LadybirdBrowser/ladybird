/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

namespace Compositing {

struct WheelHitTestResult {
    Optional<AsyncScrollNodeID> node_id;
    bool blocked_by_main_thread_region { false };
    bool blocked_by_wheel_event_region { false };
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

struct CachedBlockingWheelEventTarget {
    Compositing::ContextRef context;
    Gfx::FloatRect rect;
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
    void clear_wheel_hit_test_targets();

    Optional<Gfx::FloatPoint> scroll_offset_for_node(AsyncScrollNodeID, Compositing::ScrollStateSnapshot const&) const;
    Optional<UniqueNodeID> document_id() const;
    Optional<AsyncScrollNodeID> viewport_scroll_node_id() const;
    Optional<AsyncScrollNodeID> scroll_node_id_for_stable_id(AsyncScrollNodeStableID) const;
    AsyncScrollNode const* scroll_node_for_id(AsyncScrollNodeID) const;
    AsyncSnapContainer const* snap_container_for_node(AsyncScrollNodeID) const;

    // Scroll offsets are held in the device pixels of the display list; snap geometry is in the CSS pixels it was
    // recorded from, at the scale the display list carries.
    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }
    CSSPixelPoint css_pixels_from_device_offset(Gfx::FloatPoint) const;
    Gfx::FloatPoint device_offset_from_css_pixels(CSSPixelPoint) const;
    Optional<CSSPixelPoint> css_scroll_offset_for_node(AsyncScrollNodeID, Compositing::ScrollStateSnapshot const&) const;
    bool blocks_wheel_event_at_position(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    WheelHitTestResult hit_test_scroll_node_for_wheel(Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position, Gfx::FloatPoint delta) const;
    bool has_wheel_hit_test_targets_for(Compositing::AccumulatedVisualContextTree const& visual_context_tree) const { return m_visual_context_tree_structural_epoch == visual_context_tree.structural_epoch(); }
    // Whether something painted above the given place in paint order takes pointer input at the position. It is taken
    // to be covered whenever that cannot be told.
    bool is_covered_by_hit_test_target_painted_after(u32 paint_order_index, Compositing::AccumulatedVisualContextTree const&, Gfx::FloatPoint position) const;
    bool scroll_node_is_viewport(AsyncScrollNodeID) const;
    Optional<AsyncScrollNodeID> scroll_node_for_keyboard_scroll(AsyncScrollNodeStableID, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot const&) const;
    Gfx::FloatPoint clamped_scroll_offset_for_node(AsyncScrollNodeID, Gfx::FloatPoint) const;
    Optional<AsyncScrollOffset> apply_scroll_delta(AsyncScrollNodeID, Gfx::FloatPoint delta, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot&, ScrollChaining);
    Optional<Gfx::FloatPoint> set_scroll_offset(AsyncScrollNodeID, Gfx::FloatPoint, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot&);

private:
    static Gfx::FloatPoint clamp_scroll_offset_to_node(AsyncScrollNode const&, Gfx::FloatPoint);
    static Gfx::FloatPoint scroll_offset_for_node(AsyncScrollNode const&, Compositing::ScrollStateSnapshot const&);
    static bool can_scroll_node_by_delta(AsyncScrollNode const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint);

    WheelHitTestResult hit_test_result_for_scroll_node(AsyncScrollNodeID, Gfx::FloatPoint delta) const;
    AsyncScrollNode const* scroll_node_for_stable_id(AsyncScrollNodeStableID) const;
    Optional<AsyncScrollNodeID> scrollable_ancestor_for_node(AsyncScrollNodeID, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint delta) const;
    Gfx::FloatPoint apply_scroll_delta_to_node(AsyncScrollNode const&, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot&);

    Vector<AsyncScrollNode> m_scroll_nodes;
    Vector<AsyncSnapContainer> m_snap_containers;
    double m_device_pixels_per_css_pixel { 1.0 };
    Vector<WheelHitTestTarget> m_wheel_hit_test_regions;
    Vector<MainThreadWheelEventRegion> m_main_thread_wheel_event_regions;
    Vector<CachedWheelHitTestTarget> m_cached_wheel_hit_test_targets;
    Vector<BlockingWheelEventRegion> m_blocking_wheel_event_regions;
    Vector<CachedMainThreadWheelEventTarget> m_cached_main_thread_wheel_event_targets;
    Vector<CachedBlockingWheelEventTarget> m_cached_blocking_wheel_event_targets;
    Optional<u64> m_visual_context_tree_structural_epoch;
    Compositing::ScrollStateSnapshot m_scroll_state_snapshot;
    bool m_has_blocking_wheel_event_region_covering_viewport { false };
};

}
