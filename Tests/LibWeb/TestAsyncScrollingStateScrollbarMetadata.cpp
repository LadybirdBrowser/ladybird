/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibTest/TestCase.h>
#include <Tests/LibWeb/DisplayListTestHelpers.h>

using namespace Compositing;

static UniqueNodeID const document_id { 1 };
static UniqueNodeID const scroller_node_id { 7 };

struct TreeWithNestedScroller {
    AccumulatedVisualContextTree tree;
    SpatialNodeIndex viewport_scroll_node_index;
    SpatialNodeIndex nested_scroll_node_index;
};

static TreeWithNestedScroller tree_with_nested_scroller()
{
    VisualContextTreeTestBuilder builder;
    auto viewport_scroll_node_index = builder.append_scroll(VISUAL_VIEWPORT_NODE_INDEX);
    auto nested_scroll_node_index = builder.append_scroll(viewport_scroll_node_index);
    return { builder.finish(), viewport_scroll_node_index, nested_scroll_node_index };
}

static Compositing::AsyncScrollingState state_from(AccumulatedVisualContextTree const& tree, ByteBuffer command_bytes)
{
    auto display_list = decode_display_list(tree, move(command_bytes), {}, DisplayList::AsyncScrollingMetadata { .viewport_rect = { 0, 0, 100, 100 } });
    return Compositing::async_scrolling_state_from_display_list(*display_list);
}

static ContextRef in_spatial_node(SpatialNodeIndex spatial)
{
    return { spatial };
}

static void append_nested_scroll_node(ByteBuffer& command_bytes, TreeWithNestedScroller const& nested)
{
    append_display_list_command(
        command_bytes,
        CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = scroller_node_id,
            .scroll_node_index = nested.nested_scroll_node_index,
            .parent_scroll_node_index = nested.viewport_scroll_node_index,
            .scrollport_rect = { 10, 10, 40, 40 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = CompositorScrollNodeKind::Element,
            .pseudo_element_type = 0,
            .is_viewport = false,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        },
        {},
        in_spatial_node(nested.viewport_scroll_node_index));
}

static void append_wheel_hit_test_target(ByteBuffer& command_bytes, SpatialNodeIndex target_scroll_node_index, Gfx::FloatRect rect, ContextRef context)
{
    append_display_list_command(
        command_bytes,
        CompositorWheelHitTestTarget {
            .document_id = document_id,
            .target_scroll_node_index = target_scroll_node_index,
            .rect = rect,
        },
        {},
        context);
}

static void append_scrollbar_painted_by_display_list(ByteBuffer& command_bytes, SpatialNodeIndex scroll_node_index, ContextRef context, bool display_list_paints_enlarged_scrollbar)
{
    append_display_list_command(
        command_bytes,
        CompositorScrollbar {
            .document_id = document_id,
            .scroll_node_index = scroll_node_index,
            .gutter_rect = {},
            .thumb_rect = { 47, 11, 2, 10 },
            .track_rect = { 46, 10, 4, 40 },
            .expanded_gutter_rect = { 42, 10, 8, 40 },
            .expanded_thumb_rect = { 44, 11, 4, 10 },
            .scroll_size = 0.3,
            .expanded_scroll_size = 0.3,
            .min_scroll_offset = 0,
            .max_scroll_offset = 100,
            .thumb_color = Gfx::Color::Black,
            .track_color = Gfx::Color::Transparent,
            .vertical = true,
            .is_painted_by_compositor = false,
            .display_list_paints_enlarged_scrollbar = display_list_paints_enlarged_scrollbar,
        },
        {},
        context);
}

