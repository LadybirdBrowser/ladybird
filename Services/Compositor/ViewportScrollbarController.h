/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>

namespace Web::Compositor {

class AsyncScrollTree;

}

namespace Web::Painting {

class DisplayListPlayerSkia;
class ScrollStateSnapshot;

}

namespace Compositor {

class ViewportScrollbarController {
public:
    struct Drag {
        size_t scrollbar_index { 0 };
        float primary_position { 0 };
        float thumb_grab_position { 0 };
    };

    struct ScrollDelta {
        Web::Compositor::AsyncScrollNodeID scroll_node_id;
        Gfx::FloatPoint delta;
    };

    void clear();
    void set_scrollbars(Vector<Web::Compositor::ViewportScrollbar> const&);

    bool is_empty() const { return m_scrollbars.is_empty(); }
    bool has_captured_scrollbar() const { return m_captured_scrollbar_index.has_value(); }

    Optional<size_t> hit_test(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&, Gfx::FloatPoint position) const;
    Optional<Drag> begin_drag(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&, Gfx::FloatPoint position);
    Optional<Drag> captured_drag(Gfx::FloatPoint position);
    Optional<Drag> release_captured_drag(Gfx::FloatPoint position);
    bool set_hovered_scrollbar(Optional<size_t>);

    Optional<ScrollDelta> scroll_delta_for_drag(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&, Drag const&) const;
    bool paint(Gfx::PaintingSurface&, Web::Painting::DisplayListPlayerSkia&, Web::Painting::ScrollStateSnapshot const&) const;

private:
    bool is_expanded(size_t scrollbar_index) const;

    Vector<Web::Compositor::ViewportScrollbar> m_scrollbars;
    Optional<size_t> m_hovered_scrollbar_index;
    Optional<size_t> m_captured_scrollbar_index;
    float m_thumb_grab_position { 0 };
};

}
