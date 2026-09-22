/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>

namespace Compositing {

class AsyncScrollTree;

}

namespace Compositing {

class AccumulatedVisualContextTree;
class DisplayListPlayerSkia;
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

    void clear();
    void set_scrollbars(Vector<Compositing::AsyncScrollbar> const&);

    bool is_empty() const { return m_scrollbars.is_empty(); }
    Vector<Compositing::AsyncScrollbar> const& scrollbars() const { return m_scrollbars; }
    bool has_captured_scrollbar() const { return m_captured_scrollbar_index.has_value(); }
    // The main thread still takes the mouse events of a drag that holds a scrollbar the display list paints.
    Optional<Compositing::ScrollbarDraggedByCompositor> captured_scrollbar_painted_by_display_list() const;

    Optional<size_t> hit_test_scrollbar_painted_by_compositor(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position) const;
    Optional<Drag> begin_drag(Compositing::AsyncScrollTree const&, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);
    Optional<Drag> captured_drag(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);
    Optional<Drag> release_captured_drag(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);
    bool set_hovered_scrollbar(Optional<size_t>);

    Optional<ScrollOffset> scroll_offset_for_drag(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Drag const&) const;
    bool paint(Gfx::PaintingSurface&, Compositing::DisplayListPlayerSkia&, Compositing::ScrollStateSnapshot const&) const;

private:
    bool is_expanded(size_t scrollbar_index) const;
    Optional<size_t> hit_test_scrollbar_painted_by_display_list(Compositing::AsyncScrollTree const&, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position) const;
    float primary_position_of_captured_drag(Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&, Gfx::FloatPoint position);

    Vector<Compositing::AsyncScrollbar> m_scrollbars;
    Optional<size_t> m_hovered_scrollbar_index;
    Optional<size_t> m_captured_scrollbar_index;
    float m_thumb_grab_position { 0 };
    float m_last_primary_position_of_captured_drag { 0 };
};

}