TEST_CASE(scrollbar_painted_by_display_list_keeps_its_context_and_geometry)
{
    auto nested = tree_with_nested_scroller();
    ByteBuffer command_bytes;
    append_nested_scroll_node(command_bytes, nested);
    append_scrollbar_painted_by_display_list(command_bytes, nested.nested_scroll_node_index, in_spatial_node(nested.viewport_scroll_node_index), true);

    auto state = state_from(nested.tree, move(command_bytes));
    EXPECT_EQ(state.scrollbars.size(), 1u);
    auto const& scrollbar = state.scrollbars.first();
    EXPECT_EQ(scrollbar.context, in_spatial_node(nested.viewport_scroll_node_index));
    EXPECT_EQ(scrollbar.scroll_node_index, nested.nested_scroll_node_index);
    EXPECT_EQ(scrollbar.track_rect, (Gfx::IntRect { 46, 10, 4, 40 }));
    EXPECT_EQ(scrollbar.expanded_gutter_rect, (Gfx::IntRect { 42, 10, 8, 40 }));
    EXPECT(!scrollbar.is_painted_by_compositor);
    EXPECT(scrollbar.display_list_paints_enlarged_scrollbar);
    EXPECT(scrollbar.vertical);
}

TEST_CASE(scrollbar_names_its_scroller_by_stable_id)
{
    auto nested = tree_with_nested_scroller();
    ByteBuffer command_bytes;
    // The scrollbar is recorded after its scroll node in paint order, but resolution does not depend on that.
    append_scrollbar_painted_by_display_list(command_bytes, nested.nested_scroll_node_index, in_spatial_node(nested.viewport_scroll_node_index), false);
    append_nested_scroll_node(command_bytes, nested);

    auto state = state_from(nested.tree, move(command_bytes));
    EXPECT_EQ(state.scrollbars.size(), 1u);
    auto const& stable_id = state.scrollbars.first().scroller_stable_node_id;
    EXPECT(stable_id.has_value());
    EXPECT_EQ(stable_id->node_id, scroller_node_id);
    EXPECT_EQ(stable_id->kind, Compositing::AsyncScrollNodeKind::Element);
}

TEST_CASE(scrollbar_without_a_scroll_node_has_no_stable_id)
{
    auto nested = tree_with_nested_scroller();
    ByteBuffer command_bytes;
    append_scrollbar_painted_by_display_list(command_bytes, nested.nested_scroll_node_index, in_spatial_node(nested.viewport_scroll_node_index), false);

    auto state = state_from(nested.tree, move(command_bytes));
    EXPECT_EQ(state.scrollbars.size(), 1u);
    EXPECT(!state.scrollbars.first().scroller_stable_node_id.has_value());
}

TEST_CASE(scrollbars_and_wheel_hit_test_targets_share_one_paint_order)
{
    auto nested = tree_with_nested_scroller();
    auto outer_context = in_spatial_node(nested.viewport_scroll_node_index);
    auto inner_context = in_spatial_node(nested.nested_scroll_node_index);

    ByteBuffer command_bytes;
    append_wheel_hit_test_target(command_bytes, nested.nested_scroll_node_index, { 10, 10, 40, 40 }, outer_context);
    append_nested_scroll_node(command_bytes, nested);
    append_wheel_hit_test_target(command_bytes, nested.nested_scroll_node_index, { 10, 10, 40, 140 }, inner_context);
    append_scrollbar_painted_by_display_list(command_bytes, nested.nested_scroll_node_index, outer_context, false);
    append_wheel_hit_test_target(command_bytes, nested.viewport_scroll_node_index, { 40, 0, 30, 30 }, outer_context);

    auto state = state_from(nested.tree, move(command_bytes));
    EXPECT_EQ(state.wheel_hit_test_targets.size(), 3u);
    EXPECT_EQ(state.scrollbars.size(), 1u);
    EXPECT_EQ(state.wheel_hit_test_targets[0].paint_order_index, 0u);
    EXPECT_EQ(state.wheel_hit_test_targets[1].paint_order_index, 1u);
    EXPECT_EQ(state.scrollbars[0].paint_order_index, 2u);
    EXPECT_EQ(state.wheel_hit_test_targets[2].paint_order_index, 3u);
}
