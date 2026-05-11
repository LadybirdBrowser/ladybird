/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Compositor {

// Viewport-space cache used for wheel hit-testing, rebuilt from AsyncScrollNode whenever scroll offsets change.
struct WheelScrollTarget {
    AsyncScrollNodeID node_id;
    Gfx::FloatRect viewport_rect;
    bool can_scroll_left { false };
    bool can_scroll_right { false };
    bool can_scroll_up { false };
    bool can_scroll_down { false };
};

// Viewport-space cache of regions that force main-thread wheel routing.
struct MainThreadWheelEventTarget {
    Gfx::FloatRect viewport_rect;
};

// Mutable compositor-side copy of AsyncScrollingState. The main thread owns the source snapshot; this tree owns only
// compositor scroll offsets and derived hit-test targets.
class AsyncScrollTree {
public:
    void set_state(AsyncScrollingState&&);

    void rebuild_wheel_scroll_targets(RefPtr<Painting::DisplayList> const&, Painting::ScrollStateSnapshot const&);
    void clear_wheel_scroll_targets();

    Optional<Gfx::FloatPoint> scroll_offset_for_node(AsyncScrollNodeID) const;
    Optional<AsyncScrollNodeID> hit_test_scroll_node_for_wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta) const;
    bool scroll_node_is_viewport(AsyncScrollNodeID) const;
    bool apply_scroll_delta(AsyncScrollNodeID, Gfx::FloatPoint delta, Painting::ScrollStateSnapshot&);

private:
    static Gfx::FloatPoint clamp_scroll_offset_to_node(AsyncScrollNode const&, Gfx::FloatPoint);
    static bool can_scroll_node_by_delta(AsyncScrollNode const&, Gfx::FloatPoint);
    static bool can_scroll_target_by_delta(WheelScrollTarget const&, Gfx::FloatPoint);
    static bool has_non_zero_scroll_delta(Gfx::FloatPoint);

    AsyncScrollNode* scroll_node_for_id(AsyncScrollNodeID);
    AsyncScrollNode const* scroll_node_for_id(AsyncScrollNodeID) const;
    AsyncStickyArea const* sticky_area_for_scroll_frame_index(Painting::ScrollFrameIndex) const;
    Optional<AsyncScrollNodeID> scrollable_ancestor_for_node(AsyncScrollNodeID, Gfx::FloatPoint delta) const;
    Optional<Painting::ScrollFrameIndex> parent_scroll_frame_index(Painting::ScrollFrameIndex) const;
    Gfx::FloatPoint cumulative_device_offset_for_frame(Painting::ScrollFrameIndex, Painting::ScrollStateSnapshot const&) const;
    Gfx::FloatPoint apply_scroll_delta_to_node(AsyncScrollNode&, Gfx::FloatPoint delta, Painting::ScrollStateSnapshot&);
    void update_sticky_offsets(Painting::ScrollStateSnapshot&) const;

    Vector<AsyncScrollNode> m_scroll_nodes;
    Vector<AsyncStickyArea> m_sticky_areas;
    Vector<MainThreadWheelEventRegion> m_main_thread_wheel_event_regions;
    Vector<MainThreadWheelEventTarget> m_main_thread_wheel_event_targets;
    Vector<WheelScrollTarget> m_wheel_scroll_targets;
    Gfx::IntRect m_viewport_rect;
};

}
