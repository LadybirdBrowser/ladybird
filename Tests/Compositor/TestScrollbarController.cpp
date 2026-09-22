/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ScrollbarController.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibTest/TestCase.h>

static Compositor::ScrollbarController::Drag begin_scrollbar_drag(Gfx::Orientation orientation, Gfx::FloatPoint position, Optional<Gfx::IntRect> expanded_thumb_rect = {})
{
    auto vertical = orientation == Gfx::Orientation::Vertical;
    auto document_id = Compositing::UniqueNodeID { 1 };
    auto scroll_node_index = Compositing::SpatialNodeIndex { 1 };
    auto scroll_node_id = Compositing::AsyncScrollNodeID {
        .document_id = document_id,
        .scroll_node_index = scroll_node_index,
    };

    Compositing::AsyncScrollingState scrolling_state;
    scrolling_state.scroll_nodes.append({
        .node_id = scroll_node_id,
        .stable_node_id = {
            .node_id = Compositing::UniqueNodeID { 2 },
            .kind = Compositing::AsyncScrollNodeKind::Viewport,
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

    Compositing::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(scrolling_state));
    Compositing::ScrollStateSnapshot scroll_state_snapshot;

    Vector<Compositing::AsyncScrollbar> scrollbars;
    scrollbars.append({
        .scroll_node_id = scroll_node_id,
        .scroller_stable_node_id = {},
        .scroll_node_index = scroll_node_index,
        .context = {},
        .paint_order_index = 0,
        .gutter_rect = vertical ? Gfx::IntRect { 96, 0, 4, 100 } : Gfx::IntRect { 0, 96, 100, 4 },
        .thumb_rect = vertical ? Gfx::IntRect { 98, 20, 2, 20 } : Gfx::IntRect { 20, 98, 20, 2 },
        .track_rect = vertical ? Gfx::IntRect { 96, 0, 4, 100 } : Gfx::IntRect { 0, 96, 100, 4 },
        .expanded_gutter_rect = vertical ? Gfx::IntRect { 92, 0, 8, 100 } : Gfx::IntRect { 0, 92, 100, 8 },
        .expanded_thumb_rect = expanded_thumb_rect.value_or(vertical ? Gfx::IntRect { 94, 20, 6, 20 } : Gfx::IntRect { 20, 94, 20, 6 }),
        .scroll_size = 0.8,
        .expanded_scroll_size = 0.8,
        .min_scroll_offset = 0,
        .max_scroll_offset = 100,
        .thumb_color = Gfx::Color::Black,
        .track_color = Gfx::Color::Transparent,
        .vertical = vertical,
        .is_painted_by_compositor = true,
        .display_list_paints_enlarged_scrollbar = false,
    });

    Compositing::VisualContextTreeTestBuilder visual_context_tree_builder;
    visual_context_tree_builder.append_scroll(Compositing::VISUAL_VIEWPORT_NODE_INDEX);
    auto visual_context_tree = visual_context_tree_builder.finish();

    Compositor::ScrollbarController controller;
    controller.set_scrollbars(scrollbars);
    auto drag = controller.begin_drag(scroll_tree, visual_context_tree, scroll_state_snapshot, position);
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
