/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibTest/TestCase.h>
#include <Tests/LibCompositing/DisplayListTestHelpers.h>

using namespace Compositing;

static Web::UniqueNodeID const document_id { 1 };
static SpatialNodeIndex const scroll_node_index { 1 };

static AccumulatedVisualContextTree tree_with_one_scroll_node()
{
    VisualContextTreeTestBuilder builder;
    builder.append_scroll(VISUAL_VIEWPORT_NODE_INDEX);
    return builder.finish();
}

static Compositing::AsyncScrollingState state_from(AccumulatedVisualContextTree const& tree, TestDisplayList command_bytes)
{
    auto display_list = decode_display_list(tree, move(command_bytes), {}, DisplayList::AsyncScrollingMetadata { .viewport_rect = { 0, 0, 100, 100 } });
    return Compositing::async_scrolling_state_from_display_list(*display_list);
}

static void append_scroll_node(TestDisplayList& command_bytes)
{
    append_display_list_command(
        command_bytes,
        CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 2 },
            .scroll_node_index = scroll_node_index,
            .parent_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 400 },
            .scroll_node_kind = CompositorScrollNodeKind::Element,
            .pseudo_element_type = 0,
            .is_viewport = false,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        });
}

static void append_snap_area(TestDisplayList& command_bytes)
{
    append_display_list_command(
        command_bytes,
        CompositorSnapArea {
            .document_id = document_id,
            .scroll_node_index = scroll_node_index,
            .area_node_id = Web::UniqueNodeID { 3 },
            .pseudo_element_type = 0,
            .rect = Web::CSSPixelRect { 0, 0, 100, 100 },
            .align_x = to_underlying(Compositing::SnapAlign::None),
            .align_y = to_underlying(Compositing::SnapAlign::Start),
            .always_stop = false,
        });
}

TEST_CASE(a_snap_area_recorded_without_its_container_is_ignored)
{
    auto tree = tree_with_one_scroll_node();
    TestDisplayList command_bytes;
    append_scroll_node(command_bytes);
    append_snap_area(command_bytes);

    auto state = state_from(tree, move(command_bytes));
    EXPECT_EQ(state.scroll_nodes.size(), 1u);
    EXPECT(state.snap_containers.is_empty());
}
