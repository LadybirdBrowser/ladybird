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
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

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
        // Whether this is the press that began the drag, beside the thumb rather than on it.
        bool is_press_beside_thumb { false };
    };

    struct ScrollOffset {
        Compositing::AsyncScrollNodeID scroll_node_id;
        Gfx::FloatPoint scroll_offset;
    };

    // A scrollbar as it is painted, somewhere between its thin and its expanded geometry.
    struct PresentedScrollbar {
        Gfx::FloatRect gutter_rect;
        Gfx::FloatRect thumb_rect;
        Color thumb_color;
        Color track_color;
        float opacity { 1 };
    };

    // A scroll offset outside the scroll range moves the thumb past an end of its track, where it is cut short instead.
    static Gfx::IntRect thumb_rect_kept_inside_track(Gfx::IntRect thumb_rect, Gfx::IntRect const& track_rect, bool vertical);

    // Overlay scrollbars are hidden until their scroller scrolls. A scrollbar stays shown for this long after its
    // scroller last scrolled or the pointer left it, and then fades out.
    static constexpr auto shown_duration_after_activity = AK::Duration::from_milliseconds(1000);

    // Tests see every scrollbar at all times, as when it is shown depends on the time.
    static void show_scrollbars_always_for_testing() { s_scrollbars_always_shown = true; }

    void clear();
    void set_scrollbars(Vector<Compositing::AsyncScrollbar> const&);

    // Shows each scrollbar whose scroller scrolled since the last call, keeps a shown scrollbar that is hovered or
    // dragged shown, and starts the wait before a scrollbar that is no longer hovered or dragged fades out. A hovered or
    // dragged scrollbar that is shown grows to its expanded geometry, and keeps it until it has faded out. Returns the rect in the viewport of the scrollbars the
    // display list paints that scrolled while they are presented in another geometry than the display list gives them.
    Gfx::IntRect update_visibility(Compositing::AccumulatedVisualContextTree const*, Compositing::ScrollStateSnapshot const&, MonotonicTime now);
    // When the next scrollbar starts to fade out.
    Optional<MonotonicTime> next_fade_out_time() const;
    // The scrollbars that faded out or were shown again since the last call. The main thread hit tests the scrollbars
    // too, and passes by the ones that have faded out.
    Vector<Web::OverlayScrollbarVisibility> take_visibility_changes();
    bool fades_need_frame(MonotonicTime now) const;
    // Returns the rect in the viewport of the scrollbars whose opacity or geometry changed.
    Gfx::IntRect advance_fades(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, MonotonicTime frame_time);
    // A scrollbar command of the display list with no scrollbar here is painted as the command gives it. The colors are
    // the ones the display list resolved for the page, which it may have darkened.
    Optional<PresentedScrollbar> presented_scrollbar_painted_by_display_list(Compositing::SpatialNodeIndex scroll_node_index, bool vertical, Color thumb_color, Color track_color, Compositing::ScrollStateSnapshot const&) const;

    bool is_empty() const { return m_scrollbars.is_empty(); }
    Vector<Compositing::AsyncScrollbar> const& scrollbars() const { return m_scrollbars; }
    bool has_captured_scrollbar() const { return m_captured_scrollbar_index.has_value(); }
    Optional<size_t> captured_scrollbar_index() const { return m_captured_scrollbar_index; }
    // Which way a press beside the thumb pages: -1 or 1 toward the pointer from where the thumb is at the given scroll
    // offset, or 0 when the thumb is under the pointer.
    int paging_direction(size_t scrollbar_index, float primary_position, Gfx::FloatPoint scroll_offset) const;
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
        // How far the scrollbar has grown from its thin geometry to its expanded one.
        Compositing::OverlayFade expansion;
        // Whether the main thread was last told that the scrollbar has faded out.
        bool reported_hidden { false };
    };

    bool is_expanded(size_t scrollbar_index) const;
    bool is_held_shown(size_t scrollbar_index) const;
    float opacity(size_t scrollbar_index) const;
    float expansion(size_t scrollbar_index) const;
    PresentedScrollbar presented_scrollbar(size_t scrollbar_index, Compositing::ScrollStateSnapshot const&, Color thumb_color, Color track_color) const;
    Optional<Gfx::IntRect> rect_in_viewport(size_t scrollbar_index, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&) const;
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
