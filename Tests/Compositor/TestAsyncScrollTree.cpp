/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/ScrollState.h>

static Web::Compositor::AsyncScrollNodeID const viewport_node_id { .document_id = Web::UniqueNodeID { 1 }, .scroll_node_index = Web::Painting::SpatialNodeIndex { 1 } };

static Web::Compositor::AsyncScrollTree make_scroll_tree_with_viewport_scroll_node(float max_scroll_offset_y)
{
    Web::Compositor::AsyncScrollingState state;
    state.scroll_nodes.append({
        .node_id = viewport_node_id,
        .stable_node_id = { .node_id = Web::UniqueNodeID { 2 }, .kind = Web::Compositor::AsyncScrollNodeKind::Viewport, .pseudo_element_type = 0 },
        .parent_node_id = {},
        .scrollport_rect = { 0, 0, 800, 600 },
        .min_scroll_offset = { 0, 0 },
        .max_scroll_offset = { 0, max_scroll_offset_y },
        .is_viewport = true,
        .can_be_wheel_scrolled_horizontally = false,
        .can_be_wheel_scrolled_vertically = true,
    });
    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    return scroll_tree;
}

static NonnullRefPtr<Web::Painting::DisplayList> make_empty_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree)
{
    return Web::Painting::DisplayList::create_from_command_bytes(visual_context_tree, ByteBuffer {}, {});
}

TEST_CASE(wheel_hit_testing_rejects_a_different_visual_context_tree_version)
{
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();
    Web::Compositor::AsyncScrollingState state;
    state.main_thread_wheel_event_regions.append({
        .context = { Web::Painting::VISUAL_VIEWPORT_NODE_INDEX, Web::Painting::NO_FRAME_NODE },
        .rect = { 0, 0, 100, 100 },
    });

    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    scroll_tree.rebuild_wheel_hit_test_targets(make_empty_display_list(visual_context_tree), &visual_context_tree, {});
    auto current_result = scroll_tree.hit_test_scroll_node_for_wheel(
        visual_context_tree, { 20, 20 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor);
    EXPECT(current_result.blocked_by_main_thread_region);

    auto replacement_tree = Web::Painting::AccumulatedVisualContextTree::create();
    auto replacement_result = scroll_tree.hit_test_scroll_node_for_wheel(
        replacement_tree, { 20, 20 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor);
    EXPECT(!replacement_result.blocked_by_main_thread_region);
}

TEST_CASE(wheel_hit_testing_ignores_invalid_visual_context_indices)
{
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();
    Web::Compositor::AsyncScrollingState state;
    state.main_thread_wheel_event_regions.append({
        .context = { Web::Painting::SpatialNodeIndex { 100 }, Web::Painting::NO_FRAME_NODE },
        .rect = { 0, 0, 100, 100 },
    });

    Web::Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(state));
    scroll_tree.rebuild_wheel_hit_test_targets(make_empty_display_list(visual_context_tree), &visual_context_tree, {});
    auto result = scroll_tree.hit_test_scroll_node_for_wheel(
        visual_context_tree, { 20, 20 }, { 0, 10 }, Web::Compositor::SnapContainerHandling::ScrollOnCompositor);
    EXPECT(!result.blocked_by_main_thread_region);
}

TEST_CASE(blocking_wheel_event_hit_testing_fails_closed_for_invalid_visual_context_indices)
{
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();
    Web::Compositor::AsyncScrollingState state;
    state.has_blocking_wheel_event_listeners = true;
    state.blocking_wheel_event_regions.append({
        .context = { Web::Painting::SpatialNodeIndex { 100 }, Web::Painting::NO_FRAME_NODE },
        .rect = { 0, 0, 100, 100 },
    });

    EXPECT(Web::Compositor::blocks_wheel_event_at_position(
        state, make_empty_display_list(visual_context_tree), &visual_context_tree, {}, { 20, 20 }));
}

TEST_CASE(async_scrolling_resolves_sticky_offsets_from_the_visual_context_tree)
{
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();
    auto viewport_scroll_node = visual_context_tree.append_spatial(Web::Painting::ScrollData {}, Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    // A 50px header 100px down the document, sticking to the top of the scrollport within a 2000px containing block.
    auto header_node = visual_context_tree.append_spatial(
        Web::Painting::StickyData {
            .scroller = viewport_scroll_node,
            .parent_sticky = {},
            .position_relative_to_scroller = { 0, 100 },
            .border_box_size = { 800, 50 },
            .scrollport_size = { 800, 600 },
            .containing_block_region = { 0, 0, 800, 2000 },
            .needs_parent_offset_adjustment = true,
            .inset_top = 0.f,
            .inset_right = {},
            .inset_bottom = {},
            .inset_left = {},
        },
        viewport_scroll_node);
    // A 10px bar inside the header, sticking 20px below the scrollport top within the header's box.
    auto bar_node = visual_context_tree.append_spatial(
        Web::Painting::StickyData {
            .scroller = viewport_scroll_node,
            .parent_sticky = header_node,
            .position_relative_to_scroller = { 0, 110 },
            .border_box_size = { 800, 10 },
            .scrollport_size = { 800, 600 },
            .containing_block_region = { 0, 100, 800, 50 },
            .needs_parent_offset_adjustment = true,
            .inset_top = 20.f,
            .inset_right = {},
            .inset_bottom = {},
            .inset_left = {},
        },
        header_node);

    auto scroll_tree = make_scroll_tree_with_viewport_scroll_node(2000);
    Web::Painting::ScrollStateSnapshot snapshot;

    auto scroll_offsets = scroll_tree.apply_scroll_delta(viewport_node_id, { 0, 300 }, visual_context_tree, snapshot);
    EXPECT_EQ(scroll_offsets.size(), 1u);
    EXPECT_EQ(snapshot.device_offset_for_index(viewport_scroll_node), (Gfx::FloatPoint { 0, -300 }));
    // The scrollport top passed the header by 200px, so the header follows it by that much.
    EXPECT_EQ(snapshot.device_offset_for_index(header_node), (Gfx::FloatPoint { 0, 200 }));
    // With the header at 300, the bar sits at 310 and its inset asks for 320.
    EXPECT_EQ(snapshot.device_offset_for_index(bar_node), (Gfx::FloatPoint { 0, 10 }));

    // Past the end of the containing block, the header pins to the block's bottom edge and the bar to the header's.
    EXPECT(scroll_tree.set_scroll_offset(viewport_node_id, { 0, 1960 }, visual_context_tree, snapshot).has_value());
    EXPECT_EQ(snapshot.device_offset_for_index(header_node), (Gfx::FloatPoint { 0, 1850 }));
    EXPECT_EQ(snapshot.device_offset_for_index(bar_node), (Gfx::FloatPoint { 0, 20 }));

    // Scrolled back above the header, nothing sticks.
    EXPECT(scroll_tree.set_scroll_offset(viewport_node_id, { 0, 50 }, visual_context_tree, snapshot).has_value());
    EXPECT_EQ(snapshot.device_offset_for_index(header_node), (Gfx::FloatPoint { 0, 0 }));
    EXPECT_EQ(snapshot.device_offset_for_index(bar_node), (Gfx::FloatPoint { 0, 0 }));
}
