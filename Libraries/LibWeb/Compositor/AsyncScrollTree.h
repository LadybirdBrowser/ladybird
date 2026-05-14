/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Compositor {

struct WheelHitTestResult {
    Optional<AsyncScrollNodeID> node_id;
    bool blocked_by_main_thread_region { false };
};

struct CachedWheelHitTestTarget {
    Optional<AsyncScrollNodeID> target_node_id;
    Painting::VisualContextIndex visual_context_index;
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    Gfx::FloatRect viewport_rect;
};

struct MainThreadWheelEventTarget {
    Painting::VisualContextIndex visual_context_index;
    Gfx::FloatRect rect;
    Gfx::FloatRect viewport_rect;
};

// Mutable compositor-side copy of AsyncScrollingState. Current scroll offsets live in ScrollStateSnapshot; this tree
// owns scroll node geometry and derived hit-test targets.
class AsyncScrollTree {
public:
    void set_state(AsyncScrollingState&&);

    void rebuild_wheel_hit_test_targets(RefPtr<Painting::DisplayList> const&, Painting::ScrollStateSnapshot const&);
    void clear_wheel_hit_test_targets();

    Optional<Gfx::FloatPoint> scroll_offset_for_node(AsyncScrollNodeID, Painting::ScrollStateSnapshot const&) const;
    Optional<AsyncScrollNodeID> viewport_scroll_node_id() const;
    Optional<AsyncScrollNodeID> scroll_node_id_for_stable_id(AsyncScrollNodeStableID) const;
    WheelHitTestResult hit_test_scroll_node_for_wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta) const;
    bool scroll_node_is_viewport(AsyncScrollNodeID) const;
    bool apply_scroll_delta(AsyncScrollNodeID, Gfx::FloatPoint delta, Painting::ScrollStateSnapshot&);
    Optional<Gfx::FloatPoint> set_scroll_offset(AsyncScrollNodeID, Gfx::FloatPoint, Painting::ScrollStateSnapshot&);

private:
    static Gfx::FloatPoint clamp_scroll_offset_to_node(AsyncScrollNode const&, Gfx::FloatPoint);
    static Gfx::FloatPoint scroll_offset_for_node(AsyncScrollNode const&, Painting::ScrollStateSnapshot const&);
    static bool can_scroll_node_by_delta(AsyncScrollNode const&, Painting::ScrollStateSnapshot const&, Gfx::FloatPoint);
    static bool has_non_zero_scroll_delta(Gfx::FloatPoint);

    AsyncScrollNode const* scroll_node_for_id(AsyncScrollNodeID) const;
    WheelHitTestResult hit_test_result_for_scroll_node(AsyncScrollNodeID, Gfx::FloatPoint delta) const;
    AsyncScrollNode const* scroll_node_for_stable_id(AsyncScrollNodeStableID) const;
    AsyncStickyArea const* sticky_area_for_scroll_frame_index(Painting::ScrollFrameIndex) const;
    Optional<AsyncScrollNodeID> scrollable_ancestor_for_node(AsyncScrollNodeID, Painting::ScrollStateSnapshot const&, Gfx::FloatPoint delta) const;
    Optional<Painting::ScrollFrameIndex> parent_scroll_frame_index(Painting::ScrollFrameIndex) const;
    Gfx::FloatPoint cumulative_device_offset_for_frame(Painting::ScrollFrameIndex, Painting::ScrollStateSnapshot const&) const;
    Gfx::FloatPoint apply_scroll_delta_to_node(AsyncScrollNode const&, Gfx::FloatPoint delta, Painting::ScrollStateSnapshot&);
    void update_sticky_offsets(Painting::ScrollStateSnapshot&) const;

    Vector<AsyncScrollNode> m_scroll_nodes;
    Vector<AsyncStickyArea> m_sticky_areas;
    Vector<WheelHitTestTarget> m_wheel_hit_test_regions;
    Vector<MainThreadWheelEventRegion> m_main_thread_wheel_event_regions;
    Vector<CachedWheelHitTestTarget> m_wheel_hit_test_targets;
    Vector<MainThreadWheelEventTarget> m_main_thread_wheel_event_targets;
    RefPtr<Painting::AccumulatedVisualContextTree const> m_visual_context_tree;
    Painting::ScrollStateSnapshot m_scroll_state_snapshot;
};

}
