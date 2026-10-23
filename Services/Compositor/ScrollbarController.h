/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <Compositor/Forward.h>
#include <LibCompositing/OverlayFade.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>

namespace Compositing {

class AsyncScrollTree;

}

namespace Compositing {

class AccumulatedVisualContextTree;
class ScrollStateSnapshot;

}

namespace Compositor {

class ScrollbarController {
public:
    struct Drag {
        size_t scrollbar_index { 0 };
        float primary_position { 0 };
        float thumb_grab_position { 0 };
    };

    struct ScrollOffset {
        Compositing::AsyncScrollNodeID scroll_node_id;
        Gfx::FloatPoint scroll_offset;
    };

    // Overlay scrollbars are hidden until their scroller scrolls or the pointer hovers them. A scrollbar stays shown
    // for this long after its scroller last scrolled or the pointer left it, and then fades out.
    static constexpr auto shown_duration_after_activity = AK::Duration::from_milliseconds(1000);

    // Tests see every scrollbar at all times, as when it is shown depends on the time.
    static void show_scrollbars_always_for_testing() { s_scrollbars_always_shown = true; }

    void clear();
    void set_scrollbars(Vector<Compositing::AsyncScrollbar> const&);

    // Shows each scrollbar that is hovered or dragged, or whose scroller scrolled since the last call, and starts the
    // wait before a scrollbar that is no longer hovered or dragged fades out.
    void update_visibility(Compositing::ScrollStateSnapshot const&, MonotonicTime now);
    // When the next scrollbar starts to fade out.
    Optional<MonotonicTime> next_fade_out_time() const;
    bool fades_need_frame(MonotonicTime now) const;
    // Returns the rect in the viewport of the scrollbars whose opacity changed.
    Gfx::IntRect advance_fades(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, MonotonicTime frame_time);
    float opacity_of_scrollbar_painted_by_display_list(Compositing::SpatialNodeIndex scroll_node_index, bool vertical) const;

    bool is_empty() const { return m_scrollbars.is_empty(); }
    Vector<Compositing::AsyncScrollbar> const& scrollbars() const { return m_scrollbars; }
    bool has_captured_scrollbar() const { return m_captured_scrollbar_index.has_value(); }
    // The main thread still takes the mouse events of a drag that holds a scrollbar the display list paints.
    Optional<Web::ScrollbarDraggedByCompositor> captured_scrollbar_painted_by_display_list() const;

    Optional<size_t> hit_test_scrollbar_painted_by_compositor(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position) const;
    Optional<Drag> begin_drag(Compositing::AsyncScrollTree const&, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);
    Optional<Drag> captured_drag(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);
    Optional<Drag> release_captured_drag(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);
    bool set_hovered_scrollbar(Optional<size_t>);

    Optional<ScrollOffset> scroll_offset_for_drag(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Drag const&) const;
    bool paint(Gfx::PaintingSurface&, DisplayListPlayerSkia&, Compositing::ScrollStateSnapshot const&) const;

private:
    struct Visibility {
        Compositing::OverlayFade fade;
        // The scroll offset of the scroller as of the last update, which a scroll changes.
        Optional<Gfx::FloatPoint> scroll_offset;
        bool is_held_shown { false };
        Optional<MonotonicTime> fade_out_time;
    };

    bool is_expanded(size_t scrollbar_index) const;
    bool is_held_shown(size_t scrollbar_index) const;
    float opacity(size_t scrollbar_index) const;
    Optional<size_t> hit_test_scrollbar_painted_by_display_list(Compositing::AsyncScrollTree const&, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position) const;
    float primary_position_of_captured_drag(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);

    Vector<Compositing::AsyncScrollbar> m_scrollbars;
    // One for each scrollbar, in the same order.
    Vector<Visibility> m_visibilities;
    Optional<size_t> m_hovered_scrollbar_index;
    Optional<size_t> m_captured_scrollbar_index;
    float m_thumb_grab_position { 0 };
    float m_last_primary_position_of_captured_drag { 0 };

    static inline bool s_scrollbars_always_shown { false };
};

}
