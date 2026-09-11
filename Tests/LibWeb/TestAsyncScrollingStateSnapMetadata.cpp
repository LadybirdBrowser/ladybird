/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/VisualContextTreeTestBuilder.h>
#include <Tests/LibWeb/DisplayListTestHelpers.h>

using namespace Web;
using namespace Web::Painting;

static UniqueNodeID const document_id { 1 };
static SpatialNodeIndex const scroll_node_index { 1 };

static AccumulatedVisualContextTree tree_with_one_scroll_node()
{
    VisualContextTreeTestBuilder builder;
    builder.append_scroll(VISUAL_VIEWPORT_NODE_INDEX);
    return builder.finish();
}

static Compositor::AsyncScrollingState state_from(AccumulatedVisualContextTree const& tree, ByteBuffer command_bytes, double device_pixels_per_css_pixel)
{
    auto display_list = decode_display_list(tree, move(command_bytes), {}, DisplayList::AsyncScrollingMetadata { .viewport_rect = { 0, 0, 100, 100 }, .device_pixels_per_css_pixel = device_pixels_per_css_pixel });
    return Compositor::async_scrolling_state_from_display_list(*display_list);
}

static void append_scroll_node(ByteBuffer& command_bytes)
{
    append_display_list_command(
        command_bytes,
        CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = UniqueNodeID { 2 },
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
            .snaps_scroll_position_horizontally = false,
            .snaps_scroll_position_vertically = true,
        });
}

static void append_snap_container(ByteBuffer& command_bytes)
{
    append_display_list_command(
        command_bytes,
        CompositorSnapContainer {
            .document_id = document_id,
            .scroll_node_index = scroll_node_index,
            .snapport = CSSPixelRect { 10, 10, 80, 80 },
            .min_scroll_offset = CSSPixelPoint { 0, 0 },
            .max_scroll_offset = CSSPixelPoint { 0, 400 },
            .strictness = to_underlying(CSS::ScrollSnapStrictness::Mandatory),
            .snaps_x = false,
            .snaps_y = true,
            .horizontal_writing_mode = true,
        });
}

static void append_snap_area(ByteBuffer& command_bytes, i64 node_id, u8 pseudo_element_type, CSSPixelRect rect, CSS::ScrollSnapAlign align_y, bool always_stop)
{
    append_display_list_command(
        command_bytes,
        CompositorSnapArea {
            .document_id = document_id,
            .scroll_node_index = scroll_node_index,
            .area_node_id = UniqueNodeID { node_id },
            .pseudo_element_type = pseudo_element_type,
            .rect = rect,
            .align_x = to_underlying(CSS::ScrollSnapAlign::None),
            .align_y = to_underlying(align_y),
            .always_stop = always_stop,
        });
}

TEST_CASE(snap_geometry_is_read_from_the_display_list)
{
    auto tree = tree_with_one_scroll_node();
    ByteBuffer command_bytes;
    append_scroll_node(command_bytes);
    append_snap_container(command_bytes);
    append_snap_area(command_bytes, 3, 0, CSSPixelRect { 0, 0, 100, 100 }, CSS::ScrollSnapAlign::Start, false);
    append_snap_area(command_bytes, 2, 3, CSSPixelRect { CSSPixels(0), CSSPixels(100.5), CSSPixels(100), CSSPixels(100) }, CSS::ScrollSnapAlign::Center, true);

    auto state = state_from(tree, move(command_bytes), 2.0);
    EXPECT_EQ(state.device_pixels_per_css_pixel, 2.0);
    EXPECT_EQ(state.scroll_nodes.size(), 1u);
    EXPECT_EQ(state.snap_containers.size(), 1u);

    auto const& container = state.snap_containers.first();
    EXPECT_EQ(container.node_id, state.scroll_nodes.first().node_id);
    EXPECT_EQ(container.geometry.snapport, CSSPixelRect(10, 10, 80, 80));
    EXPECT_EQ(container.geometry.min_scroll_offset, CSSPixelPoint(0, 0));
    EXPECT_EQ(container.geometry.max_scroll_offset, CSSPixelPoint(0, 400));
    EXPECT_EQ(container.geometry.strictness, CSS::ScrollSnapStrictness::Mandatory);
    EXPECT(!container.geometry.axes.x);
    EXPECT(container.geometry.axes.y);
    EXPECT(container.geometry.horizontal_writing_mode);

    EXPECT_EQ(container.areas.size(), 2u);
    auto const& first_area = container.areas[0];
    EXPECT_EQ(first_area.identity.node_id, UniqueNodeID(3));
    EXPECT(!first_area.identity.is_pseudo_element());
    EXPECT_EQ(first_area.rect, CSSPixelRect(0, 0, 100, 100));
    EXPECT_EQ(first_area.align_x, CSS::ScrollSnapAlign::None);
    EXPECT_EQ(first_area.align_y, CSS::ScrollSnapAlign::Start);
    EXPECT(!first_area.always_stop);

    auto const& second_area = container.areas[1];
    EXPECT_EQ(second_area.identity.node_id, UniqueNodeID(2));
    EXPECT(second_area.identity.pseudo_element() == CSS::PseudoElement::Before);
    EXPECT_EQ(second_area.rect.y(), CSSPixels(100.5));
    EXPECT_EQ(second_area.align_y, CSS::ScrollSnapAlign::Center);
    EXPECT(second_area.always_stop);
}

TEST_CASE(a_snap_area_recorded_without_its_container_is_ignored)
{
    auto tree = tree_with_one_scroll_node();
    ByteBuffer command_bytes;
    append_scroll_node(command_bytes);
    append_snap_area(command_bytes, 3, 0, CSSPixelRect { 0, 0, 100, 100 }, CSS::ScrollSnapAlign::Start, false);

    auto state = state_from(tree, move(command_bytes), 1.0);
    EXPECT_EQ(state.scroll_nodes.size(), 1u);
    EXPECT(state.snap_containers.is_empty());
}
