/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ViewportScrollbarController.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Painting/ScrollState.h>

static Compositor::ViewportScrollbarController::Drag begin_scrollbar_drag(Gfx::Orientation orientation, Gfx::FloatPoint position, Optional<Gfx::IntRect> expanded_thumb_rect = {})
{
    auto vertical = orientation == Gfx::Orientation::Vertical;
    auto document_id = Web::UniqueNodeID { 1 };
    auto scroll_node_index = Web::Painting::VisualContextIndex { 1 };
    auto scroll_node_id = Web::Compositor::AsyncScrollNodeID {
        .document_id = document_id,
        .scroll_node_index = scroll_node_index,
    };

    Web::Compositor::AsyncScrollingState scrolling_state;
    scrolling_state.scroll_nodes.append({
        .node_id = scroll_node_id,
        .stable_node_id = {
            .node_id = Web::UniqueNodeID { 2 },
            .kind = Web::Compositor::AsyncScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
        },
        .parent_node_id = {},
        .scrollport_rect = { 0, 0, 100, 100 },
        .min_scroll_offset = { 0, 0 },
        .max_scroll_offset = { 100, 100 },
        .is_viewport = true,
        .can_be_wheel_scrolled_horizontally = true,
        .can_be_wheel_scrolled_vertically = true,
    });

    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(scrolling_state));
    Web::Painting::ScrollStateSnapshot scroll_state_snapshot;

    Vector<Web::Compositor::ViewportScrollbar> scrollbars;
    scrollbars.append({
        .scroll_node_id = scroll_node_id,
        .scroll_node_index = scroll_node_index,
        .gutter_rect = vertical ? Gfx::IntRect { 96, 0, 4, 100 } : Gfx::IntRect { 0, 96, 100, 4 },
        .thumb_rect = vertical ? Gfx::IntRect { 98, 20, 2, 20 } : Gfx::IntRect { 20, 98, 20, 2 },
        .expanded_gutter_rect = vertical ? Gfx::IntRect { 92, 0, 8, 100 } : Gfx::IntRect { 0, 92, 100, 8 },
        .expanded_thumb_rect = expanded_thumb_rect.value_or(vertical ? Gfx::IntRect { 94, 20, 6, 20 } : Gfx::IntRect { 20, 94, 20, 6 }),
        .scroll_size = 0.8,
        .expanded_scroll_size = 0.8,
        .min_scroll_offset = 0,
        .max_scroll_offset = 100,
        .thumb_color = Gfx::Color::Black,
        .track_color = Gfx::Color::Transparent,
        .vertical = vertical,
    });

    Compositor::ViewportScrollbarController controller;
    controller.set_scrollbars(scrollbars);
    auto drag = controller.begin_drag(scroll_tree, scroll_state_snapshot, position);
    VERIFY(drag.has_value());
    return drag.release_value();
}

TEST_CASE(clicking_scrollbar_beside_thumb_grabs_thumb_at_that_position)
{
    auto vertical_drag = begin_scrollbar_drag(Gfx::Orientation::Vertical, { 97, 25 });
    EXPECT_EQ(vertical_drag.primary_position, 25);
    EXPECT_EQ(vertical_drag.thumb_grab_position, 5);

    auto horizontal_drag = begin_scrollbar_drag(Gfx::Orientation::Horizontal, { 25, 97 });
    EXPECT_EQ(horizontal_drag.primary_position, 25);
    EXPECT_EQ(horizontal_drag.thumb_grab_position, 5);
}

TEST_CASE(clicking_scrollbar_track_outside_thumb_grabs_thumb_at_center)
{
    auto vertical_drag = begin_scrollbar_drag(Gfx::Orientation::Vertical, { 97, 60 });
    EXPECT_EQ(vertical_drag.primary_position, 60);
    EXPECT_EQ(vertical_drag.thumb_grab_position, 10);

    auto horizontal_drag = begin_scrollbar_drag(Gfx::Orientation::Horizontal, { 60, 97 });
    EXPECT_EQ(horizontal_drag.primary_position, 60);
    EXPECT_EQ(horizontal_drag.thumb_grab_position, 10);
}

TEST_CASE(starting_scrollbar_drag_uses_expanded_thumb_geometry)
{
    auto vertical_drag = begin_scrollbar_drag(Gfx::Orientation::Vertical, { 97, 25 }, Gfx::IntRect { 94, 18, 6, 24 });
    EXPECT_EQ(vertical_drag.thumb_grab_position, 7);

    auto horizontal_drag = begin_scrollbar_drag(Gfx::Orientation::Horizontal, { 25, 97 }, Gfx::IntRect { 18, 94, 24, 6 });
    EXPECT_EQ(horizontal_drag.thumb_grab_position, 7);
}
